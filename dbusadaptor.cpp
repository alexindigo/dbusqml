#include "dbusadaptor.h"
#include "dbuscatalog.h"
#include "dbusconnection.h" // wireMarshalable — shared marshal-boundary guard
#include "dbusheldreply.h"
#include "dbuspathdispatcher.h"
#include "dbustypes.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QJSValue>
#include <QJSValueList>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QRegularExpression>
#include <qqmlinfo.h>

// Map a D-Bus PascalCase member name to the QML camelCase convention.
// Same rule as the proxy's dbusPropToQml: fold only the first character.
// QML forbids uppercase-initial method names, so a spec-faithful D-Bus
// member like "ReadOne" is declared in QML as "readOne".
static QString dbusMemberToQml(const QString &name) {
    if (name.isEmpty())
        return name;
    return name.at(0).toLower() + name.mid(1);
}

// Underscore-prefixed adaptor properties are library meta-config (e.g.
// _signatures), not part of the served D-Bus surface. They are never exported
// via generateXml or Properties.Get/GetAll/Set.
static bool isPrivateProperty(const QString &name) {
    return name.startsWith(QLatin1Char('_'));
}

// Helper: forwards QML signal emissions to D-Bus.
// One relay per signal, with the signal name baked in at construction.
class SignalRelay : public QObject {
    Q_OBJECT
public:
    SignalRelay(DBusAdaptor *adaptor, const QString &signalName, QObject *parent = nullptr)
        : QObject(parent), m_adaptor(adaptor), m_name(signalName) {}

public slots:
    void forward() {
        QDBusMessage msg =
            QDBusMessage::createSignal(m_adaptor->path(), m_adaptor->iface(), m_name);
        busConn().send(msg);
    }
    void forward(QVariant a0) {
        QDBusMessage msg =
            QDBusMessage::createSignal(m_adaptor->path(), m_adaptor->iface(), m_name);
        msg.setArguments({toDbusVariant(a0)});
        busConn().send(msg);
    }
    void forward(QVariant a0, QVariant a1) {
        QDBusMessage msg =
            QDBusMessage::createSignal(m_adaptor->path(), m_adaptor->iface(), m_name);
        msg.setArguments({toDbusVariant(a0), toDbusVariant(a1)});
        busConn().send(msg);
    }
    void forward(QVariant a0, QVariant a1, QVariant a2) {
        QDBusMessage msg =
            QDBusMessage::createSignal(m_adaptor->path(), m_adaptor->iface(), m_name);
        msg.setArguments({toDbusVariant(a0), toDbusVariant(a1), toDbusVariant(a2)});
        busConn().send(msg);
    }
    void forward(QVariant a0, QVariant a1, QVariant a2, QVariant a3) {
        QDBusMessage msg =
            QDBusMessage::createSignal(m_adaptor->path(), m_adaptor->iface(), m_name);
        msg.setArguments(
            {toDbusVariant(a0), toDbusVariant(a1), toDbusVariant(a2), toDbusVariant(a3)});
        busConn().send(msg);
    }
    void forward(QVariant a0, QVariant a1, QVariant a2, QVariant a3, QVariant a4) {
        QDBusMessage msg =
            QDBusMessage::createSignal(m_adaptor->path(), m_adaptor->iface(), m_name);
        msg.setArguments({toDbusVariant(a0), toDbusVariant(a1), toDbusVariant(a2),
                          toDbusVariant(a3), toDbusVariant(a4)});
        busConn().send(msg);
    }

private:
    QDBusConnection busConn() const {
        return m_adaptor->connection() ? static_cast<QDBusConnection>(*m_adaptor->connection())
                                       : QDBusConnection::sessionBus();
    }
    DBusAdaptor *m_adaptor;
    QString m_name;
};

DBusAdaptor::DBusAdaptor(QObject *parent)
    : QDBusVirtualObject(parent),
      m_currentCall{QDBusMessage(), QDBusConnection::sessionBus(), QString(), false} {}

DBusAdaptor::~DBusAdaptor() {
    // Error out any held reply that was never settled. The adaptor is being
    // destroyed, so the deferred reply can never be answered — send an error
    // reply rather than leaving the caller to time out.
    const auto heldReplies = findChildren<DBusHeldReply *>();
    for (DBusHeldReply *reply : heldReplies) {
        if (!reply->isSettled()) {
            reply->sendError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                             QStringLiteral("adaptor destroyed with reply pending"));
        }
    }

    if (m_attached)
        DBusPathDispatcher::detach(bus(), m_path, m_service, this);
}

