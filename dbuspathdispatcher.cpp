#include "dbuspathdispatcher.h"

#include "dbusadaptor.h"
#include "dbusutils.h"

#include <QDBusConnectionInterface>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QHash>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QQueue>
#include <QSet>
#include <atomic>
#include <qqmlinfo.h>

Q_LOGGING_CATEGORY(lcDbusqmlDispatch, "dbusqml.dispatch", QtWarningMsg)

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
    // T1 relay state (blessed shape): notifyQueued coalesces manager-
    // thread observations into one pending wake; lastNotifiedOwned is
    // the ownership the relay last delivered, so the relay diffs and
    // only delivers real transitions (stale/duplicate posts harmless).
    bool notifyQueued = false;
    bool lastNotifiedOwned = false;
    // A2/A3 (the G1 fix): the RequestName reply drives the claim's initial
    // state. requestToken is a process-global monotonically increasing id,
    // never reused; latched on the claim at every send, bumped on every send,
    // on last detach, and on adoption — a reply whose token mismatches is
    // stale and a no-op. lostSinceSend: an owner-change to a foreign owner
    // for the current token marks it, making a later PRIMARY_OWNER reply a
    // no-op (astra's same-generation guard).
    uint requestToken = 0;
    bool lostSinceSend = false;
    QList<QPointer<DBusAdaptor>> holders;
};

// A5: the invariant every writer must keep — an owned claim has either been
// notified or has a note in flight (otherwise a joiner's CF-19 direct
// delivery would double with the pending note).
inline void assertClaimInvariant(const ServiceClaim &claim) {
    Q_ASSERT(!claim.owned || claim.lastNotifiedOwned || claim.notifyQueued);
}

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

// T1 relay mailbox: value-only notes, guarded by their own mutex. The
// queue mutex is NEVER nested inside registryMutex (enqueue takes the
// queue mutex alone; the claim-state update above runs under the registry
// lock but touches no queue state); the manager thread posts after
// unlock.
QQueue<OwnerChangeRelay::Note> &ownerChangeNotes() {
    static QQueue<OwnerChangeRelay::Note> q;
    return q;
}

QMutex &ownerChangeNotesMutex() {
    static QMutex m;
    return m;
}

std::atomic<bool> &ownerChangeDrainPosted() {
    static std::atomic<bool> f{false};
    return f;
}

// A6: the per-connection receivers (registry-owned). Keyed by connection
// name; created at the first flagged attach on that connection; purged only
// in handleConnectionLost. Access under registryMutex().
QHash<QString, OwnerChangeReceiver *> &ownerChangeReceivers() {
    static QHash<QString, OwnerChangeReceiver *> h;
    return h;
}

// A2: the process-global monotonically increasing request token — never
// reused (a per-claim counter would restart after erasure and collide).
std::atomic<uint> &nextRequestToken() {
    static std::atomic<uint> next{0};
    return next;
}

} // namespace

void OwnerChangeReceiver::onNameOwnerChanged(const QString &name, const QString &,
                                             const QString &newOwner) {
    // Fan out by service name into the same per-claim handling the
    // per-claim watch delivered (A6: the delivery path is unchanged).
    DBusPathDispatcher::handleServiceOwnerChange(m_connName, name, newOwner);
}

// Arm the connection's persistent subscription at the FIRST flagged attach
// on it — before that attach's RequestName is enqueued. The public
// QDBusConnection::connect hook install is a BlockingQueued call that
// enqueues the AddMatch before returning, so the subscription precedes the
// request on the socket by construction (the G1 wire pin, A8/P8).
// CALLER: registryMutex() held (attach's flagged branch).
void ensureOwnerChangeReceiver(QDBusConnection conn, const QString &connName) {
    if (ownerChangeReceivers().contains(connName))
        return;
    auto *receiver = new OwnerChangeReceiver(connName);
    conn.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                 QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
                 QStringLiteral("sss"), receiver,
                 SLOT(onNameOwnerChanged(QString, QString, QString)));
    ownerChangeReceivers().insert(connName, receiver);
}

