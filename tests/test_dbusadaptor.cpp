#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDir>
#include <QEventLoop>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

#include "dbusadaptor.h"
#include "dbusconnection.h"
#include "dbus.h"
#include "dbustypes.h"

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

// C++ Q_INVOKABLE adaptor — exercises the declared-reply-signature hook on the
// non-QML dispatch path (invokeMethod).
class SignatureTestAdaptor : public DBusAdaptor {
    Q_OBJECT

public:
    explicit SignatureTestAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

public slots:
    QVariant readAll(const QVariant &namespaces) {
        Q_UNUSED(namespaces);
        QVariantMap inner;
        inner[QStringLiteral("color-scheme")] = QVariant::fromValue(QDBusVariant(1));
        QVariantMap outer;
        outer[QStringLiteral("org.test")] = inner;
        return outer;
    }
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
    void testStructMarshal();
    void testGenerateXmlClean();
    void testStructInReply();
    void testNestedVariantInMapReply();
    void testWireSignatureVariantReply();
    void testWireSignatureStructReply();
    void testWireSignatureNestedMapsReply();
    void testWireSignatureDeclaredOutType();
    void testWireSignatureSettingChangedSignal();
    void testWireSignatureExplicitOverride();
    void testWireSignatureOverrideShapes();
    void testWireSignatureOverrideBeatsCatalog();
    void testWireSignatureCppInvokable();
    void testWireSignatureUnproducibleWarns();
    void testStructInVariantReply();
    void testStructInMapValueReply();
    void testStructSignalArg();
    void testUnmarshalableReplySurvival();
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

// Struct value type — accent-color is (ddd), a JS array would marshal as
// av. DBus::Struct wraps a QVariantList and marshals via beginStructure.
void TestDBusAdaptor::testStructMarshal() {
    // Unit-level: verify the QDBusArgument operators produce a struct.
    DBus::Struct s({0.5, 0.3, 0.8});
    QVariant v = QVariant::fromValue(s);
    QCOMPARE(v.userType(), qMetaTypeId<DBus::Struct>());

    // toDbusVariant emits the writable QDBusArgument form — variable-member
    // structs have no fixed signature, so the raw gadget would marshal as an
    // empty struct. The QDBusArgument cross-marshals in every position.
    QVariant unwrapped = toDbusVariant(v);
    QCOMPARE(unwrapped.userType(), qMetaTypeId<QDBusArgument>());
    const QDBusArgument arg = unwrapped.value<QDBusArgument>();
    QCOMPARE(arg.currentSignature(), QStringLiteral("(ddd)"));
}

// generateXml must not leak Qt internals (destroyed, objectNameChanged)
// and must type signal args from parameterTypes, not hardcode "v".
void TestDBusAdaptor::testGenerateXmlClean() {
    TestAdaptor adaptor;
    adaptor.setIface(QStringLiteral("org.dbusqml.TestAdaptor"));
    QString xml = adaptor.introspect(QString());

    // No Qt internals in the XML
    QVERIFY(!xml.contains(QStringLiteral("destroyed")));
    QVERIFY(!xml.contains(QStringLiteral("objectNameChanged")));

    // Signal args typed from parameterTypes, not hardcoded "v".
    // TestAdaptor's signals (testIntChanged, testStringChanged) have no
    // params, so no <arg> elements. The key assertion: no signal arg is
    // typed as "v" — the hardcoded fallback is gone.
    QVERIFY(!xml.contains(QStringLiteral("type=\"v\"")));
}

// Struct in a method reply — accent-color is (ddd). The struct must
// marshal as a struct on the wire, not as an array of variants.
void TestDBusAdaptor::testStructInReply() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.StructTest'\n"
                      "  path: '/Struct'\n"
                      "  iface: 'org.dbusqml.StructTest'\n"
                      "  function getColor() {\n"
                      "    return new DBusQML.struct_([0.5, 0.3, 0.8])\n"
                      "  }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.StructTest"), QStringLiteral("/Struct"),
        QStringLiteral("org.dbusqml.StructTest"), QStringLiteral("getColor"));
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!reply.arguments().isEmpty());

    // The reply arg should be a QDBusArgument containing a struct (ddd)
    QVariant arg = reply.arguments().first();
    if (arg.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument dbusArg = arg.value<QDBusArgument>();
        QCOMPARE(dbusArg.currentSignature(), QStringLiteral("(ddd)"));
    }

    delete adaptor;
}

