#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDir>
#include <QEventLoop>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

#include "dbusadaptor.h"
#include "dbusconnection.h"
#include "dbus.h"

// Test adaptor with QML-exposed properties (simulates QML usage)
class TestAdaptor : public DBusAdaptor {
    Q_OBJECT
    Q_PROPERTY(int testInt READ testInt WRITE setTestInt NOTIFY testIntChanged)
    Q_PROPERTY(QString testString READ testString WRITE setTestString NOTIFY testStringChanged)

public:
    explicit TestAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}
    int testInt() const { return m_testInt; }
    void setTestInt(int v) {
        m_testInt = v;
        emit testIntChanged();
    }
    QString testString() const { return m_testString; }
    void setTestString(const QString &v) {
        m_testString = v;
        emit testStringChanged();
    }

public slots:
    int echoInt(int v) { return v; }
    QString echoString(const QString &v) { return v; }

signals:
    void testIntChanged();
    void testStringChanged();

private:
    int m_testInt = 0;
    QString m_testString;
};

// ==================== Private Bus Fixture ====================

static QProcess *s_daemon = nullptr;
static QString s_originalAddress;

static bool startPrivateBus() {
    s_daemon = new QProcess();
    s_daemon->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    s_daemon->start("dbus-daemon", {"--session", "--print-address", "--nofork"});
    if (!s_daemon->waitForStarted(3000))
        return false;
    if (!s_daemon->waitForReadyRead(3000))
        return false;
    QByteArray address = s_daemon->readLine().trimmed();
    if (address.isEmpty())
        return false;
    s_originalAddress = QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS"));
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    return true;
}

static void stopPrivateBus() {
    if (s_daemon) {
        s_daemon->terminate();
        s_daemon->waitForFinished(3000);
        delete s_daemon;
        s_daemon = nullptr;
    }
    if (s_originalAddress.isEmpty())
        qunsetenv("DBUS_SESSION_BUS_ADDRESS");
    else
        qputenv("DBUS_SESSION_BUS_ADDRESS", s_originalAddress.toLocal8Bit());
}

// ==================== Signal Catcher ====================

class SignalCatcher : public QObject {
    Q_OBJECT
public:
    QDBusMessage lastSignal;
    int count = 0;
public slots:
    void onSignal(const QDBusMessage &msg) {
        lastSignal = msg;
        ++count;
    }
};

// ==================== Test ====================

class TestDBusAdaptor : public QObject {
    Q_OBJECT

private:
    QDBusMessage callOnAdaptor(const QString &iface, const QString &member,
                               const QVariantList &args = {});

private slots:
    void initTestCase() { QVERIFY(startPrivateBus()); }
    void cleanupTestCase() { stopPrivateBus(); }

    void testGetReturnsVariant();
    void testSetCompletesAndWrites();
    void testGetAllExcludesInternal();
    void testWrongIfaceErrors();
    void testUnknownPropertyGetErrors();
    void testGenerateXmlAccurateTypes();
    void testCaseFoldedDispatch();
    void testReplyMarshalVariant();
    void testEmitSignalMarshalVariant();
};

QDBusMessage TestDBusAdaptor::callOnAdaptor(const QString &iface, const QString &member,
                                            const QVariantList &args) {
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.TestAdaptor"), QStringLiteral("/TestAdaptor"), iface, member);
    if (!args.isEmpty())
        msg.setArguments(args);
    return bus.call(msg);
}

