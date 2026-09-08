#include "dbuspathdispatcher.h"

#include "dbusadaptor.h"

#include <QDBusConnectionInterface>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <qqmlinfo.h>

namespace {

using PathKey = QPair<QString, QString>;    // (connection name, path)
using ServiceKey = QPair<QString, QString>; // (connection name, service)

// One well-known-name claim: refcount over co-located adaptors, the
// acquisition flags of the FIRST claimant (later conflicting flag sets warn
// and are ignored — 0.5.1 duplicate-iface precedent), and whether we
// currently own (or are queued for) the name.
struct ServiceClaim {
    int refs = 0;
    bool owned = false;
    bool queued = false;
    bool pending = false;     // attach registered interest but ownership is
                              // not yet known (unflagged blocking register
                              // moved outside the lock — commit 12). A second
                              // same-name attach sees pending and skips its
                              // own registration instead of losing the name.
    bool tearingDown = false; // detach tombstone: last holder left, the
                              // blocking ReleaseName is in flight — a
                              // concurrent attach ADOPTS this record instead
                              // of re-registering (never re-register under a
                              // tombstone; never unregister an adopted name)
    bool allowReplacement = false;
    bool replaceExisting = false;
    bool queueOnBusy = false;
    QString baseService; // our unique bus name at claim time
    QMetaObject::Connection watch;
    QList<QPointer<DBusAdaptor>> holders;
};

QMutex &registryMutex() {
    static QMutex m;
    return m;
}

// Debug-only invariant guard (commit 11, part 4): a mutex must NEVER span a
// blocking bus call — the party that needs the mutex (QtDBus's manager
// thread, the sole socket reader) also owns bus I/O progress, so the pairing
// is a deadly embrace by construction. Any future recurrence trips this
// assert deterministically instead of hanging the suite for 300s.
#ifdef QT_DEBUG
thread_local bool g_holdingRegistryMutex = false;
struct RegistryMutexGuard {
    explicit RegistryMutexGuard(QMutex &m) : m_locker(&m) { g_holdingRegistryMutex = true; }
    ~RegistryMutexGuard() { g_holdingRegistryMutex = false; }
    QMutexLocker<QMutex> m_locker;
};
inline void assertNoRegistryMutex(const char *site) {
    Q_ASSERT_X(!g_holdingRegistryMutex, site, "blocking bus call under registryMutex");
}
#else
struct RegistryMutexGuard {
    explicit RegistryMutexGuard(QMutex &m) : m_locker(&m) {}
    QMutexLocker<QMutex> m_locker;
};
inline void assertNoRegistryMutex(const char *) {}
#endif

QHash<PathKey, DBusPathDispatcher *> &dispatchers() {
    static QHash<PathKey, DBusPathDispatcher *> h;
    return h;
}

QHash<ServiceKey, ServiceClaim> &serviceClaims() {
    static QHash<ServiceKey, ServiceClaim> h;
    return h;
}

} // namespace

// Owner-change watch: a name we claimed was acquired (possibly after
// queueing) or lost to another owner.
//
// T1 DIAGNOSIS (for-all-times Phase 1, 2026-09-08): a per-claim notifier
// QObject with real queued connections was prototyped here (D9 shape)
// and REVERTED before commit: the churn stress SEGVs inside QML signal
// delivery (isSignalConnected on a half-destroyed declarative adaptor),
// and the anchor's flagged-claim nameAcquired never reaches its QSignalSpy
// (queued notifier delivery + beginCreate spy timing interact — 46s
// QTRY timeout, 3/3 deterministic). The direct manager-thread call below
// is therefore KEPT (pre-existing behavior, churn-green); the notifier
// shape needs a dedicated cycle with lifetime tests, NOT a drive-by.
// The T1 hazard stays TRACKED (audit-2 addendum).
void DBusPathDispatcher::handleServiceOwnerChange(const QString &connName, const QString &service,
                                                  const QString &newOwner) {
    QList<QPointer<DBusAdaptor>> holders;
    bool acquired = false;
    bool lost = false;
    {
        RegistryMutexGuard locker(registryMutex());
        auto it = serviceClaims().find({connName, service});
        if (it == serviceClaims().end())
            return;
        ServiceClaim &claim = it.value();
        if (newOwner == claim.baseService && !claim.owned) {
            claim.owned = true;
            claim.queued = false;
            acquired = true;
        } else if (claim.owned && newOwner != claim.baseService) {
            claim.owned = false;
            lost = true;
        } else {
            return;
        }
        holders = claim.holders;
    }
    for (const auto &h : holders) {
        if (!h)
            continue;
        if (acquired)
            h->nameAcquiredInternal();
        else if (lost)
            h->nameLostInternal();
    }
}

DBusPathDispatcher::DBusPathDispatcher(const QString &connName, const QString &path,
                                       const QDBusConnection &conn)
    : m_connName(connName), m_path(path), m_conn(conn) {}

