#pragma once

#include <QDBusConnection>
#include <QDBusVirtualObject>
#include <QMetaObject>
#include <QList>
#include <QPointer>
#include <QString>

class DBusAdaptor;

// Library-private: routes incoming D-Bus calls on a shared (connection, path)
// to the co-located DBusAdaptor instances attached to it, and shares service
// name registration across adaptors. The dispatcher is the single
// QDBusVirtualObject QtDBus sees at a path; adaptors attach/detach through the
// static registry. NOT QML-exposed, NOT an installed header.
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
    static void handleServiceOwnerChange(const QString &connName, const QString &service,
                                         const QString &newOwner);

private:
    DBusPathDispatcher(const QString &connName, const QString &path, const QDBusConnection &conn);

    void attachAdaptor(DBusAdaptor *adaptor);
    void detachAdaptor(DBusAdaptor *adaptor);

    QString m_connName;
    QString m_path;
    QDBusConnection m_conn;
    QList<QPointer<DBusAdaptor>> m_adaptors;
};