void TestDBusAdaptor::testGetReturnsVariant() {
    TestAdaptor adaptor;
    adaptor.setTestInt(42);
    adaptor.setTestString(QStringLiteral("hello"));
    adaptor.setService(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.setPath(QStringLiteral("/TestAdaptor"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage reply =
        callOnAdaptor(QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                      {QStringLiteral("org.dbusqml.TestAdaptor"), QStringLiteral("testInt")});

    QVERIFY(!reply.arguments().isEmpty());
    QVariant val = reply.arguments().first();
    QVERIFY(val.userType() == qMetaTypeId<QDBusVariant>());
    QVariant inner = val.value<QDBusVariant>().variant();
    QCOMPARE(inner.toInt(), 42);
}

void TestDBusAdaptor::testSetCompletesAndWrites() {
    TestAdaptor adaptor;
    adaptor.setTestInt(0);
    adaptor.setService(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.setPath(QStringLiteral("/TestAdaptor"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage reply =
        callOnAdaptor(QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"),
                      {QStringLiteral("org.dbusqml.TestAdaptor"), QStringLiteral("testInt"),
                       QVariant::fromValue(QDBusVariant(99))});

    QVERIFY(reply.type() != QDBusMessage::ErrorMessage);
    QCOMPARE(adaptor.testInt(), 99);
}

void TestDBusAdaptor::testGetAllExcludesInternal() {
    TestAdaptor adaptor;
    adaptor.setTestInt(42);
    adaptor.setService(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.setPath(QStringLiteral("/TestAdaptor"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage reply =
        callOnAdaptor(QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"),
                      {QStringLiteral("org.dbusqml.TestAdaptor")});

    QVERIFY(!reply.arguments().isEmpty());
    // The QVariantMap is marshaled as a{sv} (QDBusArgument) — unwrap it
    QVariantMap props = unwrapDbus(reply.arguments().first()).toMap();
    QVERIFY(!props.contains(QStringLiteral("service")));
    QVERIFY(!props.contains(QStringLiteral("path")));
    QVERIFY(!props.contains(QStringLiteral("iface")));
    QVERIFY(!props.contains(QStringLiteral("objectName")));
    QVERIFY(props.contains(QStringLiteral("testInt")));
    QVERIFY(props.contains(QStringLiteral("testString")));
}

void TestDBusAdaptor::testWrongIfaceErrors() {
    TestAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.setPath(QStringLiteral("/TestAdaptor"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage reply =
        callOnAdaptor(QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                      {QStringLiteral("org.dbusqml.WrongIface"), QStringLiteral("testInt")});

    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));
}

void TestDBusAdaptor::testUnknownPropertyGetErrors() {
    TestAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.setPath(QStringLiteral("/TestAdaptor"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TestAdaptor"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage reply =
        callOnAdaptor(QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                      {QStringLiteral("org.dbusqml.TestAdaptor"), QStringLiteral("nonExistent")});

    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));
}

void TestDBusAdaptor::testGenerateXmlAccurateTypes() {
    TestAdaptor adaptor;
    adaptor.setIface(QStringLiteral("org.dbusqml.TestAdaptor"));
    QString xml = adaptor.introspect(QString());

    // Property types must be accurate
    QVERIFY(xml.contains(QStringLiteral("type=\"i\""))); // testInt is int
    QVERIFY(xml.contains(QStringLiteral("type=\"s\""))); // testString is QString

    // Method args must all be advertised
    QVERIFY(xml.contains(QStringLiteral("direction=\"in\"")));

    // No empty interface name
    QVERIFY(xml.contains(QStringLiteral("org.dbusqml.TestAdaptor")));
}

// D-Bus members are PascalCase (ReadOne); QML forbids uppercase-initial
// method names so the author writes readOne. The adaptor must fold the
// first character when dispatching — same convention as dbusPropToQml.
void TestDBusAdaptor::testCaseFoldedDispatch() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.CaseFoldTest'\n"
                      "  path: '/CaseFold'\n"
                      "  iface: 'org.dbusqml.CaseFoldTest'\n"
                      "  function readOne(ns, key) { return ns + '.' + key }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    // Call as PascalCase "ReadOne" — the D-Bus convention
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CaseFoldTest"), QStringLiteral("/CaseFold"),
        QStringLiteral("org.dbusqml.CaseFoldTest"), QStringLiteral("ReadOne"));
    msg.setArguments({QStringLiteral("ns"), QStringLiteral("key")});
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!reply.arguments().isEmpty());
    QCOMPARE(reply.arguments().first().toString(), QStringLiteral("ns.key"));

    delete adaptor;
}

// Method replies must run through toDbusVariant so DBus::Variant
// (QML variant() value type) marshals as a real D-Bus variant.
void TestDBusAdaptor::testReplyMarshalVariant() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.VariantReplyTest'\n"
                      "  path: '/VariantReply'\n"
                      "  iface: 'org.dbusqml.VariantReplyTest'\n"
                      "  function getValue() { return new DBusQML.variant(42) }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.VariantReplyTest"), QStringLiteral("/VariantReply"),
        QStringLiteral("org.dbusqml.VariantReplyTest"), QStringLiteral("getValue"));
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!reply.arguments().isEmpty());
    // The reply should be a variant wrapping the int
    QVariant arg = reply.arguments().first();
    QCOMPARE(arg.userType(), qMetaTypeId<QDBusVariant>());
    QCOMPARE(arg.value<QDBusVariant>().variant().toInt(), 42);

    delete adaptor;
}

// emitSignal must run arguments through toDbusVariant so variant
// payloads marshal correctly on the wire.
void TestDBusAdaptor::testEmitSignalMarshalVariant() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.SigMarshalTest'\n"
                      "  path: '/SigMarshal'\n"
                      "  iface: 'org.dbusqml.SigMarshalTest'\n"
                      "  function test() {\n"
                      "    emitSignal('SettingChanged',\n"
                      "      ['org.test', 'key', new DBusQML.variant(42)])\n"
                      "  }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    // Subscribe to the signal via a raw D-Bus match
    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.SigMarshalTest"), QStringLiteral("/SigMarshal"),
                        QStringLiteral("org.dbusqml.SigMarshalTest"),
                        QStringLiteral("SettingChanged"), &catcher, SLOT(onSignal(QDBusMessage))));

    // Trigger the signal via a method call
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.SigMarshalTest"), QStringLiteral("/SigMarshal"),
        QStringLiteral("org.dbusqml.SigMarshalTest"), QStringLiteral("test"));
    QDBusMessage reply = bus.call(call, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);

    // Wait for signal delivery
    for (int i = 0; i < 20; ++i) {
        QTest::qWait(100);
        if (catcher.count > 0)
            break;
    }
    QCOMPARE(catcher.count, 1);

    // Verify the signal args — third must be a variant (from
    // toDbusVariant unwrapping DBus::Variant to QDBusVariant).
    QCOMPARE(catcher.lastSignal.member(), QStringLiteral("SettingChanged"));
    QCOMPARE(catcher.lastSignal.arguments().size(), 3);
    QCOMPARE(catcher.lastSignal.arguments().at(0).toString(), QStringLiteral("org.test"));
    QCOMPARE(catcher.lastSignal.arguments().at(1).toString(), QStringLiteral("key"));
    QCOMPARE(catcher.lastSignal.arguments().at(2).userType(), qMetaTypeId<QDBusVariant>());
    QCOMPARE(catcher.lastSignal.arguments().at(2).value<QDBusVariant>().variant().toInt(), 42);

    delete adaptor;
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    int rc = 0;
    {
        TestDBusAdaptor tc;
        rc = QTest::qExec(&tc, argc, argv);
    }
    // Process deferred deletes so QML adaptors clean up
    for (int i = 0; i < 500; ++i) {
        app.processEvents();
        app.sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
    return rc;
}
#include "test_dbusadaptor.moc"
