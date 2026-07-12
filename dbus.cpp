#include "dbus.h"
#include "dbuscatalog.h"
#include "dbuspendingreply.h"
#include "dbustypes.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusVariant>
#include <QQmlEngine>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>

// Map a D-Bus type signature character to the corresponding C++ QVariant type.
// Used to convert JS values to the correct D-Bus type before calling a method.
static QVariant toTypedDbusVariant(const QVariant &v, const QString &dbusType)
{
    if (dbusType.isEmpty() || dbusType == "v")
        return toDbusVariant(v);

    // Complex types — build via QDBusArgument for correct marshaling
    if (dbusType == "as") {
        const auto list = v.toList();
        DBusAsArray arr;
        for (const auto &item : list)
            arr.value << item.toString();
        return QVariant::fromValue(arr);
    }
    if (dbusType == "a{sv}") {
        return v.toMap();
    }

    // Typed wrappers
    if (v.userType() == qMetaTypeId<DBus::Uint32>())
        return QVariant::fromValue(v.value<DBus::Uint32>().value);
    if (v.userType() == qMetaTypeId<DBus::Int32>())
        return QVariant::fromValue(v.value<DBus::Int32>().value);
    // ... other typed wrappers are handled by toDbusVariant fallback

    // Plain JS value — coerce to the expected D-Bus type
    if (dbusType == "u") {
        bool ok = false;
        uint val = v.toUInt(&ok);
        if (ok) return QVariant::fromValue(val);
    }
    if (dbusType == "i") {
        bool ok = false;
        int val = v.toInt(&ok);
        if (ok) return QVariant::fromValue(val);
    }
    if (dbusType == "b")
        return QVariant::fromValue(v.toBool());
    if (dbusType == "d")
        return QVariant::fromValue(v.toDouble());
    if (dbusType == "y")
        return QVariant::fromValue(v.value<uchar>());
    if (dbusType == "n") {
        bool ok = false;
        short val = v.toInt(&ok);
        if (ok) return QVariant::fromValue(val);
    }
    if (dbusType == "q") {
        bool ok = false;
        ushort val = v.toUInt(&ok);
        if (ok) return QVariant::fromValue(val);
    }
    if (dbusType == "x") {
        bool ok = false;
        qint64 val = v.toLongLong(&ok);
        if (ok) return QVariant::fromValue(val);
    }
    if (dbusType == "t") {
        bool ok = false;
        quint64 val = v.toULongLong(&ok);
        if (ok) return QVariant::fromValue(val);
    }
    if (dbusType == "s")
        return QVariant::fromValue(v.toString());

    return toDbusVariant(v);
}

// Helper object exposed to the JS engine so evaluated functions can make D-Bus calls
class DbusMethodHelper : public QObject {
    Q_OBJECT
public:
    DbusMethodHelper(DBusProxy *proxy, const QHash<QString, QStringList> *argTypes, QObject *parent = nullptr)
        : QObject(parent), m_proxy(proxy), m_argTypes(argTypes) {}

    Q_INVOKABLE DBusPendingReply *callMethod(const QString &method, const QVariantList &args) {
        QDBusConnection bus = m_proxy->connection()
            ? static_cast<QDBusConnection>(*m_proxy->connection())
            : QDBusConnection::sessionBus();

        // Convert arguments to match expected D-Bus types (basic types only)
        QVariantList converted = args;
        if (m_argTypes) {
            QStringList types = m_argTypes->value(method);
            for (int i = 0; i < converted.size() && i < types.size(); ++i)
                converted[i] = toTypedDbusVariant(converted[i], types[i]);
        }

        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_proxy->service(), m_proxy->path(), m_proxy->iface(), method);
        if (!converted.isEmpty())
            msg.setArguments(converted);
        auto pending = bus.asyncCall(msg);
        auto watcher = new QDBusPendingCallWatcher(pending, this);
        auto reply = new DBusPendingReply(this);
        reply->setWatcher(watcher);
        return reply;
    }

