#include "dbusobjectmanager.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

DBusObjectManager::DBusObjectManager(QObject *parent) : QObject(parent) {}

DBusObjectManager::~DBusObjectManager() {
    if (m_subscribed) {
        QDBusConnection conn =
            m_conn ? static_cast<QDBusConnection>(*m_conn) : QDBusConnection::sessionBus();
        conn.disconnect(m_service, m_path, QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                        QStringLiteral("InterfacesAdded"), this,
                        SLOT(onInterfacesAdded(QDBusMessage)));
        conn.disconnect(m_service, m_path, QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                        QStringLiteral("InterfacesRemoved"), this,
                        SLOT(onInterfacesRemoved(QDBusMessage)));
    }
}

void DBusObjectManager::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;
    m_conn = v;
    emit connectionChanged();
    setup();
}

void DBusObjectManager::setService(const QString &v) {
    if (m_service == v)
        return;
    m_service = v;
    emit serviceChanged();
    setup();
}

void DBusObjectManager::setPath(const QString &v) {
    if (m_path == v)
        return;
    m_path = v;
    emit pathChanged();
    setup();
}

void DBusObjectManager::componentComplete() {
    m_complete = true;
    setup();
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
                         SLOT(onInterfacesRemoved(QDBusMessage))))
            m_subscribed = true;
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
        if (!w->isError() && !w->reply().arguments().isEmpty()) {
            m_objects = unwrapDbus(w->reply().arguments().first()).toMap();
            if (!m_ready) {
                m_ready = true;
                emit readyChanged();
            }
            emit managedObjectsChanged();
        } else if (w->isError()) {
            qWarning("dbusqml: GetManagedObjects failed: %s", qPrintable(w->error().message()));
        }
        w->deleteLater();
    });
}

void DBusObjectManager::onInterfacesAdded(const QDBusMessage &msg) {
    if (msg.arguments().size() < 2)
        return;
    const QString objectPath = unwrapDbus(msg.arguments().at(0)).toString();
    const QVariantMap ifaces = unwrapDbus(msg.arguments().at(1)).toMap();

    QVariantMap entry = m_objects.value(objectPath).toMap();
    for (auto it = ifaces.cbegin(); it != ifaces.cend(); ++it)
        entry[it.key()] = it.value();
    m_objects[objectPath] = entry;
    emit managedObjectsChanged();
    emit interfacesAdded(objectPath, ifaces);
}

void DBusObjectManager::onInterfacesRemoved(const QDBusMessage &msg) {
    if (msg.arguments().size() < 2)
        return;
    const QString objectPath = unwrapDbus(msg.arguments().at(0)).toString();
    const QStringList ifaces = unwrapDbus(msg.arguments().at(1)).toStringList();

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
