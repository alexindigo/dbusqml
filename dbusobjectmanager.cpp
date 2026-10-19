#include "dbusobjectmanager.h"
#include "dbusutils.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

DBusObjectManager::DBusObjectManager(QObject *parent) : QObject(parent) {}

DBusObjectManager::~DBusObjectManager() {
    unsubscribe();
}

void DBusObjectManager::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;
    unsubscribe();
    m_conn = v;
    emit connectionChanged();
    setup();
}

void DBusObjectManager::setService(const QString &v) {
    if (m_service == v)
        return;
    unsubscribe();
    m_service = v;
    emit serviceChanged();
    setup();
}

void DBusObjectManager::setPath(const QString &v) {
    if (m_path == v)
        return;
    unsubscribe();
    m_path = v;
    emit pathChanged();
    setup();
}

void DBusObjectManager::componentComplete() {
    m_complete = true;
    setup();
}

// The ONE disconnect path (setters and the destructor): drops the recorded
// subscription with the exact connection + tuple it was made with.
void DBusObjectManager::unsubscribe() {
    if (!m_subscribed)
        return;
    m_subConn.disconnect(
        m_subService, m_subPath, QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("InterfacesAdded"), this, SLOT(onInterfacesAdded(QDBusMessage)));
    m_subConn.disconnect(
        m_subService, m_subPath, QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("InterfacesRemoved"), this, SLOT(onInterfacesRemoved(QDBusMessage)));
    m_subscribed = false;
}

void DBusObjectManager::setup() {
    if (!m_complete || m_service.isEmpty() || m_path.isEmpty())
        return;

    QDBusConnection conn =
        m_conn ? static_cast<QDBusConnection>(*m_conn) : QDBusConnection::sessionBus();

    if (!m_subscribed) {
        if (conn.connect(m_service, m_path, QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                         QStringLiteral("InterfacesAdded"), this,
                         SLOT(onInterfacesAdded(QDBusMessage))) &&
            conn.connect(m_service, m_path, QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                         QStringLiteral("InterfacesRemoved"), this,
                         SLOT(onInterfacesRemoved(QDBusMessage)))) {
            m_subConn = conn;
            m_subService = m_service;
            m_subPath = m_path;
            m_subscribed = true;
        }
    }

    fetchManagedObjects();
}

void DBusObjectManager::fetchManagedObjects() {
    QDBusConnection conn =
        m_conn ? static_cast<QDBusConnection>(*m_conn) : QDBusConnection::sessionBus();

    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    auto pending = conn.asyncCall(msg);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QVariantList args;
        QDBusError error = w->error();
        const bool decoded = !error.isValid() && decodeMessage(w->reply(), args, &error);
        if (decoded && !args.isEmpty()) {
            DBus::UnixFd::noteDelivered(args.first(), qmlEngine(this));
            m_objects = args.first().toMap();
            if (!m_ready) {
                m_ready = true;
                emit readyChanged();
            }
            emit managedObjectsChanged();
        } else if (error.isValid()) {
            // The error can be the peer's reply error (peer-derived,
            // escaped — #10).
            qWarning("dbusqml: GetManagedObjects failed: %s",
                     qPrintable(escapeForLog(error.message())));
        }
        w->deleteLater();
    });
}

void DBusObjectManager::onInterfacesAdded(const QDBusMessage &msg) {
    QVariantList args;
    if (!decodeMessage(msg, args) || args.size() < 2)
        return;
    DBus::UnixFd::noteDelivered(args.at(1), qmlEngine(this));
    const QString objectPath = args.at(0).toString();
    const QVariantMap ifaces = args.at(1).toMap();

    QVariantMap entry = m_objects.value(objectPath).toMap();
    for (auto it = ifaces.cbegin(); it != ifaces.cend(); ++it)
        entry[it.key()] = it.value();
    m_objects[objectPath] = entry;
    emit managedObjectsChanged();
    emit interfacesAdded(objectPath, ifaces);
}

void DBusObjectManager::onInterfacesRemoved(const QDBusMessage &msg) {
    QVariantList args;
    if (!decodeMessage(msg, args) || args.size() < 2)
        return;
    const QString objectPath = args.at(0).toString();
    const QStringList ifaces = args.at(1).toStringList();

    if (m_objects.contains(objectPath)) {
        QVariantMap entry = m_objects.value(objectPath).toMap();
        for (const QString &iface : ifaces)
            entry.remove(iface);
        if (entry.isEmpty())
            m_objects.remove(objectPath);
        else
            m_objects[objectPath] = entry;
        emit managedObjectsChanged();
    }
    emit interfacesRemoved(objectPath, ifaces);
}

#include "dbusobjectmanager.moc"