private:
    DBusProxy *m_proxy;
    const QHash<QString, QStringList> *m_argTypes;
};

// Convert D-Bus PascalCase property name to QML camelCase.
// Handles abbreviations: "Percentage" → percentage, "URL" → url, "XMLConfig" → xmlConfig
static QString dbusPropToQml(const QString &name)
{
    if (name.isEmpty()) return name;
    int upper = 0;
    while (upper < name.size() && name[upper].isUpper())
        ++upper;
    if (upper <= 1)
        return name.at(0).toLower() + name.mid(1);
    return name.left(upper).toLower() + name.mid(upper);
}

DBusProxy::DBusProxy(QObject *parent)
    : QQmlPropertyMap(this, parent)
    , m_bus(QDBusConnection::sessionBus())
{
}

DBusProxy::~DBusProxy()
{
    if (m_signalsConnected) {
        m_bus.disconnect(QString(), m_path, QString(), QString(),
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_bus.disconnect(m_service, m_path,
                         "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_signalsConnected = false;
    }
}

void DBusProxy::componentComplete()
{
    m_componentComplete = true;
    if (!m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        doIntrospect();
}

void DBusProxy::setService(const QString &v)
{
    if (m_service == v) return;
    m_service = v;
    emit serviceChanged();
    if (!m_iface.isEmpty() && !m_path.isEmpty())
        QTimer::singleShot(0, this, &DBusProxy::doIntrospect);
}

void DBusProxy::setPath(const QString &v)
{
    if (m_path == v) return;
    m_path = v;
    emit pathChanged();
    if (!m_iface.isEmpty() && !m_service.isEmpty())
        QTimer::singleShot(0, this, &DBusProxy::doIntrospect);
}

void DBusProxy::setIface(const QString &v)
{
    if (m_iface == v) return;
    QString oldIface = m_iface;
    m_iface = v;
    emit ifaceChanged();

    if (m_signalsConnected) {
        m_bus.disconnect(QString(), m_path, QString(), QString(),
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_bus.disconnect(m_service, m_path,
                         "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_signalsConnected = false;
    }

    if (m_introspectWatcher)
        m_introspectWatcher->deleteLater();

    if (!m_service.isEmpty() && !m_path.isEmpty())
        QTimer::singleShot(0, this, &DBusProxy::doIntrospect);
}

void DBusProxy::doIntrospect()
{
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    QString cacheKey = m_service + QLatin1Char('|') + m_path;
    auto it = m_introspectCache.find(cacheKey);
    if (it != m_introspectCache.end()) {
        onIntrospectionReady(it.value());
        return;
    }

    m_status = Loading;
    emit statusChanged();

    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, m_path, "org.freedesktop.DBus.Introspectable", "Introspect");
    auto pending = m_bus.asyncCall(call);
    m_introspectWatcher = new QDBusPendingCallWatcher(pending, this);

    connect(m_introspectWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, cacheKey](QDBusPendingCallWatcher *w) {
                m_introspectWatcher = nullptr;
                QDBusPendingReply<QString> reply = *w;
                if (!reply.isError()) {
                    m_introspectCache.insert(cacheKey, reply.value());
                    onIntrospectionReady(reply.value());
                } else {
                    m_status = Error;
                    emit statusChanged();
                }
                w->deleteLater();
            });
}

void DBusProxy::setSignalsEnabled(bool v)
{
    if (m_signalsEnabled == v) return;
    m_signalsEnabled = v;
    if (m_signalsConnected) {
        m_bus.disconnect(QString(), m_path, QString(), QString(),
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_bus.disconnect(m_service, m_path,
                         "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_signalsConnected = false;
    }
    if (v && !m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        QTimer::singleShot(0, this, &DBusProxy::doIntrospect);
    emit signalsEnabledChanged();
}

void DBusProxy::setPropertiesEnabled(bool v)
{
    if (m_propertiesEnabled == v) return;
    m_propertiesEnabled = v;
    if (v && !m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        fetchProperties();
    emit propertiesEnabledChanged();
}

void DBusProxy::setWatchServiceStatus(bool v)
{
    if (m_watchServiceStatus == v) return;
    m_watchServiceStatus = v;

    if (v && !m_service.isEmpty()) {
        if (!m_serviceWatcher) {
            m_serviceWatcher = new QDBusServiceWatcher(m_service, m_bus,
                QDBusServiceWatcher::WatchForRegistration
                | QDBusServiceWatcher::WatchForUnregistration, this);
            connect(m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered,
                    this, [this]() { m_serviceAvailable = true; emit serviceAvailableChanged(); });
            connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered,
                    this, [this]() { m_serviceAvailable = false; emit serviceAvailableChanged(); });
        }

        // Check initial state: call NameHasOwner on the bus daemon
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("NameHasOwner"));
        msg.setArguments({ m_service });
        QDBusPendingReply<bool> nameReply = m_bus.asyncCall(msg);
        auto *watcher = new QDBusPendingCallWatcher(nameReply, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this](QDBusPendingCallWatcher *w) {
                    QDBusPendingReply<bool> reply = *w;
                    if (!reply.isError()) {
                        m_serviceAvailable = reply.value();
                        emit serviceAvailableChanged();
                    }
                    w->deleteLater();
                });
    }

    if (!v && m_serviceWatcher) {
        m_serviceWatcher->deleteLater();
        m_serviceWatcher = nullptr;
    }

    emit watchServiceStatusChanged();
}

void DBusProxy::setConnection(DBusConnection *v)
{
    if (m_conn == v) return;

    if (m_signalsConnected) {
        m_bus.disconnect(QString(), m_path, QString(), QString(),
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_bus.disconnect(m_service, m_path,
                         "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_signalsConnected = false;
    }

    m_conn = v;
    if (v) {
        m_bus = static_cast<QDBusConnection>(*v);
    } else {
        m_bus = QDBusConnection::sessionBus();
    }
    emit connectionChanged();

    // Re-introspect on the new bus to re-establish signal subscriptions
    if (!m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        QTimer::singleShot(0, this, &DBusProxy::doIntrospect);
}

DBusConnection *DBusProxy::connectToBus(const QString &address)
{
    static int counter = 0;
    auto conn = QDBusConnection::connectToBus(address,
        QStringLiteral("dbusqml-custom-%1").arg(++counter));
    if (!conn.isConnected())
        return nullptr;
    return new DBusConnection(conn, QString());
}

void DBusProxy::emitSignal(const QString &name, const QVariantList &args)
{
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    // Try to claim the service name so the signal appears to come from the
    // expected service (e.g. org.freedesktop.portal.Desktop).
    // If the name is already owned (by the real portal), this silently fails.
    if (!m_service.startsWith(':'))
        m_bus.registerService(m_service);

    QDBusMessage msg = QDBusMessage::createSignal(m_path, m_iface, name);
    if (!args.isEmpty()) {
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i)
            converted[i] = toDbusVariant(converted[i]);
        msg.setArguments(converted);
    }
    m_bus.send(msg);
}

void DBusProxy::emitSignal(const QString &service, const QString &path,
                            const QString &iface, const QString &name,
                            const QVariantList &args)
{
    QDBusMessage msg = QDBusMessage::createSignal(path, iface, name);
    if (!args.isEmpty()) {
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i)
            converted[i] = toDbusVariant(converted[i]);
        msg.setArguments(converted);
    }
    QDBusConnection::sessionBus().send(msg);
}

DBusPendingReply *DBusProxy::call(const QString &method, const QVariantList &args)
{
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return nullptr;

    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, m_iface, method);
    if (!args.isEmpty()) {
        QStringList types = m_methodArgTypes.value(method);
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i) {
            QString expectedType;
            if (i < types.size())
                expectedType = types[i];
            converted[i] = toTypedDbusVariant(converted[i], expectedType);
        }
        msg.setArguments(converted);
    }
    auto pending = m_bus.asyncCall(msg);
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setWatcher(watcher);
    return reply;
}

DBusPendingReply *DBusProxy::getProperty(const QString &name)
{
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return nullptr;

    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, "org.freedesktop.DBus.Properties", "Get");
    msg.setArguments({ m_iface, name });

    auto pending = m_bus.asyncCall(msg);
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setWatcher(watcher);
    return reply;
}

void DBusProxy::setProperty(const QString &name, const QVariant &value)
{
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, "org.freedesktop.DBus.Properties", "Set");
    msg.setArguments({ m_iface, name, QVariant::fromValue(QDBusVariant(value)) });
    m_bus.asyncCall(msg);
}

QVariant DBusProxy::updateValue(const QString &key, const QVariant &input)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, "org.freedesktop.DBus.Properties", "Set");
    msg.setArguments({ m_iface, key, input });
    m_bus.asyncCall(msg);
    return input;
}

