#include "dbusconnection.h"
#include "dbustypes.h"

#include <QAtomicInt>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QJSValue>
#include <QJSValueList>
#include <QQmlEngine>

// Convert a QVariant into a native JS value, recursively unwrapping lists
// and maps so the JS side receives real Array / Object instances (with a
// working Array.isArray and iterable/spread semantics), not the array-like
// QVariantList wrappers QQmlEngine::toScriptValue produces by default.
static QJSValue variantToJs(QQmlEngine *engine, const QVariant &v)
{
    const int t = v.userType();
    if (t == qMetaTypeId<QVariantList>() || t == qMetaTypeId<QStringList>()) {
        const QVariantList list = v.toList();
        QJSValue arr = engine->newArray(static_cast<quint32>(list.size()));
        for (int i = 0; i < list.size(); ++i)
            arr.setProperty(static_cast<quint32>(i), variantToJs(engine, list.at(i)));
        return arr;
    }
    if (t == qMetaTypeId<QVariantMap>()) {
        const QVariantMap map = v.toMap();
        QJSValue obj = engine->newObject();
        for (auto it = map.begin(); it != map.end(); ++it)
            obj.setProperty(it.key(), variantToJs(engine, it.value()));
        return obj;
    }
    return engine->toScriptValue(v);
}

// Recursively unwrap QDBusVariant / QDBusArgument values into plain QVariant
// containers so QML can traverse them as JavaScript objects. Handles nested
// a{sv}, a{ss}, av, as, ao, etc.
QVariant unwrapDbus(const QVariant &v)
{
    if (v.userType() == qMetaTypeId<QDBusVariant>())
        return unwrapDbus(v.value<QDBusVariant>().variant());

    if (v.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        const QString sig = arg.currentSignature();

        // Dicts with string keys — a{s*}
        if (sig.startsWith("a{s")) {
            QVariantMap map;
            arg >> map;
            QVariantMap out;
            for (auto it = map.begin(); it != map.end(); ++it)
                out.insert(it.key(), unwrapDbus(it.value()));
            return out;
        }
        // Array of variants — safe to iterate with QVariant target.
        if (sig == "av") {
            QVariantList list;
            arg.beginArray();
            while (!arg.atEnd()) {
                QVariant elem;
                arg >> elem;
                list.append(unwrapDbus(elem));
            }
            arg.endArray();
            return list;
        }
        // Concrete-type arrays: demarshal via the C++ type, then flatten
        // to QVariantList. Iterating a concrete-typed QDBusArgument with
        // an untyped QVariant target crashes inside libdbus.
        if (sig == "ao") {
            QList<QDBusObjectPath> paths;
            arg >> paths;
            QVariantList out;
            out.reserve(paths.size());
            for (const auto &p : paths) out.append(p.path());
            return out;
        }
        if (sig == "as") {
            QStringList list;
            arg >> list;
            QVariantList out;
            out.reserve(list.size());
            for (const auto &s : list) out.append(s);
            return out;
        }
        if (sig == "ay") {
            QByteArray bytes;
            arg >> bytes;
            return bytes;
        }
        // Anything else — struct-array, typed-numeric-array, etc. — is
        // left as the raw QDBusArgument. Callers that need those can
        // demarshal explicitly with qdbus_cast<T>.
        return v;
    }
    return v;
}

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
    // A fixed connection name causes QtDBus to return the FIRST connection
    // for every subsequent call, silently reusing it regardless of address.
    // Use a per-call counter so callers get distinct connections.
    static QAtomicInt counter{0};
    QString name = QStringLiteral("dbusqml-custom-%1").arg(counter.fetchAndAddOrdered(1) + 1);
    auto conn = QDBusConnection::connectToBus(address, name);
    if (!conn.isConnected())
        return nullptr;
    return new DBusConnection(conn, name);
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
    // Promise-style overload: (resolve, reject) callbacks.
    //   resolve is called with the reply value converted natively to a JS
    //     value (numbers, booleans, arrays, and dicts survive; nested D-Bus
    //     containers are unwrapped via unwrapDbus).
    //   reject is called with a single error object { name, message }.
    auto reply = asyncCall(message);
    if (!resolve.isCallable() && !reject.isCallable())
        return;

    QQmlEngine *engine = qmlEngine(this);
    connect(reply, &DBusPendingReply::finished, this,
            [reply, resolve = QJSValue(resolve), reject = QJSValue(reject), engine]() mutable {
                if (reply->isError()) {
                    if (reject.isCallable()) {
                        QJSValue errObj;
                        if (engine) {
                            errObj = engine->newObject();
                            errObj.setProperty(QStringLiteral("name"),
                                               QJSValue(reply->error().name()));
                            errObj.setProperty(QStringLiteral("message"),
                                               QJSValue(reply->error().message()));
                        } else {
                            errObj = QJSValue(reply->error().message());
                        }
                        reject.call({ errObj });
                    }
                } else if (resolve.isCallable()) {
                    QVariant unwrapped = unwrapDbus(reply->value());
                    QJSValue val = engine
                        ? variantToJs(engine, unwrapped)
                        : QJSValue(reply->value().toString());
                    resolve.call({ val });
                }
            });
}

SessionBusConnection::SessionBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::sessionBus(), QString(), parent)
{
}

SystemBusConnection::SystemBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::systemBus(), QString(), parent)
{
}