// Nested variant gadget inside a returned map — ReadAll shape:
// { "ns": { "key": new DBusQML.variant(1) } }. The gadget must be
// unwrapped to QDBusVariant at every nesting level.
void TestDBusAdaptor::testNestedVariantInMapReply() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.NestedVarTest'\n"
                      "  path: '/NestedVar'\n"
                      "  iface: 'org.dbusqml.NestedVarTest'\n"
                      "  function readAll(namespaces) {\n"
                      "    var result = {}\n"
                      "    result['org.test'] = {\n"
                      "      'color-scheme': new DBusQML.variant(1)\n"
                      "    }\n"
                      "    return result\n"
                      "  }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.NestedVarTest"), QStringLiteral("/NestedVar"),
        QStringLiteral("org.dbusqml.NestedVarTest"), QStringLiteral("readAll"));
    msg.setArguments({QVariant(QStringList{QStringLiteral("org.test")})});
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!reply.arguments().isEmpty());

    delete adaptor;
}

// Wire-signature assertions — the library's contract is the bytes on the
// bus, not plausible-looking CLI output. busctl displays a{sv} with
// variant-wrapped maps identically to a{sa{sv}}; xdg-desktop-portal
// rejects the former. These tests assert literal wire signatures.

// Helper: spin up a QML adaptor from inline source, invoke one method,
// return the raw reply message.
static QDBusMessage callQmlAdaptorMethod(const QString &service, const QString &path,
                                         const QString &iface, const QString &member,
                                         const QVariantList &args, const QByteArray &qmlSrc) {
    static QQmlEngine *engine = nullptr;
    if (!engine) {
        engine = new QQmlEngine;
        QDir binDir(QCoreApplication::applicationDirPath());
        engine->addImportPath(binDir.path());
        engine->addImportPath(binDir.filePath(QStringLiteral("DBus")));
    }

    QQmlComponent component(engine);
    component.setData(qmlSrc, QUrl());
    if (!component.isReady()) {
        qWarning() << "component errors:" << component.errorString();
        return QDBusMessage();
    }
    QObject *adaptor = component.create();
    if (!adaptor)
        return QDBusMessage();
    QTest::qWait(300);

    QDBusMessage msg = QDBusMessage::createMethodCall(service, path, iface, member);
    if (!args.isEmpty())
        msg.setArguments(args);
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);

    // The adaptor must outlive the reply's demarshaling but not the test;
    // leak it to the engine (test process) — cleanup at exit.
    return reply;
}

// ReadOne-shape: variant reply must be literal "v" on the wire.
void TestDBusAdaptor::testWireSignatureVariantReply() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.WireSigVar"), QStringLiteral("/WireSigVar"),
        QStringLiteral("org.dbusqml.WireSigVar"), QStringLiteral("readOne"),
        {QStringLiteral("ns"), QStringLiteral("key")},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.WireSigVar'\n"
        "  path: '/WireSigVar'\n"
        "  iface: 'org.dbusqml.WireSigVar'\n"
        "  function readOne(ns, key) { return new DBusQML.variant(1) }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("v"));
}

// Struct reply — accent-color shape — must be literal "(ddd)".
void TestDBusAdaptor::testWireSignatureStructReply() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.WireSigStruct"), QStringLiteral("/WireSigStruct"),
        QStringLiteral("org.dbusqml.WireSigStruct"), QStringLiteral("getColor"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.WireSigStruct'\n"
        "  path: '/WireSigStruct'\n"
        "  iface: 'org.dbusqml.WireSigStruct'\n"
        "  function getColor() { return new DBusQML.struct_([0.5, 0.3, 0.8]) }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("(ddd)"));
}

// ReadAll-shape: nested object literal, NO declared signature.
// Generic-library contract: inference is stable and boring — a nested
// plain-JS object marshals as a{sv} (variant-wrapped inner maps), today
// and after any fix. If a consumer needs a{sa{sv}}, the shape comes from
// the served interface's declaration, not from the data's shape.
void TestDBusAdaptor::testWireSignatureNestedMapsReply() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.WireSigNested"), QStringLiteral("/WireSigNested"),
        QStringLiteral("org.dbusqml.WireSigNested"), QStringLiteral("readAll"),
        {QVariant(QStringList{QStringLiteral("org.test")})},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.WireSigNested'\n"
        "  path: '/WireSigNested'\n"
        "  iface: 'org.dbusqml.WireSigNested'\n"
        "  function readAll(namespaces) {\n"
        "    var result = {}\n"
        "    result['org.test'] = { 'color-scheme': new DBusQML.variant(1) }\n"
        "    return result\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
}