void DBusProxy::fetchProperties()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_service, m_path, "org.freedesktop.DBus.Properties", "GetAll");
    msg.setArguments({ m_iface });

    auto pending = m_bus.asyncCall(msg);
    auto watcher = new QDBusPendingCallWatcher(pending, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<QVariantMap> reply = *w;
                if (!reply.isError()) {
                    QVariantMap props = reply.value();
                    for (auto it = props.begin(); it != props.end(); ++it) {
                        QString qmlName = dbusPropToQml(it.key());
                        // Don't overwrite dynamic method callbacks with property values
                        if (!m_methodArgTypes.contains(it.key()) && !m_methodArgTypes.contains(qmlName))
                            insert(qmlName, unwrapDbus(it.value()));
                    }
                    m_status = Ready;
                } else {
                    m_status = Error;
                }
                w->deleteLater();
                emit statusChanged();
                emit introspectionCompleted();
            });
}

void DBusProxy::onPropertiesChanged(const QDBusMessage &msg)
{
    if (msg.type() == QDBusMessage::SignalMessage) {
        emit signalReceived(msg.member(), msg.arguments());

        if (msg.member() == "PropertiesChanged" && msg.arguments().size() >= 2) {
            QVariantMap changed = qdbus_cast<QVariantMap>(msg.arguments()[1]);
            for (auto it = changed.begin(); it != changed.end(); ++it) {
                QString qmlName = dbusPropToQml(it.key());
                insert(qmlName, unwrapDbus(it.value()));
            }
        }
    }
}