void OwnerChangeRelay::postNote(Note n) {
    {
        QMutexLocker l(&ownerChangeNotesMutex());
        ownerChangeNotes().enqueue(std::move(n));
    }
    // Coalescing wake: at most one undelivered wake event in flight (the
    // flag resets at the top of event(); a note enqueued between the
    // drain's last dequeue and the reset still gets its own post — a
    // duplicate wake is harmless: the drain re-checks the queue).
    //
    // CALLER CONTRACT: call AFTER releasing registryMutex (postEvent to a
    // live relay never blocks, but keeping it outside preserves the
    // trivial lock order registryMutex -> postEventList.mutex only).
    if (!ownerChangeDrainPosted().exchange(true))
        QCoreApplication::postEvent(instance(), new QEvent(static_cast<QEvent::Type>(eventType())));
}

bool OwnerChangeRelay::event(QEvent *e) {
    if (e->type() != static_cast<QEvent::Type>(eventType()))
        return QObject::event(e);
    ownerChangeDrainPosted().store(false);
    for (;;) {
        Note note;
        {
            QMutexLocker l(&ownerChangeNotesMutex());
            if (ownerChangeNotes().isEmpty())
                return true;
            note = ownerChangeNotes().dequeue();
        }
        QList<QPointer<DBusAdaptor>> holders;
        bool acquired = false;
        bool drop = false;
        {
            RegistryMutexGuard locker(registryMutex());
            auto it = serviceClaims().find({note.connName, note.service});
            if (it == serviceClaims().end()) {
                qCDebug(lcDbusqmlDispatch)
                    << "relay drop [claim gone]:" << note.service << "on" << note.connName;
                drop = true; // Claim gone → drop the note. A detached
                             // adaptor wants no notification.
            } else if (!it.value().notifyQueued) {
                qCDebug(lcDbusqmlDispatch)
                    << "relay drop [stale/not-queued]:" << note.service << "on" << note.connName;
                drop = true; // Stale/duplicate post — already delivered.
            } else {
                ServiceClaim &claim = it.value();
                claim.notifyQueued = false;
                if (claim.owned == claim.lastNotifiedOwned) {
                    qCDebug(lcDbusqmlDispatch)
                        << "relay drop [coalesced/no net transition]:" << note.service << "on"
                        << note.connName << "owned=" << claim.owned
                        << "lastNotified=" << claim.lastNotifiedOwned;
                    drop = true; // Coalesced burst, no net transition.
                } else {
                    claim.lastNotifiedOwned = claim.owned;
                    acquired = claim.owned;
                    holders = claim.holders;
                }
            }
        }
        if (drop)
            continue;
        // Lock DROPPED before delivery (audit #4): QML handlers run
        // unlocked, so re-entrant attach/detach re-acquires the
        // non-recursive mutex uncontended-by-self, and blocking bus calls
        // in handlers stay legal under the commit-11/12 rule.
        assertNoRegistryMutex("OwnerChangeRelay::event");
        // MAIN thread: adaptor destruction is event-loop-serialized, so
        // the per-iteration QPointer re-check closes the
        // handler-deletes-a-later-holder window. Never touch `h` after
        // the emit (a handler may deleteLater it).
        for (const auto &h : holders) {
            if (!h)
                continue;
            // Foreign-thread adaptors (unsupported per the T2 contract):
            // main-thread delivery + loud warning (blessed plan shape;
            // qwen's refuse-to-arm alternative recorded as D-item for V1).
            if (h->thread() != thread()) {
                qWarning("dbusqml: owner-change notification for adaptor on foreign thread "
                         "(service delivery on main thread; attach/detach off the adaptor's "
                         "thread is unsupported)");
            }
            if (acquired)
                h->nameAcquiredInternal();
            else
                h->nameLostInternal();
        }
    }
}

