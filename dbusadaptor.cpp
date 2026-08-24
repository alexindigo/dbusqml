#include "dbusadaptor.h"
#include "dbusconnection.h"

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

DBusAdaptor::DBusAdaptor(QObject *parent) : QDBusVirtualObject(parent) {}

DBusAdaptor::~DBusAdaptor() {
    QDBusConnection conn = bus();
    conn.unregisterObject(m_path);
    if (!m_service.isEmpty()) {
        if (!conn.unregisterService(m_service)) {
            qmlInfo(this) << "Failed to unregister service" << m_service;
        }
    }
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
    if (!conn.registerVirtualObject(m_path, this)) {
        qmlInfo(this) << "Failed to register object at" << m_path;
        return;
    }
    if (!m_service.isEmpty()) {
        if (!conn.registerService(m_service)) {
            qmlInfo(this) << "Failed to register service" << m_service;
        }
    }

    // Auto-connect user-defined QML signals to D-Bus
    const QMetaObject *meta = metaObject();
    static const QStringList builtInSignals = {
        QStringLiteral("destroyed"),      QStringLiteral("objectNameChanged"),
        QStringLiteral("serviceChanged"), QStringLiteral("pathChanged"),
        QStringLiteral("ifaceChanged"),   QStringLiteral("connectionChanged")};

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
static QVariant qjsValueToVariant(const QJSValue &jsval) {
    QVariant v = jsval.toVariant();
    // QJSValue::toVariant() on a QML value type (gadget) may produce a
    // QVariantMap if the engine converts it via the property map rather
    // than the metatype system. Detect this by checking if the value is
    // a QVariantMap with a single "value" key — the gadget's Q_PROPERTY.
    if (v.userType() == qMetaTypeId<QVariantMap>()) {
        QVariantMap m = v.toMap();
        if (m.size() == 1 && m.contains(QStringLiteral("value"))) {
            QVariant inner = m.value(QStringLiteral("value"));
            // The inner value is the gadget's payload — wrap it as a
            // QDBusVariant (the most common case for a gadget in a
            // signal/reply context).
            if (inner.isValid())
                return QVariant::fromValue(QDBusVariant(toDbusVariant(inner)));
        }
    }
    return v;
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
        if (name == QStringLiteral("service") || name == QStringLiteral("path") ||
            name == QStringLiteral("iface") || name == QStringLiteral("connection") ||
            name == QStringLiteral("objectName"))
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
        // Skip internal Qt methods
        if (name.startsWith(QStringLiteral("qml")) || name == QStringLiteral("emitSignal") ||
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
            name == QStringLiteral("connectionChanged"))
            continue;

        xml += QStringLiteral("    <signal name=\"%1\">\n").arg(name);
        for (int j = 0; j < method.parameterCount(); ++j) {
            xml += QStringLiteral("      <arg name=\"%1\" type=\"v\"/>\n")
                       .arg(QString::fromLatin1(method.parameterNames().at(j)));
        }
        xml += QStringLiteral("    </signal>\n");
    }

    xml += QStringLiteral("  </interface>\n");
    return xml;
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
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                if (QString::fromLatin1(prop.name()) != propName)
                    continue;
                QVariant val = prop.read(this);
                if (val.userType() == qMetaTypeId<QJSValue>())
                    val = val.value<QJSValue>().toVariant();
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
                if (name == QStringLiteral("service") || name == QStringLiteral("path") ||
                    name == QStringLiteral("iface") || name == QStringLiteral("connection") ||
                    name == QStringLiteral("objectName"))
                    continue;
                QVariant val = prop.read(this);
                if (val.userType() == qMetaTypeId<QJSValue>())
                    val = val.value<QJSValue>().toVariant();
                props.insert(name, val);
            }
            conn.send(msg.createReply(QVariantList{props}));
            return true;
        }
        if (member == QStringLiteral("Set") && args.size() >= 3) {
            QString propName = args[1].toString();
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
        QQmlEngine *engine = qmlEngine(this);
        if (engine) {
            // Wrap the adaptor as a QJSValue and invoke the method through
            // callWithInstance. This avoids building a JS source string
            // (which mishandles arrays/dicts and stringifies numeric args
            // without escaping) and works cleanly for multiple adaptor
            // instances sharing one engine.
            QJSValue thisObj = engine->newQObject(this);
            QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
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
                    retVal = result.isUndefined() ? QVariant() : result.toVariant();
                    invoked = true;
                }
            }
        }
        if (!invoked) {
            QByteArray methodName = matchedName.toLatin1();
            switch (dbusArgs.size()) {
            case 0:
                invoked =
                    QMetaObject::invokeMethod(this, methodName.constData(), Qt::DirectConnection);
                break;
            case 1:
                invoked =
                    QMetaObject::invokeMethod(this, methodName.constData(), Qt::DirectConnection,
                                              Q_ARG(QVariant, dbusArgs.at(0)));
                break;
            case 2:
                invoked = QMetaObject::invokeMethod(
                    this, methodName.constData(), Qt::DirectConnection,
                    Q_ARG(QVariant, dbusArgs.at(0)), Q_ARG(QVariant, dbusArgs.at(1)));
                break;
            case 3:
                invoked = QMetaObject::invokeMethod(
                    this, methodName.constData(), Qt::DirectConnection,
                    Q_ARG(QVariant, dbusArgs.at(0)), Q_ARG(QVariant, dbusArgs.at(1)),
                    Q_ARG(QVariant, dbusArgs.at(2)));
                break;
            case 4:
                invoked = QMetaObject::invokeMethod(
                    this, methodName.constData(), Qt::DirectConnection,
                    Q_ARG(QVariant, dbusArgs.at(0)), Q_ARG(QVariant, dbusArgs.at(1)),
                    Q_ARG(QVariant, dbusArgs.at(2)), Q_ARG(QVariant, dbusArgs.at(3)));
                break;
            case 5:
                invoked = QMetaObject::invokeMethod(
                    this, methodName.constData(), Qt::DirectConnection,
                    Q_ARG(QVariant, dbusArgs.at(0)), Q_ARG(QVariant, dbusArgs.at(1)),
                    Q_ARG(QVariant, dbusArgs.at(2)), Q_ARG(QVariant, dbusArgs.at(3)),
                    Q_ARG(QVariant, dbusArgs.at(4)));
                break;
            default:
                return false;
            }
        }

        retVal = toDbusVariant(retVal);
        if (retVal.isValid())
            conn.send(msg.createReply({retVal}));
        else
            conn.send(msg.createReply()); // void return — no reply args
        return true;
    }

    return false;
}

#include "dbusadaptor.moc"
