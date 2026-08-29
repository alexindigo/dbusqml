#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
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

#include <memory>

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

// QVariant-typed method — the C++ dispatch path (invokeMethod) passes call args
// as QVariant, so only QVariant-typed Q_INVOKABLEs/slots are wire-callable.
class VariantEchoAdaptor : public DBusAdaptor {
    Q_OBJECT

public:
    explicit VariantEchoAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

public slots:
    QVariant echo(const QVariant &v) { return v; }
};

// Typed C++ returns — the invokeMethod fallback must capture any
// default-constructible return (0.5.0–0.5.2 captured QVariant only, sending
// a silent EMPTY reply for typed returns).
class TypedReturnAdaptor : public DBusAdaptor {
    Q_OBJECT

public:
    explicit TypedReturnAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

    Q_INVOKABLE QString hi() { return QStringLiteral("hi"); }
    Q_INVOKABLE int five() { return 5; }
    Q_INVOKABLE QByteArray bytes() { return QByteArrayLiteral("xy"); }
    Q_INVOKABLE int many(int, int, int, int, int, int) { return 1; }

    Q_INVOKABLE QVariant ping() { return QStringLiteral("pong"); }
};

// P1/P2/P5 fixture: one healthy + one unmarshalable property. The QObject*
// poison has no D-Bus wire signature — before the 0.5.2 property guard this
// aborted the process inside QtDBus container writing when served.
class PoisonPropAdaptor : public DBusAdaptor {
    Q_OBJECT
    Q_PROPERTY(int good READ good CONSTANT)
    Q_PROPERTY(QObject *poison READ poison CONSTANT)

public:
    explicit PoisonPropAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}
    int good() const { return 42; }
    QObject *poison() { return &m_poison; }

    // QVariant return — the C++ invokeMethod dispatch path only captures
    // QVariant returns; a QString return would send an empty reply.
    Q_INVOKABLE QVariant ping() { return QStringLiteral("pong"); }

private:
    QObject m_poison;
};

// P4 fixture: gadget-valued properties — the conversion path. QVariant-typed
// properties holding DBus::Variant / DBus::Bytes gadgets must marshal as a
// single-wrapped "v" carrying the gadget's payload.
class GadgetPropAdaptor : public DBusAdaptor {
    Q_OBJECT
    Q_PROPERTY(QVariant varProp READ varProp CONSTANT)
    Q_PROPERTY(QVariant bytesProp READ bytesProp CONSTANT)

public:
    explicit GadgetPropAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}
    QVariant varProp() const {
        DBus::Variant v;
        v.value = QDBusVariant(42);
        return QVariant::fromValue(v);
    }
    QVariant bytesProp() const {
        return QVariant::fromValue(DBus::Bytes(QByteArrayLiteral("hello")));
    }
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

    void testDeferredReplyDeclaredMultiOut();
    void testDeferredSendError();
    void testDeferredInterleaving();
    void testDeferredTeardown();
    void testDeferredOnceOnly();
    void testHoldReplyOutsideDispatch();

    void testVariantTypedPayloadStringArray();
    void testVariantTypedPayloadBytes();
    void testVariantTypedPayloadStructEquivalence();
    void testVariantTypedPayloadUnproducibleWarns();
    void testVariantTypedPayloadNoSigUnchanged();
    void testVariantTypedPayloadFileChooserAcceptance();
    void testVariantTypedPayloadListInference();
    void testVariantInMapSingleWrap();
    void testVariantNestedVariantExplicit();
    void testVariantStructMemberKeepsVariant();

    // Multiple DBusAdaptor instances on the same path (M1–M9).
    void testCoLocatedSamePath();
    void testCoLocatedIntrospection();
    void testCoLocatedPropertiesRouting();
    void testCoLocatedTeardownPath();
    void testCoLocatedTeardownService();
    void testCoLocatedIfaceLessCall();
    void testCoLocatedSeparateBuses();
    void testCoLocatedDuplicateIface();

    // Attach guard (G1, G2).
    void testAttachGuardServiceTheft();
    void testAttachGuardRegistryHygiene();

    // Property marshal guard + conversion (P1–P5).
    void testPropertyGuardGet();
    void testPropertyGuardGetAll();
    void testPropertyGuardQmlStash();
    void testPropertyGadgetConversion();
    void testPropertyGadgetQml();
    void testPropertyGuardIntrospection();

    // {value:} heuristic removal pins (0.6.0 wire change).
    void testValueKeyDictIsRealDict();
    void testValueKeyStructListIsRealDict();

    // Typed C++ return capture (0.6.0).
    void testTypedCppReturnsRoundTrip();
    void testTypedCppOverArgCapWarns();
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

// ==================== Deferred replies ====================
//
// Deferred-reply tests need an async caller on a SEPARATE connection: a
// same-connection call takes QtDBus's synchronous "local loop", which cannot
// deliver a reply sent after handleMessage returns ("local-loop message
// cannot have delayed replies"). See spike-findings.md.

static QDBusConnection deferredCaller() {
    static std::unique_ptr<QDBusConnection> conn;
    if (!conn || !conn->isConnected()) {
        const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        conn = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(
            QString::fromLocal8Bit(addr), QStringLiteral("test-deferred-caller")));
    }
    return *conn;
}

static QDBusPendingCallWatcher *asyncCallDeferred(const QString &service, const QString &path,
                                                  const QString &iface, const QString &member,
                                                  const QVariantList &args = {}) {
    QDBusMessage msg = QDBusMessage::createMethodCall(service, path, iface, member);
    if (!args.isEmpty())
        msg.setArguments(args);
    QDBusPendingCall pending = deferredCaller().asyncCall(msg);
    return new QDBusPendingCallWatcher(pending);
}

static QObject *createQmlAdaptor(const QByteArray &qmlSrc) {
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
        return nullptr;
    }
    QObject *adaptor = component.create();
    QTest::qWait(300);
    return adaptor;
}

// T1 — deferred send against the bundled FileChooser catalog type. The held
// reply settles with [0, {uris:[...]}], which must split into the declared
// multi-out (u, a{sv}) and hit the wire as literal "ua{sv}".
void TestDBusAdaptor::testDeferredReplyDeclaredMultiOut() {
    QObject *adaptor = createQmlAdaptor(
        "import DBus 1.0\n"
        "import QtQml 2.15\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.DeferT1'\n"
        "  path: '/DeferT1'\n"
        "  iface: 'org.freedesktop.impl.portal.FileChooser'\n"
        "  property var held: null\n"
        "  function openFile(handle, appId, parentWindow, title, options) {\n"
        "    held = holdReply()\n"
        "    var t = Qt.createQmlObject('import QtQml 2.15; Timer { interval: 300; running: true; "
        "repeat: false }', this)\n"
        "    t.triggered.connect(function() { held.send([0, { uris: ['file:///tmp/x'] }]) })\n"
        "  }\n"
        "}");
    QVERIFY(adaptor != nullptr);

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.DeferT1"), QStringLiteral("/DeferT1"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy.wait(5000));

    QVERIFY(!watcher->isError());
    QDBusMessage reply = watcher->reply();
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("ua{sv}"));
    QVariantList args = reply.arguments();
    QCOMPARE(args.size(), 2);
    QCOMPARE(args.at(0).toUInt(), 0u);
    QVariantMap results = unwrapDbus(args.at(1)).toMap();
    QVERIFY(results.contains(QStringLiteral("uris")));
    QVariantList uris = unwrapDbus(results.value(QStringLiteral("uris"))).toList();
    QCOMPARE(uris.size(), 1);
    QCOMPARE(uris.at(0).toString(), QStringLiteral("file:///tmp/x"));

    delete watcher;
    delete adaptor;
}