// THE TODO gap, stated as a library contract: when the served interface
// DECLARES ReadAll → a{sa{sv}} (bundled catalog XML), the reply must carry
// that exact signature — xdg-desktop-portal rejects a{sv} outright. The
// reply path on 0.3.1 never consults declarations, so this fails until
// the fix. Serves the real org.freedesktop.impl.portal.Settings iface
// from the bundled type catalog.
void TestDBusAdaptor::testWireSignatureDeclaredOutType() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.PortalServe"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.impl.portal.Settings"), QStringLiteral("ReadAll"),
        {QVariant(QStringList{QStringLiteral("org.freedesktop.appearance")})},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.PortalServe'\n"
        "  path: '/org/freedesktop/portal/desktop'\n"
        "  iface: 'org.freedesktop.impl.portal.Settings'\n"
        "  function readAll(namespaces) {\n"
        "    var result = {}\n"
        "    result['org.freedesktop.appearance'] = { 'color-scheme': new DBusQML.variant(1) }\n"
        "    return result\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sa{sv}}"));
}

// SettingChanged must be (ssv) — xdg-desktop-portal drops (ssi).
void TestDBusAdaptor::testWireSignatureSettingChangedSignal() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.WireSigSig'\n"
                      "  path: '/WireSigSig'\n"
                      "  iface: 'org.dbusqml.WireSigSig'\n"
                      "  function fire() {\n"
                      "    emitSignal('SettingChanged',\n"
                      "      ['org.test', 'color-scheme', new DBusQML.variant(0)])\n"
                      "  }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.WireSigSig"), QStringLiteral("/WireSigSig"),
                        QStringLiteral("org.dbusqml.WireSigSig"), QStringLiteral("SettingChanged"),
                        &catcher, SLOT(onSignal(QDBusMessage))));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.WireSigSig"), QStringLiteral("/WireSigSig"),
        QStringLiteral("org.dbusqml.WireSigSig"), QStringLiteral("fire"));
    QCOMPARE(bus.call(call, QDBus::Block, 3000).type(), QDBusMessage::ReplyMessage);

    for (int i = 0; i < 20 && catcher.count == 0; ++i)
        QTest::qWait(100);
    QCOMPARE(catcher.count, 1);
    QCOMPARE(catcher.lastSignal.member(), QStringLiteral("SettingChanged"));
    QCOMPARE(catcher.lastSignal.signature(), QStringLiteral("ssv"));
}

// Explicit _signatures override on an UNCALOGED iface — the override alone
// drives the reply signature to a{sa{sv}}.
void TestDBusAdaptor::testWireSignatureExplicitOverride() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.OverrideSig"), QStringLiteral("/OverrideSig"),
        QStringLiteral("org.dbusqml.OverrideSig"), QStringLiteral("readAll"),
        {QVariant(QStringList{QStringLiteral("org.test")})},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.OverrideSig'\n"
        "  path: '/OverrideSig'\n"
        "  iface: 'org.dbusqml.OverrideSig'\n"
        "  _signatures: ({ readAll: 'a{sa{sv}}' })\n"
        "  function readAll(namespaces) {\n"
        "    var r = {}\n"
        "    r['org.test'] = { 'color-scheme': new DBusQML.variant(1) }\n"
        "    return r\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sa{sv}}"));
}

// One adaptor, several overridden shapes — each reply must carry its declared
// wire signature (ay, as, a{sv}, aa{sv}).
void TestDBusAdaptor::testWireSignatureOverrideShapes() {
    struct Shape {
        const char *member;
        const char *sig;
        const char *body;
        const char *expected;
    } shapes[] = {
        {"bytes", "ay", "function bytes() { return [65, 66] }", "ay"},
        {"strings", "as", "function strings() { return ['a', 'b'] }", "as"},
        {"dict", "a{sv}", "function dict() { return { k: 'v' } }", "a{sv}"},
        {"dicts", "aa{sv}", "function dicts() { return [{ a: 1 }] }", "aa{sv}"},
    };
    for (const auto &s : shapes) {
        const QString service = QStringLiteral("org.dbusqml.Shape.%1").arg(s.member);
        const QString path = QStringLiteral("/Shape/%1").arg(s.member);
        const QString qml = QStringLiteral("import DBus 1.0\n"
                                           "DBusAdaptor {\n"
                                           "  service: '%1'\n"
                                           "  path: '%2'\n"
                                           "  iface: '%1'\n"
                                           "  _signatures: ({ %3: '%4' })\n"
                                           "  %5\n"
                                           "}")
                                .arg(service, path, s.member, s.sig, s.body);
        QDBusMessage reply = callQmlAdaptorMethod(service, path, service,
                                                  QString::fromLatin1(s.member), {}, qml.toUtf8());
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.signature(), QString::fromLatin1(s.expected));
    }
}