void DBusProxy::setupDynamicMethods(const QStringList &methodNames)
{
    if (methodNames.isEmpty()) return;

    auto *engine = qmlEngine(this);
    if (!engine) return;

    auto *helper = new DbusMethodHelper(this, &m_methodArgTypes, this);
    QJSValue helperObj = engine->newQObject(helper);

    // Use a per-proxy key so multiple instances sharing the same engine
    // don't overwrite each other's helper
    QString helperKey = QStringLiteral("__dbus_helper_%1").arg(reinterpret_cast<quintptr>(this));
    engine->globalObject().setProperty(helperKey, helperObj);

    QString jsTemplate = QStringLiteral(
        "(function() {"
        "  var $g = Function('return this')();"
        "  var helperKey = '%1';"
        "  var methodName = '%2';"
        "  return function(...args) {"
        "    return $g[helperKey].callMethod(methodName, args);"
        "  };"
        "})()");

    for (const QString &name : methodNames) {
        if (name.isEmpty()) continue;

        QJSValue fn = engine->evaluate(jsTemplate.arg(helperKey, name));
        if (fn.isError()) {
            qWarning("DBusProxy: failed to create method '%s': %s",
                     qPrintable(name), qPrintable(fn.toString()));
            continue;
        }

        m_cachedFunctions.append(fn);
        QString qmlName = dbusPropToQml(name);
        insert(qmlName, QVariant::fromValue(fn));
    }
}