// T2 — sendError settles the held reply with an exact D-Bus error.
void TestDBusAdaptor::testDeferredSendError() {
    QObject *adaptor = createQmlAdaptor(
        "import DBus 1.0\n"
        "import QtQml 2.15\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.DeferT2'\n"
        "  path: '/DeferT2'\n"
        "  iface: 'org.freedesktop.impl.portal.FileChooser'\n"
        "  property var held: null\n"
        "  function openFile(handle, appId, parentWindow, title, options) {\n"
        "    held = holdReply()\n"
        "    var t = Qt.createQmlObject('import QtQml 2.15; Timer { interval: 300; running: true; "
        "repeat: false }', this)\n"
        "    t.triggered.connect(function() { held.sendError('org.dbusqml.TestError', 'nope') })\n"
        "  }\n"
        "}");
    QVERIFY(adaptor != nullptr);

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.DeferT2"), QStringLiteral("/DeferT2"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy.wait(5000));

    QVERIFY(watcher->isError());
    QDBusMessage reply = watcher->reply();
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.dbusqml.TestError"));
    QCOMPARE(reply.errorMessage(), QStringLiteral("nope"));

    delete watcher;
    delete adaptor;
}

// T3 — interleaving: while a call is held, a synchronous method on the SAME
// adaptor answers immediately; the held call then settles normally.
void TestDBusAdaptor::testDeferredInterleaving() {
    QObject *adaptor =
        createQmlAdaptor("import DBus 1.0\n"
                         "import QtQml 2.15\n"
                         "DBusAdaptor {\n"
                         "  service: 'org.dbusqml.DeferT3'\n"
                         "  path: '/DeferT3'\n"
                         "  iface: 'org.freedesktop.impl.portal.FileChooser'\n"
                         "  property var held: null\n"
                         "  function openFile(handle, appId, parentWindow, title, options) {\n"
                         "    held = holdReply()\n"
                         "    var t = Qt.createQmlObject('import QtQml 2.15; Timer { interval: "
                         "500; running: true; repeat: false }', this)\n"
                         "    t.triggered.connect(function() { held.send([1, {}]) })\n"
                         "  }\n"
                         "  function ping() { return 'pong' }\n"
                         "}");
    QVERIFY(adaptor != nullptr);

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.DeferT3"), QStringLiteral("/DeferT3"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);

    // While OpenFile is held, a synchronous ping on the same adaptor answers
    // immediately.
    QDBusMessage pingCall = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.DeferT3"), QStringLiteral("/DeferT3"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("ping"));
    QDBusMessage pingReply = QDBusConnection::sessionBus().call(pingCall, QDBus::Block, 3000);
    QCOMPARE(pingReply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!pingReply.arguments().isEmpty());
    QCOMPARE(pingReply.arguments().first().toString(), QStringLiteral("pong"));

    // Then the held call settles fine.
    QVERIFY(spy.wait(5000));
    QVERIFY(!watcher->isError());
    QDBusMessage reply = watcher->reply();
    QCOMPARE(reply.signature(), QStringLiteral("ua{sv}"));
    QVariantList args = reply.arguments();
    QCOMPARE(args.size(), 2);
    QCOMPARE(args.at(0).toUInt(), 1u);

    delete watcher;
    delete adaptor;
}

// T4 — teardown: destroying the adaptor while a call is held errors out the
// caller (an error reply, not a timeout).
void TestDBusAdaptor::testDeferredTeardown() {
    QObject *adaptor =
        createQmlAdaptor("import DBus 1.0\n"
                         "DBusAdaptor {\n"
                         "  service: 'org.dbusqml.DeferT4'\n"
                         "  path: '/DeferT4'\n"
                         "  iface: 'org.freedesktop.impl.portal.FileChooser'\n"
                         "  property bool heldOpen: false\n"
                         "  function openFile(handle, appId, parentWindow, title, options) {\n"
                         "    holdReply()\n"
                         "    heldOpen = true\n"
                         "  }\n"
                         "}");
    QVERIFY(adaptor != nullptr);

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.DeferT4"), QStringLiteral("/DeferT4"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);

    QTRY_COMPARE_WITH_TIMEOUT(adaptor->property("heldOpen").toBool(), true, 3000);
    delete adaptor;

    QVERIFY(spy.wait(5000));
    QDBusMessage reply = watcher->reply();
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.Failed"));
    QCOMPARE(reply.errorMessage(), QStringLiteral("adaptor destroyed with reply pending"));

    delete watcher;
}

// T5 — once-only settle: a second send() after settle warns and sends nothing.
void TestDBusAdaptor::testDeferredOnceOnly() {
    QObject *adaptor = createQmlAdaptor(
        "import DBus 1.0\n"
        "import QtQml 2.15\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.DeferT5'\n"
        "  path: '/DeferT5'\n"
        "  iface: 'org.freedesktop.impl.portal.FileChooser'\n"
        "  property var held: null\n"
        "  function openFile(handle, appId, parentWindow, title, options) {\n"
        "    held = holdReply()\n"
        "    var t = Qt.createQmlObject('import QtQml 2.15; Timer { interval: 300; running: true; "
        "repeat: false }', this)\n"
        "    t.triggered.connect(function() { held.send([0, {}]); held.send([0, {}]) })\n"
        "  }\n"
        "}");
    QVERIFY(adaptor != nullptr);

    QTest::ignoreMessage(
        QtWarningMsg, QRegularExpression(QStringLiteral("dbusqml: DBusHeldReply already settled")));

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.DeferT5"), QStringLiteral("/DeferT5"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(watcher->reply().signature(), QStringLiteral("ua{sv}"));

    // Give any (erroneous) second reply time to arrive — there must be none.
    QTest::qWait(500);
    QCOMPARE(spy.count(), 1);

    delete watcher;
    delete adaptor;
}

// T6 — misuse: holdReply() outside dispatch warns and returns null; the
// adaptor is unharmed.
void TestDBusAdaptor::testHoldReplyOutsideDispatch() {
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral("dbusqml: holdReply.*outside")));
    QObject *adaptor =
        createQmlAdaptor("import DBus 1.0\n"
                         "import QtQml 2.15\n"
                         "DBusAdaptor {\n"
                         "  service: 'org.dbusqml.DeferT6'\n"
                         "  path: '/DeferT6'\n"
                         "  iface: 'org.dbusqml.DeferT6'\n"
                         "  property bool grabbedNull: false\n"
                         "  Component.onCompleted: { grabbedNull = (holdReply() === null) }\n"
                         "}");
    QVERIFY(adaptor != nullptr);
    QCOMPARE(adaptor->property("grabbedNull").toBool(), true);
    delete adaptor;
}

// ==================== Typed variant payloads ====================
//
// DBus.variant(value, signature) must drive the variant payload's wire
// signature. These tests assert the payload TYPE and wire signature, not
// eyeballed output.