// Precedence: on a cataloged iface, an explicit override beats the bundled
// declaration.
void TestDBusAdaptor::testWireSignatureOverrideBeatsCatalog() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.Precedence"), QStringLiteral("/Precedence"),
        QStringLiteral("org.freedesktop.impl.portal.Settings"), QStringLiteral("ReadAll"),
        {QVariant(QStringList{QStringLiteral("org.freedesktop.appearance")})},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.Precedence'\n"
        "  path: '/Precedence'\n"
        "  iface: 'org.freedesktop.impl.portal.Settings'\n"
        "  _signatures: ({ readAll: 'a{sv}' })\n"
        "  function readAll(namespaces) {\n"
        "    var result = {}\n"
        "    result['org.freedesktop.appearance'] = { 'color-scheme': new DBusQML.variant(1) }\n"
        "    return result\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
}

// C++ Q_INVOKABLE adaptor path — declared signature honored through the shared
// reply hook (no QQmlEngine dispatch involved).
void TestDBusAdaptor::testWireSignatureCppInvokable() {
    SignatureTestAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.CppSig"));
    adaptor.setPath(QStringLiteral("/CppSig"));
    adaptor.setIface(QStringLiteral("org.dbusqml.CppSig"));
    adaptor.setSignatures(QVariantMap{{QStringLiteral("ReadAll"), QStringLiteral("a{sa{sv}}")}});
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CppSig"), QStringLiteral("/CppSig"),
        QStringLiteral("org.dbusqml.CppSig"), QStringLiteral("ReadAll"));
    msg.setArguments({QVariant(QStringList{QStringLiteral("org.test")})});
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sa{sv}}"));
}

// A declared signature that cannot be produced must warn and fall back to
// inference, never silently emit a different wire type or crash.
void TestDBusAdaptor::testWireSignatureUnproducibleWarns() {
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral("dbusqml: cannot produce declared signature a\\(ii\\)")));
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.Unproducible"), QStringLiteral("/Unproducible"),
        QStringLiteral("org.dbusqml.Unproducible"), QStringLiteral("getPairs"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.Unproducible'\n"
        "  path: '/Unproducible'\n"
        "  iface: 'org.dbusqml.Unproducible'\n"
        "  _signatures: ({ getPairs: 'a(ii)' })\n"
        "  function getPairs() { return [[1, 2]] }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("av"));
}

// T1 — struct inside a variant (the TODO reproducer). readOne returns
// variant(struct_(…)) — the accent-color shape. Must reply "v" with a
// (ddd) payload, not an empty struct, and must not drop the connection.
void TestDBusAdaptor::testStructInVariantReply() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.StructInVar"), QStringLiteral("/StructInVar"),
        QStringLiteral("org.dbusqml.StructInVar"), QStringLiteral("readOne"),
        {QStringLiteral("org.freedesktop.appearance"), QStringLiteral("accent-color")},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.StructInVar'\n"
        "  path: '/StructInVar'\n"
        "  iface: 'org.dbusqml.StructInVar'\n"
        "  function readOne(ns, key) {\n"
        "    return new DBusQML.variant(new DBusQML.struct_([0.039, 0.518, new "
        "DBusQML.double(1.0)]))\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("v"));
    QVariant payload = unwrapDbus(reply.arguments().first());
    QCOMPARE(payload.userType(), qMetaTypeId<QVariantList>());
    QVariantList list = payload.toList();
    QCOMPARE(list.size(), 3);
    QCOMPARE(list.at(0).toDouble(), 0.039);
    QCOMPARE(list.at(1).toDouble(), 0.518);
    QCOMPARE(list.at(2).toDouble(), 1.0);
}