void DBusAdaptor::setService(const QString &v) {
    if (m_service == v)
        return;
    m_service = v;
    emit serviceChanged();
}

void DBusAdaptor::setPath(const QString &v) {
    if (m_path == v)
        return;
    m_path = v;
    emit pathChanged();
}

void DBusAdaptor::setIface(const QString &v) {
    if (m_iface == v)
        return;
    m_iface = v;
    emit ifaceChanged();
}

void DBusAdaptor::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;
    m_conn = v;
    emit connectionChanged();
}

void DBusAdaptor::setSignatures(const QVariantMap &v) {
    if (m_signatures == v)
        return;
    m_signatures = v;
    emit signaturesChanged();
}

QDBusConnection DBusAdaptor::bus() const {
    if (m_conn)
        return static_cast<QDBusConnection>(*m_conn);
    return QDBusConnection::sessionBus();
}

void DBusAdaptor::componentComplete() {
    if (m_iface.isEmpty())
        qmlInfo(this)
            << "DBusAdaptor: iface is empty — introspection XML will have an empty interface name";

    QDBusConnection conn = bus();
    m_attached = DBusPathDispatcher::attach(conn, m_path, m_service, this);
    if (!m_attached)
        return;

    // Auto-connect user-defined QML signals to D-Bus
    const QMetaObject *meta = metaObject();
    static const QStringList builtInSignals = {
        QStringLiteral("destroyed"),        QStringLiteral("objectNameChanged"),
        QStringLiteral("serviceChanged"),   QStringLiteral("pathChanged"),
        QStringLiteral("ifaceChanged"),     QStringLiteral("connectionChanged"),
        QStringLiteral("signaturesChanged")};

    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod sig = meta->method(i);
        if (sig.methodType() != QMetaMethod::Signal)
            continue;
        QString name = QString::fromLatin1(sig.name());
        if (builtInSignals.contains(name))
            continue;

        int paramCount = sig.parameterCount();
        if (paramCount > 5) {
            qmlInfo(this) << "Signal" << name << "has" << paramCount
                          << "parameters — max 5 supported for auto-forwarding";
            continue;
        }

        auto *relay = new SignalRelay(this, name, this);
        int slotIdx = relay->metaObject()->methodOffset() + paramCount;
        QMetaMethod slot = relay->metaObject()->method(slotIdx);
        QByteArray signalSig = "2" + sig.methodSignature();
        QByteArray slotSig = "1" + QByteArray(slot.methodSignature());
        QObject::connect(this, signalSig.constData(), relay, slotSig.constData());
    }
}

QString DBusAdaptor::introspect(const QString &) const {
    return generateXml();
}

// Convert a QJSValue to QVariant for D-Bus marshaling. Gadget types
// (DBus::Variant, DBus::Dict, etc.) are preserved by QJSValue::toVariant()
// in Qt 6 when the gadget's metatype is registered — which the plugin's
// static initializer ensures.
//
// No shape-guessing here: a former heuristic treated any single-"value"-key
// map as a flattened gadget (and its all-doubles list payload as a struct),
// silently turning a legitimate `{value: 42}` dict into `v(i)`. Gadgets
// survive QJSValue conversion intact; a flattened-gadget shape is a real
// dict. Explicit typing is `new DBusQML.variant(x)` / `struct_` — the
// documented contract since 0.3.x/0.4.0.
QVariant qjsValueToVariant(const QJSValue &jsval) {
    return jsval.toVariant();
}

