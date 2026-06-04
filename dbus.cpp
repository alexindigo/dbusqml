#include "dbus.h"
#include "dbuspendingreply.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QQmlEngine>
#include <QTimer>
#include <QXmlStreamReader>

// Helper object exposed to the JS engine so evaluated functions can make D-Bus calls
class DbusMethodHelper : public QObject {
    Q_OBJECT
public:
    DbusMethodHelper(DBusProxy *proxy, QObject *parent = nullptr)
        : QObject(parent), m_proxy(proxy) {}

    Q_INVOKABLE DBusPendingReply *callMethod(const QString &method, const QVariantList &args) {
        QDBusConnection bus = m_proxy->connection()
            ? static_cast<QDBusConnection>(*m_proxy->connection())
            : QDBusConnection::sessionBus();
        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_proxy->service(), m_proxy->path(), m_proxy->iface(), method);
        if (!args.isEmpty()) {
            QVariantList converted = args;
            for (int i = 0; i < converted.size(); ++i)
                converted[i] = toDbusVariant(converted[i]);
            msg.setArguments(converted);
        }
        auto pending = bus.asyncCall(msg);
        auto watcher = new QDBusPendingCallWatcher(pending, this);
        auto reply = new DBusPendingReply(this);
        reply->setWatcher(watcher);
        return reply;
    }

private:
    DBusProxy *m_proxy;
};

DBusProxy::DBusProxy(QObject *parent)
    : QQmlPropertyMap(this, parent)
    , m_bus(QDBusConnection::sessionBus())
{
}

DBusProxy::~DBusProxy()
{
    if (m_signalsConnected) {
        m_bus.disconnect(m_service, m_path, QString(), QString(),
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_bus.disconnect(m_service, m_path, m_iface, QString(),
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
    m_iface = v;
    emit ifaceChanged();

    if (m_signalsConnected) {
        m_bus.disconnect(m_service, m_path, QString(), QString(),
                         this, SLOT(onPropertiesChanged(QDBusMessage)));
        m_bus.disconnect(m_service, m_path, m_iface, QString(),
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

    m_status = Loading;
    emit statusChanged();

    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, m_path, "org.freedesktop.DBus.Introspectable", "Introspect");
    auto pending = m_bus.asyncCall(call);
    m_introspectWatcher = new QDBusPendingCallWatcher(pending, this);

    connect(m_introspectWatcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                m_introspectWatcher = nullptr;
                QDBusPendingReply<QString> reply = *w;
                if (!reply.isError())
                    onIntrospectionReady(reply.value());
                else {
                    m_status = Error;
                    emit statusChanged();
                }
                w->deleteLater();
            });
}

void DBusProxy::setConnection(DBusConnection *v)
{
    if (m_conn == v) return;
    m_conn = v;
    if (v) {
        m_bus = static_cast<QDBusConnection>(*v);
    } else {
        m_bus = QDBusConnection::sessionBus();
    }
    emit connectionChanged();
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

void DBusProxy::call(const QString &method, const QVariantList &args)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, m_iface, method);
    if (!args.isEmpty()) {
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i)
            converted[i] = toDbusVariant(converted[i]);
        msg.setArguments(converted);
    }
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
                        QString qmlName = it.key().at(0).toLower() + it.key().mid(1);
                        insert(qmlName, it.value());
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
                QString qmlName = it.key().at(0).toLower() + it.key().mid(1);
                insert(qmlName, it.value());
            }
        }
    }
}

void DBusProxy::setupDynamicMethods(const QStringList &methodNames)
{
    if (methodNames.isEmpty()) return;

    auto *engine = qmlEngine(this);
    if (!engine) return;

    auto *helper = new DbusMethodHelper(this, this);
    QJSValue helperObj = engine->newQObject(helper);
    engine->globalObject().setProperty(QStringLiteral("__dbus_helper"), helperObj);

    for (const QString &name : methodNames) {
        if (name.isEmpty()) continue;

        QString js = QStringLiteral(
            "(function() {"
            "  var methodName = '%1';"
            "  return function(...args) {"
            "    return __dbus_helper.callMethod(methodName, args);"
            "  };"
            "})()")
            .arg(name);

        QJSValue fn = engine->evaluate(js);
        if (fn.isError()) {
            qWarning("DBusProxy: failed to create method '%s': %s",
                     qPrintable(name), qPrintable(fn.toString()));
            continue;
        }

        m_cachedFunctions.append(fn);
        insert(name, QVariant::fromValue(fn));
    }
}

void DBusProxy::onIntrospectionReady(const QString &xml)
{
    QXmlStreamReader reader(xml);
    QStringList signalNames;
    QStringList methodNames;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == "interface" &&
                reader.attributes().value("name") == m_iface) {
                while (!(reader.isEndElement() && reader.name() == "interface")) {
                    reader.readNext();
                    if (reader.isStartElement()) {
                        if (reader.name() == "signal") {
                            signalNames << reader.attributes().value("name").toString();
                        } else if (reader.name() == "method") {
                            methodNames << reader.attributes().value("name").toString();
                        }
                    }
                }
                break;
            }
        }
    }

    if (reader.hasError()) {
        qWarning("DBus: introspection XML error for %s: %s",
                 qPrintable(m_iface), qPrintable(reader.errorString()));
    }

    for (const QString &sigName : signalNames) {
        m_bus.connect(m_service, m_path, m_iface, sigName,
                      this, SLOT(onPropertiesChanged(QDBusMessage)));
    }

    m_bus.connect(m_service, m_path,
                  "org.freedesktop.DBus.Properties", "PropertiesChanged",
                  this, SLOT(onPropertiesChanged(QDBusMessage)));

    m_signalsConnected = true;

    setupDynamicMethods(methodNames);

    fetchProperties();
}

#include "dbus.moc"