// Commit 12 helpers: keep attach()'s two claim paths readable.
inline bool flaggedForClaim(bool allowReplacement, bool replaceExisting, bool queueOnBusy) {
    return allowReplacement || replaceExisting || queueOnBusy;
}

inline void armClaimWatch(const QDBusConnection &conn, const QString &connName,
                          const QString &service, ServiceClaim &claim) {
    // Owner-change watch (flagged claims only): nameAcquired for queued
    // acquisitions and nameLost for takeovers are driven by the daemon's
    // owner changes. An unflagged claim cannot be taken away and releases
    // its name only on its own teardown, so it needs no watch.
    if (claim.watch)
        return;
    auto *iface = conn.interface();
    claim.watch = QObject::connect(
        iface, &QDBusConnectionInterface::serviceOwnerChanged, iface,
        [connName, service](const QString &name, const QString &, const QString &newOwner) {
            if (name == service)
                DBusPathDispatcher::handleServiceOwnerChange(connName, service, newOwner);
        });
}

inline void sendFlaggedRequest(const QDBusConnection &conn, DBusPathDispatcher *disp,
                               const QString &service, DBusAdaptor *adaptor, bool allowReplacement,
                               bool replaceExisting, bool queueOnBusy) {
    // Flagged claim: RequestName carrying the flags, sent non-blocking.
    // Acquisition or queueing is observed through the owner-change watch —
    // nameAcquired fires from there (consumers with flags wait for
    // nameAcquired before relying on the name being routed).
    const uint requestFlags = (allowReplacement ? 1u : 0u) | // ALLOW_REPLACEMENT
                              (replaceExisting ? 2u : 0u) |  // REPLACE_EXISTING
                              (queueOnBusy ? 0u : 4u);       // DO_NOT_QUEUE
    QDBusMessage req = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("RequestName"));
    req.setArguments({service, requestFlags});
    auto pending = conn.asyncCall(req);
    auto *watcher = new QDBusPendingCallWatcher(pending, disp);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, adaptor,
                     [service](QDBusPendingCallWatcher *w) {
                         QDBusPendingReply<uint> r = *w;
                         if (r.isError()) {
                             qWarning("dbusqml: Failed to register service %s: %s",
                                      qPrintable(service), qPrintable(r.error().message()));
                         } else if (r.value() == 0) {
                             qWarning("dbusqml: Failed to register service %s (in "
                                      "use, not queued)",
                                      qPrintable(service));
                         }
                         w->deleteLater();
                     });
}

bool DBusPathDispatcher::attach(QDBusConnection conn, const QString &path, const QString &service,
                                DBusAdaptor *adaptor, bool allowReplacement, bool replaceExisting,
                                bool queueOnBusy) {
    const QString connName = conn.name();
    const PathKey key{connName, path};

    DBusPathDispatcher *disp = nullptr;
    {
        RegistryMutexGuard locker(registryMutex());
        auto it = dispatchers().find(key);
        if (it != dispatchers().end()) {
            disp = it.value();
        } else {
            disp = new DBusPathDispatcher(connName, path, conn);
            if (!conn.registerVirtualObject(path, disp)) {
                qmlInfo(adaptor) << "Failed to register object at" << path;
                delete disp;
                return false;
            }
            dispatchers().insert(key, disp);
        }
    }

    disp->attachAdaptor(adaptor);

    if (!service.isEmpty()) {
        const ServiceKey svcKey{connName, service};
        // Commit 12: the blocking registerService happens OUTSIDE the lock
        // (same embrace as detach — the default unflagged path). The claim
        // record is pre-inserted in `pending` state FIRST, so a second
        // same-name attach skips its own registration instead of losing the
        // name; ownership is resolved after the bus call returns.
        bool doRegister = false;
        {
            RegistryMutexGuard locker(registryMutex());
            ServiceClaim &claim = serviceClaims()[svcKey];
            // Tombstone adoption (detach's teardown marker): the name is
            // still registered at the daemon — a ReleaseName is either in
            // flight or was skipped — so take over the record: refresh the
            // claim state, re-arm the watch if these flags need one, and
            // count the holder. No re-register, no unregister window to
            // clobber.
            if (claim.tearingDown) {
                claim.tearingDown = false;
                claim.refs = 0;
                claim.holders.clear();
                claim.owned = false;
                claim.queued = false;
                claim.pending = false;
                claim.baseService = conn.baseService();
            }
            if (claim.pending || claim.refs > 0) {
                // Registration already in flight or owned — join as a
                // holder; ownership notification arrives via the watch (or
                // is already recorded) exactly as for any co-located
                // second adaptor.
                claim.refs++;
                claim.holders.append(adaptor);
            } else {
                claim.refs = 1;
                claim.holders.append(adaptor);
                claim.baseService = conn.baseService();
                claim.pending = !flaggedForClaim(allowReplacement, replaceExisting, queueOnBusy);
                doRegister = claim.pending;
                if (flaggedForClaim(allowReplacement, replaceExisting, queueOnBusy)) {
                    armClaimWatch(conn, connName, service, claim);
                    // Flagged path unchanged: async RequestName, observed
                    // through the watch.
                    sendFlaggedRequest(conn, disp, service, adaptor, allowReplacement,
                                       replaceExisting, queueOnBusy);
                    claim.pending = false;
                }
            }
        }
        if (doRegister) {
            // OUTSIDE the lock (guard asserts it in debug). Unflagged path:
            // synchronous Qt registration, exactly as before.
            assertNoRegistryMutex("DBusPathDispatcher::attach");
            const bool ok = conn.registerService(service);
            RegistryMutexGuard locker(registryMutex());
            auto it = serviceClaims().find(svcKey);
            if (it == serviceClaims().end() || it.value().tearingDown) {
                // Detach raced us and tore the record down (or tombstoned
                // it): release what we just acquired so the name does not
                // leak, then re-attach cleanly through the normal path.
                locker.m_locker.unlock();
                if (ok)
                    conn.unregisterService(service);
                return attach(conn, path, service, adaptor, allowReplacement, replaceExisting,
                              queueOnBusy);
            }
            ServiceClaim &claim = it.value();
            claim.pending = false;
            if (ok) {
                claim.owned = true;
                locker.m_locker.unlock();
                adaptor->nameAcquiredInternal();
            } else {
                locker.m_locker.unlock();
                qmlInfo(adaptor) << "Failed to register service" << service;
            }
        }
    }

    return true;
}

