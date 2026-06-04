#pragma once

#include <QDBusConnection>
#include <QJSValue>
#include <QObject>
#include <QVariantList>
#include <qqmlregistration.h>

#include "dbusmessage.h"
#include "dbuspendingreply.h"

QVariant toDbusVariant(const QVariant &v);

class busType {
    Q_GADGET
    QML_NAMED_ELEMENT(busType)
    QML_UNCREATABLE("Enum type")
public:
    enum Type { Session, System };
    Q_ENUM(Type)
};

class DBusConnection : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DBusConnection)

public:
    explicit DBusConnection(const QDBusConnection &conn, const QString &name, QObject *parent = nullptr);
    ~DBusConnection() override;

    Q_INVOKABLE static DBusConnection *connectToBus(const QString &address);

    Q_INVOKABLE DBusPendingReply *asyncCall(const DBusMessage &message);
    Q_INVOKABLE void asyncCall(const DBusMessage &message,
                               const QJSValue &resolve,
                               const QJSValue &reject);

    operator QDBusConnection() const { return m_connection; }

private:
    QDBusConnection m_connection;
    QString m_connectionName;
};

class SessionBusConnection : public DBusConnection {
    Q_OBJECT
    QML_NAMED_ELEMENT(SessionBus)
    QML_SINGLETON

public:
    explicit SessionBusConnection(QObject *parent = nullptr);
};

class SystemBusConnection : public DBusConnection {
    Q_OBJECT
    QML_NAMED_ELEMENT(SystemBus)
    QML_SINGLETON

public:
    explicit SystemBusConnection(QObject *parent = nullptr);
};