// Owner-change watch: a name we claimed was acquired (possibly after
// queueing) or lost to another owner.
//
// T1 (features train, Phase 2 — candidate 4, concilium-unanimous): this
// runs on QtDBus's MANAGER thread and NEVER touches adaptors. Under the
// lock it updates the claim state and sets the coalescing notifyQueued
// flag — holders are NOT copied here; the main-thread relay re-resolves
// them. Unlock; post a value-only note (drop if !qApp). No bus calls,
// no adaptor calls, no posts under the lock.
void DBusPathDispatcher::handleServiceOwnerChange(const QString &connName, const QString &service,
                                                  const QString &newOwner) {
    if (!QCoreApplication::instance())
        return;
    OwnerChangeRelay::Note note;
    bool haveNote = false;
    {
        RegistryMutexGuard locker(registryMutex());
        auto it = serviceClaims().find({connName, service});
        if (it == serviceClaims().end()) {
            qCDebug(lcDbusqmlDispatch) << "owner-change for unclaimed" << service << "on"
                                       << connName << "(no claim record — skipped)";
            return;
        }
        ServiceClaim &claim = it.value();
        if (newOwner == claim.baseService && !claim.owned) {
            claim.owned = true;
            claim.queued = false;
        } else if (claim.owned && newOwner != claim.baseService) {
            claim.owned = false;
            // Same-generation guard (astra): a takeover to a foreign owner
            // for the current token makes a later PRIMARY_OWNER reply a
            // no-op (the reply predates the loss).
            claim.lostSinceSend = true;
        } else {
            qCDebug(lcDbusqmlDispatch) << "owner-change for" << service << "on" << connName
                                       << "is no transition (owned=" << claim.owned
                                       << "newOwner=" << newOwner << ") — skipped";
            return;
        }
        // Coalesce: the relay diffs owned vs lastNotifiedOwned, so any
        // number of posts collapse into the net transition.
        claim.notifyQueued = true;
        assertClaimInvariant(claim);
        qCDebug(lcDbusqmlDispatch)
            << "owner-change transition for" << service << "on" << connName
            << "-> owned=" << claim.owned << "(was notified" << claim.lastNotifiedOwned << ")";
        note = {connName, service};
        haveNote = true;
        // Publish the relay before any watch can fire (creation under the
        // lock). Enqueue + post run AFTER unlock (below).
        OwnerChangeRelay::instance();
    }
    if (haveNote)
        OwnerChangeRelay::postNote(std::move(note));
}

void DBusPathDispatcher::handleConnectionLost(const QString &connName) {
    // P5 fan-out: every owned claim on the dead connection flips to
    // unowned; one value-only note per claim wakes the relay (which
    // re-resolves and delivers nameLost to the claim's holders on the
    // main thread — the same lifetime discipline as owner-changes).
    // Claims already unowned produce no note. Idempotent: a second call
    // finds no owned claims.
    if (!QCoreApplication::instance())
        return;
    QList<OwnerChangeRelay::Note> notes;
    {
        RegistryMutexGuard locker(registryMutex());
        for (auto it = serviceClaims().begin(); it != serviceClaims().end(); ++it) {
            if (it.key().first != connName)
                continue;
            ServiceClaim &claim = it.value();
            if (!claim.owned)
                continue;
            claim.owned = false;
            claim.queued = false;
            claim.notifyQueued = true;
            notes.append({it.key().first, it.key().second});
        }
        if (!notes.isEmpty())
            OwnerChangeRelay::instance();
        // A6: the dead connection's receiver goes with it.
        auto rit = ownerChangeReceivers().find(connName);
        if (rit != ownerChangeReceivers().end()) {
            rit.value()->deleteLater();
            ownerChangeReceivers().erase(rit);
        }
    }
    for (auto &n : notes)
        OwnerChangeRelay::postNote(std::move(n));
}