void DBusPathDispatcher::detach(QDBusConnection conn, const QString &path, const QString &service,
                                DBusAdaptor *adaptor) {
    const QString connName = conn.name();

    DBusPathDispatcher *disp = nullptr;
    {
        RegistryMutexGuard locker(registryMutex());
        auto it = dispatchers().find({connName, path});
        if (it != dispatchers().end())
            disp = it.value();
    }
    if (disp)
        disp->detachAdaptor(adaptor);

    if (!service.isEmpty()) {
        // Commit 11, parts 1-3: the blocking ReleaseName happens OUTSIDE the
        // lock; gated on ownership (a lost name makes it a guaranteed-futile
        // NOT_OWNER round trip); the claim becomes a TOMBSTONE (not erased)
        // so a concurrent same-name attach adopts it instead of registering
        // into the unregister window.
        const ServiceKey svcKey{connName, service};
        bool needUnregister = false;
        {
            RegistryMutexGuard locker(registryMutex());
            auto it = serviceClaims().find(svcKey);
            if (it != serviceClaims().end()) {
                ServiceClaim &claim = it.value();
                for (int i = 0; i < claim.holders.size(); ++i) {
                    if (claim.holders.at(i).data() == adaptor) {
                        claim.holders.removeAt(i);
                        break;
                    }
                }
                const int refs = claim.refs - 1;
                if (refs <= 0) {
                    if (claim.watch) {
                        QObject::disconnect(claim.watch);
                        claim.watch = QMetaObject::Connection();
                    }
                    claim.refs = 0;
                    claim.holders.clear();
                    // Part 2: only an OWNED name is worth the blocking
                    // ReleaseName. A lost name (takeover path) would return
                    // NOT_OWNER — futile by construction, and it used to log
                    // a spurious "Failed to unregister" on the happy path.
                    needUnregister = claim.owned;
                    claim.owned = false;
                    // Part 3: tombstone — stays in the map until the bus call
                    // below returns. attach() adopts it (see there).
                    claim.tearingDown = true;
                } else {
                    claim.refs = refs;
                }
            }
        }
        if (needUnregister) {
            // Part 1 + 4: OUTSIDE the lock (guard asserts it in debug).
            assertNoRegistryMutex("DBusPathDispatcher::detach");
            const bool released = conn.unregisterService(service);
            {
                RegistryMutexGuard locker(registryMutex());
                auto it = serviceClaims().find(svcKey);
                // Erase the tombstone — UNLESS a concurrent attach adopted
                // it meanwhile (refs > 0 or !tearingDown): then the adopters
                // own the record and the bus call above was theirs to skip
                // (adopt path never re-registers, so nothing to un-register).
                if (it != serviceClaims().end() && it.value().tearingDown && it.value().refs == 0) {
                    serviceClaims().erase(it);
                }
            }
            if (!released) {
                const QString err = conn.lastError().message();
                // NOT_OWNER (lost the name between gating and ReleaseName)
                // is expected fallout of a takeover, not a failure.
                if (!err.contains(QStringLiteral("NOT_OWNER")) &&
                    !err.contains(QStringLiteral("not an owner")))
                    qmlInfo(adaptor) << "Failed to unregister service" << service << err;
            }
        }
    }
}

