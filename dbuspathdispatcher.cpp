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
void DBusPathDispatcher::handleServiceOwnerChange(const QString &connName, const QString &service,
                                                  const QString &newOwner) {
    QList<QPointer<DBusAdaptor>> holders;
    bool acquired = false;
    bool lost = false;
    {
        QMutexLocker locker(&registryMutex());
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

bool DBusPathDispatcher::attach(QDBusConnection conn, const QString &path, const QString &service,
                                DBusAdaptor *adaptor, bool allowReplacement, bool replaceExisting,
                                bool queueOnBusy) {
    const QString connName = conn.name();
    const PathKey key{connName, path};

    DBusPathDispatcher *disp = nullptr;
    {
        QMutexLocker locker(&registryMutex());
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
        QMutexLocker locker(&registryMutex());
        ServiceClaim &claim = serviceClaims()[svcKey];
        const bool flagged = allowReplacement || replaceExisting || queueOnBusy;
        // Owner-change watch (flagged claims only): nameAcquired for queued
        // acquisitions and nameLost for takeovers are driven by the daemon's
        // owner changes. An unflagged claim cannot be taken away and releases
        // its name only on its own teardown, so it needs no watch.
        if (flagged && !claim.watch) {
            auto *iface = conn.interface();
            const QString connNameLocal = connName;
            claim.watch =
                QObject::connect(iface, &QDBusConnectionInterface::serviceOwnerChanged, iface,
                                 [connNameLocal, service](const QString &name, const QString &,
                                                          const QString &newOwner) {
                                     if (name == service)
                                         DBusPathDispatcher::handleServiceOwnerChange(
                                             connNameLocal, service, newOwner);
                                 });
        }
        if (claim.refs == 0) {
            claim.baseService = conn.baseService();
            if (!flagged) {
                // Unflagged claim (the default): synchronous Qt registration —
                // the name is owned by the time attach returns, so immediate
                // wire calls (including same-connection local-loop calls)
                // route correctly, exactly as before 0.9.0.
                if (conn.registerService(service)) {
                    claim.owned = true;
                    adaptor->nameAcquiredInternal();
                } else {
                    qmlInfo(adaptor) << "Failed to register service" << service;
                }
            } else {
                // Flagged claim: RequestName carrying the flags, sent
                // non-blocking. Acquisition or queueing is observed through
                // the owner-change watch — nameAcquired fires from there
                // (consumers with flags wait for nameAcquired before
                // relying on the name being routed).
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
                                                  qPrintable(service),
                                                  qPrintable(r.error().message()));
                                     } else if (r.value() == 0) {
                                         qWarning("dbusqml: Failed to register service %s (in "
                                                  "use, not queued)",
                                                  qPrintable(service));
                                     }
                                     w->deleteLater();
                                 });
            }
        }
        claim.refs++;
        claim.holders.append(adaptor);
    }

    return true;
}

void DBusPathDispatcher::detach(QDBusConnection conn, const QString &path, const QString &service,
                                DBusAdaptor *adaptor) {
    const QString connName = conn.name();

    DBusPathDispatcher *disp = nullptr;
    {
        QMutexLocker locker(&registryMutex());
        auto it = dispatchers().find({connName, path});
        if (it != dispatchers().end())
            disp = it.value();
    }
    if (disp)
        disp->detachAdaptor(adaptor);

    if (!service.isEmpty()) {
        const ServiceKey svcKey{connName, service};
        QMutexLocker locker(&registryMutex());
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
                serviceClaims().erase(it);
                if (!conn.unregisterService(service))
                    qmlInfo(adaptor) << "Failed to unregister service" << service;
            } else {
                claim.refs = refs;
            }
        }
    }
}

int DBusPathDispatcher::liveCount() {
    QMutexLocker locker(&registryMutex());
    return dispatchers().size();
}

void DBusPathDispatcher::attachAdaptor(DBusAdaptor *adaptor) {
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
        QMutexLocker locker(&registryMutex());
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
