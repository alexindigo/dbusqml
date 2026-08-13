#include "dbus.h"
#include "dbuscatalog.h"
#include "dbusintrospection.h"
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
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>

// Map a D-Bus type signature character to the corresponding C++ QVariant type.
// Used to convert JS values to the correct D-Bus type before calling a method.
static QVariant toTypedDbusVariant(const QVariant &v, const QString &dbusType) {
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
        if (ok)
            return QVariant::fromValue(val);
    }
    if (dbusType == "i") {
        bool ok = false;
        int val = v.toInt(&ok);
        if (ok)
            return QVariant::fromValue(val);
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
        if (ok)
            return QVariant::fromValue(val);
    }
    if (dbusType == "q") {
        bool ok = false;
        ushort val = v.toUInt(&ok);
        if (ok)
            return QVariant::fromValue(val);
    }
    if (dbusType == "x") {
        bool ok = false;
        qint64 val = v.toLongLong(&ok);
        if (ok)
            return QVariant::fromValue(val);
    }
    if (dbusType == "t") {
        bool ok = false;
        quint64 val = v.toULongLong(&ok);
        if (ok)
            return QVariant::fromValue(val);
    }
    if (dbusType == "s")
        return QVariant::fromValue(v.toString());

    return toDbusVariant(v);
}

// Helper object exposed to the JS engine so evaluated functions can make D-Bus calls
class DbusMethodHelper : public QObject {
    Q_OBJECT
public:
    DbusMethodHelper(DBusProxy *proxy, const QHash<QString, QStringList> *argTypes,
                     QObject *parent = nullptr)
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

        QDBusMessage msg = QDBusMessage::createMethodCall(m_proxy->service(), m_proxy->path(),
                                                          m_proxy->iface(), method);
        if (!converted.isEmpty())
            msg.setArguments(converted);
        auto pending = bus.asyncCall(msg);
        auto watcher = new QDBusPendingCallWatcher(pending, this);
        auto reply = new DBusPendingReply(this);
        reply->setEngine(qmlEngine(m_proxy));
        reply->setWatcher(watcher);
        return reply;
    }

private:
    DBusProxy *m_proxy;
    const QHash<QString, QStringList> *m_argTypes;
};

// Convert D-Bus PascalCase property name to QML camelCase.
// Handles abbreviations: "Percentage" → percentage, "URL" → url, "XMLConfig" → xmlConfig
static QString dbusPropToQml(const QString &name) {
    if (name.isEmpty())
        return name;
    int upper = 0;
    while (upper < name.size() && name[upper].isUpper())
        ++upper;
    if (upper <= 1)
        return name.at(0).toLower() + name.mid(1);
    return name.left(upper).toLower() + name.mid(upper);
}

DBusProxy::DBusProxy(QObject *parent)
    : QQmlPropertyMap(this, parent), m_bus(QDBusConnection::sessionBus()) {}

DBusProxy::~DBusProxy() {
    disconnectSignals();
}

void DBusProxy::componentComplete() {
    m_componentComplete = true;
    if (!m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        doIntrospect();
}

void DBusProxy::setService(const QString &v) {
    if (m_service == v)
        return;
    m_service = v;
    if (m_serviceWatcher)
        m_serviceWatcher->setWatchedServices({m_service});
    emit serviceChanged();
    prepopulateFromCatalog();
    if (!m_iface.isEmpty() && !m_path.isEmpty())
        scheduleIntrospect();
}

void DBusProxy::setPath(const QString &v) {
    if (m_path == v)
        return;
    m_path = v;
    emit pathChanged();
    prepopulateFromCatalog();
    if (!m_iface.isEmpty() && !m_service.isEmpty())
        scheduleIntrospect();
}

void DBusProxy::setIface(const QString &v) {
    if (m_iface == v)
        return;
    QString oldIface = m_iface;
    m_iface = v;
    emit ifaceChanged();
    prepopulateFromCatalog();

    disconnectSignals();

    if (!m_service.isEmpty() && !m_path.isEmpty())
        scheduleIntrospect();
}

void DBusProxy::scheduleIntrospect() {
    if (!m_componentComplete) {
        // C++-created proxy — no QML lifecycle, introspect directly.
        doIntrospect();
        return;
    }
    if (m_introspectQueued)
        return;
    m_introspectQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_introspectQueued = false;
        doIntrospect();
    });
}

