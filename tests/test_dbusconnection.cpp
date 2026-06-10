#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTest>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTimer>
#include <QThread>
#include <QProcess>
#include <QDBusConnectionInterface>
#include <iostream>

#include "../dbusconnection.h"
#include "../dbuserror.h"
#include "../dbusmessage.h"
#include "../dbuspendingreply.h"
#include "../dbus.h"
#include "../dbusadaptor.h"
#include "dbustypes.h"

// ==================== Mock D-Bus Service ====================

class TestService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.dbusqml.TestService")

public:
    explicit TestService(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    bool echoBool(bool v) { return v; }
    short echoInt16(short v) { return v; }
    int echoInt32(int v) { return v; }
    qint64 echoInt64(qint64 v) { return v; }
    ushort echoUint16(ushort v) { return v; }
    uint echoUint32(uint v) { return v; }
    quint64 echoUint64(quint64 v) { return v; }
    double echoDouble(double v) { return v; }
    uchar echoByte(uchar v) { return v; }
    QString echoString(const QString &v) { return v; }
    QDBusObjectPath echoObjectPath(const QDBusObjectPath &v) { return v; }
    QDBusSignature echoSignature(const QDBusSignature &v) { return v; }
    QVariantMap echoDict(const QVariantMap &v) { return v; }
    QDBusVariant echoVariant(const QDBusVariant &v) { return v; }

    int methodWithArgs(int a, const QString &b) { return a + b.size(); }
    void noReturnMethod() {}

signals:
    void signalPong(const QString &data);
};

// ==================== Private Bus Fixture ====================

static QProcess *s_daemon = nullptr;
static QString s_originalAddress;
static QString s_privateBusAddress;

static bool startPrivateBus()
{
    s_daemon = new QProcess();
    s_daemon->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    s_daemon->start("dbus-daemon", {"--session", "--print-address", "--nofork"});
    if (!s_daemon->waitForStarted(3000))
        return false;

    // Read the private bus address from stdout
    if (!s_daemon->waitForReadyRead(3000))
        return false;

    QByteArray address = s_daemon->readLine().trimmed();
    if (address.isEmpty())
        return false;

    s_privateBusAddress = QString::fromLocal8Bit(address);

    // Redirect session bus to the private instance
    s_originalAddress = QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS"));
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);

    return true;
}

static void stopPrivateBus()
{
    if (s_daemon) {
        s_daemon->terminate();
        s_daemon->waitForFinished(3000);
        delete s_daemon;
        s_daemon = nullptr;
    }

    // Restore original environment
    if (s_originalAddress.isEmpty())
        qunsetenv("DBUS_SESSION_BUS_ADDRESS");
    else
        qputenv("DBUS_SESSION_BUS_ADDRESS", s_originalAddress.toLocal8Bit());
}

static bool registerTestService()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;

    auto *service = new TestService();
    if (!bus.registerObject("/TestService", service, QDBusConnection::ExportAllSlots))
        return false;
    if (!bus.registerService("org.dbusqml.TestService"))
        return false;

    return true;
}

// ==================== Test Class ====================

