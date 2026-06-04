#include "dbusconnection.h"
#include "dbustypes.h"

#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QJSValue>
#include <QJSValueList>

QVariant toDbusVariant(const QVariant &v)
{
    int type = v.userType();

    auto convert = [&](auto id, const auto &fn) -> QVariant {
        if (type == id)
            return QVariant::fromValue(fn(v.value<std::decay_t<decltype(fn(v))>>()));
        return {};
    };

    if (type == qMetaTypeId<DBus::Bool>())
        return QVariant::fromValue(v.value<DBus::Bool>().value);
    if (type == qMetaTypeId<DBus::Int16>())
        return QVariant::fromValue(v.value<DBus::Int16>().value);
    if (type == qMetaTypeId<DBus::Int32>())
        return QVariant::fromValue(v.value<DBus::Int32>().value);
    if (type == qMetaTypeId<DBus::Int64>())
        return QVariant::fromValue(v.value<DBus::Int64>().value);
    if (type == qMetaTypeId<DBus::Uint16>())
        return QVariant::fromValue(v.value<DBus::Uint16>().value);
    if (type == qMetaTypeId<DBus::Uint32>())
        return QVariant::fromValue(v.value<DBus::Uint32>().value);
    if (type == qMetaTypeId<DBus::Uint64>())
        return QVariant::fromValue(v.value<DBus::Uint64>().value);
    if (type == qMetaTypeId<DBus::Double>())
        return QVariant::fromValue(v.value<DBus::Double>().value);
    if (type == qMetaTypeId<DBus::Byte>())
        return QVariant::fromValue(v.value<DBus::Byte>().value);
    if (type == qMetaTypeId<DBus::String>())
        return QVariant::fromValue(v.value<DBus::String>().value);
    if (type == qMetaTypeId<DBus::ObjectPath>())
        return QVariant::fromValue(v.value<DBus::ObjectPath>().value);
    if (type == qMetaTypeId<DBus::Signature>())
        return QVariant::fromValue(v.value<DBus::Signature>().value);
    if (type == qMetaTypeId<DBus::Dict>())
        return QVariant::fromValue(v.value<DBus::Dict>().value);
    if (type == qMetaTypeId<DBus::Variant>())
        return QVariant::fromValue(v.value<DBus::Variant>().value);

    return v;
}

static QDBusMessage toQDBusMessage(const DBusMessage &msg)
{
    auto qmsg = QDBusMessage::createMethodCall(
        msg.service(), msg.path(), msg.iface(), msg.member());

    if (!msg.arguments().isEmpty()) {
        QVariantList args = msg.arguments();
        for (int i = 0; i < args.size(); ++i)
            args[i] = toDbusVariant(args[i]);
        qmsg.setArguments(args);
    }

    return qmsg;
}

DBusConnection::DBusConnection(const QDBusConnection &conn, const QString &name, QObject *parent)
    : QObject(parent)
    , m_connection(conn)
    , m_connectionName(name)
{
}

DBusConnection::~DBusConnection()
{
}

DBusConnection *DBusConnection::connectToBus(const QString &address)
{
    auto conn = QDBusConnection::connectToBus(address, QStringLiteral("dbusqml-custom"));
    if (!conn.isConnected())
        return nullptr;
    return new DBusConnection(conn, QString());
}

DBusPendingReply *DBusConnection::asyncCall(const DBusMessage &message)
{
    auto qmsg = toQDBusMessage(message);
    auto pending = m_connection.asyncCall(qmsg);
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setWatcher(watcher);
    return reply;
}

void DBusConnection::asyncCall(const DBusMessage &message,
                                const QJSValue &resolve,
                                const QJSValue &reject)
{
    // Promise-style overload — simplified for v1, takes (result, error) callbacks
    auto reply = asyncCall(message);
    if (resolve.isCallable()) {
        connect(reply, &DBusPendingReply::finished, this,
                [reply, resolve = QJSValue(resolve), reject = QJSValue(reject)]() {
                    if (reply->isError()) {
                        if (reject.isCallable()) {
                            const_cast<QJSValue&>(reject).call({
                                QJSValue(reply->error().name()),
                                QJSValue(reply->error().message())
                            });
                        }
                    } else {
                        if (resolve.isCallable()) {
                            const_cast<QJSValue&>(resolve).call({
                                QJSValue(reply->value().toString())
                            });
                        }
                    }
                });
    }
}

SessionBusConnection::SessionBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::sessionBus(), QString(), parent)
{
}

SystemBusConnection::SystemBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::systemBus(), QString(), parent)
{
}