void DBusProxy::doIntrospect() {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    // Cancel any in-flight watcher — a newer introspection request
    // supersedes the old one.
    if (m_introspectWatcher) {
        m_introspectWatcher->disconnect(this);
        m_introspectWatcher->deleteLater();
        m_introspectWatcher = nullptr;
    }

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
                // Ignore stale watchers — a newer introspection was started
                if (w != m_introspectWatcher) {
                    w->deleteLater();
                    return;
                }
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

void DBusProxy::setSignalsEnabled(bool v) {
    if (m_signalsEnabled == v)
        return;
    m_signalsEnabled = v;
    disconnectSignals();
    if (v && !m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        scheduleIntrospect();
    emit signalsEnabledChanged();
}

void DBusProxy::setPropertiesEnabled(bool v) {
    if (m_propertiesEnabled == v)
        return;
    m_propertiesEnabled = v;
    if (v && !m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        fetchProperties();
    emit propertiesEnabledChanged();
}

void DBusProxy::setWatchServiceStatus(bool v) {
    if (m_watchServiceStatus == v)
        return;
    m_watchServiceStatus = v;

    if (v && !m_service.isEmpty()) {
        if (!m_serviceWatcher) {
            m_serviceWatcher =
                new QDBusServiceWatcher(m_service, m_bus,
                                        QDBusServiceWatcher::WatchForRegistration |
                                            QDBusServiceWatcher::WatchForUnregistration,
                                        this);
            connect(m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered, this, [this]() {
                m_serviceAvailable = true;
                emit serviceAvailableChanged();
            });
            connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
                m_serviceAvailable = false;
                emit serviceAvailableChanged();
            });
        }

        // Check initial state: call NameHasOwner on the bus daemon
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
        msg.setArguments({m_service});
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

void DBusProxy::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;

    disconnectSignals();
    m_introspectCache.clear();

    m_conn = v;
    if (v) {
        m_bus = static_cast<QDBusConnection>(*v);
    } else {
        m_bus = QDBusConnection::sessionBus();
    }
    emit connectionChanged();

    // Re-introspect on the new bus to re-establish signal subscriptions
    if (!m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        scheduleIntrospect();
}

DBusConnection *DBusProxy::connectToBus(const QString &address) {
    // Delegate to DBusConnection so both entry points share one counter
    // and can never collide on connection names.
    return DBusConnection::connectToBus(address);
}

void DBusProxy::emitSignal(const QString &name, const QVariantList &args) {
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

void DBusProxy::emitSignal(const QString &service, const QString &path, const QString &iface,
                           const QString &name, const QVariantList &args) {
    QDBusMessage msg = QDBusMessage::createSignal(path, iface, name);
    if (!args.isEmpty()) {
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i)
            converted[i] = toDbusVariant(converted[i]);
        msg.setArguments(converted);
    }
    QDBusConnection::sessionBus().send(msg);
}

DBusPendingReply *DBusProxy::call(const QString &method, const QVariantList &args) {
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
    reply->setEngine(qmlEngine(this));
    reply->setWatcher(watcher);
    return reply;
}

DBusPendingReply *DBusProxy::getProperty(const QString &name) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return nullptr;

    QDBusMessage msg =
        QDBusMessage::createMethodCall(m_service, m_path, "org.freedesktop.DBus.Properties", "Get");
    msg.setArguments({m_iface, name});

    auto pending = m_bus.asyncCall(msg);
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setEngine(qmlEngine(this));
    reply->setWatcher(watcher);
    return reply;
}

void DBusProxy::setProperty(const QString &name, const QVariant &value) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    QDBusMessage msg =
        QDBusMessage::createMethodCall(m_service, m_path, "org.freedesktop.DBus.Properties", "Set");
    msg.setArguments({m_iface, name, QVariant::fromValue(QDBusVariant(value))});
    m_bus.asyncCall(msg);
}

QVariant DBusProxy::updateValue(const QString &key, const QVariant &input) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return input;

    // Map the QML camelCase name back to the D-Bus PascalCase name.
    // Unknown keys fall back to the verbatim name (services with lowercase
    // property names exist).
    const QString dbusName = m_qmlToDbusName.value(key, key);
    QDBusMessage msg =
        QDBusMessage::createMethodCall(m_service, m_path, "org.freedesktop.DBus.Properties", "Set");
    msg.setArguments({m_iface, dbusName, QVariant::fromValue(QDBusVariant(toDbusVariant(input)))});
    m_bus.asyncCall(msg);
    return input;
}

