#include "dbussignalwatcher.h"
#include "dbusutils.h"

#include <QDBusConnection>

DBusSignalWatcher::DBusSignalWatcher(QObject *parent) : QObject(parent) {}

DBusSignalWatcher::~DBusSignalWatcher() {
    unsubscribe();
}

void DBusSignalWatcher::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;
    unsubscribe();
    m_conn = v;
    emit connectionChanged();
    resubscribe();
}

void DBusSignalWatcher::setService(const QString &v) {
    if (m_service == v)
        return;
    unsubscribe();
    m_service = v;
    emit serviceChanged();
    resubscribe();
}

void DBusSignalWatcher::setPath(const QString &v) {
    if (m_path == v)
        return;
    unsubscribe();
    m_path = v;
    emit pathChanged();
    resubscribe();
}

void DBusSignalWatcher::setIface(const QString &v) {
    if (m_iface == v)
        return;
    unsubscribe();
    m_iface = v;
    emit ifaceChanged();
    resubscribe();
}

void DBusSignalWatcher::setMember(const QString &v) {
    if (m_member == v)
        return;
    unsubscribe();
    m_member = v;
    emit memberChanged();
    resubscribe();
}

void DBusSignalWatcher::setEnabled(bool v) {
    if (m_enabled == v)
        return;
    unsubscribe();
    m_enabled = v;
    emit enabledChanged();
    resubscribe();
}

void DBusSignalWatcher::componentComplete() {
    m_complete = true;
    resubscribe();
}

// The ONE disconnect path (setters and the destructor): drops the recorded
// subscription with the exact connection + tuple it was made with (KDE
// plasma-workspace dbussignalwatcher shape — disconnect before mutating).
void DBusSignalWatcher::unsubscribe() {
    if (!m_connected)
        return;
    m_subConn.disconnect(m_subService, m_subPath, m_subIface, m_subMember, this,
                         SLOT(onSignal(QDBusMessage)));
    m_connected = false;
}

void DBusSignalWatcher::resubscribe() {
    if (!m_complete)
        return;

    if (!m_enabled)
        return;

    // Empty service/member strings are wildcards (QtDBus match-rule semantics)
    // — the dbus-monitor shape. Path and iface must be present.
    if (m_path.isEmpty() || m_iface.isEmpty())
        return;

    QDBusConnection conn =
        m_conn ? static_cast<QDBusConnection>(*m_conn) : QDBusConnection::sessionBus();

    if (conn.connect(m_service, m_path, m_iface, m_member, this, SLOT(onSignal(QDBusMessage)))) {
        m_subConn = conn;
        m_subService = m_service;
        m_subPath = m_path;
        m_subIface = m_iface;
        m_subMember = m_member;
        m_connected = true;
    }
}

void DBusSignalWatcher::onSignal(const QDBusMessage &msg) {
    QVariantList args;
    if (!decodeMessage(msg, args))
        return;
    emit received(msg.member(), args);
}

#include "dbussignalwatcher.moc"