// Extract one key's variant payload from a top-level a{sv} reply argument,
// demarshaled as a raw QVariant (NOT flattened through unwrapDbus, which
// would collapse QStringList "as" into QVariantList).
static QVariant mapValueRawArg(const QVariant &arg, const QString &key) {
    QVariant val;
    if (arg.userType() == qMetaTypeId<QVariantMap>()) {
        val = arg.toMap().value(key);
    } else {
        const QDBusArgument map = arg.value<QDBusArgument>();
        map.beginMap();
        while (!map.atEnd()) {
            map.beginMapEntry();
            QString k;
            map >> k;
            QDBusVariant v;
            map >> v;
            if (k == key)
                val = v.variant();
            map.endMapEntry();
        }
        map.endMap();
    }
    return val;
}

static QVariant mapValueRaw(const QDBusMessage &reply, const QString &key) {
    return mapValueRawArg(reply.arguments().first(), key);
}

// Map a demarshaled payload back to its D-Bus signature, for wire-literal
// assertions. QStringList -> as, QVariantList -> av, QByteArray -> ay, and a
// QDBusArgument (structs/other) carries its own currentSignature().
static QString payloadSignature(const QVariant &payload) {
    if (payload.userType() == qMetaTypeId<QStringList>())
        return QStringLiteral("as");
    if (payload.userType() == qMetaTypeId<QVariantList>())
        return QStringLiteral("av");
    if (payload.userType() == qMetaTypeId<QByteArray>())
        return QStringLiteral("ay");
    if (payload.userType() == qMetaTypeId<QDBusArgument>())
        return payload.value<QDBusArgument>().currentSignature();
    return QString::fromLatin1(payload.typeName());
}