int DBusPathDispatcher::liveCount() {
    RegistryMutexGuard locker(registryMutex());
    return dispatchers().size();
}

void DBusPathDispatcher::attachAdaptor(DBusAdaptor *adaptor) {
    // T2 (for-all-times Phase 1): claim-scoped thread check would live in
    // attach()/detach() per (connection, service) — but the dispatcher only
    // sees (connection, path) here, and the registry already serializes
    // consumer access via RegistryMutexGuard. The enforceable contract is
    // the documented one (PARITY.md threading contract + API.md); a
    // QThread::currentThreadId assert per PATH is recorded here as the
    // T2 enforcement point if a future cycle wants it hard. No-op today
    // by decision: the churn/takeover/attach stress suite (TSan-clean
    // target, §6.2 CI) is the live guard, not an assert that would fire
    // on legitimate same-thread re-entrant attach (componentComplete →
    // attach → nameAcquiredInternal re-entrancy exists on this path).
    // Duplicate iface at one path: warn, keep attaching — first-attached wins
    // for iface-scoped calls (attach order is the routing order).
    const QString iface = adaptor->iface();
    if (!iface.isEmpty()) {
        for (const auto &a : m_adaptors) {
            if (a && a->iface() == iface) {
                qWarning("dbusqml: duplicate iface %s at path %s — first-attached adaptor wins",
                         qPrintable(iface), qPrintable(m_path));
                break;
            }
        }
    }
    m_adaptors.append(QPointer<DBusAdaptor>(adaptor));
}

void DBusPathDispatcher::detachAdaptor(DBusAdaptor *adaptor) {
    for (int i = 0; i < m_adaptors.size(); ++i) {
        if (m_adaptors.at(i).data() == adaptor) {
            m_adaptors.removeAt(i);
            break;
        }
    }

    if (!m_adaptors.isEmpty())
        return;

    // Last adaptor gone — drop the path and tear down the dispatcher.
    {
        RegistryMutexGuard locker(registryMutex());
        dispatchers().remove({m_connName, m_path});
    }
    m_conn.unregisterObject(m_path);
    deleteLater();
}

QString DBusPathDispatcher::introspect(const QString &) const {
    // Merge per-adaptor interface blocks cleanly: for co-located same-iface
    // adaptors, first-attached wins for dispatch — the advertised surface
    // mirrors that exactly. Naive concatenation would emit duplicate
    // members (duplicate signals/methods), which busctl/GDBus reject.
    QString xml;
    QSet<QString> servedIfaces;
    bool servedEmptyIface = false; // A17: empty ifaces dedupe too
    for (const auto &a : m_adaptors) {
        if (!a)
            continue;
        const QString iface = a->iface();
        if (!iface.isEmpty()) {
            if (servedIfaces.contains(iface))
                continue;
            servedIfaces.insert(iface);
        } else {
            // A17: two co-located empty-iface adaptors emitted duplicate
            // <interface name=""> blocks (busctl-reject class).
            if (servedEmptyIface)
                continue;
            servedEmptyIface = true;
        }
        xml += a->introspect(QString());
    }
    return xml;
}

bool DBusPathDispatcher::handleMessage(const QDBusMessage &msg, const QDBusConnection &conn) {
    const QString interface = msg.interface();

    // QtDBus serves introspect() for the Introspectable interface.
    if (interface == QStringLiteral("org.freedesktop.DBus.Introspectable"))
        return false;

    if (interface == QStringLiteral("org.freedesktop.DBus.Properties")) {
        // Route by the interface ARGUMENT to the matching adaptor.
        QString reqIface;
        if (!msg.arguments().isEmpty())
            reqIface = msg.arguments().first().toString();
        if (!reqIface.isEmpty()) {
            for (const auto &a : m_adaptors) {
                if (a && a->iface() == reqIface)
                    return a->handleMessage(msg, conn);
            }
            // No matching adaptor — route to the first so it reports
            // "No such interface" (preserves the single-adaptor error).
            if (!m_adaptors.isEmpty() && m_adaptors.first())
                return m_adaptors.first()->handleMessage(msg, conn);
            return false;
        }
        // Empty interface arg — try all in attach order, first that handles.
        for (const auto &a : m_adaptors) {
            if (a && a->handleMessage(msg, conn))
                return true;
        }
        return false;
    }

    if (!interface.isEmpty()) {
        for (const auto &a : m_adaptors) {
            if (a && a->iface() == interface)
                return a->handleMessage(msg, conn);
        }
        return false;
    }

    // Empty interface — member-name dispatch across adaptors in attach order.
    for (const auto &a : m_adaptors) {
        if (a && a->handleMessage(msg, conn))
            return true;
    }
    return false;
}
