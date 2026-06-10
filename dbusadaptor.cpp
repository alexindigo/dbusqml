#include "dbusadaptor.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QRegularExpression>
#include <qqmlinfo.h>

DBusAdaptor::DBusAdaptor(QObject *parent)
    : QDBusVirtualObject(parent)
{
}

DBusAdaptor::~DBusAdaptor()
{
    QDBusConnection conn = bus();
    conn.unregisterObject(m_path);
    if (!m_service.isEmpty()) {
        if (!conn.unregisterService(m_service)) {
            qmlInfo(this) << "Failed to unregister service" << m_service;
        }
    }
}

void DBusAdaptor::setService(const QString &v)
{
    if (m_service == v) return;
    m_service = v;
    emit serviceChanged();
}

void DBusAdaptor::setPath(const QString &v)
{
    if (m_path == v) return;
    m_path = v;
    emit pathChanged();
}

void DBusAdaptor::setIface(const QString &v)
{
    if (m_iface == v) return;
    m_iface = v;
    emit ifaceChanged();
}

void DBusAdaptor::setConnection(DBusConnection *v)
{
    if (m_conn == v) return;
    m_conn = v;
    emit connectionChanged();
}

QDBusConnection DBusAdaptor::bus() const
{
    if (m_conn)
        return static_cast<QDBusConnection>(*m_conn);
    return QDBusConnection::sessionBus();
}

void DBusAdaptor::componentComplete()
{
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
}

QString DBusAdaptor::introspect(const QString &) const
{
    return generateXml();
}

void DBusAdaptor::emitSignal(const QString &name, const QJSValue &arguments)
{
    QDBusMessage msg = QDBusMessage::createSignal(m_path, m_iface, name);
    if (!arguments.isUndefined()) {
        QVariantList args;
        if (arguments.isArray()) {
            int len = arguments.property(QStringLiteral("length")).toInt();
            for (int i = 0; i < len; ++i)
                args.append(arguments.property(i).toVariant());
        } else {
            args.append(arguments.toVariant());
        }
        msg.setArguments(args);
    }
    QDBusConnection conn = bus();
    if (!conn.send(msg))
        qmlInfo(this) << conn.lastError();
}

QString DBusAdaptor::generateXml() const
{
    const QMetaObject *meta = metaObject();
    QString xml;
    xml += QStringLiteral("  <interface name=\"%1\">\n").arg(m_iface);

    // Properties
    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        QString name = QString::fromLatin1(prop.name());
        if (name == QStringLiteral("service") || name == QStringLiteral("path")
            || name == QStringLiteral("iface") || name == QStringLiteral("connection")
            || name == QStringLiteral("objectName"))
            continue;

        QString dbusType = QStringLiteral("s"); // default string
        switch (static_cast<int>(prop.typeId())) {
        case QMetaType::Bool:   dbusType = QStringLiteral("b"); break;
        case QMetaType::Int:
        case QMetaType::UInt:   dbusType = QStringLiteral("i"); break;
        case QMetaType::Double: dbusType = QStringLiteral("d"); break;
        case QMetaType::QString: dbusType = QStringLiteral("s"); break;
        case QMetaType::QVariantList: dbusType = QStringLiteral("as"); break;
        case QMetaType::QVariantMap:  dbusType = QStringLiteral("a{sv}"); break;
        default: break;
        }

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
        if (name.startsWith(QStringLiteral("qml")) || name == QStringLiteral("emitSignal")
            || name == QStringLiteral("deleteLater") || name == QStringLiteral("destroyed")
            || name == QStringLiteral("objectNameChanged"))
            continue;

        xml += QStringLiteral("    <method name=\"%1\">\n").arg(name);

        const int inCount = method.parameterCount();
        for (int j = 0; j < inCount; ++j) {
            if (method.parameterTypes().at(j) == "QVariant") {
                xml += QStringLiteral("      <arg name=\"arg%1\" type=\"v\" direction=\"in\"/>\n").arg(j);
            }
        }
        if (method.returnType() != QMetaType::Void) {
            xml += QStringLiteral("      <arg name=\"result\" type=\"v\" direction=\"out\"/>\n");
        }
        xml += QStringLiteral("    </method>\n");
    }

    // Signals
    for (int i = 0; i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Signal)
            continue;
        QString name = QString::fromLatin1(method.name());
        if (name.startsWith(QStringLiteral("qml")) || name == QStringLiteral("serviceChanged")
            || name == QStringLiteral("pathChanged") || name == QStringLiteral("ifaceChanged")
            || name == QStringLiteral("connectionChanged"))
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

bool DBusAdaptor::handleMessage(const QDBusMessage &msg, const QDBusConnection &)
{
    QDBusConnection conn = bus();
    const QString interface = msg.interface();
    const QString member = msg.member();
    const QVariantList args = msg.arguments();
    const QMetaObject *meta = metaObject();

    // Properties interface
    if (interface == QStringLiteral("org.freedesktop.DBus.Introspectable"))
        return false;

    if (interface == QStringLiteral("org.freedesktop.DBus.Properties")) {
        if (member == QStringLiteral("Get") && args.size() >= 2) {
            QString iface = args[0].toString();
            QString propName = args[1].toString();
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                if (QString::fromLatin1(prop.name()) != propName)
                    continue;
                QVariant val = prop.read(this);
                if (val.userType() == qMetaTypeId<QJSValue>())
                    val = val.value<QJSValue>().toVariant();
                conn.send(msg.createReply(QVariantList{val}));
                return true;
            }
        }
        if (member == QStringLiteral("GetAll") && args.size() >= 1) {
            QVariantMap props;
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                QString name = QString::fromLatin1(prop.name());
                if (name == QStringLiteral("service") || name == QStringLiteral("path")
                    || name == QStringLiteral("iface") || name == QStringLiteral("connection")
                    || name == QStringLiteral("objectName"))
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
            QString iface = args[0].toString();
            QString propName = args[1].toString();
            QVariant value = args[2];
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                if (QString::fromLatin1(prop.name()) != propName || !prop.isWritable())
                    continue;
                prop.write(this, value);
                return true;
            }
        }
        return false;
    }

    // Method dispatch
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Method && method.methodType() != QMetaMethod::Slot)
            continue;
        if (QString::fromLatin1(method.name()) != member)
            continue;

        QVariantList dbusArgs = msg.arguments();
        if (method.parameterCount() != dbusArgs.size())
            continue;

        QVariant retVal = method.invoke(this, dbusArgs);
        if (!retVal.isValid())
            return false;

        if (method.returnType() != QMetaType::Void && retVal.isValid()) {
            QList<QVariant> replyArgs = { retVal };
            conn.send(msg.createReply(replyArgs));
        } else {
            conn.send(msg.createReply());
        }
        return true;
    }

    return false;
}