// V1 — the FileChooser uris case verbatim: a{sv} value variant(paths,"as")
// must demarshal as a real string list with wire payload signature "as".
void TestDBusAdaptor::testVariantTypedPayloadStringArray() {
    QDBusMessage reply =
        callQmlAdaptorMethod(QStringLiteral("org.dbusqml.VTypedAs"), QStringLiteral("/VTypedAs"),
                             QStringLiteral("org.dbusqml.VTypedAs"), QStringLiteral("readAll"), {},
                             "import DBus 1.0\n"
                             "import DBus 1.0 as DBusQML\n"
                             "DBusAdaptor {\n"
                             "  service: 'org.dbusqml.VTypedAs'\n"
                             "  path: '/VTypedAs'\n"
                             "  iface: 'org.dbusqml.VTypedAs'\n"
                             "  function readAll() {\n"
                             "    return { uris: new DBusQML.variant(['file:///tmp/x'], 'as') }\n"
                             "  }\n"
                             "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
    const QVariant payload = mapValueRaw(reply, QStringLiteral("uris"));
    QCOMPARE(payloadSignature(payload), QStringLiteral("as"));
    QCOMPARE(payload.userType(), qMetaTypeId<QStringList>());
    QCOMPARE(payload.toStringList(), QStringList{QStringLiteral("file:///tmp/x")});
}

// V2 — variant(value, "ay") produces a byte-array payload.
void TestDBusAdaptor::testVariantTypedPayloadBytes() {
    QDBusMessage reply =
        callQmlAdaptorMethod(QStringLiteral("org.dbusqml.VTypedAy"), QStringLiteral("/VTypedAy"),
                             QStringLiteral("org.dbusqml.VTypedAy"), QStringLiteral("readAll"), {},
                             "import DBus 1.0\n"
                             "import DBus 1.0 as DBusQML\n"
                             "DBusAdaptor {\n"
                             "  service: 'org.dbusqml.VTypedAy'\n"
                             "  path: '/VTypedAy'\n"
                             "  iface: 'org.dbusqml.VTypedAy'\n"
                             "  function readAll() {\n"
                             "    return { data: new DBusQML.variant('hello', 'ay') }\n"
                             "  }\n"
                             "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
    const QVariant payload = mapValueRaw(reply, QStringLiteral("data"));
    QCOMPARE(payloadSignature(payload), QStringLiteral("ay"));
    QCOMPARE(payload.userType(), qMetaTypeId<QByteArray>());
    QCOMPARE(payload.toByteArray(), QByteArray("hello"));
}

// V3 — variant([0.1,0.2,0.3], "(ddd)") is wire-equivalent to
// variant(struct_([0.1,0.2,0.3])): both v((ddd)), same payload.
void TestDBusAdaptor::testVariantTypedPayloadStructEquivalence() {
    QDBusMessage replyA = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.VTypedStructA"), QStringLiteral("/VTypedStructA"),
        QStringLiteral("org.dbusqml.VTypedStructA"), QStringLiteral("get"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.VTypedStructA'\n"
        "  path: '/VTypedStructA'\n"
        "  iface: 'org.dbusqml.VTypedStructA'\n"
        "  function get() {\n"
        "    return new DBusQML.variant([0.1, 0.2, 0.3], '(ddd)')\n"
        "  }\n"
        "}");
    QDBusMessage replyB = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.VTypedStructB"), QStringLiteral("/VTypedStructB"),
        QStringLiteral("org.dbusqml.VTypedStructB"), QStringLiteral("get"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.VTypedStructB'\n"
        "  path: '/VTypedStructB'\n"
        "  iface: 'org.dbusqml.VTypedStructB'\n"
        "  function get() {\n"
        "    return new DBusQML.variant(new DBusQML.struct_([0.1, 0.2, 0.3]))\n"
        "  }\n"
        "}");
    QCOMPARE(replyA.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(replyB.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(replyA.signature(), QStringLiteral("v"));
    QCOMPARE(replyB.signature(), QStringLiteral("v"));

    QVariant pa = replyA.arguments().first().value<QDBusVariant>().variant();
    QVariant pb = replyB.arguments().first().value<QDBusVariant>().variant();
    QCOMPARE(payloadSignature(pa), QStringLiteral("(ddd)"));
    QCOMPARE(payloadSignature(pb), QStringLiteral("(ddd)"));
    QCOMPARE(unwrapDbus(pa).toList(), unwrapDbus(pb).toList());
}

// V4 — variant(x, "a(ii)") is an unproducible boundary: warn + fall back to
// inference, never a silent wrong type or a dropped connection.
void TestDBusAdaptor::testVariantTypedPayloadUnproducibleWarns() {
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral("dbusqml: cannot produce declared signature a\\(ii\\)")));
    QDBusMessage reply =
        callQmlAdaptorMethod(QStringLiteral("org.dbusqml.VTypedBad"), QStringLiteral("/VTypedBad"),
                             QStringLiteral("org.dbusqml.VTypedBad"), QStringLiteral("get"), {},
                             "import DBus 1.0\n"
                             "import DBus 1.0 as DBusQML\n"
                             "DBusAdaptor {\n"
                             "  service: 'org.dbusqml.VTypedBad'\n"
                             "  path: '/VTypedBad'\n"
                             "  iface: 'org.dbusqml.VTypedBad'\n"
                             "  function get() {\n"
                             "    return new DBusQML.variant([1, 2], 'a(ii)')\n"
                             "  }\n"
                             "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("v"));
    const QVariant payload = reply.arguments().first().value<QDBusVariant>().variant();
    QCOMPARE(payloadSignature(payload), QStringLiteral("av"));
}

// V5 — no-signature variant(value) is unchanged: payload still inferred (av).
void TestDBusAdaptor::testVariantTypedPayloadNoSigUnchanged() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.VTypedNoSig"), QStringLiteral("/VTypedNoSig"),
        QStringLiteral("org.dbusqml.VTypedNoSig"), QStringLiteral("readAll"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.VTypedNoSig'\n"
        "  path: '/VTypedNoSig'\n"
        "  iface: 'org.dbusqml.VTypedNoSig'\n"
        "  function readAll() {\n"
        "    return { uris: new DBusQML.variant(['file:///tmp/x']) }\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
    const QVariant payload = mapValueRaw(reply, QStringLiteral("uris"));
    QCOMPARE(payloadSignature(payload), QStringLiteral("av"));
}

// V6 — the FileChooser acceptance example verbatim: multi-out reply
// [0, { uris: variant(paths,"as") }] against the bundled FileChooser catalog
// must be literal ua{sv} with a uris payload of wire signature "as".
void TestDBusAdaptor::testVariantTypedPayloadFileChooserAcceptance() {
    QObject *adaptor =
        createQmlAdaptor("import DBus 1.0\n"
                         "import DBus 1.0 as DBusQML\n"
                         "import QtQml 2.15\n"
                         "DBusAdaptor {\n"
                         "  service: 'org.dbusqml.VTypedFC'\n"
                         "  path: '/VTypedFC'\n"
                         "  iface: 'org.freedesktop.impl.portal.FileChooser'\n"
                         "  property var held: null\n"
                         "  function openFile(handle, appId, parentWindow, title, options) {\n"
                         "    held = holdReply()\n"
                         "    var t = Qt.createQmlObject('import QtQml 2.15; Timer { interval: "
                         "300; running: true; repeat: false }', this)\n"
                         "    t.triggered.connect(function() { held.send([0, { uris: new "
                         "DBusQML.variant(['file:///tmp/x'], 'as') }]) })\n"
                         "  }\n"
                         "}");
    QVERIFY(adaptor != nullptr);

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.VTypedFC"), QStringLiteral("/VTypedFC"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy.wait(5000));

    QVERIFY(!watcher->isError());
    const QDBusMessage reply = watcher->reply();
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("ua{sv}"));
    const QVariantList args = reply.arguments();
    QCOMPARE(args.size(), 2);
    QCOMPARE(args.at(0).toUInt(), 0u);
    const QVariant payload = mapValueRawArg(args.at(1), QStringLiteral("uris"));
    QCOMPARE(payloadSignature(payload), QStringLiteral("as"));
    QCOMPARE(payload.userType(), qMetaTypeId<QStringList>());
    QCOMPARE(payload.toStringList(), QStringList{QStringLiteral("file:///tmp/x")});

    delete watcher;
    delete adaptor;
}

// V7 — a gadget in a plain list position with no declared signature: the
// QVariantList recursion in toDbusVariant must convert it (inference path).
void TestDBusAdaptor::testVariantTypedPayloadListInference() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.VTypedList"), QStringLiteral("/VTypedList"),
        QStringLiteral("org.dbusqml.VTypedList"), QStringLiteral("get"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.VTypedList'\n"
        "  path: '/VTypedList'\n"
        "  iface: 'org.dbusqml.VTypedList'\n"
        "  function get() {\n"
        "    return [new DBusQML.variant('hello', 'ay')]\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("av"));

    const QVariant arg = reply.arguments().first();
    QVariant first;
    if (arg.userType() == qMetaTypeId<QVariantList>()) {
        const QVariantList list = arg.toList();
        if (!list.isEmpty())
            first = list.first();
    } else {
        const QDBusArgument arr = arg.value<QDBusArgument>();
        arr.beginArray();
        if (!arr.atEnd()) {
            QDBusVariant v;
            arr >> v;
            first = v.variant();
        }
        arr.endArray();
    }
    QCOMPARE(payloadSignature(first), QStringLiteral("ay"));
    QCOMPARE(first.userType(), qMetaTypeId<QByteArray>());
    QCOMPARE(first.toByteArray(), QByteArray("hello"));
}

// A variant value inside an a{sv} map must demarshal to its payload (single
// "v"), not a nested QDBusVariant — the slot-aware conversion, not a
// double-wrap. Pre-existing 0.4.0 bug; GLib's concrete-type lookup depends on it.
void TestDBusAdaptor::testVariantInMapSingleWrap() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.VSingleMap"), QStringLiteral("/VSingleMap"),
        QStringLiteral("org.dbusqml.VSingleMap"), QStringLiteral("readAll"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.VSingleMap'\n"
        "  path: '/VSingleMap'\n"
        "  iface: 'org.dbusqml.VSingleMap'\n"
        "  function readAll() {\n"
        "    return { 'color-scheme': new DBusQML.variant('hello') }\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
    const QVariant payload = mapValueRaw(reply, QStringLiteral("color-scheme"));
    QCOMPARE(payload.userType(), QMetaType::QString);
    QCOMPARE(payload.toString(), QStringLiteral("hello"));
}

// variant(variant(x)) is an intentional nested variant: the outer "v" carries
// an inner "v". The slot-aware conversion must preserve it (v(v(i))), not
// collapse it.
void TestDBusAdaptor::testVariantNestedVariantExplicit() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.VNestedVar"), QStringLiteral("/VNestedVar"),
        QStringLiteral("org.dbusqml.VNestedVar"), QStringLiteral("get"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.VNestedVar'\n"
        "  path: '/VNestedVar'\n"
        "  iface: 'org.dbusqml.VNestedVar'\n"
        "  function get() {\n"
        "    return new DBusQML.variant(new DBusQML.variant(42))\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("v"));
    const QVariant outer = reply.arguments().first().value<QDBusVariant>().variant();
    QCOMPARE(outer.userType(), qMetaTypeId<QDBusVariant>());
    QCOMPARE(outer.value<QDBusVariant>().variant().toInt(), 42);
}

// A Variant member inside a struct must keep its "v" — struct_([ns,key,variant])
// is (ssv), not (ssx). The struct-member slot is variant-free.
void TestDBusAdaptor::testVariantStructMemberKeepsVariant() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.VStructMember"), QStringLiteral("/VStructMember"),
        QStringLiteral("org.dbusqml.VStructMember"), QStringLiteral("get"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.VStructMember'\n"
        "  path: '/VStructMember'\n"
        "  iface: 'org.dbusqml.VStructMember'\n"
        "  function get() {\n"
        "    return new DBusQML.struct_(['ns', 'key', new DBusQML.variant(1)])\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("(ssv)"));
}

// ==================== Multiple adaptors on one path (M1–M9) ====================
//
// DESIGN.md promises "multiple DBusAdaptor instances with the same service and
// path but different iface". The 0.5.0 implementation registers each adaptor as
// its own virtual object, so only the first survives. M1–M6/M9 pin the fix;
// M7/M8 are regression pins (separate buses; single-adaptor path through the
// dispatcher).

// M1 — the napkin repro verbatim: two adaptors, same service + path, ifaces A/B
// (ping→"a", pong→"b"), each callable WITH the interface set in the call.
void TestDBusAdaptor::testCoLocatedSamePath() {
    QObject *a = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi1'\n"
                                  "  path: '/Multi1'\n"
                                  "  iface: 'org.dbusqml.IfaceA'\n"
                                  "  function ping() { return 'a' }\n"
                                  "}");
    QVERIFY(a != nullptr);
    QObject *b = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi1'\n"
                                  "  path: '/Multi1'\n"
                                  "  iface: 'org.dbusqml.IfaceB'\n"
                                  "  function pong() { return 'b' }\n"
                                  "}");
    QVERIFY(b != nullptr);

    const QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusMessage ca = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi1"), QStringLiteral("/Multi1"),
        QStringLiteral("org.dbusqml.IfaceA"), QStringLiteral("ping"));
    QDBusMessage ra = bus.call(ca, QDBus::Block, 3000);
    QCOMPARE(ra.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(ra.arguments().first().toString(), QStringLiteral("a"));

    QDBusMessage cb = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi1"), QStringLiteral("/Multi1"),
        QStringLiteral("org.dbusqml.IfaceB"), QStringLiteral("pong"));
    QDBusMessage rb = bus.call(cb, QDBus::Block, 3000);
    QCOMPARE(rb.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(rb.arguments().first().toString(), QStringLiteral("b"));

    delete a;
    delete b;
}

