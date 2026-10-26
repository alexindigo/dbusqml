#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <functional>

// Internal receiver identity for one subscription lifetime. QtDBus queues
// delivery events to this object, never to the long-lived consumer.
class DBusSubscription : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<void(const QDBusMessage &)>;

    // All members share the recorded connection/service/path/interface.
    // An empty member matches every signal on the interface. Duplicate
    // members install only one hook. A partial setup is rolled back.
    static DBusSubscription *create(const QDBusConnection &connection, const QString &service,
                                    const QString &path, const QString &iface,
                                    const QStringList &members, Handler handler, QObject *parent);
    ~DBusSubscription() override;

    void revoke();
    bool isRevoked() const { return m_revoked; }

private slots:
    void deliver(const QDBusMessage &message);

private:
    DBusSubscription(const QDBusConnection &connection, const QString &service, const QString &path,
                     const QString &iface, Handler handler, QObject *parent);
    void disconnectMembers();

    QDBusConnection m_connection;
    QString m_service;
    QString m_path;
    QString m_iface;
    QSet<QString> m_members;
    Handler m_handler;
    bool m_revoked = false;
};