// T2 — struct as a map value inside a nested return. Outer wire signature
// unchanged; the struct payload survives demarshal.
void TestDBusAdaptor::testStructInMapValueReply() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.StructInMap"), QStringLiteral("/StructInMap"),
        QStringLiteral("org.dbusqml.StructInMap"), QStringLiteral("readAll"),
        {QVariant(QStringList{QStringLiteral("org.freedesktop.appearance")})},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.StructInMap'\n"
        "  path: '/StructInMap'\n"
        "  iface: 'org.dbusqml.StructInMap'\n"
        "  function readAll(namespaces) {\n"
        "    var result = {}\n"
        "    result['org.freedesktop.appearance'] = {\n"
        "      'accent-color': new DBusQML.struct_([0.039, 0.518, new DBusQML.double(1.0)])\n"
        "    }\n"
        "    return result\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
    QVariantMap outer = unwrapDbus(reply.arguments().first()).toMap();
    QVERIFY(outer.contains(QStringLiteral("org.freedesktop.appearance")));
    QVariantMap inner = outer.value(QStringLiteral("org.freedesktop.appearance")).toMap();
    QVariantList acc = inner.value(QStringLiteral("accent-color")).toList();
    QCOMPARE(acc.size(), 3);
    QCOMPARE(acc.at(0).toDouble(), 0.039);
    QCOMPARE(acc.at(1).toDouble(), 0.518);
    QCOMPARE(acc.at(2).toDouble(), 1.0);
}

// T3 — struct as a signal arg. emitSignal with a struct_ arg must hit the
// wire as literal "(ddd)".
void TestDBusAdaptor::testStructSignalArg() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.StructSig'\n"
                      "  path: '/StructSig'\n"
                      "  iface: 'org.dbusqml.StructSig'\n"
                      "  function fire() {\n"
                      "    emitSignal('ColorChanged',\n"
                      "      [new DBusQML.struct_([0.039, 0.518, new DBusQML.double(1.0)])])\n"
                      "  }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.StructSig"), QStringLiteral("/StructSig"),
                        QStringLiteral("org.dbusqml.StructSig"), QStringLiteral("ColorChanged"),
                        &catcher, SLOT(onSignal(QDBusMessage))));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.StructSig"), QStringLiteral("/StructSig"),
        QStringLiteral("org.dbusqml.StructSig"), QStringLiteral("fire"));
    QCOMPARE(bus.call(call, QDBus::Block, 3000).type(), QDBusMessage::ReplyMessage);

    for (int i = 0; i < 20 && catcher.count == 0; ++i)
        QTest::qWait(100);
    QCOMPARE(catcher.count, 1);
    QCOMPARE(catcher.lastSignal.member(), QStringLiteral("ColorChanged"));
    QCOMPARE(catcher.lastSignal.signature(), QStringLiteral("(ddd)"));

    delete adaptor;
}

// T4 — robustness: an unmarshalable return value degrades to an error reply,
// never kills the connection. A subsequent normal call still succeeds.
void TestDBusAdaptor::testUnmarshalableReplySurvival() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.UnmarshalSurvival'\n"
                      "  path: '/UnmarshalSurvival'\n"
                      "  iface: 'org.dbusqml.UnmarshalSurvival'\n"
                      "  function bad() { return function(){} }\n"
                      "  function good() { return 42 }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    QDBusConnection bus = QDBusConnection::sessionBus();

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                           "dbusqml: reply for .* is not marshalable")));
    QDBusMessage badCall = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.UnmarshalSurvival"), QStringLiteral("/UnmarshalSurvival"),
        QStringLiteral("org.dbusqml.UnmarshalSurvival"), QStringLiteral("bad"));
    QDBusMessage badReply = bus.call(badCall, QDBus::Block, 3000);
    QCOMPARE(badReply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(badReply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.Failed"));

    QVERIFY(bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.UnmarshalSurvival")));

    QDBusMessage goodCall = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.UnmarshalSurvival"), QStringLiteral("/UnmarshalSurvival"),
        QStringLiteral("org.dbusqml.UnmarshalSurvival"), QStringLiteral("good"));
    QDBusMessage goodReply = bus.call(goodCall, QDBus::Block, 3000);
    QCOMPARE(goodReply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!goodReply.arguments().isEmpty());
    QCOMPARE(goodReply.arguments().first().toInt(), 42);

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