void DBusProxy::disconnectSignals() {
    if (!m_signalsConnected)
        return;

    // Disconnect each recorded per-signal hook with the exact arguments
    // used at connect time. QtDBus disconnect requires exact-arg match.
    for (const QString &sigName : std::as_const(m_connectedSignals)) {
        m_bus.disconnect(QString(), m_connectedPath, m_connectedIface, sigName, this,
                         SLOT(onPropertiesChanged(QDBusMessage)));
    }
    m_bus.disconnect(m_connectedService, m_connectedPath, "org.freedesktop.DBus.Properties",
                     "PropertiesChanged", this, SLOT(onPropertiesChanged(QDBusMessage)));

    m_connectedSignals.clear();
    m_connectedService.clear();
    m_connectedPath.clear();
    m_connectedIface.clear();
    m_signalsConnected = false;
}

void DBusProxy::fetchProperties() {
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path,
                                                      "org.freedesktop.DBus.Properties", "GetAll");
    msg.setArguments({m_iface});

    auto pending = m_bus.asyncCall(msg);
    auto watcher = new QDBusPendingCallWatcher(pending, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QVariantMap> reply = *w;
        if (!reply.isError()) {
            QVariantMap props = reply.value();
            for (auto it = props.begin(); it != props.end(); ++it) {
                QString qmlName = dbusPropToQml(it.key());
                // Don't overwrite dynamic method callbacks with property values
                if (!m_methodArgTypes.contains(it.key()) && !m_methodArgTypes.contains(qmlName)) {
                    m_qmlToDbusName.insert(qmlName, it.key());
                    insert(qmlName, unwrapDbus(it.value()));
                }
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

void DBusProxy::onPropertiesChanged(const QDBusMessage &msg) {
    if (msg.type() == QDBusMessage::SignalMessage) {
        QVariantList unwrapped;
        for (const QVariant &arg : msg.arguments())
            unwrapped.append(unwrapDbus(arg));
        emit signalReceived(msg.member(), unwrapped);

        if (msg.member() == "PropertiesChanged" && msg.arguments().size() >= 2) {
            QVariantMap changed = qdbus_cast<QVariantMap>(msg.arguments()[1]);
            for (auto it = changed.begin(); it != changed.end(); ++it) {
                QString qmlName = dbusPropToQml(it.key());
                m_qmlToDbusName.insert(qmlName, it.key());
                insert(qmlName, unwrapDbus(it.value()));
            }
        }
    }
}

void DBusProxy::setupDynamicMethods(const QStringList &methodNames) {
    auto *engine = qmlEngine(this);

    // 1. Drop stale dynamic method keys from the property map.
    //    Without this, switching iface leaves the old iface's methods
    //    live and callable, dispatching D-Bus method-not-found errors.
    for (const QString &key : std::as_const(m_dynamicMethodKeys))
        clear(key);
    m_dynamicMethodKeys.clear();

    // 2. Release cached QJSValues held from prior introspection.
    m_cachedFunctions.clear();

    if (methodNames.isEmpty())
        return;
    if (!engine)
        return;

    // Shared factory — evaluated once per proxy, method names passed as
    // VALUES (not interpolated into JS source). Eliminates JS injection
    // via remote-provided method names.
    static const char kFactorySrc[] =
        "(function(helper, name) {"
        "  return function(...args) { return helper.callMethod(name, args); };"
        "})";
    QJSValue factory = engine->evaluate(QString::fromLatin1(kFactorySrc));
    if (factory.isError()) {
        qWarning("DBusProxy: failed to evaluate method factory: %s",
                 qPrintable(factory.toString()));
        return;
    }

    auto *helper = new DbusMethodHelper(this, &m_methodArgTypes, this);
    QJSValue helperObj = engine->newQObject(helper);

    for (const QString &name : methodNames) {
        if (name.isEmpty())
            continue;

        // Validate: D-Bus member names must be valid identifiers
        if (!name.contains(QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$")))) {
            qWarning("DBusProxy: skipping invalid method name '%s'", qPrintable(name));
            continue;
        }

        QJSValue fn = factory.call({helperObj, QJSValue(name)});
        if (fn.isError()) {
            qWarning("DBusProxy: failed to create method '%s': %s", qPrintable(name),
                     qPrintable(fn.toString()));
            continue;
        }

        m_cachedFunctions.append(fn);
        QString qmlName = dbusPropToQml(name);
        insert(qmlName, QVariant::fromValue(fn));
        m_dynamicMethodKeys.append(qmlName);
    }
}

void DBusProxy::onIntrospectionReady(const QString &xml) {
    m_methodArgTypes.clear();

    DBusIntrospectionData data = parseDBusIntrospection(xml, m_iface);
    QStringList signalNames = data.signalNames;
    QStringList methodNames = data.methodNames;
    m_methodArgTypes = data.methodArgTypes;
    QStringList propertyNames = data.propertyNames;

    // Merge with user-land catalog (interface descriptors from XDG paths and
    // bundled resources). Live introspection wins on arg types; catalog fills
    // in missing methods / signals.
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
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
            qWarning().nospace() << "DBusProxy: interface " << m_iface
                                 << " returned empty Introspect XML and no catalog entry exists. "
                                 << "Use proxy.call(\"MethodName\", args) to invoke methods, "
                                 << "or drop " << m_iface << ".xml at "
                                 << QStandardPaths::writableLocation(
                                        QStandardPaths::GenericConfigLocation)
                                 << "/dbusqml/types/";
        }
    }

    if (m_signalsEnabled) {
        for (const QString &sigName : signalNames) {
            m_bus.connect(QString(), m_path, m_iface, sigName, this,
                          SLOT(onPropertiesChanged(QDBusMessage)));
        }

        m_bus.connect(m_service, m_path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                      this, SLOT(onPropertiesChanged(QDBusMessage)));

        // Record exact connect args for exact disconnect later
        m_connectedSignals = signalNames;
        m_connectedService = m_service;
        m_connectedPath = m_path;
        m_connectedIface = m_iface;
        m_signalsConnected = true;
    }

    // Pre-populate null placeholders for every property declared in the
    // introspection XML. This makes keys exist at QML binding-evaluation
    // time, so QQmlPropertyMap's built-in reactivity can update them
    // when GetAll/PropertiesChanged arrive. Without this, properties
    // auto-created by the engine resolve to invalid QVariant (undefined)
    // and never re-evaluate when the real value is later inserted.
    for (const QString &propName : std::as_const(propertyNames)) {
        QString qmlName = dbusPropToQml(propName);
        if (!contains(qmlName))
            insert(qmlName, QVariant::fromValue(nullptr));
    }

    setupDynamicMethods(methodNames);

    if (m_propertiesEnabled) {
        fetchProperties();
    } else {
        // No property fetch — still need to signal Ready and
        // introspectionCompleted so consumers don't wait forever.
        m_status = Ready;
        emit statusChanged();
        emit introspectionCompleted();
    }
}

void DBusProxy::reloadTypes() {
    DBusCatalog::instance().reload();
}

bool DBusProxy::reactiveBindingsSupported() {
    // Reactive bindings are the only supported mode since 0.3.0 — catalog
    // and introspection pre-population is unconditional. Property kept for
    // consumer compat (they may already read it to detect the capability).
    return true;
}

bool DBusProxy::hasReactiveBindings() const {
    return reactiveBindingsSupported();
}

void DBusProxy::prepopulateFromCatalog() {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        for (const QString &propName : spec->properties) {
            QString qmlName = dbusPropToQml(propName);
            m_qmlToDbusName.insert(qmlName, propName);
            insert(qmlName, QVariant::fromValue(nullptr));
        }
    }
}

#include "dbus.moc"