void DBusAdaptor::emitSignal(const QString &name, const QJSValue &arguments) {
    QDBusMessage msg = QDBusMessage::createSignal(m_path, m_iface, name);
    if (!arguments.isUndefined()) {
        QVariantList args;
        if (arguments.isArray()) {
            int len = arguments.property(QStringLiteral("length")).toInt();
            for (int i = 0; i < len; ++i) {
                QJSValue item = arguments.property(i);
                args.append(toDbusVariant(qjsValueToVariant(item)));
            }
        } else {
            args.append(toDbusVariant(qjsValueToVariant(arguments)));
        }
        // Robustness guard: an unmarshalable arg would kill the connection.
        // Signals have no error-reply channel, so warn and skip the send.
        for (const QVariant &a : std::as_const(args)) {
            if (!wireMarshalable(a)) {
                qWarning("dbusqml: signal %s arg is not marshalable (type %s) — skipping send",
                         qPrintable(name), QMetaType(a.userType()).name());
                return;
            }
        }
        msg.setArguments(args);
    }
    QDBusConnection conn = bus();
    if (!conn.send(msg))
        qmlInfo(this) << conn.lastError();
}
// Map a QMetaType to its D-Bus type signature.
static QString metaTypeToDbusSignature(int typeId) {
    switch (typeId) {
    case QMetaType::Bool:
        return QStringLiteral("b");
    case QMetaType::Int:
        return QStringLiteral("i");
    case QMetaType::UInt:
        return QStringLiteral("u");
    case QMetaType::Short:
        return QStringLiteral("n");
    case QMetaType::UShort:
        return QStringLiteral("q");
    case QMetaType::LongLong:
        return QStringLiteral("x");
    case QMetaType::ULongLong:
        return QStringLiteral("t");
    case QMetaType::Double:
        return QStringLiteral("d");
    case QMetaType::QString:
        return QStringLiteral("s");
    case QMetaType::QByteArray:
        return QStringLiteral("ay");
    case QMetaType::QStringList:
        return QStringLiteral("as");
    case QMetaType::QVariantList:
        return QStringLiteral("av");
    case QMetaType::QVariantMap:
        return QStringLiteral("a{sv}");
    case QMetaType::QVariant:
        return QStringLiteral("v");
    default:
        return QStringLiteral("v");
    }
}

QString DBusAdaptor::generateXml() const {
    const QMetaObject *meta = metaObject();
    QString xml;
    xml += QStringLiteral("  <interface name=\"%1\">\n").arg(m_iface);

    // Properties
    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        QString name = QString::fromLatin1(prop.name());
        if (isPrivateProperty(name))
            continue;
        if (name == QStringLiteral("service") || name == QStringLiteral("path") ||
            name == QStringLiteral("iface") || name == QStringLiteral("connection") ||
            name == QStringLiteral("objectName"))
            continue;
        // A QObject*-derived property has no D-Bus representation — advertising
        // it (the signature fallback would promise "v") offers a value GetAll
        // can never serve. var (QVariant-typed) properties stay advertised "v".
        if (prop.metaType().flags().testFlag(QMetaType::PointerToQObject))
            continue;

        QString dbusType = metaTypeToDbusSignature(static_cast<int>(prop.typeId()));

        QString access = prop.isWritable() ? QStringLiteral("readwrite") : QStringLiteral("read");
        xml += QStringLiteral("    <property name=\"%1\" type=\"%2\" access=\"%3\"/>\n")
                   .arg(name, dbusType, access);
    }

    // Methods — iterate over Q_INVOKABLE/Q_SLOTS methods (skip inherited Qt methods)
    for (int i = 0; i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Method && method.methodType() != QMetaMethod::Slot)
            continue;
        QString name = QString::fromLatin1(method.name());
        // Skip internal Qt methods and library mechanisms
        if (name.startsWith(QStringLiteral("qml")) || name == QStringLiteral("emitSignal") ||
            name == QStringLiteral("holdReply") || name == QStringLiteral("unregister") ||
            name == QStringLiteral("deleteLater") || name == QStringLiteral("destroyed") ||
            name == QStringLiteral("objectNameChanged"))
            continue;

        xml += QStringLiteral("    <method name=\"%1\">\n").arg(name);

        // Emit an <arg> for EVERY parameter — typed params must be advertised
        // so callers know the arity and types.
        const int inCount = method.parameterCount();
        const auto paramTypes = method.parameterTypes();
        for (int j = 0; j < inCount; ++j) {
            int typeId = QMetaType::fromName(paramTypes.at(j)).id();
            QString dbusType = metaTypeToDbusSignature(typeId);
            xml += QStringLiteral("      <arg name=\"arg%1\" type=\"%2\" direction=\"in\"/>\n")
                       .arg(j)
                       .arg(dbusType);
        }
        if (method.returnType() != QMetaType::Void) {
            QString retType = metaTypeToDbusSignature(method.returnType());
            xml += QStringLiteral("      <arg name=\"result\" type=\"%1\" direction=\"out\"/>\n")
                       .arg(retType);
        }
        xml += QStringLiteral("    </method>\n");
    }

    // Signals
    for (int i = 0; i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Signal)
            continue;
        QString name = QString::fromLatin1(method.name());
        if (name.startsWith(QStringLiteral("qml")) || name == QStringLiteral("serviceChanged") ||
            name == QStringLiteral("pathChanged") || name == QStringLiteral("ifaceChanged") ||
            name == QStringLiteral("connectionChanged") || name == QStringLiteral("destroyed") ||
            name == QStringLiteral("objectNameChanged"))
            continue;

        xml += QStringLiteral("    <signal name=\"%1\">\n").arg(name);
        const auto sigParamTypes = method.parameterTypes();
        const auto sigParamNames = method.parameterNames();
        for (int j = 0; j < method.parameterCount(); ++j) {
            int typeId = QMetaType::fromName(sigParamTypes.at(j)).id();
            QString dbusType = metaTypeToDbusSignature(typeId);
            xml += QStringLiteral("      <arg name=\"%1\" type=\"%2\"/>\n")
                       .arg(QString::fromLatin1(sigParamNames.at(j)), dbusType);
        }
        xml += QStringLiteral("    </signal>\n");
    }

    xml += QStringLiteral("  </interface>\n");
    return xml;
}

