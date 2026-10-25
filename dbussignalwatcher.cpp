#include "dbussignalwatcher.h"
#include "dbussubscription.h"
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

// Revoke before mutating: queued deliveries belong to the old receiver.
void DBusSignalWatcher::unsubscribe() {
    if (m_subscription) {
        auto *subscription = m_subscription;
        m_subscription = nullptr;
        subscription->revoke();
    }
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

    unsubscribe();
    m_subscription = DBusSubscription::create(
        conn, m_service, m_path, m_iface, {m_member},
        [this](const QDBusMessage &msg) { onSignal(msg); }, this);
}

void DBusSignalWatcher::onSignal(const QDBusMessage &msg) {
    QVariantList args;
    if (!decodeMessage(msg, args))
        return;
    // Each watcher decodes for itself: its fds are its own dups (P1).
    DBus::UnixFd::noteDelivered(QVariant(args), qmlEngine(this));
    emit received(msg.member(), args);
}

#include "dbussignalwatcher.moc"