// M2 — merged introspection: Introspect on the shared path returns XML with
// BOTH <interface> blocks (wire-literal string asserts).
void TestDBusAdaptor::testCoLocatedIntrospection() {
    QObject *a = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi2'\n"
                                  "  path: '/Multi2'\n"
                                  "  iface: 'org.dbusqml.IfaceA'\n"
                                  "  function ping() { return 'a' }\n"
                                  "}");
    QVERIFY(a != nullptr);
    QObject *b = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi2'\n"
                                  "  path: '/Multi2'\n"
                                  "  iface: 'org.dbusqml.IfaceB'\n"
                                  "  function pong() { return 'b' }\n"
                                  "}");
    QVERIFY(b != nullptr);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi2"), QStringLiteral("/Multi2"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!reply.arguments().isEmpty());
    const QString xml = reply.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("org.dbusqml.IfaceA")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("org.dbusqml.IfaceB")), qPrintable(xml));

    delete a;
    delete b;
}

// M3 — properties route by the interface ARGUMENT: GetAll("IfaceA") returns only
// A's properties, GetAll("IfaceB") only B's.
void TestDBusAdaptor::testCoLocatedPropertiesRouting() {
    QObject *a = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi3'\n"
                                  "  path: '/Multi3'\n"
                                  "  iface: 'org.dbusqml.IfaceA'\n"
                                  "  property int alpha: 1\n"
                                  "}");
    QVERIFY(a != nullptr);
    QObject *b = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi3'\n"
                                  "  path: '/Multi3'\n"
                                  "  iface: 'org.dbusqml.IfaceB'\n"
                                  "  property int beta: 2\n"
                                  "}");
    QVERIFY(b != nullptr);

    const QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusMessage ga = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi3"), QStringLiteral("/Multi3"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    ga.setArguments({QStringLiteral("org.dbusqml.IfaceA")});
    QDBusMessage ra = bus.call(ga, QDBus::Block, 3000);
    QCOMPARE(ra.type(), QDBusMessage::ReplyMessage);
    const QVariantMap pa = unwrapDbus(ra.arguments().first()).toMap();
    QVERIFY(pa.contains(QStringLiteral("alpha")));
    QVERIFY(!pa.contains(QStringLiteral("beta")));

    QDBusMessage gb = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi3"), QStringLiteral("/Multi3"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    gb.setArguments({QStringLiteral("org.dbusqml.IfaceB")});
    QDBusMessage rb = bus.call(gb, QDBus::Block, 3000);
    QCOMPARE(rb.type(), QDBusMessage::ReplyMessage);
    const QVariantMap pb = unwrapDbus(rb.arguments().first()).toMap();
    QVERIFY(pb.contains(QStringLiteral("beta")));
    QVERIFY(!pb.contains(QStringLiteral("alpha")));

    delete a;
    delete b;
}

// M4 — teardown order (path half): destroy A, B still answers and the path
// stays registered; destroy B, the path unregisters (call errors, no hang).
void TestDBusAdaptor::testCoLocatedTeardownPath() {
    QObject *a = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi4'\n"
                                  "  path: '/Multi4'\n"
                                  "  iface: 'org.dbusqml.IfaceA'\n"
                                  "  function ping() { return 'a' }\n"
                                  "}");
    QVERIFY(a != nullptr);
    QObject *b = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi4'\n"
                                  "  path: '/Multi4'\n"
                                  "  iface: 'org.dbusqml.IfaceB'\n"
                                  "  function pong() { return 'b' }\n"
                                  "}");
    QVERIFY(b != nullptr);

    const QDBusConnection bus = QDBusConnection::sessionBus();

    delete a;

    QDBusMessage cb = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi4"), QStringLiteral("/Multi4"),
        QStringLiteral("org.dbusqml.IfaceB"), QStringLiteral("pong"));
    QDBusMessage rb = bus.call(cb, QDBus::Block, 3000);
    QCOMPARE(rb.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(rb.arguments().first().toString(), QStringLiteral("b"));

    delete b;

    QDBusMessage c2 = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi4"), QStringLiteral("/Multi4"),
        QStringLiteral("org.dbusqml.IfaceB"), QStringLiteral("pong"));
    QDBusMessage r2 = bus.call(c2, QDBus::Block, 3000);
    QCOMPARE(r2.type(), QDBusMessage::ErrorMessage);
    QVERIFY2(r2.errorName().contains(QStringLiteral("Unknown")), qPrintable(r2.errorName()));
}

// M5 — teardown order (service half): A and B share a service name; destroy A,
// the NAME is still owned and B answers via it; destroy B, the name releases.
void TestDBusAdaptor::testCoLocatedTeardownService() {
    QObject *a = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi5'\n"
                                  "  path: '/Multi5'\n"
                                  "  iface: 'org.dbusqml.IfaceA'\n"
                                  "  function ping() { return 'a' }\n"
                                  "}");
    QVERIFY(a != nullptr);
    QObject *b = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi5'\n"
                                  "  path: '/Multi5'\n"
                                  "  iface: 'org.dbusqml.IfaceB'\n"
                                  "  function pong() { return 'b' }\n"
                                  "}");
    QVERIFY(b != nullptr);

    const QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.Multi5")));

    delete a;

    QVERIFY(bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.Multi5")));
    QDBusMessage cb = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi5"), QStringLiteral("/Multi5"),
        QStringLiteral("org.dbusqml.IfaceB"), QStringLiteral("pong"));
    QDBusMessage rb = bus.call(cb, QDBus::Block, 3000);
    QCOMPARE(rb.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(rb.arguments().first().toString(), QStringLiteral("b"));

    delete b;

    QTRY_VERIFY_WITH_TIMEOUT(
        !bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.Multi5")), 3000);
}

// M6 — iface-less call (D-Bus allows empty interface): routes by member name
// across attached adaptors in attach order.
void TestDBusAdaptor::testCoLocatedIfaceLessCall() {
    QObject *a = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi6'\n"
                                  "  path: '/Multi6'\n"
                                  "  iface: 'org.dbusqml.IfaceA'\n"
                                  "  function ping() { return 'a' }\n"
                                  "}");
    QVERIFY(a != nullptr);
    QObject *b = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi6'\n"
                                  "  path: '/Multi6'\n"
                                  "  iface: 'org.dbusqml.IfaceB'\n"
                                  "  function pong() { return 'b' }\n"
                                  "}");
    QVERIFY(b != nullptr);

    const QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusMessage c1 = QDBusMessage::createMethodCall(QStringLiteral("org.dbusqml.Multi6"),
                                                     QStringLiteral("/Multi6"), QString(),
                                                     QStringLiteral("ping"));
    QDBusMessage r1 = bus.call(c1, QDBus::Block, 3000);
    QCOMPARE(r1.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r1.arguments().first().toString(), QStringLiteral("a"));

    QDBusMessage c2 = QDBusMessage::createMethodCall(QStringLiteral("org.dbusqml.Multi6"),
                                                     QStringLiteral("/Multi6"), QString(),
                                                     QStringLiteral("pong"));
    QDBusMessage r2 = bus.call(c2, QDBus::Block, 3000);
    QCOMPARE(r2.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r2.arguments().first().toString(), QStringLiteral("b"));

    delete a;
    delete b;
}

