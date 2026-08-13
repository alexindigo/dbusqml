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
#include <QPointer>
#include <QQmlEngine>

// Convert a QVariant into a native JS value, recursively unwrapping lists
// and maps so the JS side receives real Array / Object instances (with a
// working Array.isArray and iterable/spread semantics), not the array-like
// QVariantList wrappers QQmlEngine::toScriptValue produces by default.
QJSValue variantToJs(QQmlEngine *engine, const QVariant &v) {
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

// Read a single element from a QDBusArgument using the correct type
// based on the current signature. Avoids operator>>(QDBusArgument,
// QVariant) which crashes inside libdbus inside nested containers.
static QVariant readBySignature(const QDBusArgument &arg) {
    const QString sig = arg.currentSignature();

    // Basic types — single char signatures
    if (sig == "y") {
        uchar v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "b") {
        bool v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "n") {
        short v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "q") {
        ushort v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "i") {
        int v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "u") {
        uint v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "x") {
        qint64 v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "t") {
        quint64 v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "d") {
        double v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "s") {
        QString v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "o") {
        QDBusObjectPath v;
        arg >> v;
        return QVariant::fromValue(v.path());
    }
    if (sig == "g") {
        QDBusSignature v;
        arg >> v;
        return QVariant::fromValue(v.signature());
    }
    if (sig == "v") {
        QDBusVariant v;
        arg >> v;
        return unwrapDbus(v.variant());
    }
    if (sig == "ay") {
        QByteArray v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "as") {
        QStringList v;
        arg >> v;
        return QVariant::fromValue(v);
    }

    // Containers — recursive, signature-driven. The caller has already
    // opened the container (beginStructure / beginMap / beginMapEntry /
    // beginArray) and we're positioned at one complete element. For a
    // nested container we open it, recurse per member/element/entry, and
    // close it. Never touches operator>>(QDBusArgument, QVariant&).
    if (sig.startsWith(QStringLiteral("a{"))) {
        // Array of dict entries — a{KV}. Read entries one by one.
        QVariantMap map;
        arg.beginMap();
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            QVariant key = readBySignature(arg);
            QVariant value = readBySignature(arg);
            map.insert(key.toString(), unwrapDbus(value));
            arg.endMapEntry();
        }
        arg.endMap();
        return map;
    }
    if (sig.startsWith(QStringLiteral("a(")) || sig.startsWith(QStringLiteral("a["))) {
        // Array of structs — a(...). Elements are structs.
        QVariantList out;
        arg.beginArray();
        while (!arg.atEnd())
            out.append(readBySignature(arg));
        arg.endArray();
        return out;
    }
    if (sig.startsWith(QStringLiteral("aa"))) {
        // Array of arrays — aa* (including aa{...}). Elements are arrays.
        QVariantList out;
        arg.beginArray();
        while (!arg.atEnd())
            out.append(readBySignature(arg));
        arg.endArray();
        return out;
    }
    if (sig.startsWith(QStringLiteral("("))) {
        // Struct / tuple — (...). Members read per-signature.
        QVariantList members;
        arg.beginStructure();
        while (!arg.atEnd())
            members.append(readBySignature(arg));
        arg.endStructure();
        return members;
    }

    // Unrecognized signature — make the gap visible in logs.
    qWarning("DBus: readBySignature: unsupported signature %s", qPrintable(sig));
    return {};
}

// Recursively unwrap QDBusVariant / QDBusArgument values into plain QVariant
// containers so QML can traverse them as JavaScript objects. Handles nested
// a{sv}, a{ss}, av, as, ao, etc.
QVariant unwrapDbus(const QVariant &v) {
    if (v.userType() == qMetaTypeId<QDBusVariant>())
        return unwrapDbus(v.value<QDBusVariant>().variant());

    if (v.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        const QString sig = arg.currentSignature();

        // Top-level dict with string keys — fast path via the concrete
        // QVariantMap read. Nested dicts (a{sa{sv}} etc.) are handled by
        // the recursive readBySignature branch below.
        if (sig.startsWith("a{") && sig.length() == 4 && sig[2] == 's') {
            QVariantMap map;
            arg >> map;
            QVariantMap out;
            for (auto it = map.begin(); it != map.end(); ++it)
                out.insert(it.key(), unwrapDbus(it.value()));
            return out;
        }
        // Array of variants — safe to iterate with QVariant target at top level.
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
            for (const auto &p : paths)
                out.append(p.path());
            return out;
        }
        if (sig == "as") {
            QStringList list;
            arg >> list;
            QVariantList out;
            out.reserve(list.size());
            for (const auto &s : list)
                out.append(s);
            return out;
        }
        if (sig == "ay") {
            QByteArray bytes;
            arg >> bytes;
            return bytes;
        }

        // Everything else — nested containers, dict arrays, structs,
        // tuples, deep nesting. readBySignature recurses per-element and
        // never touches operator>>(QDBusArgument, QVariant&).
        return readBySignature(arg);
    }
    return v;
}