// Resolve the declared out-arg signatures for a method reply, in precedence
// order: explicit _signatures override → catalog declaration. Returns empty
// when no declaration exists (caller falls back to stable inference).
QStringList DBusAdaptor::declaredOutTypes(const QString &member) const {
    const QString qmlMember = dbusMemberToQml(member);

    // 1. Explicit override — a concatenated signature string, split per-arg.
    auto it = m_signatures.constFind(member);
    if (it == m_signatures.constEnd())
        it = m_signatures.constFind(qmlMember);
    if (it != m_signatures.constEnd()) {
        QStringList out;
        const QString sig = it.value().toString();
        int pos = 0;
        while (pos < sig.size()) {
            const QString argSig = firstCompleteType(sig, pos);
            if (argSig.isEmpty())
                break;
            out << argSig;
        }
        return out;
    }

    // 2. Catalog declaration (bundled or user XML).
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        auto mit = spec->methods.constFind(member);
        if (mit == spec->methods.constEnd())
            mit = spec->methods.constFind(qmlMember);
        if (mit != spec->methods.constEnd())
            return mit->outTypes;
    }
    return {};
}

bool DBusAdaptor::handleMessage(const QDBusMessage &msg, const QDBusConnection &) {
    QDBusConnection conn = bus();
    const QString interface = msg.interface();
    const QString member = msg.member();
    const QVariantList args = msg.arguments();
    const QMetaObject *meta = metaObject();

    // Properties interface
    if (interface == QStringLiteral("org.freedesktop.DBus.Introspectable"))
        return false;

    if (interface == QStringLiteral("org.freedesktop.DBus.Properties")) {
        // Validate the interface name if provided
        if (!args.isEmpty()) {
            QString reqIface = args[0].toString();
            if (!reqIface.isEmpty() && reqIface != m_iface) {
                conn.send(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such interface: %1").arg(reqIface)));
                return true;
            }
        }

        if (member == QStringLiteral("Get") && args.size() >= 2) {
            QString propName = args[1].toString();
            if (isPrivateProperty(propName)) {
                conn.send(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such property: %1").arg(propName)));
                return true;
            }
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                if (QString::fromLatin1(prop.name()) != propName)
                    continue;
                QVariant val = prop.read(this);
                if (val.userType() == qMetaTypeId<QJSValue>())
                    val = qjsValueToVariant(val.value<QJSValue>());
                // V-providing slot: the explicit QDBusVariant wrap below is the
                // reply's "v", so gadget values contribute their payload.
                val = toDbusVariantNested(val);
                if (!wireMarshalable(val)) {
                    qWarning("dbusqml: property %s on %s is not marshalable (type %s) — "
                             "replying InvalidArgs",
                             qPrintable(propName), qPrintable(m_iface),
                             QMetaType(val.userType()).name());
                    conn.send(msg.createErrorReply(
                        QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                        QStringLiteral("Property not marshalable: %1").arg(propName)));
                    return true;
                }
                // D-Bus spec: Get returns a variant (signature "v")
                conn.send(msg.createReply(QVariantList{QVariant::fromValue(QDBusVariant(val))}));
                return true;
            }
            // Unknown property
            conn.send(msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                           QStringLiteral("No such property: %1").arg(propName)));
            return true;
        }
        if (member == QStringLiteral("GetAll") && args.size() >= 1) {
            QVariantMap props;
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                QString name = QString::fromLatin1(prop.name());
                if (isPrivateProperty(name))
                    continue;
                if (name == QStringLiteral("service") || name == QStringLiteral("path") ||
                    name == QStringLiteral("iface") || name == QStringLiteral("connection") ||
                    name == QStringLiteral("objectName"))
                    continue;
                QVariant val = prop.read(this);
                if (val.userType() == qMetaTypeId<QJSValue>())
                    val = qjsValueToVariant(val.value<QJSValue>());
                // V-providing slot: each a{sv} map value carries its own "v",
                // so gadget values contribute their payload (single wrap).
                val = toDbusVariantNested(val);
                if (!wireMarshalable(val)) {
                    // GetAll cannot represent a per-property error — skip.
                    // Skipping also keeps introspection (busctl populates its
                    // property values via GetAll) from killing the process.
                    qWarning("dbusqml: skipping non-marshalable property reply on %s in GetAll "
                             "(type %s)",
                             qPrintable(m_iface), QMetaType(val.userType()).name());
                    continue;
                }
                props.insert(name, val);
            }
            conn.send(msg.createReply(QVariantList{props}));
            return true;
        }
        if (member == QStringLiteral("Set") && args.size() >= 3) {
            QString propName = args[1].toString();
            if (isPrivateProperty(propName)) {
                conn.send(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such property: %1").arg(propName)));
                return true;
            }
            QVariant value = unwrapDbus(args[2]);
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                if (QString::fromLatin1(prop.name()) != propName || !prop.isWritable())
                    continue;
                prop.write(this, value);
                // D-Bus spec: Set must send an empty reply
                conn.send(msg.createReply());
                return true;
            }
            // Unknown property
            conn.send(msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                           QStringLiteral("No such property: %1").arg(propName)));
            return true;
        }
        return false;
    }

    // Method dispatch
    QVariantList dbusArgs = msg.arguments();
    // Unwrap D-Bus containers so QML/JS receives traversable data
    // (arrays, dicts, structs) instead of opaque QDBusArgument objects.
    for (QVariant &a : dbusArgs)
        a = unwrapDbus(a);

    // D-Bus members are PascalCase; QML methods are camelCase (QML
    // forbids uppercase-initial names). Try exact match first (C++
    // Q_INVOKABLEs can be PascalCase), then the folded name.
    const QString qmlMember = dbusMemberToQml(member);
    QString matchedName; // the method name that matched (exact or folded)

    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Method && method.methodType() != QMetaMethod::Slot)
            continue;
        const QString methodName = QString::fromLatin1(method.name());
        // holdReply() and unregister() are library mechanisms for the
        // handler, not D-Bus methods — never dispatch to them over the wire.
        if (methodName == QStringLiteral("holdReply") || methodName == QStringLiteral("unregister"))
            continue;
        if (methodName == member) {
            matchedName = member;
        } else if (methodName == qmlMember) {
            matchedName = qmlMember;
        } else {
            continue;
        }

        if (method.parameterCount() != dbusArgs.size()) {
            continue;
        }

        QVariant retVal;
        bool invoked = false;

        // Establish the dispatch context so holdReply() works synchronously
        // inside the handler. Cleared immediately after invocation.
        m_currentCall.msg = msg;
        m_currentCall.conn = conn;
        m_currentCall.member = member;
        m_currentCall.held = false;
        m_inDispatch = true;

        QQmlEngine *engine = qmlEngine(this);
        if (engine) {
            // Ownership lifecycle (0.7.0): protecting the adaptor from the JS
            // GC is scoped to the dispatch only. Record the pre-dispatch
            // ownership, flip to CppOwnership for the call (protection against
            // the JS GC collecting the adaptor while it is wrapped as a
            // QJSValue and invoked), and restore afterwards — deferred while
            // any DBusHeldReply is outstanding (the adaptor is the only object
            // that can settle callers; restore happens at the last settle,
            // notified by DBusHeldReply::settle()). Effect matrix: declarative
            // adaptors report CppOwnership → flip+restore are no-ops (QML
            // forbids destroy() on them anyway); dynamically created
            // adaptors (Component.createObject — the portal Request pattern)
            // report JavaScriptOwnership → restored → QML destroy()/GC work
            // again after dispatch. JS-owned QObjects still referenced from JS
            // are never collected; parented ones are not collected either.
            //
            // The adaptor is wrapped as a QJSValue and invoked through
            // callWithInstance. This avoids building a JS source string
            // (which mishandles arrays/dicts and stringifies numeric args
            // without escaping) and works cleanly for multiple adaptor
            // instances sharing one engine.
            m_savedOwnership = QQmlEngine::objectOwnership(this);
            m_ownershipPendingRestore = true;
            QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
            QJSValue thisObj = engine->newQObject(this);
            QJSValue fn = thisObj.property(member);
            if (!fn.isCallable() && qmlMember != member)
                fn = thisObj.property(qmlMember);
            if (fn.isCallable()) {
                QJSValueList jsArgs;
                jsArgs.reserve(dbusArgs.size());
                for (const QVariant &arg : std::as_const(dbusArgs))
                    jsArgs << engine->toScriptValue(arg);
                QJSValue result = fn.callWithInstance(thisObj, jsArgs);
                if (!result.isError()) {
                    retVal = result.isUndefined() ? QVariant() : qjsValueToVariant(result);
                    invoked = true;
                }
            }
        }
        if (!invoked) {
            QByteArray methodName = matchedName.toLatin1();
            // C++ Q_INVOKABLEs take QVariant args (matching the Q_ARG dispatch
            // below). ANY default-constructible return type is captured via a
            // generic return argument and round-trips into the reply — an
            // uncapturable typed return used to silently send an EMPTY reply
            // (0.5.0–0.5.2 captured QVariant only).
            const QMetaType rt = method.returnMetaType();
            bool captureReturn = false;
            // QMetaMethod::invoke is the type-erased entry point (runtime
            // return types cannot use the Q_RETURN_ARG macro). For QVariant
            // returns the argument addresses retVal itself — a data()
            // -addressed return writes a nested QVariant slot and leaves
            // retVal QVariant-typed, which the reply tail rejects.
            QGenericReturnArgument retArg(nullptr, nullptr);
            if (rt.id() == QMetaType::QVariant) {
                captureReturn = true;
                retArg = QGenericReturnArgument(rt.name(), &retVal);
            } else if (rt.id() != QMetaType::UnknownType && rt.id() != QMetaType::Void &&
                       rt.isDefaultConstructible()) {
                retVal = QVariant(rt);
                retArg = QGenericReturnArgument(rt.name(), retVal.data());
                captureReturn = true;
            } else if (rt.id() != QMetaType::UnknownType && rt.id() != QMetaType::Void) {
                qWarning("dbusqml: method %s returns non-default-constructible %s — declare a "
                         "QVariant return for wire-callable C++ methods",
                         qPrintable(matchedName), rt.name());
            }
            switch (dbusArgs.size()) {
            case 0:
                if (captureReturn)
                    invoked = method.invoke(this, Qt::DirectConnection, retArg);
                else
                    invoked = method.invoke(this, Qt::DirectConnection);
                break;
            case 1:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))));
                break;
            case 2:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))));
                break;
            case 3:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))));
                break;
            case 4:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))));
                break;
            case 5:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(4))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(4))));
                break;
            default:
                qWarning("dbusqml: method %s takes %d arguments — the C++ dispatch path supports "
                         "at most 5; declare fewer parameters",
                         qPrintable(matchedName), int(dbusArgs.size()));
                maybeRestoreOwnership();
                m_inDispatch = false;
                return false;
            }
        }

        // Ownership lifecycle: the JS-GC protection is scoped to the dispatch.
        // Restore now unless held replies are still outstanding (no-op when
        // the engine path did not flip) — the last settle restores otherwise.
        maybeRestoreOwnership();

        m_inDispatch = false;

        // If the handler deferred the reply via holdReply(), the held reply
        // will settle it later — skip the synchronous tail. The handler's
        // return value (if any) is ignored in that case.
        if (m_currentCall.held) {
            if (retVal.isValid()) {
                qWarning("dbusqml: handler return value ignored when holdReply() was called");
            }
            return true;
        }

        sendMethodReply(conn, msg, member, retVal);
        return true;
    }

    return false;
}