// M7 — regression pin, separate buses: the same path on the session bus and a
// custom connectToBus connection do NOT share a dispatcher (registry key is
// (connection name, path)).
void TestDBusAdaptor::testCoLocatedSeparateBuses() {
    auto *session = new VariantEchoAdaptor;
    session->setService(QStringLiteral("org.dbusqml.Multi7Sess"));
    session->setPath(QStringLiteral("/Multi7"));
    session->setIface(QStringLiteral("org.dbusqml.Multi7Sess"));
    session->classBegin();
    session->componentComplete();

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *custom = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(custom != nullptr);

    auto *other = new VariantEchoAdaptor;
    other->setService(QStringLiteral("org.dbusqml.Multi7Cust"));
    other->setPath(QStringLiteral("/Multi7"));
    other->setIface(QStringLiteral("org.dbusqml.Multi7Cust"));
    other->setConnection(custom);
    other->classBegin();
    other->componentComplete();

    const QDBusConnection bus = QDBusConnection::sessionBus();

    // Both services are owned — the registry did not conflate the two
    // connections (a path-only key would skip the custom connection's
    // registration and leave its service unserved).
    QVERIFY(bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.Multi7Sess")));
    QVERIFY(bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.Multi7Cust")));

    QDBusMessage c1 = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi7Sess"), QStringLiteral("/Multi7"),
        QStringLiteral("org.dbusqml.Multi7Sess"), QStringLiteral("echo"));
    c1.setArguments({42});
    QDBusMessage r1 = bus.call(c1, QDBus::Block, 3000);
    QCOMPARE(r1.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r1.arguments().first().toInt(), 42);

    // The custom-connection adaptor answers via an async call — a same-thread
    // blocking call would not pump the custom connection's socket.
    QDBusMessage c2 = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi7Cust"), QStringLiteral("/Multi7"),
        QStringLiteral("org.dbusqml.Multi7Cust"), QStringLiteral("echo"));
    c2.setArguments({43});
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(bus.asyncCall(c2));
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy.wait(5000));
    QVERIFY(!watcher->isError());
    QCOMPARE(watcher->reply().type(), QDBusMessage::ReplyMessage);
    QCOMPARE(watcher->reply().arguments().first().toInt(), 43);

    delete watcher;
    delete session;
    delete other;
    delete custom;
}

// M9 — duplicate iface at one path: qWarning observed at attach, first-attached
// wins for iface-scoped calls, no crash.
void TestDBusAdaptor::testCoLocatedDuplicateIface() {
    QObject *a = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi9'\n"
                                  "  path: '/Multi9'\n"
                                  "  iface: 'org.dbusqml.DupIface'\n"
                                  "  function first() { return 'a' }\n"
                                  "}");
    QVERIFY(a != nullptr);

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                           "duplicate iface org\\.dbusqml\\.DupIface")));
    QObject *b = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.Multi9'\n"
                                  "  path: '/Multi9'\n"
                                  "  iface: 'org.dbusqml.DupIface'\n"
                                  "  function second() { return 'b' }\n"
                                  "}");
    QVERIFY(b != nullptr);

    const QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusMessage c1 = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi9"), QStringLiteral("/Multi9"),
        QStringLiteral("org.dbusqml.DupIface"), QStringLiteral("first"));
    QDBusMessage r1 = bus.call(c1, QDBus::Block, 3000);
    QCOMPARE(r1.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r1.arguments().first().toString(), QStringLiteral("a"));

    // First-attached wins: the second adaptor's member is shadowed for
    // iface-scoped calls.
    QDBusMessage c2 = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi9"), QStringLiteral("/Multi9"),
        QStringLiteral("org.dbusqml.DupIface"), QStringLiteral("second"));
    QDBusMessage r2 = bus.call(c2, QDBus::Block, 3000);
    QCOMPARE(r2.type(), QDBusMessage::ErrorMessage);

    delete a;
    delete b;
}

// ==================== Attach guard (G1, G2) ====================
//
// A failed path registration (foreign object already at the path) must not
// poison the service-name refcount: the adaptor never took the claim, so its
// destructor must not release it (that would steal a shared service name from
// healthy adaptors — the exact bug class 0.5.1 fixed, resurrected via the
// failure path).

// G1 — the reproduction: healthy A (path /P1, service S), a foreign plain
// object at /P2, adaptor C (path /P2, service S) whose attach fails; destroy
// C → S must stay registered and A must still answer; destroy A → S released.
void TestDBusAdaptor::testAttachGuardServiceTheft() {
    QDBusConnection bus = QDBusConnection::sessionBus();

    auto *a = new VariantEchoAdaptor;
    a->setService(QStringLiteral("org.dbusqml.AttachGuard"));
    a->setPath(QStringLiteral("/AttachGuardP1"));
    a->setIface(QStringLiteral("org.dbusqml.AttachGuardA"));
    a->classBegin();
    a->componentComplete();

    QObject foreign;
    QVERIFY(bus.registerObject(QStringLiteral("/AttachGuardP2"), &foreign));

    QTest::ignoreMessage(QtInfoMsg,
                         QRegularExpression(QStringLiteral("Failed to register object")));
    QObject *c = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.AttachGuard'\n"
                                  "  path: '/AttachGuardP2'\n"
                                  "  iface: 'org.dbusqml.AttachGuardC'\n"
                                  "  function pingC() { return 'c' }\n"
                                  "}");
    QVERIFY(c != nullptr);

    // Destroy C — must NOT release the shared service name.
    delete c;

    QVERIFY(bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.AttachGuard")));

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.AttachGuard"), QStringLiteral("/AttachGuardP1"),
        QStringLiteral("org.dbusqml.AttachGuardA"), QStringLiteral("echo"));
    m.setArguments({42});
    QDBusMessage r = bus.call(m, QDBus::Block, 3000);
    QCOMPARE(r.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r.arguments().first().toInt(), 42);

    bus.unregisterObject(QStringLiteral("/AttachGuardP2"));
    delete a;

    QTRY_VERIFY_WITH_TIMEOUT(
        !bus.interface()->isServiceRegistered(QStringLiteral("org.dbusqml.AttachGuard")), 3000);
}