QVariant toDbusVariant(const QVariant &v) {
    int type = v.userType();

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
    if (type == qMetaTypeId<DBus::Bytes>())
        return QVariant::fromValue(v.value<DBus::Bytes>().value);
    if (type == qMetaTypeId<DBus::Dict>()) {
        // Unwrap recursively: a Dict's QVariantMap may itself hold Dict /
        // Variant values (e.g. NetworkManager connection dicts a{sa{sv}}).
        // Raw DBus::Dict values are not registered with QtDBus — leaving them
        // nested crashes the marshaller ("type 'DBus::Dict' is not
        // registered", caught on the arch-niri VM).
        QVariantMap m = v.value<DBus::Dict>().value;
        for (auto it = m.begin(); it != m.end(); ++it)
            it.value() = toDbusVariant(it.value());
        return QVariant::fromValue(m);
    }
    if (type == qMetaTypeId<DBus::Variant>()) {
        // Recurse into the variant payload for the same reason.
        return QVariant::fromValue(
            QDBusVariant(toDbusVariant(v.value<DBus::Variant>().propValue())));
    }

    return v;
}

// ==================== Signature-driven marshaller ====================

// Parse one complete D-Bus type from `sig` starting at `pos`.
// Returns the type's signature substring and advances pos past it.
// Returns empty on parse failure.
static QString firstCompleteType(const QString &sig, int &pos) {
    if (pos >= sig.size())
        return {};
    int start = pos;
    QChar c = sig.at(pos);

    // Basic single-char types
    if (QStringLiteral("ybnqiuxtdhsogv").contains(c)) {
        ++pos;
        return sig.mid(start, 1);
    }

    if (c == QLatin1Char('a')) {
        // Array — 'a' followed by one complete type
        ++pos;
        QString elem = firstCompleteType(sig, pos);
        if (elem.isEmpty())
            return {};
        // Dict entry shorthand: a{KV} — the {KV} is one element type
        return sig.mid(start, pos - start);
    }

    if (c == QLatin1Char('(')) {
        // Struct — (...) with member types
        int depth = 1;
        ++pos;
        while (pos < sig.size() && depth > 0) {
            if (sig.at(pos) == QLatin1Char('('))
                ++depth;
            else if (sig.at(pos) == QLatin1Char(')'))
                --depth;
            ++pos;
        }
        if (depth != 0)
            return {};
        return sig.mid(start, pos - start);
    }

    if (c == QLatin1Char('{')) {
        // Dict entry — {KV} (only valid inside an array, but parse it anyway)
        int depth = 1;
        ++pos;
        while (pos < sig.size() && depth > 0) {
            if (sig.at(pos) == QLatin1Char('{'))
                ++depth;
            else if (sig.at(pos) == QLatin1Char('}'))
                --depth;
            ++pos;
        }
        if (depth != 0)
            return {};
        return sig.mid(start, pos - start);
    }

    return {};
}

// Forward declaration — mutual recursion between marshalBySignature and
// marshalContainerBySignature.
static QVariant marshalContainerBySignature(const QString &sig, const QVariant &value);