// A3: the reply drives the initial state. Whichever of the reply or the
// signal arrives first wins; the relay's diff (owned vs lastNotifiedOwned)
// makes the second a no-op. Same-generation ordering argument (astra): the
// reply (via the watcher) and the signal (via A6's receiver) are both posted
// to the receiver's thread in daemon order (FIFO) — a takeover signal for
// the current token sets lostSinceSend, making a later PRIMARY_OWNER reply
// a no-op.
void DBusPathDispatcher::handleRequestNameReply(const QString &connName, const QString &service,
                                                uint code, uint token) {
    if (!QCoreApplication::instance())
        return;
    bool wakeRelay = false;
    {
        RegistryMutexGuard locker(registryMutex());
        auto it = serviceClaims().find({connName, service});
        if (it == serviceClaims().end())
            return;
        ServiceClaim &claim = it.value();
        if (claim.requestToken != token || claim.refs == 0 || claim.tearingDown)
            return; // stale reply — a later send/detach/adoption bumped the token
        if (claim.lostSinceSend) {
            claim.lostSinceSend = false;
            return; // a foreign takeover for this token already moved the state
        }
        switch (code) {
        case 1: // DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
        case 4: // DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER
            if (!claim.owned) {
                claim.owned = true;
                claim.queued = false;
                claim.notifyQueued = true;
                wakeRelay = true;
                OwnerChangeRelay::instance();
            }
            break;
        case 2: // DBUS_REQUEST_NAME_REPLY_IN_QUEUE
            claim.queued = true;
            break;
        case 3: // DBUS_REQUEST_NAME_REPLY_EXISTS
            // EXISTS (do-not-queue and the name is taken): warn only — the
            // claim was never acquired, so no nameLost (never owned).
            qCWarning(lcDbusqmlDispatch) << "service" << service << "on" << connName
                                         << "exists and we were not queued (do-not-queue claim)";
            break;
        default:
            break;
        }
        assertClaimInvariant(claim);
    }
    if (wakeRelay)
        OwnerChangeRelay::postNote({connName, service});
}

DBusPathDispatcher::DBusPathDispatcher(const QString &connName, const QString &path,
                                       const QDBusConnection &conn)
    : m_connName(connName), m_path(path), m_conn(conn) {}

// Commit 12 helpers: keep attach()'s two claim paths readable.
inline bool flaggedForClaim(bool allowReplacement, bool replaceExisting, bool queueOnBusy) {
    return allowReplacement || replaceExisting || queueOnBusy;
}

inline void sendFlaggedRequest(const QDBusConnection &conn, OwnerChangeReceiver *receiver,
                               const QString &service, uint token, bool allowReplacement,
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
    // A3: the reply drives the initial state. The watcher lives on the
    // registry-owned receiver (A2); the reply code is mapped into the claim
    // by handleRequestNameReply under the registry lock, guarded by the
    // claim's request token.
    auto pending = conn.asyncCall(req);
    // A2: the reply watcher's parent AND connect-context are the
    // registry-owned per-connection receiver — never the adaptor, never the
    // dispatcher (both can die before the reply arrives).
    auto *watcher = new QDBusPendingCallWatcher(pending, receiver);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, receiver,
                     [conn, service, token](QDBusPendingCallWatcher *w) {
                         QDBusPendingReply<uint> r = *w;
                         if (r.isError()) {
                             // Documented unchanged: consumers waiting on
                             // nameAcquired get no failure signal.
                             qWarning("dbusqml: Failed to register service %s: %s",
                                      qPrintable(service), qPrintable(r.error().message()));
                         } else {
                             DBusPathDispatcher::handleRequestNameReply(conn.name(), service,
                                                                        r.value(), token);
                         }
                         w->deleteLater();
                     });
}