// G2 — registry hygiene after a failed attach: after C's failed attach and
// destruction, unregister the foreign object; a new adaptor at /P2 attaches
// and serves normally (no stale registry entry, no leaked dispatcher).
void TestDBusAdaptor::testAttachGuardRegistryHygiene() {
    QDBusConnection bus = QDBusConnection::sessionBus();

    QObject foreign;
    QVERIFY(bus.registerObject(QStringLiteral("/AttachGuardP2"), &foreign));

    QTest::ignoreMessage(QtInfoMsg,
                         QRegularExpression(QStringLiteral("Failed to register object")));
    QObject *c = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.AttachGuardHygiene'\n"
                                  "  path: '/AttachGuardP2'\n"
                                  "  iface: 'org.dbusqml.AttachGuardHygiene'\n"
                                  "  function pingC() { return 'c' }\n"
                                  "}");
    QVERIFY(c != nullptr);
    delete c;

    bus.unregisterObject(QStringLiteral("/AttachGuardP2"));

    QObject *d = createQmlAdaptor("import DBus 1.0\n"
                                  "DBusAdaptor {\n"
                                  "  service: 'org.dbusqml.AttachGuardHygiene'\n"
                                  "  path: '/AttachGuardP2'\n"
                                  "  iface: 'org.dbusqml.AttachGuardHygiene'\n"
                                  "  function pingD() { return 'd' }\n"
                                  "}");
    QVERIFY(d != nullptr);

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.AttachGuardHygiene"), QStringLiteral("/AttachGuardP2"),
        QStringLiteral("org.dbusqml.AttachGuardHygiene"), QStringLiteral("pingD"));
    QDBusMessage r = bus.call(m, QDBus::Block, 3000);
    QCOMPARE(r.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r.arguments().first().toString(), QStringLiteral("d"));

    delete d;
}

// ==================== Property marshal guard + conversion (P1–P5) ====================
//
// The Properties.Get/GetAll handlers were the last unguarded marshal path:
// a property value with no D-Bus wire signature (organic case: a QML `var`
// holding a DBusHeldReply*) aborted the process inside QtDBus container
// writing — remotely triggerable via routine introspection (busctl populates
// its RESULT/VALUE column through GetAll). Pre-fix evidence: recorded SIGABRT
// run in the execution report (an abort cannot live inside the suite).

// P1 — Get of an unmarshalable property errors instead of marshaling; a
// healthy property on the same adaptor is unaffected.
void TestDBusAdaptor::testPropertyGuardGet() {
    PoisonPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.Poison"));
    adaptor.setPath(QStringLiteral("/Poison"));
    adaptor.setIface(QStringLiteral("org.dbusqml.Poison"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusConnection bus = QDBusConnection::sessionBus();

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                           "dbusqml: property poison .* not marshalable")));
    QDBusMessage get = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("/Poison"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get.setArguments({QStringLiteral("org.dbusqml.Poison"), QStringLiteral("poison")});
    QDBusMessage reply = bus.call(get, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));

    QDBusMessage good = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("/Poison"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    good.setArguments({QStringLiteral("org.dbusqml.Poison"), QStringLiteral("good")});
    QDBusMessage goodReply = bus.call(good, QDBus::Block, 3000);
    QCOMPARE(goodReply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(goodReply.arguments().first().value<QDBusVariant>().variant().toInt(), 42);
}

// P2 — GetAll succeeds with the poison property skipped (wording matches the
// downstream-observed live log) and the healthy property present.
void TestDBusAdaptor::testPropertyGuardGetAll() {
    PoisonPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.Poison"));
    adaptor.setPath(QStringLiteral("/Poison"));
    adaptor.setIface(QStringLiteral("org.dbusqml.Poison"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusConnection bus = QDBusConnection::sessionBus();

    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(
            "dbusqml: skipping non-marshalable property reply on org\\.dbusqml\\.Poison in "
            "GetAll \\(type QObject\\*\\)")));
    QDBusMessage getAll = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("/Poison"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    getAll.setArguments({QStringLiteral("org.dbusqml.Poison")});
    QDBusMessage reply = bus.call(getAll, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QVariantMap props = unwrapDbus(reply.arguments().first()).toMap();
    QVERIFY(props.contains(QStringLiteral("good")));
    QVERIFY(!props.contains(QStringLiteral("poison")));
    QCOMPARE(props.value(QStringLiteral("good")).toInt(), 42);
}

// P3 — the organic QML repro: `property var` holding a QObject. GetAll must
// not error and must omit the stash; a subsequent normal call succeeds (the
// process is alive — the whole point of the fix). GetAll goes through a
// separate-connection caller: the remote path is what killed pre-fix.
void TestDBusAdaptor::testPropertyGuardQmlStash() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "import QtQml\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.QmlStash'\n"
                                        "  path: '/QmlStash'\n"
                                        "  iface: 'org.dbusqml.QmlStash'\n"
                                        "  property var stash: QtObject {}\n"
                                        "  function ping() { return 'p' }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                           "dbusqml: skipping non-marshalable property reply on "
                                           "org\\.dbusqml\\.QmlStash in GetAll")));
    QDBusPendingCallWatcher *w =
        asyncCallDeferred(QStringLiteral("org.dbusqml.QmlStash"), QStringLiteral("/QmlStash"),
                          QStringLiteral("org.freedesktop.DBus.Properties"),
                          QStringLiteral("GetAll"), {QStringLiteral("org.dbusqml.QmlStash")});
    QSignalSpy spy(w, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy.wait(5000));
    QVERIFY(!w->isError());
    const QVariantMap props = unwrapDbus(w->reply().arguments().first()).toMap();
    QVERIFY(!props.contains(QStringLiteral("stash")));
    delete w;

    // Process alive — a subsequent normal call still answers.
    QDBusMessage ping = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.QmlStash"), QStringLiteral("/QmlStash"),
        QStringLiteral("org.dbusqml.QmlStash"), QStringLiteral("ping"));
    QDBusMessage pingReply = QDBusConnection::sessionBus().call(ping, QDBus::Block, 3000);
    QCOMPARE(pingReply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(pingReply.arguments().first().toString(), QStringLiteral("p"));

    delete adaptor;
}