DBusHeldReply *DBusAdaptor::holdReply() {
    if (!m_inDispatch) {
        qWarning("dbusqml: holdReply() called outside method dispatch - ignored");
        return nullptr;
    }
    auto *reply = new DBusHeldReply(this);
    reply->setContext(this, m_currentCall.msg, m_currentCall.conn, m_currentCall.member);
    // Ownership audit (0.7.0): CppOwnership while pending — the held reply is
    // the only handle that can answer the caller and must not be GC-collected
    // mid-flight. After settle() it is handed to the JS GC (0.5.0 design,
    // intended); it stays parented to this adaptor, so a parented JS-owned
    // object is never collected and it is destroyed with the adaptor.
    QQmlEngine::setObjectOwnership(reply, QQmlEngine::CppOwnership);
    m_currentCall.held = true;
    return reply;
}

void DBusAdaptor::unregister() {
    if (!m_attached) {
        qWarning("dbusqml: unregister() on an already detached adaptor (path %s) — ignored",
                 qPrintable(m_path));
        return;
    }

    // Same tail as the destructor: a deferred reply can never be answered
    // once the path is gone — error it rather than leaving the caller to
    // time out.
    const auto heldReplies = findChildren<DBusHeldReply *>();
    for (DBusHeldReply *reply : heldReplies) {
        if (!reply->isSettled()) {
            reply->sendError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                             QStringLiteral("adaptor unregistered with reply pending"));
        }
    }

    // Detach the path + service reference via the dispatcher registry
    // (idempotent tail). One-way: re-registration is not supported. The
    // QObject stays alive for QML to drop whenever.
    DBusPathDispatcher::detach(bus(), m_path, m_service, this);
    m_attached = false;
}