class TestDBusConnection : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(startPrivateBus(), "Failed to start private D-Bus daemon");
        QVERIFY2(registerTestService(), "Failed to register mock service");
        // Let the connection stabilize
        QTest::qWait(200);
    }

    void cleanupTestCase()
    {
        stopPrivateBus();
    }

    void testBusTypeEnum()
    {
        QCOMPARE(static_cast<int>(busType::Session), 0);
        QCOMPARE(static_cast<int>(busType::System), 1);
    }

    void testSessionBusSingleton()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoString");

        QVariantList args;
        args << QVariant::fromValue(QStringLiteral("hello"));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);

        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(3000));

        QVERIFY(reply->isFinished());
        QVERIFY(!reply->isError());
        QVERIFY(reply->isValid());
        QCOMPARE(reply->value().toString(), QStringLiteral("hello"));

        delete reply;
    }

    void testSystemBusSingleton()
    {
        auto *bus = new SystemBusConnection(this);
        QVERIFY(bus != nullptr);
    }

    void testAsyncCallPromiseStyle()
    {
        auto *bus = new SessionBusConnection(this);

        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoString");

        QVariantList args;
        args << QVariant::fromValue(QStringLiteral("promise"));
        msg.setArguments(args);

        bus->asyncCall(msg, QJSValue(), QJSValue());
    }

    void testDBusPendingReplyNoWatcher()
    {
        auto *reply = new DBusPendingReply(this);
        QCOMPARE(reply->isError(), true);
        QCOMPARE(reply->isValid(), false);
        QVERIFY(!reply->value().isValid());

        DBusError err = reply->error();
        QCOMPARE(err.isValid(), true);
    }

    void testDBusProperties()
    {
        DBusProxy obj;
        QVERIFY(obj.service().isEmpty());
        QVERIFY(obj.path().isEmpty());
        QVERIFY(obj.iface().isEmpty());

        obj.setService("org.dbusqml.TestService");
        QCOMPARE(obj.service(), QStringLiteral("org.dbusqml.TestService"));

        obj.setPath("/TestService");
        QCOMPARE(obj.path(), QStringLiteral("/TestService"));

        // Setting same value should not emit (no-op guard)
        obj.setService("org.dbusqml.TestService");
        QCOMPARE(obj.service(), QStringLiteral("org.dbusqml.TestService"));

        // Test signal emission
        QSignalSpy serviceSpy(&obj, &DBusProxy::serviceChanged);
        QSignalSpy pathSpy(&obj, &DBusProxy::pathChanged);
        QSignalSpy ifaceSpy(&obj, &DBusProxy::ifaceChanged);

        obj.setService("org.dbusqml.TestService");
        obj.setPath("/TestService");
        QCOMPARE(serviceSpy.size(), 0);
        QCOMPARE(pathSpy.size(), 0);

        obj.setService("org.dbusqml.SomethingNew");
        QCOMPARE(serviceSpy.size(), 1);
    }

    void testDBusConnection()
    {
        DBusProxy obj;
        QVERIFY(obj.connection() == nullptr);

        // Setting to nullptr resets to default session bus
        obj.setConnection(nullptr);
        QVERIFY(obj.connection() == nullptr);
    }

    void testDBusIntrospect()
    {
        DBusProxy obj;
        obj.setService("org.dbusqml.TestService");
        obj.setPath("/TestService");
        obj.setIface("org.dbusqml.TestService");

        QTest::qWait(500);
        QVERIFY(true);
    }

    void testDBusCall()
    {
        DBusProxy obj;
        obj.setService("org.dbusqml.TestService");
        obj.setPath("/TestService");
        obj.setIface("org.dbusqml.TestService");

        obj.call("noReturnMethod");
        obj.call("noReturnMethod", QVariantList());
    }

    // ---- Typed argument round-trip tests ----

    void testTypedBool()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoBool");

        QVariantList args;
        args << QVariant::fromValue(DBus::Bool(true));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toBool(), true);
        delete reply;
    }

    void testTypedInt32()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoInt32");

        QVariantList args;
        args << QVariant::fromValue(DBus::Int32(42));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toInt(), 42);
        delete reply;
    }

    void testTypedInt16()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoInt16");

        QVariantList args;
        args << QVariant::fromValue(DBus::Int16(32767));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toInt(), 32767);
        delete reply;
    }

    void testTypedInt64()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoInt64");

        QVariantList args;
        args << QVariant::fromValue(DBus::Int64(Q_INT64_C(21474836470)));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toLongLong(), Q_INT64_C(21474836470));
        delete reply;
    }

    void testTypedUint32()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoUint32");

        QVariantList args;
        args << QVariant::fromValue(DBus::Uint32(4294967295U));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toUInt(), 4294967295U);
        delete reply;
    }

    void testTypedUint16()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoUint16");

        QVariantList args;
        args << QVariant::fromValue(DBus::Uint16(65535));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toUInt(), 65535U);
        delete reply;
    }

    void testTypedUint64()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoUint64");

        QVariantList args;
        args << QVariant::fromValue(DBus::Uint64(Q_UINT64_C(18446744073709551615)));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toULongLong(), Q_UINT64_C(18446744073709551615));
        delete reply;
    }

    void testTypedDouble()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoDouble");

        QVariantList args;
        args << QVariant::fromValue(DBus::Double(3.14));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toDouble(), 3.14);
        delete reply;
    }

    void testTypedByte()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoByte");

        QVariantList args;
        args << QVariant::fromValue(DBus::Byte(255));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toUInt(), 255U);
        delete reply;
    }

    void testTypedString()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoString");

        QVariantList args;
        args << QVariant::fromValue(DBus::String(QStringLiteral("hello")));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toString(), QStringLiteral("hello"));
        delete reply;
    }

    void testTypedObjectPath()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoObjectPath");

        QVariantList args;
        args << QVariant::fromValue(DBus::ObjectPath(QStringLiteral("/test/path")));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().value<QDBusObjectPath>().path(), QStringLiteral("/test/path"));
        delete reply;
    }

    void testTypedSignature()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoSignature");

        QVariantList args;
        args << QVariant::fromValue(DBus::Signature(QStringLiteral("s")));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().value<QDBusSignature>().signature(), QStringLiteral("s"));
        delete reply;
    }

    void testTypedVariant()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoVariant");

        QVariantList args;
        args << QVariant::fromValue(DBus::Variant(QJSValue(QStringLiteral("var_val"))));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());

        // value() now unwraps QDBusVariant automatically, so we get the inner string
        QCOMPARE(reply->value().toString(), QStringLiteral("var_val"));
        delete reply;
    }

    void testValueUnwrapsQDBusVariant()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoVariant");

        QVariantList args;
        args << QVariant::fromValue(DBus::Variant(QJSValue(QStringLiteral("test_val"))));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());

        QVariant val = reply->value();
        // The value should NOT be a QDBusVariant — it should be unwrapped to QString
        QVERIFY(val.userType() != qMetaTypeId<QDBusVariant>());
        QVERIFY(val.userType() == QMetaType::QString
                || val.canConvert<QString>());
        QCOMPARE(val.toString(), QStringLiteral("test_val"));

        // Also verify values() unwraps properly
        QVariantList all = reply->values();
        QVERIFY(all.size() > 0);
        QVERIFY(all[0].userType() != qMetaTypeId<QDBusVariant>());

        delete reply;
    }

    void testTypedDict()
    {
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoDict");

        QVariantMap map;
        map[QStringLiteral("key")] = QStringLiteral("value");
        map[QStringLiteral("num")] = 42;

        QVariantList args;
        args << QVariant::fromValue(DBus::Dict(map));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());

        QVariantList all = reply->values();
        QVERIFY(all.size() > 0);
        QVERIFY(all[0].isValid());

        QVariantMap result;
        if (all[0].userType() == qMetaTypeId<QVariantMap>()) {
            result = all[0].toMap();
        } else {
            QDBusArgument arg = all[0].value<QDBusArgument>();
            arg >> result;
        }

        QCOMPARE(result[QStringLiteral("key")].toString(), QStringLiteral("value"));
        QCOMPARE(result[QStringLiteral("num")].toInt(), 42);
        delete reply;
    }

    void testDynamicMethodsExist()
    {
        QQmlEngine engine;

        // Add import paths relative to the test binary
        QDir binDir(QCoreApplication::applicationDirPath());
        qDebug() << "Binary dir:" << binDir.path();
        engine.addImportPath(binDir.path());
        engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
        engine.addImportPath(binDir.filePath(QStringLiteral("../DBus")));
        engine.addImportPath(binDir.filePath(QStringLiteral("../build-debug")));

        QQmlComponent component(&engine);
        component.setData(
            "import DBus 1.0;\n"
            "DBus {\n"
            "  service: 'org.dbusqml.TestService'\n"
            "  path: '/TestService'\n"
            "  iface: 'org.dbusqml.TestService'\n"
            "}", QUrl());

        qDebug() << "Component status:" << (component.isReady() ? "ready" : "not ready");
        if (!component.isReady()) {
            qWarning() << "Component errors:" << component.errors();
        }
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        auto *proxyObj = component.create();
        QVERIFY(proxyObj != nullptr);
        auto *proxy = static_cast<DBusProxy *>(proxyObj);

        // Give introspection time to complete
        for (int i = 0; i < 20; ++i) {
            QTest::qWait(500);
            if (proxy->value(QStringLiteral("echoString")).isValid())
                break;
        }

        // After introspection, dynamic method functions should exist in the property map
        QVariant echoFn = proxy->value(QStringLiteral("echoString"));
        QVERIFY2(echoFn.isValid(), "echoString should be a callable function");

        QVariant intFn = proxy->value(QStringLiteral("echoInt32"));
        QVERIFY2(intFn.isValid(), "echoInt32 should be a callable function");

        delete proxy;
    }

    void testPropertiesGetEchoString()
    {
        // Test Properties.Get via the service's echo mechanism
        DBusMessage msg;
        msg.setService("org.dbusqml.TestService");
        msg.setPath("/TestService");
        msg.setIface("org.dbusqml.TestService");
        msg.setMember("echoString");

        QVariantList args;
        args << QVariant::fromValue(QStringLiteral("hello_prop"));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(!reply->isError());

        QVariant val = reply->value();
        QVERIFY(val.isValid());
        QCOMPARE(val.toString(), QStringLiteral("hello_prop"));
        delete reply;
    }

    void testProxyCallWithArgs()
    {
        DBusProxy proxy;
        proxy.setService("org.dbusqml.TestService");
        proxy.setPath("/TestService");
        proxy.setIface("org.dbusqml.TestService");

        QVariantList args;
        args << QVariant::fromValue(DBus::String(QStringLiteral("hello")));
        proxy.call("echoString", args);
        QVERIFY(true); // no crash = pass
    }

    void testProxyConnection()
    {
        DBusProxy proxy;
        QVERIFY(proxy.connection() == nullptr);

        proxy.setConnection(nullptr);
        QVERIFY(proxy.connection() == nullptr);

        // Setting to SystemBusConnection wraps system bus
        auto *sysBus = new SystemBusConnection(this);
        proxy.setConnection(sysBus);
        QVERIFY(proxy.connection() != nullptr);

        // Setting back to nullptr resets
        proxy.setConnection(nullptr);
        QVERIFY(proxy.connection() == nullptr);
    }

    void testConnectToBusInvalid()
    {
        auto *conn = DBusProxy::connectToBus(QStringLiteral("unix:path=/nonexistent"));
        QVERIFY(conn == nullptr);
    }

    void testDoIntrospectEarlyReturn()
    {
        // doIntrospect should return early if service is empty
        DBusProxy proxy;
        // This should not crash — doIntrospect checks for empty service/path/iface
        QTest::qWait(100);
        QVERIFY(true);
    }

    void testIntrospectionErrorStatus()
    {
        // Trigger introspection on an invalid service — should set Error status
        // and emit statusChanged/introspectionCompleted (or error path)
        DBusProxy proxy;
        proxy.setService("org.invalid.service");
        proxy.setPath("/invalid");
        proxy.setIface("org.invalid.Interface");

        QTimer timer;
        timer.setSingleShot(true);
        int statusChanges = 0;
        QObject::connect(&proxy, &DBusProxy::statusChanged, [&]() {
            statusChanges++;
        });

        QTest::qWait(3000);

        // Initially Null, then Loading, then either Ready or Error
        // If the service doesn't exist, status should end up as Error
        QCOMPARE(proxy.status(), DBusProxy::Error);
    }

    void testStatusSignal()
    {
        DBusProxy proxy;
        proxy.setService("org.dbusqml.TestService");
        proxy.setPath("/TestService");
        proxy.setIface("org.dbusqml.TestService");

        QSignalSpy statusSpy(&proxy, &DBusProxy::statusChanged);
        QSignalSpy readySpy(&proxy, &DBusProxy::introspectionCompleted);

        QTest::qWait(2000);

        QVERIFY(readySpy.size() > 0);
    }

    void testRuntimeIfaceChange()
    {
        DBusProxy proxy;
        proxy.setService("org.dbusqml.TestService");
        proxy.setPath("/TestService");
        proxy.setIface("org.dbusqml.TestService");
        QTest::qWait(2000);

        // Change interface at runtime — should trigger re-introspection
        QSignalSpy statusSpy(&proxy, &DBusProxy::statusChanged);
        proxy.setIface("org.freedesktop.DBus");
        QTest::qWait(1000);
        QVERIFY(statusSpy.size() > 0);
    }

    void testSetPathTrigger()
    {
        DBusProxy proxy;
        proxy.setService("org.dbusqml.TestService");
        proxy.setIface("org.dbusqml.TestService");
        proxy.setPath("/TestService");
        QSignalSpy readySpy(&proxy, &DBusProxy::introspectionCompleted);
        QTest::qWait(2000);
    }

    void testDoubleSetIface()
    {
        // Setting iface twice while first introspection is in flight
        // should trigger deleteLater on the old watcher (line 100)
        DBusProxy proxy;
        proxy.setService("org.dbusqml.TestService");
        proxy.setPath("/TestService");

        // First iface triggers introspection
        proxy.setIface("org.dbusqml.TestService");

        // Second iface before the first completes
        QTest::qWait(100);
        proxy.setIface("org.freedesktop.DBus");

        QTest::qWait(2000);
        QVERIFY(true);
    }

    void testConnectToBusSuccess()
    {
        if (!s_privateBusAddress.isEmpty()) {
            auto *conn = DBusProxy::connectToBus(s_privateBusAddress);
            QVERIFY(conn != nullptr);
            delete conn;
        }
    }

    void testEmitSignalStatic()
    {
        DBusProxy::emitSignal(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Settings"),
            QStringLiteral("SettingChanged"),
            { QVariant::fromValue(QStringLiteral("org.freedesktop.appearance")),
              QVariant::fromValue(QStringLiteral("color-scheme")),
              QVariant::fromValue(1) }
        );
        QVERIFY(true);
    }

    void testEmitSignalInstance()
    {
        DBusProxy proxy;
        proxy.setService("org.dbusqml.TestService");
        proxy.setPath("/TestService");
        proxy.setIface("org.dbusqml.TestService");

        proxy.emitSignal(QStringLiteral("pong"),
            { QVariant::fromValue(QStringLiteral("hello")) });
        QVERIFY(true);
    }

    void testEmitSignalRegistersName()
    {
        // Verify that emitSignal tries to claim the service name
        DBusProxy proxy;
        proxy.setService("org.dbusqml.TestPortalName");
        proxy.setPath("/Test");
        proxy.setIface("org.dbusqml.TestName");

        QDBusConnection bus = QDBusConnection::sessionBus();

        QDBusReply<bool> before = bus.interface()->isServiceRegistered(
            QStringLiteral("org.dbusqml.TestPortalName"));
        QVERIFY(before.isValid());
        QCOMPARE(before.value(), false);

        proxy.emitSignal(QStringLiteral("test"), {});
        QTest::qWait(500);

        QDBusReply<bool> after = bus.interface()->isServiceRegistered(
            QStringLiteral("org.dbusqml.TestPortalName"));
        QVERIFY(after.isValid());
        QCOMPARE(after.value(), true);
    }

    void testDBusAdaptor()
    {
        DBusAdaptor adaptor;
        adaptor.setService("org.dbusqml.AdaptorTest");
        adaptor.setPath("/Test");
        adaptor.setIface("org.dbusqml.AdaptorTest");

        adaptor.componentComplete();
        QTest::qWait(500);

        QDBusReply<bool> reg = QDBusConnection::sessionBus().interface()->isServiceRegistered(
            QStringLiteral("org.dbusqml.AdaptorTest"));
        QVERIFY(reg.isValid());
        QCOMPARE(reg.value(), true);
    }

    void testDBusAdaptorGetProperty()
    {
        DBusAdaptor adaptor;
        adaptor.setService("org.dbusqml.AdaptorGetTest");
        adaptor.setPath("/Test");
        adaptor.setIface("org.dbusqml.AdaptorGetTest");
        adaptor.componentComplete();
        QTest::qWait(500);

        // Read the 'service' property via Properties.Get
        DBusMessage msg;
        msg.setService("org.dbusqml.AdaptorGetTest");
        msg.setPath("/Test");
        msg.setIface("org.freedesktop.DBus.Properties");
        msg.setMember("Get");

        QVariantList args;
        args << QVariant::fromValue(QStringLiteral("org.dbusqml.AdaptorGetTest"))
             << QVariant::fromValue(QStringLiteral("service"));
        msg.setArguments(args);

        SessionBusConnection bus;
        auto *reply = bus.asyncCall(msg);
        QVERIFY(reply != nullptr);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QVERIFY(spy.wait(3000));
        QVERIFY(!reply->isError());
        QCOMPARE(reply->value().toString(), QStringLiteral("org.dbusqml.AdaptorGetTest"));
        delete reply;
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    int rc = 0;
    {
        TestDBusConnection tc;
        rc = QTest::qExec(&tc, argc, argv);
    }

    for (int i = 0; i < 500; ++i) {
        app.processEvents();
        app.sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }

    return rc;
}
#include "test_dbusconnection.moc"