void DBusProxy::onIntrospectionReady(const QString &xml)
{
    QXmlStreamReader reader(xml);
    QStringList signalNames;
    QStringList methodNames;
    m_methodArgTypes.clear();

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == "interface" &&
                reader.attributes().value("name") == m_iface) {
                QString currentMethod;
                QStringList currentArgs;
                while (!(reader.isEndElement() && reader.name() == "interface")) {
                    reader.readNext();
                    if (reader.isStartElement()) {
                        if (reader.name() == "signal") {
                            signalNames << reader.attributes().value("name").toString();
                        } else if (reader.name() == "method") {
                if (!currentMethod.isEmpty()) {
                    m_methodArgTypes.insert(currentMethod, currentArgs);
                    m_methodArgTypes.insert(dbusPropToQml(currentMethod), currentArgs);
                }
                currentMethod = reader.attributes().value("name").toString();
                currentArgs.clear();
                methodNames << currentMethod;
                        } else if (reader.name() == "arg" && !currentMethod.isEmpty()) {
                            currentArgs << reader.attributes().value("type").toString();
                        }
                    }
                }
                if (!currentMethod.isEmpty()) {
                    m_methodArgTypes.insert(currentMethod, currentArgs);
                    m_methodArgTypes.insert(dbusPropToQml(currentMethod), currentArgs);
                }
                break;
            }
        }
    }

    if (reader.hasError()) {
        qWarning("DBus: introspection XML error for %s: %s",
                 qPrintable(m_iface), qPrintable(reader.errorString()));
    }

    // Merge with user-land catalog (interface descriptors from XDG paths and
    // bundled resources). Live introspection wins on arg types; catalog fills
    // in missing methods / signals.
    if (const auto *spec = DBusCatalog::instance().lookup(m_iface)) {
        for (auto it = spec->methods.constBegin(); it != spec->methods.constEnd(); ++it) {
            const QString &methodName = it.key();
            if (!methodNames.contains(methodName)) {
                methodNames << methodName;
                m_methodArgTypes.insert(methodName, it.value().argTypes);
                m_methodArgTypes.insert(dbusPropToQml(methodName), it.value().argTypes);
            }
        }
        for (auto it = spec->signals_.constBegin(); it != spec->signals_.constEnd(); ++it) {
            if (!signalNames.contains(it.key()))
                signalNames << it.key();
        }
    } else if (methodNames.isEmpty() && signalNames.isEmpty()) {
        static QSet<QString> warned;
        if (!warned.contains(m_iface)) {
            warned.insert(m_iface);
            qWarning().nospace()
                << "DBusProxy: interface " << m_iface
                << " returned empty Introspect XML and no catalog entry exists. "
                << "Use proxy.call(\"MethodName\", args) to invoke methods, "
                << "or drop " << m_iface << ".xml at "
                << QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                << "/dbusqml/types/";
        }
    }

    if (m_signalsEnabled) {
        for (const QString &sigName : signalNames) {
            m_bus.connect(QString(), m_path, m_iface, sigName,
                          this, SLOT(onPropertiesChanged(QDBusMessage)));
        }

        m_bus.connect(m_service, m_path,
                      "org.freedesktop.DBus.Properties", "PropertiesChanged",
                      this, SLOT(onPropertiesChanged(QDBusMessage)));

        m_signalsConnected = true;
    }

    setupDynamicMethods(methodNames);

    if (m_propertiesEnabled)
        fetchProperties();
}

void DBusProxy::reloadTypes()
{
    DBusCatalog::instance().reload();
}

#include "dbus.moc"