// Restore the pre-dispatch QML ownership unless held replies are still
// outstanding — the adaptor is the only object that can settle callers, so it
// must stay GC-protected until the last settle (notified by
// DBusHeldReply::settle() → heldReplySettled()).
void DBusAdaptor::maybeRestoreOwnership() {
    if (!m_ownershipPendingRestore)
        return;
    const auto heldReplies = findChildren<DBusHeldReply *>(Qt::FindDirectChildrenOnly);
    for (DBusHeldReply *reply : heldReplies) {
        if (!reply->isSettled())
            return;
    }
    QQmlEngine::setObjectOwnership(this, m_savedOwnership);
    m_ownershipPendingRestore = false;
}

void DBusAdaptor::heldReplySettled() {
    maybeRestoreOwnership();
}

void DBusAdaptor::sendMethodReply(const QDBusConnection &conn, const QDBusMessage &msg,
                                  const QString &member, const QVariant &retVal) {
    const QVariant value = toDbusVariant(retVal);
    if (value.isValid()) {
        // Robustness guard: an unmarshalable payload (e.g. a returned JS
        // function) would otherwise make QtDBus drop the bus connection.
        // Degrade to an error reply — one failed call, never the service.
        if (!wireMarshalable(value)) {
            qWarning("dbusqml: reply for %s is not marshalable (type %s) — sending error reply",
                     qPrintable(member), QMetaType(value.userType()).name());
            conn.send(msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                           QStringLiteral("Reply value is not marshalable")));
            return;
        }

        // Honor a declared reply signature: explicit _signatures override
        // → catalog declaration → stable inference (unchanged). Struct
        // replies now arrive here already in writable-QDBusArgument form
        // (toDbusVariant), so no top-level special case is needed.
        const QStringList outTypes = declaredOutTypes(member);
        if (outTypes.size() == 1) {
            conn.send(msg.createReply({marshalBySignature(outTypes.first(), value)}));
            return;
        }
        if (outTypes.size() > 1) {
            // Multi-out: the method returned a list of out values.
            const QVariantList values = value.toList();
            QVariantList reply;
            for (int i = 0; i < outTypes.size(); ++i)
                reply << marshalBySignature(outTypes.at(i),
                                            i < values.size() ? values.at(i) : QVariant());
            conn.send(msg.createReply(reply));
            return;
        }
        conn.send(msg.createReply({value})); // stable inference
    } else {
        conn.send(msg.createReply()); // void return — no reply args
    }
}

#include "dbusadaptor.moc"