bool DBusPathDispatcher::attach(QDBusConnection conn, const QString &path, const QString &service,
                                DBusAdaptor *adaptor, bool allowReplacement, bool replaceExisting,
                                bool queueOnBusy, bool captureSubtree) {
    const QString connName = conn.name();
    const PathKey key{connName, path};

    DBusPathDispatcher *disp = nullptr;
    {
        RegistryMutexGuard locker(registryMutex());
        auto it = dispatchers().find(key);
        if (it != dispatchers().end()) {
            disp = it.value();
            if (captureSubtree != disp->m_requestedCapture) {
                qmlWarning(adaptor)
                    << "captureSubtree mismatch at" << path
                    << ": the dispatcher was registered with requested captureSubtree="
                    << disp->m_requestedCapture
                    << "by the first adaptor; co-located adaptors must agree — this adaptor was "
                       "NOT registered";
                return false;
            }
        } else {
            DBusPathDispatcher *root = nullptr;
            QString parent = path;
            for (;;) {
                const int slash = parent.lastIndexOf(QLatin1Char('/'));
                if (slash < 0)
                    break;
                parent = (slash == 0) ? QStringLiteral("/") : parent.left(slash);
                auto anc = dispatchers().find({connName, parent});
                if (anc != dispatchers().end() && anc.value()->m_captures) {
                    root = anc.value();
                    break;
                }
                if (slash == 0)
                    break;
            }
            if (root) {
                if (captureSubtree) {
                    qmlWarning(adaptor) << "captureSubtree at" << path
                                        << "ignored: already captured by" << root->m_path;
                }
                disp = new DBusPathDispatcher(connName, path, conn);
                disp->m_capturedBy = root;
                disp->m_requestedCapture = captureSubtree;
                root->m_children.insert(path, disp);
                dispatchers().insert(key, disp);
            } else {
                const auto mode =
                    captureSubtree ? QDBusConnection::SubPath : QDBusConnection::SingleNode;
                disp = new DBusPathDispatcher(connName, path, conn);
                disp->m_captures = (mode == QDBusConnection::SubPath);
                disp->m_requestedCapture = captureSubtree;
                if (!conn.registerVirtualObject(path, disp, mode)) {
                    if (disp->m_captures)
                        qmlWarning(adaptor)
                            << "captureSubtree at" << path
                            << "refused: QtDBus rejected SubPath registration — most "
                               "likely paths already registered beneath it; create the "
                               "capturing adaptor before its children";
                    else
                        qmlInfo(adaptor) << "Failed to register object at" << path;
                    delete disp;
                    return false;
                }
                dispatchers().insert(key, disp);
            }
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
        bool joinerNeedsAcquired = false;
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
                claim.notifyQueued = false;
                claim.lastNotifiedOwned = false;
                claim.baseService = conn.baseService();
            }
            if (claim.pending || claim.refs > 0) {
                // Registration already in flight or owned — join as a
                // holder; ownership notification arrives via the watch (or
                // is already recorded) exactly as for any co-located
                // second adaptor. CF-19: when the claim is ALREADY owned,
                // no watch transition will ever fire for the joiner — so
                // deliver the current state directly (outside the lock).
                claim.refs++;
                claim.holders.append(adaptor);
                // A5: direct delivery only when the acquisition was already
                // delivered (lastNotifiedOwned) — otherwise the relay serves
                // the joiner (it re-resolves holders). The invariant every
                // writer keeps: owned ⟹ (lastNotifiedOwned || notifyQueued).
                assertClaimInvariant(claim);
                if (claim.owned && claim.lastNotifiedOwned)
                    joinerNeedsAcquired = true;
            } else {
                claim.refs = 1;
                claim.holders.append(adaptor);
                claim.baseService = conn.baseService();
                claim.pending = !flaggedForClaim(allowReplacement, replaceExisting, queueOnBusy);
                claim.notifyQueued = false;
                claim.lastNotifiedOwned = false;
                doRegister = claim.pending;
                if (flaggedForClaim(allowReplacement, replaceExisting, queueOnBusy)) {
                    // A6: arm the connection's registry-owned receiver
                    // BEFORE the RequestName is enqueued — the public
                    // QDBusConnection::connect hook install is a
                    // BlockingQueued call that enqueues the AddMatch before
                    // returning, so the match precedes the request on the
                    // socket by construction (the wire pin).
                    ensureOwnerChangeReceiver(conn, connName);
                    // Publish the relay at first flagged attach, before
                    // any owner change can fire (blessed shape): by the time
                    // the manager thread observes one, instance() is already
                    // published — creation on the fire path is only the
                    // idempotent fallback.
                    OwnerChangeRelay::instance();
                    // A2/A3: the token identifies this send; the reply drives
                    // the claim's initial state via handleRequestNameReply.
                    claim.requestToken = ++nextRequestToken();
                    claim.lostSinceSend = false;
                    sendFlaggedRequest(conn, ownerChangeReceivers().value(connName), service,
                                       claim.requestToken, allowReplacement, replaceExisting,
                                       queueOnBusy);
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
                              queueOnBusy, captureSubtree);
            }
            ServiceClaim &claim = it.value();
            claim.pending = false;
            if (ok) {
                claim.owned = true;
                // Direct (synchronous) acquisition: the relay's diff
                // baseline must agree, or a later duplicate post would
                // re-deliver a stale acquired. notifyQueued stays false —
                // nothing for the relay to do.
                claim.lastNotifiedOwned = true;
                assertClaimInvariant(claim);
                locker.m_locker.unlock();
                adaptor->nameAcquiredInternal();
            } else {
                locker.m_locker.unlock();
                qmlInfo(adaptor) << "Failed to register service" << service;
            }
        }
        // CF-19: late joiner on an already-owned claim — deliver the
        // current state directly (outside the lock, like the registrant
        // path above). A pending claim still resolves through the watch.
        if (joinerNeedsAcquired)
            adaptor->nameAcquiredInternal();
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
                    claim.refs = 0;
                    claim.holders.clear();
                    // A2: a reply to a pre-detach RequestName arriving now is
                    // stale — bump the token so it no-ops (A3's guard).
                    claim.requestToken = ++nextRequestToken();
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

// G1 investigation (debug-only caller: the owner-change storm test): dump the
// claim record for (connName, service). Reads under the registry lock; no
// state is modified. Returns an empty string when no claim exists.
QString DBusPathDispatcher::debugClaimState(const QString &connName, const QString &service) {
    RegistryMutexGuard locker(registryMutex());
    auto it = serviceClaims().find({connName, service});
    if (it == serviceClaims().end())
        return QString();
    const ServiceClaim &c = it.value();
    return QStringLiteral("owned=%1 lastNotifiedOwned=%2 notifyQueued=%3 refs=%4 pending=%5 "
                          "tearingDown=%6 watchArmed=%7 base=%8 token=%9 lostSinceSend=%10")
        .arg(c.owned)
        .arg(c.lastNotifiedOwned)
        .arg(c.notifyQueued)
        .arg(c.refs)
        .arg(c.pending)
        .arg(c.tearingDown)
        .arg(ownerChangeReceivers().contains(connName))
        .arg(c.baseService)
        .arg(c.requestToken)
        .arg(c.lostSinceSend);
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

    if (m_capturedBy) {
        DBusPathDispatcher *root = m_capturedBy;
        {
            RegistryMutexGuard locker(registryMutex());
            root->m_children.remove(m_path);
            dispatchers().remove({m_connName, m_path});
        }
        deleteLater();
        if (root->m_adaptors.isEmpty() && root->m_children.isEmpty())
            root->unregisterAndDelete();
        return;
    }

    if (!m_children.isEmpty())
        return;

    unregisterAndDelete();
}

void DBusPathDispatcher::unregisterAndDelete() {
    {
        RegistryMutexGuard locker(registryMutex());
        dispatchers().remove({m_connName, m_path});
    }
    m_conn.unregisterObject(m_path);
    deleteLater();
}

QString DBusPathDispatcher::interfacesXml(const QList<QPointer<DBusAdaptor>> &adaptors) const {
    // Merge per-adaptor interface blocks cleanly: for co-located same-iface
    // adaptors, first-attached wins for dispatch — the advertised surface
    // mirrors that exactly. Naive concatenation would emit duplicate
    // members (duplicate signals/methods), which busctl/GDBus reject.
    QString xml;
    QSet<QString> servedIfaces;
    bool servedEmptyIface = false; // A17: empty ifaces dedupe too
    for (const auto &a : adaptors) {
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

QString DBusPathDispatcher::introspect(const QString &path) const {
    if (!m_captures)
        return interfacesXml(m_adaptors);

    const QList<QPointer<DBusAdaptor>> *adaptors = &m_adaptors;
    if (path != m_path) {
        DBusPathDispatcher *child = m_children.value(path);
        if (child)
            adaptors = &child->m_adaptors;
        else
            adaptors = nullptr;
    }
    QString xml;
    if (adaptors)
        xml += interfacesXml(*adaptors);

    const QString prefix =
        (path == QLatin1String("/")) ? QStringLiteral("/") : path + QLatin1Char('/');
    QSet<QString> names;
    for (auto it = m_children.cbegin(); it != m_children.cend(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        const QString rest = it.key().mid(prefix.size());
        const int slash = rest.indexOf(QLatin1Char('/'));
        const QString c = slash < 0 ? rest : rest.left(slash);
        if (!c.isEmpty())
            names.insert(c);
    }
    for (const QString &c : names)
        xml += QStringLiteral("  <node name=\"%1\"/>\n").arg(c);
    return xml;
}

bool DBusPathDispatcher::routeToAdaptors(const QList<QPointer<DBusAdaptor>> &adaptors,
                                         const QDBusMessage &msg, const QDBusConnection &conn) {
    const QString interface = msg.interface();

    // QtDBus serves introspect() for the Introspectable interface.
    if (interface == QStringLiteral("org.freedesktop.DBus.Introspectable"))
        return false;

    if (interface == QStringLiteral("org.freedesktop.DBus.Properties")) {
        QString reqIface;
        if (!msg.arguments().isEmpty())
            reqIface = msg.arguments().first().toString();
        if (!reqIface.isEmpty()) {
            for (const auto &a : adaptors) {
                if (a && a->iface() == reqIface)
                    return a->handleMessage(msg, conn);
            }
            if (!adaptors.isEmpty() && adaptors.first())
                return adaptors.first()->handleMessage(msg, conn);
            return false;
        }
        for (const auto &a : adaptors) {
            if (a && a->handleMessage(msg, conn))
                return true;
        }
        return false;
    }

    if (!interface.isEmpty()) {
        for (const auto &a : adaptors) {
            if (a && a->iface() == interface) {
                if (a->handleMessage(msg, conn))
                    return true;
                // The interface IS served here (first-attached wins — no other
                // adaptor gets a turn) and the adaptor declined: the method does
                // not exist on it (name miss, arity miss, or a library-mechanism
                // name that is deliberately never served). Stock Qt's adaptor
                // dispatch, GDBus and sd-bus all say UnknownMethod for this;
                // letting `false` fall to the bottom fallback said
                // UnknownInterface — a lie about an interface introspection
                // advertises (consumer leg 8a, 2026-09-23). Qt's text, B4 guard.
                if (msg.isReplyRequired())
                    checkedSend(conn,
                                msg.createErrorReply(
                                    QDBusError::UnknownMethod,
                                    QStringLiteral("No such method '%1' in "
                                                   "interface '%2' at object "
                                                   "path '%3' (signature '%4')")
                                        .arg(msg.member(), interface, msg.path(), msg.signature())),
                                "method miss");
                return true;
            }
        }
        return false; // no adaptor serves this interface → callers' fallbacks
    }

    // Peer (Ping, GetMachineId) is answered by libdbus's built-in filter on
    // every client connection before Qt dispatch — any path, even
    // unregistered ones — exactly as GDBus and sd-bus answer it in their
    // libraries. dbusqml does not shadow it. Same-connection loopback
    // callers bypass libdbus and get Qt's fallback (UnknownInterface):
    // stock Qt behavior, documented in KNOWN_ISSUES.

    for (const auto &a : adaptors) {
        if (a && a->handleMessage(msg, conn))
            return true;
    }
    return false;
}

// Qt's activateInternalFilters (QDBusConnectionPrivate, qtbase 6.11) answers
// these shapes itself for ANY virtual-object node, before its leftover-path
// branch:
//   iface ∈ {"", Introspectable}: Introspect() sig ""            → XML
//   iface == Introspectable, any other member                    → UnknownMethod
//   iface ∈ {"", Properties}: Get(ss) | Set(ssv) | GetAll(s)      → property filter
//   iface == Properties, any other member/signature              → UnknownMethod
// For a captured child we return false on exactly these so Qt answers the
// child as it answers a plain adaptor; everything else falls to the
// exact-path bottom fallback we mirror below (empty iface → UnknownMethod,
// named iface → UnknownInterface). This predicate IS the parity contract;
// change it only together with the Qt source it mirrors. Cite Qt by
// function name and version, never by line number.
//
// Two Qt facts the parity rests on (council-verified; keep them written down):
//  * Qt's filters are guarded by `node.obj &&`. For dbusqml that is always
//    true: registerVirtualObject stores the dispatcher as node.obj
//    (obj/treeNode are one union member), for the child's root and for a
//    plain adaptor alike. We omit the guard on purpose.
//  * SubPath == 0x1 aliases the ExportAdaptors flag bit, so the capturing
//    root's node enters Qt's adaptor branches that a SingleNode node skips.
//    They are inert: qDBusFindAdaptorConnector(dispatcher) is null (a
//    DBusPathDispatcher owns no QDBusAbstractAdaptor children), and
//    ExportAllProperties is never set — so Get/Set/GetAll resolve to the same
//    interface-not-found / empty-dict outcomes on both sides, path-only text
//    differences. The parity pin asserts the observable consequence (both
//    Introspect XMLs carry org.freedesktop.DBus.Properties; identical
//    Get/Set/GetAll replies).
static bool qtInternalFiltersHandle(const QDBusMessage &msg) {
    const QString iface = msg.interface(), member = msg.member(), sig = msg.signature();
    const bool introspectable = iface == QLatin1String("org.freedesktop.DBus.Introspectable");
    const bool properties = iface == QLatin1String("org.freedesktop.DBus.Properties");
    if (iface.isEmpty() || introspectable) {
        if (member == QLatin1String("Introspect") && sig.isEmpty())
            return true;
        if (introspectable)
            return true;
    }
    if (iface.isEmpty() || properties) {
        if ((member == QLatin1String("Get") && sig == QLatin1String("ss")) ||
            (member == QLatin1String("Set") && sig == QLatin1String("ssv")) ||
            (member == QLatin1String("GetAll") && sig == QLatin1String("s")))
            return true;
        if (properties)
            return true;
    }
    return false;
}

bool DBusPathDispatcher::handleMessage(const QDBusMessage &msg, const QDBusConnection &conn) {
    if (msg.path() == m_path)
        return routeToAdaptors(m_adaptors, msg, conn);
    if (!m_captures)
        return false;
    if (DBusPathDispatcher *child = m_children.value(msg.path())) {
        if (child->routeToAdaptors(child->m_adaptors, msg, conn))
            return true;
        if (qtInternalFiltersHandle(msg))
            return false; // Qt answers, same as a plain adaptor
        const QString iface = msg.interface();
        // Anything else would fall into Qt's leftover-path UnknownObject for a
        // path that exists (qdbusintegrator.cpp activateObject, pathStartPos !=
        // size). Mirror Qt's bottom fallback for the exact-path case instead —
        // same split, same texts (qdbusintegrator.cpp sendError).
        if (msg.isReplyRequired()) {
            QDBusMessage err =
                iface.isEmpty()
                    ? msg.createErrorReply(
                          QDBusError::UnknownMethod,
                          QStringLiteral("No such method '%1' in any interface at object path "
                                         "'%2' (signature '%3')")
                              .arg(msg.member(), msg.path(), msg.signature()))
                    : msg.createErrorReply(
                          QDBusError::UnknownInterface,
                          QStringLiteral("No such interface '%1' at object path '%2'")
                              .arg(iface, msg.path()));
            checkedSend(conn, err, "child fallback error");
        }
        return true;
    }
    if (msg.interface() == QStringLiteral("org.freedesktop.DBus.Introspectable"))
        return false;
    qCDebug(lcDbusqmlDispatch) << "no object at" << msg.path() << "for" << msg.interface()
                               << msg.member() << "from" << msg.service();
    if (msg.isReplyRequired())
        checkedSend(
            conn,
            msg.createErrorReply(QDBusError::UnknownObject,
                                 QStringLiteral("No such object path '%1'").arg(msg.path())),
            "absent-path UnknownObject");
    return true;
}