// Marshal a JS-supplied QVariant against a known D-Bus signature.
// Produces a QVariant with the correct C++ type for QtDBus to marshal
// to the wire format matching `sig`. Falls back to toDbusVariant for
// inference when the signature is empty, "v", or unrecognized.
QVariant marshalBySignature(const QString &sig, const QVariant &value) {
    if (sig.isEmpty() || sig == QLatin1String("v"))
        return toDbusVariant(value);

    // Explicit DBus.* wrapper types always win — the caller chose the type.
    if (value.userType() != qMetaTypeId<QVariantList>() &&
        value.userType() != qMetaTypeId<QVariantMap>() && value.userType() != QMetaType::QString &&
        value.userType() != QMetaType::Bool && value.userType() != QMetaType::Int &&
        value.userType() != QMetaType::Double && value.userType() != QMetaType::UInt &&
        value.userType() != QMetaType::LongLong && value.userType() != QMetaType::ULongLong &&
        value.userType() != QMetaType::QByteArray) {
        // Non-plain type — likely a DBus.* gadget or QDBusObjectPath etc.
        // Unwrap via toDbusVariant; the resulting type should match the
        // signature already.
        return toDbusVariant(value);
    }

    // Basic types — coerce the QVariant to the exact C++ type.
    if (sig == QLatin1String("y"))
        return QVariant::fromValue(static_cast<uchar>(value.toUInt()));
    if (sig == QLatin1String("b"))
        return QVariant::fromValue(value.toBool());
    if (sig == QLatin1String("n"))
        return QVariant::fromValue(static_cast<short>(value.toInt()));
    if (sig == QLatin1String("q"))
        return QVariant::fromValue(static_cast<ushort>(value.toUInt()));
    if (sig == QLatin1String("i"))
        return QVariant::fromValue(value.toInt());
    if (sig == QLatin1String("u"))
        return QVariant::fromValue(value.toUInt());
    if (sig == QLatin1String("x"))
        return QVariant::fromValue(static_cast<qint64>(value.toLongLong()));
    if (sig == QLatin1String("t"))
        return QVariant::fromValue(static_cast<quint64>(value.toULongLong()));
    if (sig == QLatin1String("d"))
        return QVariant::fromValue(value.toDouble());
    if (sig == QLatin1String("s"))
        return QVariant::fromValue(value.toString());
    if (sig == QLatin1String("o"))
        return QVariant::fromValue(QDBusObjectPath(value.toString()));
    if (sig == QLatin1String("g"))
        return QVariant::fromValue(QDBusSignature(value.toString()));

    // Byte array — ay. Accept string (UTF-8), number array, or QByteArray.
    if (sig == QLatin1String("ay")) {
        if (value.userType() == QMetaType::QByteArray)
            return value;
        if (value.userType() == QMetaType::QString)
            return QVariant::fromValue(value.toString().toUtf8());
        if (value.userType() == qMetaTypeId<QVariantList>()) {
            QByteArray bytes;
            const QVariantList list = value.toList();
            bytes.reserve(list.size());
            for (const QVariant &b : list)
                bytes.append(static_cast<char>(b.toInt()));
            return QVariant::fromValue(bytes);
        }
        // Fallback: try string conversion
        return QVariant::fromValue(value.toString().toUtf8());
    }

    // String array — as. Use DBusAsArray to force the correct marshaling
    // (QStringList alone may marshal as av).
    if (sig == QLatin1String("as")) {
        DBusAsArray arr;
        const QVariantList list = value.toList();
        for (const QVariant &item : list)
            arr.value << item.toString();
        return QVariant::fromValue(arr);
    }

    // Variant — wrap in QDBusVariant after unwrapping any DBus.* types.
    if (sig == QLatin1String("v"))
        return QVariant::fromValue(QDBusVariant(toDbusVariant(value)));

    // Container types — delegate to the recursive container marshaller.
    if (sig.startsWith(QLatin1Char('a')) || sig.startsWith(QLatin1Char('(')) ||
        sig.startsWith(QLatin1Char('{')))
        return marshalContainerBySignature(sig, value);

    // Unrecognized — fall back to inference.
    return toDbusVariant(value);
}

