#pragma once

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusVirtualObject>
#include <QEvent>
#include <QMetaObject>
#include <QList>
#include <QPointer>
#include <QString>

class DBusAdaptor;

// OwnerChangeRelay delivers service-ownership transitions on the main
// thread; it needs the same private entry points as the dispatcher.
class OwnerChangeRelay;

// T1 watch context: the QObject that QtDBus's signal delivery targets
// for one flagged claim's serviceOwnerChanged watch. It is created on
// the consumer (attach) thread and deliberately NEVER moved — QtDBus
// posts matched-signal delivery events to hook.obj's thread, and the
// hook.obj here is the QDBusConnectionPrivate (manager thread), so the
// lambda runs on the manager thread with this object as its context.
// Using a library-owned context (instead of conn.interface()) keeps the
// connect-time BlockingQueued metacall out of attach's registry-locked
// path: with iface as context, QObject::connect blocks the attaching
// thread on the manager thread while the manager thread may itself be
// blocked delivering an earlier owner-change into handleServiceOwnerChange
// under registryMutex — a self-deadlock through Qt internals, not our
// lock. Owned by the claim record; destroyed via deleteLater on
// teardown (its delivery affinity is the manager thread).
class OwnerChangeWatch : public QObject {
    Q_OBJECT

public:
    OwnerChangeWatch(const QString &connName, const QString &service, QObject *parent = nullptr)
        : QObject(parent), m_connName(connName), m_service(service) {}
    QString connName() const { return m_connName; }
    QString service() const { return m_service; }

private:
    QString m_connName;
    QString m_service;
};

// Library-private: routes incoming D-Bus calls on a shared (connection, path)
// to the co-located DBusAdaptor instances attached to it, and shares service
// name registration across adaptors. The dispatcher is the single
// QDBusVirtualObject QtDBus sees at a path; adaptors attach/detach through the
// static registry. NOT QML-exposed, NOT an installed header.
//
// Threading (attach/detach contract): attach/detach for a given (connection,
// service) are consumer-serialized — typically both run on the QML/main
// thread, which is the only configuration the registry's teardown markers
// protect. A C++ consumer driving attach/detach for the SAME name from two
// threads concurrently is unsupported (the registry mutex guards the manager
// thread, not concurrent consumer threads).
class DBusPathDispatcher : public QDBusVirtualObject {
    Q_OBJECT

public:
    // Attach `adaptor` at `path` on `conn` (registry key is the connection
    // name). Registers the path's dispatcher on first attach and the service
    // name on first claim; later co-located adaptors reuse both. Returns false
    // (and warns) when the path cannot be registered.
    static bool attach(QDBusConnection conn, const QString &path, const QString &service,
                       DBusAdaptor *adaptor, bool allowReplacement, bool replaceExisting,
                       bool queueOnBusy);

    // Detach `adaptor`; drops the path and service name only when the last
    // attached adaptor / claim goes away.
    static void detach(QDBusConnection conn, const QString &path, const QString &service,
                       DBusAdaptor *adaptor);

    // Library-private diagnostics: number of live path dispatchers. Used by
    // the lifecycle tests to assert teardown returns the registry to its
    // baseline (no leaked bus registrations).
    static int liveCount();

    // QDBusVirtualObject
    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

    // Owner-change watch receiver (library-private; connected per claim).
    // Runs on QtDBus's manager thread: records the claim transition and
    // marshals delivery to the main thread (see OwnerChangeRelay below) —
    // it NEVER touches adaptors itself (T1).
    static void handleServiceOwnerChange(const QString &connName, const QString &service,
                                         const QString &newOwner);

    // P5 (features train, Phase 3): connection-loss fan-out. The
    // DBusConnection that observed Local.Disconnected calls this with its
    // QDBusConnection identity; every claim on that connection flips to
    // unowned and its holders get nameLost on the CALLING thread's event
    // loop via the relay (same lifetime discipline as owner-changes — no
    // adaptor pointer crosses a thread). Idempotent per connection.
    static void handleConnectionLost(const QString &connName);

private:
    DBusPathDispatcher(const QString &connName, const QString &path, const QDBusConnection &conn);

    void attachAdaptor(DBusAdaptor *adaptor);
    void detachAdaptor(DBusAdaptor *adaptor);

    QString m_connName;
    QString m_path;
    QDBusConnection m_conn;
    QList<QPointer<DBusAdaptor>> m_adaptors;
};

// T1 (features train, Phase 2 — concilium-blessed candidate 4, the
// main-thread relay): no adaptor pointer ever crosses a thread. The
// manager thread only updates claim state + sets a coalescing flag and
// posts a value-only note to this process-lifetime, main-thread-affine
// relay; the relay re-takes the lock ON THE MAIN THREAD, re-resolves
// the claim (miss or !notifyQueued → drop), clears the flag, diffs
// `owned` against `lastNotifiedOwned` (coalescing makes stale/duplicate
// posts harmless), copies holders, drops the lock, then delivers.
// QPointer checks there are safe because adaptor destruction is
// main-thread-serialized by the event loop.
//
// Lifetime: intentionally never deleted (leaked at exit). That is what
// makes the F3 poster race structurally impossible —
// QObjectPrivate::threadData stays permanently valid, so a concurrent
// postEvent can never dereference half-torn-down ~QObject state.
// Affinity: moveToThread(qApp) at creation (a function-local static
// inherits its CREATOR's affinity, which may be foreign).
class OwnerChangeRelay : public QObject {
    Q_OBJECT

public:
    // A value-only ownership note: identifies the claim whose state
    // changed. The transition itself is NEVER carried — the relay
    // re-resolves owned/acquired-lost from the claim record under the
    // lock, so stale or duplicate posts collapse harmlessly.
    struct Note {
        QString connName;
        QString service;
    };

    static int eventType() {
        static int t = QEvent::registerEventType();
        return t;
    }

    // Caller MUST hold registryMutex (creation is under the lock so the
    // relay is published before any watch can fire).
    static OwnerChangeRelay *instance() {
        static OwnerChangeRelay *r = [] {
            auto *p = new OwnerChangeRelay;
            if (auto *app = QCoreApplication::instance())
                p->moveToThread(app->thread());
            return p;
        }();
        return r;
    }

    // Manager-thread marshal: enqueue + wake. Takes the queue mutex
    // alone (NEVER nested inside registryMutex) and posts AFTER the
    // caller released the registry lock: postEvent to a live relay is
    // not I/O and never blocks, but keeping it outside preserves the
    // trivial lock order (registryMutex -> postEventList.mutex only).
    static void postNote(Note n);

protected:
    bool event(QEvent *e) override;

private:
    OwnerChangeRelay() = default;
};