// P4 — gadget-valued properties are served CORRECTLY (the conversion): a
// QVariant property holding a DBus::Variant / DBus::Bytes gadget marshals as a
// single-wrapped "v" with the gadget's payload, not skipped as unmarshalable.
void TestDBusAdaptor::testPropertyGadgetConversion() {
    GadgetPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.GadgetProp"));
    adaptor.setPath(QStringLiteral("/GadgetProp"));
    adaptor.setIface(QStringLiteral("org.dbusqml.GadgetProp"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusMessage get = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.GadgetProp"), QStringLiteral("/GadgetProp"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get.setArguments({QStringLiteral("org.dbusqml.GadgetProp"), QStringLiteral("varProp")});
    QDBusMessage r1 = bus.call(get, QDBus::Block, 3000);
    QCOMPARE(r1.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r1.signature(), QStringLiteral("v"));
    QCOMPARE(r1.arguments().first().value<QDBusVariant>().variant().toInt(), 42);

    get.setArguments({QStringLiteral("org.dbusqml.GadgetProp"), QStringLiteral("bytesProp")});
    QDBusMessage r2 = bus.call(get, QDBus::Block, 3000);
    QCOMPARE(r2.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r2.signature(), QStringLiteral("v"));
    const QVariant payload = r2.arguments().first().value<QDBusVariant>().variant();
    QCOMPARE(payload.userType(), QMetaType::QByteArray);
    QCOMPARE(payload.toByteArray(), QByteArrayLiteral("hello"));
}

// P4 (QML side, best-effort per plan) — `property var` holding a gadget.
// Whether QJSValue::toVariant preserves or flattens the gadget decides the
// payload; the abort-prevention holds regardless. Asserts the expected
// single-wrap; actual behavior recorded in the execution report.
void TestDBusAdaptor::testPropertyGadgetQml() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "import DBus 1.0 as DBusQML\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.GadgetQml'\n"
                                        "  path: '/GadgetQml'\n"
                                        "  iface: 'org.dbusqml.GadgetQml'\n"
                                        "  property var gv: new DBusQML.variant(42)\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QDBusMessage get = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.GadgetQml"), QStringLiteral("/GadgetQml"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get.setArguments({QStringLiteral("org.dbusqml.GadgetQml"), QStringLiteral("gv")});
    QDBusMessage reply = QDBusConnection::sessionBus().call(get, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("v"));
    QCOMPARE(reply.arguments().first().value<QDBusVariant>().variant().toInt(), 42);

    delete adaptor;
}

// P5 — introspection honesty + regression: the poison property (QObject*) is
// never advertised (a "v" promise GetAll could never keep), the healthy one
// stays, and wire Introspect on the poison adaptor is safe.
void TestDBusAdaptor::testPropertyGuardIntrospection() {
    PoisonPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.Poison"));
    adaptor.setPath(QStringLiteral("/Poison"));
    adaptor.setIface(QStringLiteral("org.dbusqml.Poison"));
    adaptor.classBegin();
    adaptor.componentComplete();

    const QString xml = adaptor.introspect(QString());
    QVERIFY(!xml.contains(QStringLiteral("poison")));
    QVERIFY(xml.contains(QStringLiteral("good")));

    // Wire Introspect (local loop, same as testCoLocatedIntrospection): the
    // XML served through the dispatcher never advertises poison either.
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage intro = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("/Poison"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    QDBusMessage introReply = bus.call(intro, QDBus::Block, 3000);
    QCOMPARE(introReply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(introReply.arguments().size(), 1);
    QVERIFY(!introReply.arguments().first().toString().contains(QStringLiteral("poison")));
    QVERIFY(introReply.arguments().first().toString().contains(QStringLiteral("good")));

    // Still serving methods after the guarded paths ran.
    QDBusMessage ping = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("/Poison"),
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("ping"));
    QDBusMessage pingReply = bus.call(ping, QDBus::Block, 3000);
    QCOMPARE(pingReply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(pingReply.arguments().first().toString(), QStringLiteral("pong"));
}

// The 0.6.0 heuristic removal: a `{value: 42}` dict is a REAL dict on the
// wire (a{sv} with key "value"), not a guessed variant. The explicit forms
// (`new DBusQML.variant(x)`) are the documented typing contract.
void TestDBusAdaptor::testValueKeyDictIsRealDict() {
    QDBusMessage reply =
        callQmlAdaptorMethod(QStringLiteral("org.dbusqml.ValueDict"), QStringLiteral("/ValueDict"),
                             QStringLiteral("org.dbusqml.ValueDict"), QStringLiteral("get"), {},
                             "import DBus 1.0\n"
                             "DBusAdaptor {\n"
                             "  service: 'org.dbusqml.ValueDict'\n"
                             "  path: '/ValueDict'\n"
                             "  iface: 'org.dbusqml.ValueDict'\n"
                             "  function get() { return { value: 42 } }\n"
                             "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
    const QVariantMap m = unwrapDbus(reply.arguments().first()).toMap();
    QCOMPARE(m.size(), 1);
    QVERIFY(m.contains(QStringLiteral("value")));
    QCOMPARE(m.value(QStringLiteral("value")).toInt(), 42);
}

// The struct sub-heuristic removal: `{value: [0.1, 0.2]}` is likewise a real
// dict (a{sv} whose value is an av list), not a guessed (dd) struct.
void TestDBusAdaptor::testValueKeyStructListIsRealDict() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.ValueStruct"), QStringLiteral("/ValueStruct"),
        QStringLiteral("org.dbusqml.ValueStruct"), QStringLiteral("get"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.ValueStruct'\n"
        "  path: '/ValueStruct'\n"
        "  iface: 'org.dbusqml.ValueStruct'\n"
        "  function get() { return { value: [0.1, 0.2] } }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
    const QVariantMap m = unwrapDbus(reply.arguments().first()).toMap();
    QVERIFY(m.contains(QStringLiteral("value")));
    const QVariantList l = m.value(QStringLiteral("value")).toList();
    QCOMPARE(l.size(), 2);
    QCOMPARE(l.at(0).toDouble(), 0.1);
    QCOMPARE(l.at(1).toDouble(), 0.2);
}

// QString/int/QByteArray-returning C++ Q_INVOKABLEs round-trip with
// wire-literal signatures (pre-0.6.0: silent empty replies).
void TestDBusAdaptor::testTypedCppReturnsRoundTrip() {
    TypedReturnAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.TypedRet"));
    adaptor.setPath(QStringLiteral("/TypedRet"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TypedRet"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusConnection bus = QDBusConnection::sessionBus();
    auto call = [&](const QString &member, const QVariantList &args) {
        QDBusMessage m = QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.TypedRet"), QStringLiteral("/TypedRet"),
            QStringLiteral("org.dbusqml.TypedRet"), member);
        if (!args.isEmpty())
            m.setArguments(args);
        return bus.call(m, QDBus::Block, 3000);
    };

    QDBusMessage r = call(QStringLiteral("hi"), {});
    QCOMPARE(r.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r.signature(), QStringLiteral("s"));
    QCOMPARE(r.arguments().first().toString(), QStringLiteral("hi"));

    r = call(QStringLiteral("five"), {});
    QCOMPARE(r.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r.signature(), QStringLiteral("i"));
    QCOMPARE(r.arguments().first().toInt(), 5);

    r = call(QStringLiteral("bytes"), {});
    if (r.type() == QDBusMessage::ErrorMessage)
        std::fprintf(stderr, "BYTES-DBG err=%s msg=%s\n", qPrintable(r.errorName()),
                     qPrintable(r.errorMessage()));
    QCOMPARE(r.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r.signature(), QStringLiteral("ay"));
    QCOMPARE(r.arguments().first().toByteArray(), QByteArrayLiteral("xy"));

    // QVariant return unchanged (the pre-0.6.0 captured path).
    r = call(QStringLiteral("ping"), {});
    QCOMPARE(r.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(r.arguments().first().toString(), QStringLiteral("pong"));
}

// A C++ method beyond the 5-arg cap warns loudly and errors (no silent drop).
void TestDBusAdaptor::testTypedCppOverArgCapWarns() {
    TypedReturnAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.TypedCap"));
    adaptor.setPath(QStringLiteral("/TypedCap"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TypedCap"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QTest::ignoreMessage(
        QtWarningMsg, QRegularExpression(QStringLiteral("dbusqml: method many takes 6 arguments")));
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.TypedCap"), QStringLiteral("/TypedCap"),
        QStringLiteral("org.dbusqml.TypedCap"), QStringLiteral("many"));
    m.setArguments({1, 2, 3, 4, 5, 6});
    QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    QCOMPARE(r.type(), QDBusMessage::ErrorMessage);
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