// Container marshaling — handles a{...}, a<complex>, (...), etc.
// For dicts we register the correct QtDBus type on the fly via
// QDBusMetaType::registerCustomType or use known container types.
static QVariant marshalContainerBySignature(const QString &sig, const QVariant &value) {
    // a{sv} — dict with string keys and variant values.
    // QVariantMap is exactly a{sv} in QtDBus.
    if (sig == QLatin1String("a{sv}")) {
        QVariantMap map = value.toMap();
        // Recurse into values — nested Dict/Variant payloads must be unwrapped.
        for (auto it = map.begin(); it != map.end(); ++it)
            it.value() = toDbusVariant(it.value());
        return QVariant::fromValue(map);
    }

    // a{sa{sv}} — dict of dicts. NM connection settings shape.
    // Needs QMap<QString,QVariantMap> — registered in dbusplugin.cpp.
    if (sig == QLatin1String("a{sa{sv}}")) {
        QVariantMap outer = value.toMap();
        QMap<QString, QVariantMap> typed;
        for (auto it = outer.begin(); it != outer.end(); ++it) {
            QVariantMap inner = it.value().toMap();
            for (auto jt = inner.begin(); jt != inner.end(); ++jt)
                jt.value() = toDbusVariant(jt.value());
            typed.insert(it.key(), inner);
        }
        return QVariant::fromValue(typed);
    }

    // a(sss...) — array of structs with homogeneous members.
    // Marshal as QVariantList of QVariantList; QtDBus can't type-check this
    // without a registered struct type, so we hand-marshal via QDBusArgument.
    // This is the general path for any a(...) where ... is not a basic type.

    // Generic container marshaling via QDBusArgument — the escape hatch.
    // Build a writable QDBusArgument, populate it per the signature, and
    // wrap it as a QVariant. QtDBus will cross-marshal it into the message.
    // NOTE: This requires QtDBus to accept a QDBusArgument as a message
    // argument. The fallback mechanism (registerCustomType) covers the
    // known shapes; for unknown deep nesting we use QDBusArgument.
    {
        // For now, handle aay (array of byte arrays) explicitly.
        if (sig == QLatin1String("aay")) {
            QList<QByteArray> list;
            const QVariantList items = value.toList();
            for (const QVariant &item : items) {
                if (item.userType() == QMetaType::QByteArray)
                    list << item.toByteArray();
                else if (item.userType() == QMetaType::QString)
                    list << item.toString().toUtf8();
                else
                    list << item.toByteArray();
            }
            return QVariant::fromValue(list);
        }

        // Generic: try inference. The typed-container registrations handle
        // the common shapes; anything else falls through to toDbusVariant.
        return toDbusVariant(value);
    }
}

static QDBusMessage toQDBusMessage(const DBusMessage &msg) {
    auto qmsg =
        QDBusMessage::createMethodCall(msg.service(), msg.path(), msg.iface(), msg.member());

    if (!msg.arguments().isEmpty()) {
        QVariantList args = msg.arguments();

        // If the message carries an explicit signature, use it to drive
        // per-argument marshaling. The signature is a concatenation of
        // per-arg complete types, e.g. "sa{sa{sv}}ay" for three args.
        if (!msg.signature().isEmpty()) {
            QString sig = msg.signature();
            int pos = 0;
            for (int i = 0; i < args.size() && pos < sig.size(); ++i) {
                QString argSig = firstCompleteType(sig, pos);
                if (argSig.isEmpty())
                    break;
                args[i] = marshalBySignature(argSig, args[i]);
            }
        } else {
            for (int i = 0; i < args.size(); ++i)
                args[i] = toDbusVariant(args[i]);
        }
        qmsg.setArguments(args);
    }

    return qmsg;
}

DBusConnection::DBusConnection(const QDBusConnection &conn, const QString &name, QObject *parent)
    : QObject(parent), m_connection(conn), m_connectionName(name) {}

DBusConnection::~DBusConnection() {}

DBusConnection *DBusConnection::connectToBus(const QString &address) {
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

DBusPendingReply *DBusConnection::asyncCall(const DBusMessage &message) {
    auto qmsg = toQDBusMessage(message);
    auto pending = m_connection.asyncCall(qmsg);
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setEngine(qmlEngine(this));
    reply->setWatcher(watcher);
    return reply;
}

void DBusConnection::asyncCall(const DBusMessage &message, const QJSValue &resolve,
                               const QJSValue &reject) {
    // Promise-style overload: (resolve, reject) callbacks.
    //   resolve is called with the reply value converted natively to a JS
    //     value (numbers, booleans, arrays, and dicts survive; nested D-Bus
    //     containers are unwrapped via unwrapDbus).
    //   reject is called with a single error object { name, message }.
    auto reply = asyncCall(message);
    if (!resolve.isCallable() && !reject.isCallable())
        return;

    QPointer<QQmlEngine> engine = qmlEngine(this);
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
                        reject.call({errObj});
                    }
                } else if (resolve.isCallable()) {
                    QVariant unwrapped = unwrapDbus(reply->value());
                    QJSValue val = engine ? variantToJs(engine.data(), unwrapped)
                                          : QJSValue(reply->value().toString());
                    resolve.call({val});
                }
            });
}

SessionBusConnection::SessionBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::sessionBus(), QString(), parent) {}

SystemBusConnection::SystemBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::systemBus(), QString(), parent) {}
