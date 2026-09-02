#include "dbusservicewatcher.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>

DBusServiceWatcher::DBusServiceWatcher(QObject *parent) : QObject(parent) {}

DBusServiceWatcher::~DBusServiceWatcher() {
    delete m_watcher;
}

void DBusServiceWatcher::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;
    m_conn = v;
    emit connectionChanged();
    setup();
}

void DBusServiceWatcher::setService(const QString &v) {
    if (m_service == v)
        return;
    m_service = v;
    emit serviceChanged();
    setup();
}

void DBusServiceWatcher::componentComplete() {
    m_complete = true;
    setup();
}

void DBusServiceWatcher::setup() {
    if (!m_complete)
        return;

    delete m_watcher;
    m_watcher = nullptr;

    if (m_service.isEmpty())
        return;

    QDBusConnection conn =
        m_conn ? static_cast<QDBusConnection>(*m_conn) : QDBusConnection::sessionBus();

    m_watcher =
        new QDBusServiceWatcher(m_service, conn, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &service, const QString &oldOwner, const QString &newOwner) {
                Q_UNUSED(service);
                const bool nowRegistered = !newOwner.isEmpty();
                if (nowRegistered != m_registered)
                    setRegistered(nowRegistered);
                emit ownerChanged(oldOwner, newOwner);
            });

    setRegistered(conn.interface()->isServiceRegistered(m_service));
}

void DBusServiceWatcher::setRegistered(bool v) {
    if (m_registered == v)
        return;
    m_registered = v;
    emit registeredChanged();
}

#include "dbusservicewatcher.moc"
