#include "dbussubscription.h"

#include <utility>

DBusSubscription::DBusSubscription(const QDBusConnection &connection, const QString &service,
                                   const QString &path, const QString &iface, Handler handler,
                                   QObject *parent)
    : QObject(parent), m_connection(connection), m_service(service), m_path(path), m_iface(iface),
      m_handler(std::move(handler)) {}

DBusSubscription *DBusSubscription::create(const QDBusConnection &connection,
                                           const QString &service, const QString &path,
                                           const QString &iface, const QStringList &members,
                                           Handler handler, QObject *parent) {
    auto *subscription =
        new DBusSubscription(connection, service, path, iface, std::move(handler), parent);
    for (const QString &member : members) {
        if (subscription->m_members.contains(member))
            continue;
        if (!subscription->m_connection.connect(service, path, iface, member, subscription,
                                                SLOT(deliver(QDBusMessage)))) {
            subscription->revoke();
            return nullptr;
        }
        subscription->m_members.append(member);
    }
    return subscription;
}

DBusSubscription::~DBusSubscription() {
    m_revoked = true;
    disconnectMembers();
}

void DBusSubscription::disconnectMembers() {
    for (const QString &member : std::as_const(m_members))
        m_connection.disconnect(m_service, m_path, m_iface, member, this,
                                SLOT(deliver(QDBusMessage)));
    m_members.clear();
}

void DBusSubscription::revoke() {
    if (m_revoked)
        return;
    // Hook removal does not cancel QDBusCallDeliveryEvents already posted
    // to the receiver (qdbusintegrator.cpp, Qt 6.8.2 and 6.11.2). Mark first,
    // so even re-entrant event processing during teardown cannot forward.
    m_revoked = true;
    disconnectMembers();
    deleteLater();
}

void DBusSubscription::deliver(const QDBusMessage &message) {
    if (m_revoked)
        return;
    // The consumer may revoke us from inside the handler. Keep the callable
    // alive independently even if it destroys its parent or runs a nested loop.
    const Handler handler = m_handler;
    handler(message);
}

#include "dbussubscription.moc"
