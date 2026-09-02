#include "dbussignalwatcher.h"

#include <QDBusConnection>

DBusSignalWatcher::DBusSignalWatcher(QObject *parent) : QObject(parent) {}

DBusSignalWatcher::~DBusSignalWatcher() {
    if (m_connected)
        QDBusConnection::sessionBus().disconnect(m_service, m_path, m_iface, m_member, this,
                                                 SLOT(onSignal(QDBusMessage)));
}

void DBusSignalWatcher::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;
    m_conn = v;
    emit connectionChanged();
    resubscribe();
}

void DBusSignalWatcher::setService(const QString &v) {
    if (m_service == v)
        return;
    m_service = v;
    emit serviceChanged();
    resubscribe();
}

void DBusSignalWatcher::setPath(const QString &v) {
    if (m_path == v)
        return;
    m_path = v;
    emit pathChanged();
    resubscribe();
}

void DBusSignalWatcher::setIface(const QString &v) {
    if (m_iface == v)
        return;
    m_iface = v;
    emit ifaceChanged();
    resubscribe();
}

void DBusSignalWatcher::setMember(const QString &v) {
    if (m_member == v)
        return;
    m_member = v;
    emit memberChanged();
    resubscribe();
}

void DBusSignalWatcher::setEnabled(bool v) {
    if (m_enabled == v)
        return;
    m_enabled = v;
    emit enabledChanged();
    resubscribe();
}

void DBusSignalWatcher::componentComplete() {
    m_complete = true;
    resubscribe();
}

void DBusSignalWatcher::resubscribe() {
    if (!m_complete)
        return;

    QDBusConnection conn =
        m_conn ? static_cast<QDBusConnection>(*m_conn) : QDBusConnection::sessionBus();

    // Drop the previous subscription (its filter may no longer match).
    if (m_connected) {
        conn.disconnect(m_service, m_path, m_iface, m_member, this, SLOT(onSignal(QDBusMessage)));
        m_connected = false;
    }

    if (!m_enabled)
        return;

    // Empty service/member strings are wildcards (QtDBus match-rule semantics)
    // — the dbus-monitor shape. Path and iface must be present.
    if (m_path.isEmpty() || m_iface.isEmpty())
        return;

    if (conn.connect(m_service, m_path, m_iface, m_member, this, SLOT(onSignal(QDBusMessage))))
        m_connected = true;
}

void DBusSignalWatcher::onSignal(const QDBusMessage &msg) {
    QVariantList args;
    for (const QVariant &a : msg.arguments())
        args << unwrapDbus(a);
    emit received(msg.member(), args);
}

#include "dbussignalwatcher.moc"
