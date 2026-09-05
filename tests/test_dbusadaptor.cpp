#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QXmlStreamReader>
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
#include "dbusmessage.h"
#include "dbusservicewatcher.h"
#include "dbusobjectmanager.h"
#include <QFile>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include "dbus.h"
#include "dbuscatalog.h"
#include "dbuspathdispatcher.h"
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

// Typed C++ properties for the W9 signature-gap test (B8): uchar/float/
// objectpath/signature properties must map to y/d/o/g in the XML.
class TypedPropsAdaptor : public DBusAdaptor {
    Q_OBJECT
    Q_PROPERTY(uchar level READ level CONSTANT)
    Q_PROPERTY(float ratio READ ratio CONSTANT)
    Q_PROPERTY(QDBusObjectPath objPath READ objPath CONSTANT)
    Q_PROPERTY(QDBusSignature sig READ sig CONSTANT)
public:
    explicit TypedPropsAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}
    uchar level() const { return 7; }
    float ratio() const { return 1.5f; }
    QDBusObjectPath objPath() const { return QDBusObjectPath(QStringLiteral("/p")); }
    QDBusSignature sig() const { return QDBusSignature(QStringLiteral("s")); }
};

// C++-declared value signals — the organic host for PascalCase names
// (QML forbids uppercase-initial signal declarations). Used by the A5/A6/A11
// emission-truthfulness tests.
class DeclaredSignalAdaptor : public DBusAdaptor {
    Q_OBJECT
public:
    explicit DeclaredSignalAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

signals:
    void StateChanged(QVariant a0, QVariant a1); // declared: oa{sv}
    void SixArgs(int a1, int a2, int a3, int a4, int a5, int a6);
    void qmlPing(QVariant v); // qml* prefix
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

class MatrixCatcher : public QObject {
    Q_OBJECT
public:
    QDBusMessage last;
    int count = 0;
public slots:
    void onSignal(const QDBusMessage &msg) {
        last = msg;
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

    // 0.9.0 served-surface: spec-cased property dispatch, Set guard, NO_REPLY.
    void testPropertyCasedGet();
    void testPropertyCasedSet();
    void testPropertySetInconvertibleErrors();
    void testNoReplyExpectedServed();
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

    // C0 relay guard (fix-parity): relayed signal args are guarded.
    void testRelayGuardSkipsUnmarshalable();

    // A4/A5/A6/A11 (fix-parity): emission truthfulness.
    void testPrivateNotifyNotBroadcast();
    void testAdvertisedNameSignalTiers();
    void testDeclaredSignalTypesAtEmission();
    void testAttachXmlSetParity();

    // A7/A8/A9/B8/A10 (fix-parity): the napkin + XML truthfulness.
    void testDeclaredOutArgsAndVoid();
    void testCatalogPropertyTypes();
    void testMetaTypeSignatureGaps();
    void testLibraryMechanismSkips();

    // {value:} heuristic removal pins (0.6.0 wire change).
    void testValueKeyDictIsRealDict();

    // 0.9.0 served surface: truthful introspection, naming ladder, _members.
    void testServedSignalsExplicit();
    void testServedSignalsCatalog();
    void testServedXmlBuiltinsFiltered();
    void testServedMethodNameFoldFallback();
    void testServedMethodArgTypesFromCatalog();
    void testMembersAliasDispatch();
    void testMembersAliasServing();

    // A1 (fix-parity): aliased handlers run through the JS path — named
    // error replies and precision-safe 64-bit arg delivery.
    void testAliasJsPathNamedError();
    void testAliasJsPathPrecision64();
    void testCatalogSourcesAndPrecedence();
    void testCatalogMalformedXmlSkipped();

    // 0.9.0 PropertiesChanged + relay retirement.
    void testPropertiesChangedReachesProxy();
    void testNotifyRelaysRemoved();
    void testPlainSignalFoldsOnWire();

    // 0.9.0 built-in collision prevention.
    void testBuiltinShadowFailsToLoad();
    void testMemberCollisionWarns();

    // 0.9.0 call options + fire-and-forget + nested-container pin.
    void testCallTimeout();
    void testFireAndForgetSend();
    void testMessageGadgetCallOptions();
    void testNestedContainerRoundTrip();

    // 0.9.0 named error replies from handlers (S2).
    void testNamedErrorReply();
    void testPlainExceptionFailedReply();

    // 0.9.0 service-name acquisition (S1).
    void testServiceAcquisitionTakeover();

    // 0.9.0 standalone watcher elements (N1).
    void testSignalWatcherDelivers();
    void testSignalWatcherWildcardMember();
    void testServiceWatcherAppearDisappear();

    // 0.9.0 ObjectManager client (B6).
    void testObjectManagerClient();

    // 0.9.0 unix fd (h) passing (B1/S3).
    void testFdRoundTripSend();
    void testFdRoundTripReceive();
    void testFdContainerPosition();

    // 0.9.0 lossless 64-bit delivery (C2).
    void testInt64StringRoundTrip();
    void testInt64SmallValueStaysNumber();

    // 0.9.0 fix cycle: cross-process fd transfer.
    void testFdCrossProcess();
    void testValueKeyStructListIsRealDict();

    // 0.9.0 fix cycle: co-location/leak regression.
    void testCoLocatedIntrospectionClean();

    // Typed C++ return capture (0.6.0).
    void testTypedCppReturnsRoundTrip();
    void testTypedCppOverArgCapWarns();

    // Malformed-signature crash fix (0.6.0): variant(x, "(") loud-fails.
    void testMalformedVariantSigLoudFails();
    void testMalformedVariantSigSignalSafe();

    // Adversarial input matrix (0.6.0) — data-driven pins per exit.
    void testMatrixReplyValues();
    void testMatrixGetAllValues();
    void testMatrixSignalValues();

    // Adaptor lifecycle (0.7.0) — the QML-ownership axis.
    void testLifecycleDestroyAfterDispatch();
    void testLifecycleGCCollectsAfterDispatch();
    void testLifecycleInHandlerDestroy();
    void testLifecycleDestroyWhilePendingErrorsCallers();
    void testLifecycleAbandonedWhilePendingCollected();
    void testLifecycleGcInsideHandler();
    void testLifecycleDeclarativeAdaptorUnchanged();
    void testLifecycleUnregister();
    void testLifecycleUnregisterErrorsHeldReply();
    void testLifecycleLeakRegression();
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

// ==================== 0.9.0 served surface: property dispatch ===============

// Served test adaptor with a PascalCase-conventional property layout that a
// spec caller would address as "Version" (MPRIS-style).
class CasedPropAdaptor : public DBusAdaptor {
    Q_OBJECT
    Q_PROPERTY(int version READ version WRITE setVersion NOTIFY versionChanged)

public:
    explicit CasedPropAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

    // Server-side record of what the wire delivered (B4 observability).
    bool lastReplyRequired = true;
    bool handleMessage(const QDBusMessage &msg, const QDBusConnection &c) override {
        lastReplyRequired = msg.isReplyRequired();
        return DBusAdaptor::handleMessage(msg, c);
    }
    int version() const { return m_version; }
    void setVersion(int v) {
        m_version = v;
        emit versionChanged();
    }

public slots:
    QVariant bump() {
        ++m_version;
        return m_version;
    }

signals:
    void versionChanged();

private:
    int m_version = 7;
};

// A2 — Properties.Get with the PascalCase wire name must find the camelCase
// QML property (mirror of the 0.3.1 method dual lookup). 0.8.0: InvalidArgs.
void TestDBusAdaptor::testPropertyCasedGet() {
    CasedPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.setPath(QStringLiteral("/CasedProp"));
    adaptor.setIface(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusConnection bus = QDBusConnection::sessionBus();

    // Exact camelCase still works (unchanged).
    QDBusMessage exact = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("/CasedProp"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    exact.setArguments({QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("version")});
    exact = bus.call(exact, QDBus::Block, 3000);
    QCOMPARE(exact.type(), QDBusMessage::ReplyMessage);

    // PascalCase wire name folds.
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("/CasedProp"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    m.setArguments({QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("Version")});
    QDBusMessage reply = bus.call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!reply.arguments().isEmpty());
    QCOMPARE(reply.arguments().first().value<QDBusVariant>().variant().toInt(), 7);
}

// A2 — Properties.Set with the PascalCase wire name must write the camelCase
// QML property. 0.8.0: InvalidArgs, property unchanged.
void TestDBusAdaptor::testPropertyCasedSet() {
    CasedPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.setPath(QStringLiteral("/CasedProp"));
    adaptor.setIface(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("/CasedProp"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
    m.setArguments({QVariant(QStringLiteral("org.dbusqml.CasedProp")),
                    QVariant(QStringLiteral("Version")), QVariant::fromValue(QDBusVariant(42))});
    QDBusMessage reply = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(adaptor.version(), 42);
}

// C1 — a Set whose value cannot be converted must reply InvalidArgs (with the
// property unchanged), not a silent empty success. 0.8.0: silent success.
void TestDBusAdaptor::testPropertySetInconvertibleErrors() {
    CasedPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.setPath(QStringLiteral("/CasedProp"));
    adaptor.setIface(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("/CasedProp"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
    m.setArguments({QVariant(QStringLiteral("org.dbusqml.CasedProp")),
                    QVariant(QStringLiteral("version")),
                    QVariant::fromValue(QDBusVariant(QStringLiteral("not-a-number")))});
    QDBusMessage reply = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));
    QCOMPARE(adaptor.version(), 7); // unchanged
}

// B4 — NO_REPLY_EXPECTED: the method executes, but no reply is constructed or
// sent (spec conformance; the caller used send(), not a reply-waiting call).
void TestDBusAdaptor::testNoReplyExpectedServed() {
    CasedPropAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.setPath(QStringLiteral("/CasedProp"));
    adaptor.setIface(QStringLiteral("org.dbusqml.CasedProp"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("/CasedProp"),
        QStringLiteral("org.dbusqml.CasedProp"), QStringLiteral("bump"));
    QVERIFY(QDBusConnection::sessionBus().send(m));

    // The handler must run (side effect observable), with or without a reply.
    QTRY_VERIFY_WITH_TIMEOUT(adaptor.version() == 8, 3000);
    qWarning("PROBE-B4: server saw isReplyRequired=%d", int(adaptor.lastReplyRequired));
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
    QVERIFY(props.contains(QStringLiteral("TestInt")));
    QVERIFY(props.contains(QStringLiteral("TestString")));
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
    QVERIFY(pa.contains(QStringLiteral("Alpha")));
    QVERIFY(!pa.contains(QStringLiteral("Beta")));

    QDBusMessage gb = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Multi3"), QStringLiteral("/Multi3"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    gb.setArguments({QStringLiteral("org.dbusqml.IfaceB")});
    QDBusMessage rb = bus.call(gb, QDBus::Block, 3000);
    QCOMPARE(rb.type(), QDBusMessage::ReplyMessage);
    const QVariantMap pb = unwrapDbus(rb.arguments().first()).toMap();
    QVERIFY(pb.contains(QStringLiteral("Beta")));
    QVERIFY(!pb.contains(QStringLiteral("Alpha")));

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
    QVERIFY(props.contains(QStringLiteral("Good")));
    QVERIFY(!props.contains(QStringLiteral("Poison")));
    QCOMPARE(props.value(QStringLiteral("Good")).toInt(), 42);
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
    QVERIFY(!props.contains(QStringLiteral("Stash")));
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
    QVERIFY(!xml.contains(QStringLiteral("<property name=\"Poison\"")));
    QVERIFY(xml.contains(QStringLiteral("<property name=\"Good\"")));

    // Wire Introspect (local loop, same as testCoLocatedIntrospection): the
    // XML served through the dispatcher never advertises poison either.
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage intro = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("/Poison"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    QDBusMessage introReply = bus.call(intro, QDBus::Block, 3000);
    QCOMPARE(introReply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(introReply.arguments().size(), 1);
    QVERIFY(!introReply.arguments().first().toString().contains(
        QStringLiteral("<property name=\"Poison\"")));
    QVERIFY(introReply.arguments().first().toString().contains(
        QStringLiteral("<property name=\"Good\"")));

    // Still serving methods after the guarded paths ran.
    QDBusMessage ping = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("/Poison"),
        QStringLiteral("org.dbusqml.Poison"), QStringLiteral("ping"));
    QDBusMessage pingReply = bus.call(ping, QDBus::Block, 3000);
    QCOMPARE(pingReply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(pingReply.arguments().first().toString(), QStringLiteral("pong"));
}

// C0 — relay guard: a QML-declared value signal relayed with unmarshalable
// args must warn, skip the send, and leave the service alive. The kill
// itself was observed pre-fix in a timeboxed qmltestrunner subprocess
// (0.5.2 protocol) — the JS-function poison killed the connection.
void TestDBusAdaptor::testRelayGuardSkipsUnmarshalable() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "import QtQml\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.RelayGuard'\n"
                                        "  path: '/RelayGuard'\n"
                                        "  iface: 'org.dbusqml.RelayGuard'\n"
                                        "  property var poisonObject: QtObject {}\n"
                                        "  property var poisonFunction: () => {}\n"
                                        "  signal out(var v)\n"
                                        "  function ping() { return 'pong' }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    // Baseline: the service answers over the wire.
    QDBusPendingCallWatcher *w =
        asyncCallDeferred(QStringLiteral("org.dbusqml.RelayGuard"), QStringLiteral("/RelayGuard"),
                          QStringLiteral("org.dbusqml.RelayGuard"), QStringLiteral("Ping"), {});
    QSignalSpy spy(w, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy.wait(5000));
    QVERIFY2(!w->isError(), qPrintable(w->error().message()));
    delete w;

    // Emit the poisons through the relay: each warns + skips the send.
    const QVariant objPoison = adaptor->property("poisonObject");
    const QVariant fnPoison = adaptor->property("poisonFunction");
    QVERIFY(objPoison.isValid());
    QVERIFY(fnPoison.isValid());
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(".*signal Out arg is not marshalable.*skipping send.*")));
    QVERIFY(QMetaObject::invokeMethod(adaptor, "out", Q_ARG(QVariant, objPoison)));
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(".*signal Out arg is not marshalable.*skipping send.*")));
    QVERIFY(QMetaObject::invokeMethod(adaptor, "out", Q_ARG(QVariant, fnPoison)));

    // Service alive: the follow-up wire call still answers.
    w = asyncCallDeferred(QStringLiteral("org.dbusqml.RelayGuard"), QStringLiteral("/RelayGuard"),
                          QStringLiteral("org.dbusqml.RelayGuard"), QStringLiteral("Ping"), {});
    QSignalSpy spy2(w, &QDBusPendingCallWatcher::finished);
    QVERIFY(spy2.wait(5000));
    QVERIFY2(!w->isError(), qPrintable(w->error().message()));
    QCOMPARE(w->reply().arguments().first().toString(), QStringLiteral("pong"));
    delete w;
}

// A4 — a private property's notify signal must not leak onto the bus: the
// privacy exclusion applies at RELAY ATTACH time, while the notify INDEXES
// are recorded for every property (pre-fix the early skip left the private
// notify index unrecorded, so the signal loop attached a relay and the
// PropertiesChanged emission named a property the XML suppresses).
void TestDBusAdaptor::testPrivateNotifyNotBroadcast() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.PrivNotify'\n"
                                        "  path: '/PrivNotify'\n"
                                        "  iface: 'org.dbusqml.PrivNotify'\n"
                                        "  property var _cache: 1\n"
                                        "  property int level: 2\n"
                                        "  function ping() { return 'pong' }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.PrivNotify"), QStringLiteral("/PrivNotify"),
                        QStringLiteral("org.freedesktop.DBus.Properties"),
                        QStringLiteral("PropertiesChanged"), &catcher,
                        SLOT(onSignal(QDBusMessage))));

    // Private change: no PropertiesChanged for the suppressed property.
    adaptor->setProperty("_cache", 5);
    QTest::qWait(500);
    QCOMPARE(catcher.count, 0);

    // Positive control: a public property change DOES reach the proxy shape.
    adaptor->setProperty("level", 3);
    QTRY_VERIFY_WITH_TIMEOUT(catcher.count >= 1, 5000);
    const QVariantMap changed = unwrapDbus(catcher.lastSignal.arguments().at(1)).toMap();
    QVERIFY(changed.contains(QStringLiteral("Level")));
}

// A5 — advertisedName gains the _signals tier (and the catalog-signals tier):
// a declared lowercase signal name is advertised AND relayed under the SAME
// wire name (pre-fix the relay emitted the fold — wire/XML divergence).
void TestDBusAdaptor::testAdvertisedNameSignalTiers() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.SigTier'\n"
                                        "  path: '/SigTier'\n"
                                        "  iface: 'org.dbusqml.SigTier'\n"
                                        "  _signals: ({ batteryLow: 'i' })\n"
                                        "  signal batteryLow(var v)\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    // The XML advertises the declared name (0.9.0 behavior, unchanged).
    QDBusMessage intro = QDBusConnection::sessionBus().call(
        QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.SigTier"), QStringLiteral("/SigTier"),
            QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")),
        QDBus::Block, 3000);
    QCOMPARE(intro.type(), QDBusMessage::ReplyMessage);
    const QString xml = intro.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("<signal name=\"batteryLow\">")), qPrintable(xml));

    // The relay must emit the DECLARED wire name too.
    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.SigTier"), QStringLiteral("/SigTier"),
                        QStringLiteral("org.dbusqml.SigTier"), QStringLiteral("batteryLow"),
                        &catcher, SLOT(onSignal(QDBusMessage))));
    QVERIFY(QMetaObject::invokeMethod(adaptor, "batteryLow", Q_ARG(QVariant, 5)));
    QTRY_VERIFY_WITH_TIMEOUT(catcher.count == 1, 5000);
    QCOMPARE(catcher.lastSignal.signature(), QByteArrayLiteral("i"));
}

// A6 — declared signal types are applied at EMISSION (relay path): the wire
// signature must equal the declared "oa{sv}"; non-conforming values warn and
// skip the send (a signal has no error-reply channel).
void TestDBusAdaptor::testDeclaredSignalTypesAtEmission() {
    DeclaredSignalAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.DeclSig"));
    adaptor.setPath(QStringLiteral("/DeclSig"));
    adaptor.setIface(QStringLiteral("org.dbusqml.DeclSig"));
    adaptor.setSignalSpecs(
        QVariantMap{{QStringLiteral("StateChanged"), QVariant(QStringLiteral("oa{sv}"))}});
    adaptor.classBegin();
    adaptor.componentComplete();

    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.DeclSig"), QStringLiteral("/DeclSig"),
                        QStringLiteral("org.dbusqml.DeclSig"), QStringLiteral("StateChanged"),
                        &catcher, SLOT(onSignal(QDBusMessage))));

    // Conforming emission: on the wire with the DECLARED signature.
    emit adaptor.StateChanged(QVariant::fromValue(QDBusObjectPath(QStringLiteral("/x"))),
                              QVariant(QVariantMap{{QStringLiteral("k"), 1}}));
    QTRY_VERIFY_WITH_TIMEOUT(catcher.count == 1, 5000);
    QCOMPARE(catcher.lastSignal.signature(), QByteArrayLiteral("oa{sv}"));

    // Non-conforming emission: cannot produce the declared shape → warn +
    // skip (no wire signal, count unchanged).
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral(
                             ".*signal StateChanged arg 0 cannot produce declared type.*")));
    emit adaptor.StateChanged(QVariant(5), QVariant(6));
    QTest::qWait(500);
    QCOMPARE(catcher.count, 1);
}

// A11 — attach-set and XML-set parity: >5-param signals are neither relayed
// nor ADVERTISED (pre-fix XML advertised what the relay refused); qml*
// -prefixed signals are neither relayed nor advertised.
void TestDBusAdaptor::testAttachXmlSetParity() {
    DeclaredSignalAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.AttachParity"));
    adaptor.setPath(QStringLiteral("/AttachParity"));
    adaptor.setIface(QStringLiteral("org.dbusqml.AttachParity"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage intro = QDBusConnection::sessionBus().call(
        QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.AttachParity"), QStringLiteral("/AttachParity"),
            QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")),
        QDBus::Block, 3000);
    QCOMPARE(intro.type(), QDBusMessage::ReplyMessage);
    const QString xml = intro.arguments().first().toString();
    QVERIFY2(!xml.contains(QStringLiteral("SixArgs")), qPrintable(xml));
    QVERIFY2(!xml.contains(QStringLiteral("QmlPing")), qPrintable(xml));

    // The qml* signal is not relayed either.
    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.AttachParity"), QStringLiteral("/AttachParity"),
                        QStringLiteral("org.dbusqml.AttachParity"), QStringLiteral("QmlPing"),
                        &catcher, SLOT(onSignal(QDBusMessage))));
    emit adaptor.qmlPing(QVariant(1));
    QTest::qWait(500);
    QCOMPARE(catcher.count, 0);
}

// A7 (the napkin) — declared out-args replace the phantom `result v`;
// declared-void methods carry NO out-arg; D6 — a declared-void handler that
// returns a value is warned and the value DROPPED (the declaration is the
// contract). A8 rides along: the catalog lookup also matches the resolved
// wire name.
void TestDBusAdaptor::testDeclaredOutArgsAndVoid() {
    QTemporaryDir userDir;
    QVERIFY(userDir.isValid());
    const QByteArray savedTypesPath = qgetenv("DBUSQML_TYPES_PATH");
    qputenv("DBUSQML_TYPES_PATH", userDir.path().toLocal8Bit());
    {
        QFile f(userDir.filePath(QStringLiteral("org.dbusqml.OutArgs.xml")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"(<node>
  <interface name="org.dbusqml.OutArgs">
    <method name="CreateMonitor">
      <arg type="o" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="u" direction="out"/>
    </method>
    <method name="Inhibit">
      <arg type="s" direction="in"/>
    </method>
  </interface>
</node>
)");
    }
    DBusCatalog::instance().reload();

    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.OutArgs'\n"
                                        "  path: '/OutArgs'\n"
                                        "  iface: 'org.dbusqml.OutArgs'\n"
                                        "  function createMonitor(a, b, c, d) { return 7 }\n"
                                        "  function inhibit(what) { return 5 }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage intro = bus.call(
        QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.OutArgs"), QStringLiteral("/OutArgs"),
            QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")),
        QDBus::Block, 3000);
    QCOMPARE(intro.type(), QDBusMessage::ReplyMessage);
    const QString xml = intro.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("<arg type=\"u\" direction=\"out\"/>")), qPrintable(xml));
    QVERIFY2(!xml.contains(QStringLiteral("name=\"result\"")), qPrintable(xml));
    // The Inhibit method must carry NO out-arg (declared-void wins).
    const int inhibitBlock = xml.indexOf(QStringLiteral("<method name=\"Inhibit\">"));
    QVERIFY(inhibitBlock >= 0);
    const int inhibitEnd = xml.indexOf(QStringLiteral("</method>"), inhibitBlock);
    QVERIFY(inhibitEnd > inhibitBlock);
    QVERIFY2(!xml.mid(inhibitBlock, inhibitEnd - inhibitBlock)
                  .contains(QStringLiteral("direction=\"out\"")),
             qPrintable(xml));

    // The wire reply carries the DECLARED out type, not a phantom v.
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.OutArgs"), QStringLiteral("/OutArgs"),
        QStringLiteral("org.dbusqml.OutArgs"), QStringLiteral("CreateMonitor"));
    m.setArguments({QVariant::fromValue(QDBusObjectPath(QStringLiteral("/o"))), QVariant("a"),
                    QVariant("b"), QVariant("c")});
    QDBusMessage reply = bus.call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QByteArrayLiteral("u"));
    QCOMPARE(reply.arguments().first().toUInt(), 7u);

    // D6: declared-void + handler returns a value → warn + drop.
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                           ".*Inhibit.*declared void.*dropping return value.*")));
    QDBusMessage mi = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.OutArgs"), QStringLiteral("/OutArgs"),
        QStringLiteral("org.dbusqml.OutArgs"), QStringLiteral("Inhibit"));
    mi.setArguments({QVariant("screensaver")});
    QDBusMessage ri = bus.call(mi, QDBus::Block, 3000);
    QCOMPARE(ri.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(ri.arguments().size(), 0);

    qputenv("DBUSQML_TYPES_PATH", savedTypesPath);
    DBusCatalog::instance().reload();
}

// A9 — catalog property type/access reach the XML (the parser keeps what it
// used to drop; the XML property loop consumes the declaration).
void TestDBusAdaptor::testCatalogPropertyTypes() {
    QTemporaryDir userDir;
    QVERIFY(userDir.isValid());
    const QByteArray savedTypesPath = qgetenv("DBUSQML_TYPES_PATH");
    qputenv("DBUSQML_TYPES_PATH", userDir.path().toLocal8Bit());
    {
        QFile f(userDir.filePath(QStringLiteral("org.dbusqml.CatProps.xml")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"(<node>
  <interface name="org.dbusqml.CatProps">
    <property name="Percentage" type="u" access="read"/>
  </interface>
</node>
)");
    }
    DBusCatalog::instance().reload();

    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.CatProps'\n"
                                        "  path: '/CatProps'\n"
                                        "  iface: 'org.dbusqml.CatProps'\n"
                                        "  property int percentage: 50\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QDBusMessage intro = QDBusConnection::sessionBus().call(
        QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.CatProps"), QStringLiteral("/CatProps"),
            QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")),
        QDBus::Block, 3000);
    QCOMPARE(intro.type(), QDBusMessage::ReplyMessage);
    const QString xml = intro.arguments().first().toString();
    QVERIFY2(
        xml.contains(QStringLiteral("<property name=\"Percentage\" type=\"u\" access=\"read\"/>")),
        qPrintable(xml));

    qputenv("DBUSQML_TYPES_PATH", savedTypesPath);
    DBusCatalog::instance().reload();
}

// B8 — the W9 meta-type→signature map covers y/o/g/h/Float/Long; a uchar
// C++ property can no longer advertise as "v".
void TestDBusAdaptor::testMetaTypeSignatureGaps() {
    TypedPropsAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.TypedProps"));
    adaptor.setPath(QStringLiteral("/TypedProps"));
    adaptor.setIface(QStringLiteral("org.dbusqml.TypedProps"));
    adaptor.classBegin();
    adaptor.componentComplete();

    QDBusMessage intro = QDBusConnection::sessionBus().call(
        QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.TypedProps"), QStringLiteral("/TypedProps"),
            QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")),
        QDBus::Block, 3000);
    QCOMPARE(intro.type(), QDBusMessage::ReplyMessage);
    const QString xml = intro.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("type=\"y\"")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("type=\"d\"")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("type=\"o\"")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("type=\"g\"")), qPrintable(xml));
}

// A10/D1 — library-mechanism names are never served: QML-declared emitSignal
// is SKIPPED by dispatch (the load warning becomes true) and by the XML; the
// XML also skips holdReply/unregister (advertised-but-UnknownMethod dies).
void TestDBusAdaptor::testLibraryMechanismSkips() {
    QTest::ignoreMessage(
        QtInfoMsg, QRegularExpression(QStringLiteral(".*emitSignal member cannot be served.*")));
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.MechSkip'\n"
                                        "  path: '/MechSkip'\n"
                                        "  iface: 'org.dbusqml.MechSkip'\n"
                                        "  function emitSignal(name, args) { return 'emitted' }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QDBusMessage intro = QDBusConnection::sessionBus().call(
        QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.MechSkip"), QStringLiteral("/MechSkip"),
            QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")),
        QDBus::Block, 3000);
    QCOMPARE(intro.type(), QDBusMessage::ReplyMessage);
    const QString xml = intro.arguments().first().toString();
    QVERIFY2(!xml.contains(QStringLiteral("EmitSignal")), qPrintable(xml));

    // Dispatch refuses the library name too (the load warning becomes true).
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.MechSkip"), QStringLiteral("/MechSkip"),
        QStringLiteral("org.dbusqml.MechSkip"), QStringLiteral("EmitSignal"));
    m.setArguments({QVariant("x"), QVariant(QStringList{})});
    QDBusMessage reply = bus.call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
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

// A malformed declared signature ("(" — unbalanced struct) used to build a
// QDBusArgument libdbus aborts on (dbus_message_iter_open_container
// assertion). It must loud-fail to inference instead — no crash, reply still
// served.
void TestDBusAdaptor::testMalformedVariantSigLoudFails() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.BadSig'\n"
                      "  path: '/BadSig'\n"
                      "  iface: 'org.dbusqml.BadSig'\n"
                      "  function get() { return new DBusQML.variant([1, 2], '(') }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    // Arm the expectation only now — engine setup warnings must not consume it.
    QTest::ignoreMessage(QtWarningMsg,
                         "dbusqml: cannot produce declared signature ( for value of type "
                         "QVariantList — falling back to inference");
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.BadSig"), QStringLiteral("/BadSig"),
        QStringLiteral("org.dbusqml.BadSig"), QStringLiteral("get"));
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    // Loud-fail to inference: the variant payload falls back to an av list,
    // still wrapped as v — no crash, no silent wrong struct.
    QCOMPARE(reply.signature(), QStringLiteral("v"));
    const QVariant payload = unwrapDbus(reply.arguments().first());
    QCOMPARE(payload.toList().size(), 2);
    QCOMPARE(payload.toList().at(0).toInt(), 1);
    QCOMPARE(payload.toList().at(1).toInt(), 2);
    delete adaptor;
}

// Same malformed signature on the signal path: warned and skipped, process
// alive (signals have no error-reply channel).
void TestDBusAdaptor::testMalformedVariantSigSignalSafe() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "import DBus 1.0 as DBusQML\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.BadSigSig'\n"
                      "  path: '/BadSigSig'\n"
                      "  iface: 'org.dbusqml.BadSigSig'\n"
                      "  function fire() {\n"
                      "    emitSignal('Sig', [new DBusQML.variant([1, 2], '(')])\n"
                      "  }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    SignalCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.BadSigSig"), QStringLiteral("/BadSigSig"),
                        QStringLiteral("org.dbusqml.BadSigSig"), QStringLiteral("Sig"), &catcher,
                        SLOT(onSignal(QDBusMessage))));

    QTest::ignoreMessage(QtWarningMsg,
                         "dbusqml: cannot produce declared signature ( for value of type "
                         "QVariantList — falling back to inference");
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.BadSigSig"), QStringLiteral("/BadSigSig"),
        QStringLiteral("org.dbusqml.BadSigSig"), QStringLiteral("fire"));
    QCOMPARE(bus.call(call, QDBus::Block, 3000).type(), QDBusMessage::ReplyMessage);

    // The signal IS delivered with the inferred payload (loud-fail falls back
    // to inference rather than skipping) — and the process is alive.
    for (int i = 0; i < 20 && catcher.count == 0; ++i)
        QTest::qWait(100);
    QCOMPARE(catcher.count, 1);
    QCOMPARE(catcher.lastSignal.signature(), QStringLiteral("v"));

    delete adaptor;
}

// ==================== 0.9.0 truthful served introspection ==================
//
// The naming ladder (owner decision Q4): explicit (_signals/_members) →
// declared (catalog) → stable inference (the deterministic first-char-upper
// fold, readOne → ReadOne). Wire names are never guessed from data.

static QString introspectOverBus(const QString &service, const QString &path) {
    QDBusMessage m = QDBusMessage::createMethodCall(
        service, path, QStringLiteral("org.freedesktop.DBus.Introspectable"),
        QStringLiteral("Introspect"));
    QDBusMessage reply = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return QString();
    return reply.arguments().first().toString();
}

// A1 — a _signals-declared signal is served in introspection with its arg
// types split from the concatenated signature. 0.8.0: _signals doesn't exist
// (fixture fails to load).
void TestDBusAdaptor::testServedSignalsExplicit() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.SigServe"), QStringLiteral("/SigServe"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.SigServe'\n"
        "  path: '/SigServe'\n"
        "  iface: 'org.freedesktop.impl.portal.Inhibit'\n"
        "  _signals: ({ StateChanged: 'oa{sv}' })\n"
        "  function inhibit(handle, appId, window, flags, options) { return 0 }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QString xml = reply.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("<signal name=\"StateChanged\">")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("type=\"o\"")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("type=\"a{sv}\"")), qPrintable(xml));
}

// A1 — catalog-declared signals are served for the adaptor's iface without
// any QML declaration (impl.portal.Settings declares SettingChanged with
// (ssv) args). 0.8.0: absent.
void TestDBusAdaptor::testServedSignalsCatalog() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.SigServeCat"), QStringLiteral("/SigServeCat"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.SigServeCat'\n"
        "  path: '/SigServeCat'\n"
        "  iface: 'org.freedesktop.impl.portal.Settings'\n"
        "  function readOne(ns, key) { return 1 }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QString xml = reply.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("<signal name=\"SettingChanged\">")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("type=\"s\"")), qPrintable(xml));
}

// A4 — the library's own notify signals must never appear in served XML.
void TestDBusAdaptor::testServedXmlBuiltinsFiltered() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.SigServeBuiltin"), QStringLiteral("/SigServeBuiltin"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.SigServeBuiltin'\n"
        "  path: '/SigServeBuiltin'\n"
        "  iface: 'org.dbusqml.SigServeBuiltin'\n"
        "  function ping() { return 1 }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QString xml = reply.arguments().first().toString();
    QVERIFY2(!xml.contains(QStringLiteral("signaturesChanged")), qPrintable(xml));
    QVERIFY2(!xml.contains(QStringLiteral("_signalsChanged")), qPrintable(xml));
    QVERIFY2(!xml.contains(QStringLiteral("_membersChanged")), qPrintable(xml));
}

// A3/Q4 — the advertised method name for an undeclared interface is the
// deterministic fold of the QML name: readOne → ReadOne. 0.8.0: advertises
// the QML name verbatim.
void TestDBusAdaptor::testServedMethodNameFoldFallback() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.FoldServe"), QStringLiteral("/FoldServe"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.FoldServe'\n"
        "  path: '/FoldServe'\n"
        "  iface: 'org.dbusqml.FoldServe'\n"
        "  function readOne(ns, key) { return ns + '.' + key }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QString xml = reply.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("<method name=\"ReadOne\">")), qPrintable(xml));
    QVERIFY2(!xml.contains(QStringLiteral("<method name=\"readOne\">")), qPrintable(xml));
}

// The napkin's "related observation": when the catalog declares the method,
// its advertised in-arg types replace the metaobject-derived vvvv.
void TestDBusAdaptor::testServedMethodArgTypesFromCatalog() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.ArgServe"), QStringLiteral("/ArgServe"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.ArgServe'\n"
        "  path: '/ArgServe'\n"
        "  iface: 'org.freedesktop.impl.portal.Settings'\n"
        "  function read(ns, key) { return 1 }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QString xml = reply.arguments().first().toString();
    // Read is declared (ss in) in the bundled catalog: the served <method>
    // advertises the wire name and the declared string in-args, not the
    // metaobject-derived variants.
    QVERIFY2(xml.contains(QStringLiteral("<method name=\"Read\">")), qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("<arg name=\"arg0\" type=\"s\" direction=\"in\"/>")),
             qPrintable(xml));
    QVERIFY2(xml.contains(QStringLiteral("<arg name=\"arg1\" type=\"s\" direction=\"in\"/>")),
             qPrintable(xml));
}

// A7/A8 — _members aliases make colliding and reserved-word members servable:
// wire member "Delete" dispatches to QML doDelete. 0.8.0: no _members
// property (fixture fails to load).
void TestDBusAdaptor::testMembersAliasDispatch() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.AliasServe"), QStringLiteral("/AliasServe"),
        QStringLiteral("org.dbusqml.AliasServe"), QStringLiteral("Delete"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.AliasServe'\n"
        "  path: '/AliasServe'\n"
        "  iface: 'org.dbusqml.AliasServe'\n"
        "  _members: ({ Delete: 'doDelete' })\n"
        "  function doDelete() { return 'deleted' }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.arguments().first().toString(), QStringLiteral("deleted"));
}

// The alias also drives the advertised name (dispatch and introspection can
// never disagree).
void TestDBusAdaptor::testMembersAliasServing() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.AliasServe2"), QStringLiteral("/AliasServe2"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {},
        "import DBus 1.0\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.AliasServe2'\n"
        "  path: '/AliasServe2'\n"
        "  iface: 'org.dbusqml.AliasServe2'\n"
        "  _members: ({ Delete: 'doDelete' })\n"
        "  function doDelete() { return 'deleted' }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QString xml = reply.arguments().first().toString();
    QVERIFY2(xml.contains(QStringLiteral("<method name=\"Delete\">")), qPrintable(xml));
    QVERIFY2(!xml.contains(QStringLiteral("doDelete")), qPrintable(xml));
}

// A1 — an _members-aliased handler must dispatch through the JS path
// (lookup must try the matched alias name), not the C++ invoke fallback:
// a thrown DBusUtils.error becomes a NAMED error reply (pre-fix: the
// C++ invoke swallowed the throw → empty success reply).
void TestDBusAdaptor::testAliasJsPathNamedError() {
    QDBusMessage reply = callQmlAdaptorMethod(
        QStringLiteral("org.dbusqml.AliasErr"), QStringLiteral("/AliasErr"),
        QStringLiteral("org.dbusqml.AliasErr"), QStringLiteral("Guard"), {},
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.AliasErr'\n"
        "  path: '/AliasErr'\n"
        "  iface: 'org.dbusqml.AliasErr'\n"
        "  _members: ({ Guard: 'guardAction' })\n"
        "  function guardAction() {\n"
        "    throw DBusQML.DBusUtils.error('org.dbusqml.Denied', 'aliased throw')\n"
        "  }\n"
        "}");
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.dbusqml.Denied"));
    QCOMPARE(reply.errorMessage(), QStringLiteral("aliased throw"));
}

// A1 (precision facet) — the JS path delivers 64-bit args precision-safe
// (int64 > 2^53 arrives in JS as a string). Through the C++ invoke fallback
// the QML param is a lossy Number (…4993 → …4992).
void TestDBusAdaptor::testAliasJsPathPrecision64() {
    QDBusMessage reply =
        callQmlAdaptorMethod(QStringLiteral("org.dbusqml.Alias64"), QStringLiteral("/Alias64"),
                             QStringLiteral("org.dbusqml.Alias64"), QStringLiteral("BigEcho"),
                             {QVariant::fromValue<qlonglong>(9007199254740993LL)},
                             "import DBus 1.0\n"
                             "DBusAdaptor {\n"
                             "  service: 'org.dbusqml.Alias64'\n"
                             "  path: '/Alias64'\n"
                             "  iface: 'org.dbusqml.Alias64'\n"
                             "  _members: ({ BigEcho: 'doBigEcho' })\n"
                             "  _signatures: ({ BigEcho: 'x' })\n"
                             "  function doBigEcho(v) { return v }\n"
                             "}");
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.arguments().first().toString(), QStringLiteral("9007199254740993"));
}

// Q3 — catalog sources: the freedesktop-standard <data>/dbus-1/interfaces/
// directory is scanned at LOWEST precedence (below bundled + user dirs);
// user-supplied XML (~/.config/dbusqml/types/, DBUSQML_TYPES_PATH) wins.
void TestDBusAdaptor::testCatalogSourcesAndPrecedence() {
    QTemporaryDir dataDir; // stands in for XDG_DATA_HOME (dbus-1/interfaces)
    QTemporaryDir userDir; // DBUSQML_TYPES_PATH (highest precedence)
    QVERIFY(dataDir.isValid() && userDir.isValid());
    QDir(dataDir.filePath(QStringLiteral("dbus-1/interfaces"))).mkpath(QStringLiteral("."));
    QDir(userDir.path()).mkpath(QStringLiteral("."));

    const QByteArray ifaceXml = R"(<node>
  <interface name="org.dbusqml.Precedence">
    <method name="WhoAmI">
      <arg type="s" direction="out"/>
    </method>
  </interface>
</node>
)";
    const char *systemDecl = "system-tier";
    const char *userDecl = "user-tier";
    {
        QFile f(dataDir.filePath(QStringLiteral("dbus-1/interfaces/org.dbusqml.Precedence.xml")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(ifaceXml);
    }
    {
        QFile f(userDir.filePath(QStringLiteral("org.dbusqml.Precedence.xml")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(ifaceXml);
        // Distinguish the winning tier via the method's out-arg annotation —
        // instead we simply verify the SOURCE file through a marker interface.
    }

    // Mark the system-tier file with an extra property so the two tiers are
    // distinguishable by content.
    {
        QFile f(dataDir.filePath(QStringLiteral("dbus-1/interfaces/org.dbusqml.Precedence.xml")));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"(<node>
  <interface name="org.dbusqml.Precedence">
    <method name="WhoAmI">
      <arg type="s" direction="out"/>
    </method>
    <signal name="SystemTierMarker"/>
  </interface>
</node>
)");
    }
    {
        QFile f(userDir.filePath(QStringLiteral("org.dbusqml.Precedence.xml")));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"(<node>
  <interface name="org.dbusqml.Precedence">
    <method name="WhoAmI">
      <arg type="s" direction="out"/>
    </method>
    <signal name="UserTierMarker"/>
  </interface>
</node>
)");
    }

    const QByteArray savedDataHome = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", dataDir.path().toLocal8Bit());
    qputenv("DBUSQML_TYPES_PATH", userDir.path().toLocal8Bit());
    DBusCatalog::instance().reload();
    auto spec = DBusCatalog::instance().lookup(QStringLiteral("org.dbusqml.Precedence"));
    QVERIFY(spec.has_value());
    // Highest tier wins: the user-tier marker signal is present, the
    // system-tier one is not.
    QVERIFY(spec->signals_.contains(QStringLiteral("UserTierMarker")));
    QVERIFY(!spec->signals_.contains(QStringLiteral("SystemTierMarker")));

    // Remove the user tier: the dbus-1/interfaces scan (lowest) takes over.
    qunsetenv("DBUSQML_TYPES_PATH");
    DBusCatalog::instance().reload();
    spec = DBusCatalog::instance().lookup(QStringLiteral("org.dbusqml.Precedence"));
    QVERIFY(spec.has_value());
    QVERIFY2(spec->signals_.contains(QStringLiteral("SystemTierMarker")),
             "the freedesktop-standard dbus-1/interfaces dir must be scanned");
    QVERIFY(!spec->signals_.contains(QStringLiteral("UserTierMarker")));

    // Restore the process environment and the bundled catalog.
    if (savedDataHome.isEmpty())
        qunsetenv("XDG_DATA_HOME");
    else
        qputenv("XDG_DATA_HOME", savedDataHome);
    DBusCatalog::instance().reload();
}

// Loader robustness: a malformed XML file in a scanned directory warns and is
// skipped — the whole file is discarded, no partial interface leaks in.
void TestDBusAdaptor::testCatalogMalformedXmlSkipped() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile f(dir.filePath(QStringLiteral("broken.xml")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("<node><interface name=\"org.dbusqml.Broken\"><method name=\"M\">");
        // deliberately unterminated
    }
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral("DBusCatalog: XML error")));
    qputenv("DBUSQML_TYPES_PATH", dir.path().toLocal8Bit());
    DBusCatalog::instance().reload();
    QVERIFY(!DBusCatalog::instance().lookup(QStringLiteral("org.dbusqml.Broken")).has_value());
    qunsetenv("DBUSQML_TYPES_PATH");
    DBusCatalog::instance().reload();
}

// ==================== 0.9.0 PropertiesChanged / relay retirement ===========

// A6 — the consumer-outcome test: a DBus proxy in the same process binds an
// adaptor property; the adaptor changes it; the proxy's QML-visible value
// updates via org.freedesktop.DBus.Properties.PropertiesChanged. 0.8.0: the
// server never emits PropertiesChanged, so the value never updates.
void TestDBusAdaptor::testPropertiesChangedReachesProxy() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent adaptorComp(&engine);
    adaptorComp.setData("import DBus 1.0\n"
                        "DBusAdaptor {\n"
                        "  service: 'org.dbusqml.Reactive'\n"
                        "  path: '/Reactive'\n"
                        "  iface: 'org.dbusqml.Reactive'\n"
                        "  property int alpha: 1\n"
                        "  function ping() { return 'p' }\n"
                        "}",
                        QUrl());
    QVERIFY2(adaptorComp.isReady(), qPrintable(adaptorComp.errorString()));
    QPointer<DBusAdaptor> adaptor = qobject_cast<DBusAdaptor *>(adaptorComp.create());
    QVERIFY(adaptor != nullptr);

    QQmlComponent proxyComp(&engine);
    proxyComp.setData("import DBus 1.0\n"
                      "DBus {\n"
                      "  service: 'org.dbusqml.Reactive'\n"
                      "  path: '/Reactive'\n"
                      "  iface: 'org.dbusqml.Reactive'\n"
                      "}",
                      QUrl());
    QVERIFY2(proxyComp.isReady(), qPrintable(proxyComp.errorString()));
    QObject *proxy = proxyComp.create();
    QVERIFY(proxy != nullptr);

    // Wait for the proxy to become Ready (GetAll + subscription complete).
    QTRY_COMPARE_WITH_TIMEOUT(proxy->property("status").toInt(), 2, 5000);
    QCOMPARE(proxy->property("alpha").toInt(), 1);

    // The adaptor changes the property — the proxy must observe it.
    adaptor->setProperty("alpha", 42);
    QTRY_COMPARE_WITH_TIMEOUT(proxy->property("alpha").toInt(), 42, 5000);

    delete proxy;
}

// A5 — property notify signals are no longer relayed onto the bus as
// broadcast signals. 0.8.0: alphaChanged fires on the wire.
void TestDBusAdaptor::testNotifyRelaysRemoved() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.NoRelay'\n"
                      "  path: '/NoRelay'\n"
                      "  iface: 'org.dbusqml.NoRelay'\n"
                      "  property int alpha: 1\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    MatrixCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.NoRelay"), QStringLiteral("/NoRelay"),
                        QStringLiteral("org.dbusqml.NoRelay"), QStringLiteral("alphaChanged"),
                        &catcher, SLOT(onSignal(QDBusMessage))));

    adaptor->setProperty("alpha", 42);
    QTest::qWait(500);
    QCOMPARE(catcher.count, 0);
    bus.disconnect(QStringLiteral("org.dbusqml.NoRelay"), QStringLiteral("/NoRelay"),
                   QStringLiteral("org.dbusqml.NoRelay"), QStringLiteral("alphaChanged"), &catcher,
                   SLOT(onSignal(QDBusMessage)));
    delete adaptor;
}

// Q4 — QML-declared plain signals broadcast under their folded wire name
// (SomethingHappened), and introspection advertises the same. 0.8.0: the
// lowercase QML name goes on the wire.
void TestDBusAdaptor::testPlainSignalFoldsOnWire() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.FoldSig'\n"
                      "  path: '/FoldSig'\n"
                      "  iface: 'org.dbusqml.FoldSig'\n"
                      "  signal somethingHappened()\n"
                      "  function fire() { somethingHappened() }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *adaptor = component.create();
    QVERIFY(adaptor != nullptr);
    QTest::qWait(300);

    MatrixCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.FoldSig"), QStringLiteral("/FoldSig"),
                        QStringLiteral("org.dbusqml.FoldSig"), QStringLiteral("SomethingHappened"),
                        &catcher, SLOT(onSignal(QDBusMessage))));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.FoldSig"), QStringLiteral("/FoldSig"),
        QStringLiteral("org.dbusqml.FoldSig"), QStringLiteral("fire"));
    QCOMPARE(bus.call(call, QDBus::Block, 3000).type(), QDBusMessage::ReplyMessage);

    for (int i = 0; i < 20 && catcher.count == 0; ++i)
        QTest::qWait(100);
    QCOMPARE(catcher.count, 1);
    QCOMPARE(catcher.last.member(), QStringLiteral("SomethingHappened"));
    delete adaptor;
}

// Co-located same-iface adaptors must introspect CLEANLY: no duplicate
// signals (the busctl-reject class) and no library base-class member in the
// served XML.
void TestDBusAdaptor::testCoLocatedIntrospectionClean() {
    const QByteArray qmlA = "import DBus 1.0\n"
                            "DBusAdaptor {\n"
                            "  service: 'org.dbusqml.CoClean'\n"
                            "  path: '/CoClean'\n"
                            "  iface: 'org.dbusqml.CoClean'\n"
                            "  property int alpha: 1\n"
                            "  function ping() { return 'a' }\n"
                            "}";
    QObject *a = createQmlAdaptor(qmlA);
    QVERIFY(a != nullptr);
    QObject *b = createQmlAdaptor(qmlA);
    QVERIFY(b != nullptr);
    QTest::qWait(300);

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.CoClean"), QStringLiteral("/CoClean"),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    QDBusMessage reply = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QString xml = reply.arguments().first().toString();

    // No library base-class member may appear — by name or wire-cased fold.
    const QStringList forbidden = {QStringLiteral("nameAcquired"),
                                   QStringLiteral("nameLost"),
                                   QStringLiteral("NameAcquired"),
                                   QStringLiteral("NameLost"),
                                   QStringLiteral("AllowReplacement"),
                                   QStringLiteral("ReplaceExisting"),
                                   QStringLiteral("QueueOnBusy"),
                                   QStringLiteral("signaturesChanged"),
                                   QStringLiteral("SignaturesChanged"),
                                   QStringLiteral("_signalsChanged"),
                                   QStringLiteral("_membersChanged"),
                                   QStringLiteral("serviceChanged"),
                                   QStringLiteral("pathChanged"),
                                   QStringLiteral("ifaceChanged"),
                                   QStringLiteral("connectionChanged"),
                                   QStringLiteral("destroyed"),
                                   QStringLiteral("objectNameChanged"),
                                   QStringLiteral("deleteLater"),
                                   QStringLiteral("Service"),
                                   QStringLiteral("Path"),
                                   QStringLiteral("Iface"),
                                   QStringLiteral("Connection")};
    for (const QString &bad : forbidden)
        QVERIFY2(!xml.contains(bad), qPrintable(QStringLiteral("leaked: %1\n%2").arg(bad, xml)));

    // No duplicate <signal name="..."> — the busctl-reject class.
    QSet<QString> seen;
    QXmlStreamReader xr(xml);
    while (!xr.atEnd()) {
        if (xr.readNext() == QXmlStreamReader::StartElement &&
            xr.name() == QLatin1String("signal")) {
            const QString n = xr.attributes().value(QStringLiteral("name")).toString();
            QVERIFY2(!seen.contains(n),
                     qPrintable(QStringLiteral("duplicate signal %1\n%2").arg(n, xml)));
            seen.insert(n);
        }
    }
    QVERIFY2(!xr.hasError(), qPrintable(xr.errorString()));

    delete a;
    delete b;
}

// ==================== 0.9.0 built-in collision prevention ==================

// Q1 — shadowing a built-in property (service/path/iface/connection) is a
// load-time error (FINAL), not a silently broken adaptor. 0.8.0: loads fine.
void TestDBusAdaptor::testBuiltinShadowFailsToLoad() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.Shadow'\n"
                      "  iface: 'org.dbusqml.Shadow'\n"
                      "  property string path\n"
                      "  function ping() { return 'p' }\n"
                      "}",
                      QUrl());
    QObject *adaptor = component.create();
    QVERIFY2(adaptor == nullptr, "shadowing a built-in property must fail to load");
    QVERIFY2(component.errorString().contains(QStringLiteral("FINAL")),
             qPrintable(component.errorString()));
}

// A7a — a method folding onto a library mechanism name warns at load time and
// points at the _members escape hatch. 0.8.0: silent.
void TestDBusAdaptor::testMemberCollisionWarns() {
    QTest::ignoreMessage(
        QtInfoMsg, QRegularExpression(QStringLiteral(
                       "member cannot be served under this name.*declare an alias in `_members`")));
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.Collide'\n"
                                        "  path: '/Collide'\n"
                                        "  iface: 'org.dbusqml.Collide'\n"
                                        "  function unregister() { return 'x' }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);
    delete adaptor;
}

// ==================== 0.9.0 call options / send / containers ================

// B2 — per-call timeout: a deliberately-slow service (held reply, no settle)
// answers the caller with NoReply at the CONFIGURED timeout, not Qt's 25 s.
void TestDBusAdaptor::testCallTimeout() {
    QObject *adaptor =
        createQmlAdaptor("import DBus 1.0\n"
                         "DBusAdaptor {\n"
                         "  service: 'org.dbusqml.Timeout'\n"
                         "  path: '/Timeout'\n"
                         "  iface: 'org.freedesktop.impl.portal.FileChooser'\n"
                         "  function openFile(handle, appId, parentWindow, title, options) {\n"
                         "    holdReply()\n"
                         "  }\n"
                         "}");
    QVERIFY(adaptor != nullptr);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);

    DBusMessage m;
    m.setService(QStringLiteral("org.dbusqml.Timeout"));
    m.setPath(QStringLiteral("/Timeout"));
    m.setIface(QStringLiteral("org.freedesktop.impl.portal.FileChooser"));
    m.setMember(QStringLiteral("OpenFile"));
    m.setArguments({QVariant::fromValue(QDBusObjectPath(QStringLiteral("/req/1"))),
                    QVariant(QStringLiteral("app")), QVariant(QString()),
                    QVariant(QStringLiteral("t")), QVariantMap{}});
    m.setTimeout(500);

    DBusPendingReply *reply = conn->asyncCall(m);
    QVERIFY(reply != nullptr);
    QSignalSpy spy(reply, &DBusPendingReply::finished);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY(reply->isError());
    QCOMPARE(reply->error().name(), QStringLiteral("org.freedesktop.DBus.Error.NoReply"));

    delete reply;
    delete conn;
}

// N2 — fire-and-forget send: the message executes server-side and nothing is
// sent back.
void TestDBusAdaptor::testFireAndForgetSend() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.FaF'\n"
                                        "  path: '/FaF'\n"
                                        "  iface: 'org.dbusqml.FaF'\n"
                                        "  property string lastCall: ''\n"
                                        "  function ping() { lastCall = 'pinged'; return 0 }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);

    DBusMessage m;
    m.setService(QStringLiteral("org.dbusqml.FaF"));
    m.setPath(QStringLiteral("/FaF"));
    m.setIface(QStringLiteral("org.dbusqml.FaF"));
    m.setMember(QStringLiteral("ping"));
    conn->send(m);

    QTRY_COMPARE_WITH_TIMEOUT(adaptor->property("lastCall").toString(), QStringLiteral("pinged"),
                              3000);
    delete conn;
}

// The message gadget's call-option members exist with their documented
// defaults and ride the wire without breaking anything.
void TestDBusAdaptor::testMessageGadgetCallOptions() {
    DBusMessage m;
    QCOMPARE(m.timeout(), -1);
    QVERIFY(m.autoStart());
    QVERIFY(!m.interactiveAuthorization());

    m.setTimeout(1234);
    m.setInteractiveAuthorization(true);
    m.setAutoStart(false);
    QCOMPARE(m.timeout(), 1234);
    QVERIFY(m.interactiveAuthorization());
    QVERIFY(!m.autoStart());

    // The flags survive into an actual call (a served method runs).
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.MsgFlags'\n"
                                        "  path: '/MsgFlags'\n"
                                        "  iface: 'org.dbusqml.MsgFlags'\n"
                                        "  property string lastCall: ''\n"
                                        "  function ping() { lastCall = 'pinged'; return 0 }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);
    DBusMessage msg;
    msg.setService(QStringLiteral("org.dbusqml.MsgFlags"));
    msg.setPath(QStringLiteral("/MsgFlags"));
    msg.setIface(QStringLiteral("org.dbusqml.MsgFlags"));
    msg.setMember(QStringLiteral("ping"));
    msg.setInteractiveAuthorization(true);
    msg.setAutoStart(false);
    msg.setTimeout(3000);
    conn->send(msg);
    QTRY_COMPARE_WITH_TIMEOUT(adaptor->property("lastCall").toString(), QStringLiteral("pinged"),
                              3000);
    delete conn;
}

// N3 — multi-element nested containers round-trip (aa{sv} and aai with ≥2
// elements; KDE's encoder drops everything past element 0 — pinned here).
void TestDBusAdaptor::testNestedContainerRoundTrip() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.Nested'\n"
                                        "  path: '/Nested'\n"
                                        "  iface: 'org.dbusqml.Nested'\n"
                                        "  function echo(v) { return v }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);

    // N3 pin — aa{sv} with two elements (caller-side note: pass the outer
    // array as ONE QVariant argument — a braced {QVariantList{...}} argument
    // copy-constructs the parameter list and flattens it).
    {
        DBusMessage m;
        m.setService(QStringLiteral("org.dbusqml.Nested"));
        m.setPath(QStringLiteral("/Nested"));
        m.setIface(QStringLiteral("org.dbusqml.Nested"));
        m.setMember(QStringLiteral("echo"));
        m.setSignature(QStringLiteral("aa{sv}"));
        QVariantMap first{{QStringLiteral("a"), 1}};
        QVariantMap second{{QStringLiteral("b"), 2}};
        QVariantList aasvArgs;
        aasvArgs.append(QVariant(QVariantList{first, second}));
        m.setArguments(aasvArgs);
        DBusPendingReply *reply = conn->asyncCall(m);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
        QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
        const QVariantList outer = reply->values();
        QCOMPARE(outer.size(), 1);
        const QVariantList arr = unwrapDbus(outer.first()).toList();
        QCOMPARE(arr.size(), 2);
        QCOMPARE(unwrapDbus(arr.first()).toMap().value(QStringLiteral("a")).toInt(), 1);
        QCOMPARE(unwrapDbus(arr.at(1)).toMap().value(QStringLiteral("b")).toInt(), 2);
        delete reply;
    }

    // aai with two rows.
    {
        DBusMessage m;
        m.setService(QStringLiteral("org.dbusqml.Nested"));
        m.setPath(QStringLiteral("/Nested"));
        m.setIface(QStringLiteral("org.dbusqml.Nested"));
        m.setMember(QStringLiteral("echo"));
        m.setSignature(QStringLiteral("aai"));
        QVariantList aaiArgs;
        aaiArgs.append(QVariant(QVariantList{QVariantList{1, 2}, QVariantList{3, 4}}));
        m.setArguments(aaiArgs);
        DBusPendingReply *reply = conn->asyncCall(m);
        QSignalSpy spy(reply, &DBusPendingReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
        QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
        const QVariantList outer = reply->values();
        QCOMPARE(outer.size(), 1);
        const QVariantList arr = unwrapDbus(outer.first()).toList();
        QCOMPARE(arr.size(), 2);
        const QVariantList row0 = unwrapDbus(arr.first()).toList();
        QCOMPARE(row0.size(), 2);
        QCOMPARE(row0.at(0).toInt(), 1);
        QCOMPARE(row0.at(1).toInt(), 2);
        const QVariantList row1 = unwrapDbus(arr.at(1)).toList();
        QCOMPARE(row1.at(0).toInt(), 3);
        QCOMPARE(row1.at(1).toInt(), 4);
        delete reply;
    }

    delete conn;
}

// ==================== 0.9.0 named error replies (S2) =======================

// A handler throwing DBusQML.DBusUtils.error(name, message) produces an error
// reply carrying exactly that name and message. Pre-change: silent empty reply
// (the JS error fell through to the C++ invoke path, which re-ran the
// handler).
void TestDBusAdaptor::testNamedErrorReply() {
    QObject *adaptor =
        createQmlAdaptor("import DBus 1.0\n"
                         "import DBus 1.0 as DBusQML\n"
                         "DBusAdaptor {\n"
                         "  service: 'org.dbusqml.NamedErr'\n"
                         "  path: '/NamedErr'\n"
                         "  iface: 'org.dbusqml.NamedErr'\n"
                         "  function boom() {\n"
                         "    throw DBusQML.DBusUtils.error('org.dbusqml.TestError', 'nope')\n"
                         "  }\n"
                         "  property string afterThrow: 'untouched'\n"
                         "  function ping() { afterThrow = 'pinged'; return 'pong' }\n"
                         "}");
    QVERIFY(adaptor != nullptr);

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.NamedErr"), QStringLiteral("/NamedErr"),
        QStringLiteral("org.dbusqml.NamedErr"), QStringLiteral("boom"));
    QDBusMessage reply = bus.call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.dbusqml.TestError"));
    QCOMPARE(reply.errorMessage(), QStringLiteral("nope"));

    // The service is alive and dispatch is not corrupted by the throw.
    QDBusMessage ping = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.NamedErr"), QStringLiteral("/NamedErr"),
        QStringLiteral("org.dbusqml.NamedErr"), QStringLiteral("ping"));
    QDBusMessage pr = bus.call(ping, QDBus::Block, 3000);
    QCOMPARE(pr.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(pr.arguments().first().toString(), QStringLiteral("pong"));
}

// Any other thrown value becomes org.freedesktop.DBus.Error.Failed with the
// exception message.
void TestDBusAdaptor::testPlainExceptionFailedReply() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.PlainErr'\n"
                                        "  path: '/PlainErr'\n"
                                        "  iface: 'org.dbusqml.PlainErr'\n"
                                        "  function boom() { throw new Error('broken') }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.PlainErr"), QStringLiteral("/PlainErr"),
        QStringLiteral("org.dbusqml.PlainErr"), QStringLiteral("boom"));
    QDBusMessage reply = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.Failed"));
    QVERIFY2(reply.errorMessage().contains(QStringLiteral("broken")),
             qPrintable(reply.errorMessage()));
}

// ==================== 0.9.0 service-name acquisition (S1) ==================

// S1 — two connections contend for one well-known name: the winner's
// nameAcquired fires, the loser's nameLost fires. A claims with
// allowReplacement; B takes the name with replaceExisting.
void TestDBusAdaptor::testServiceAcquisitionTakeover() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    // A: session bus, replaceable.
    QQmlComponent compA(&engine);
    compA.setData("import DBus 1.0\n"
                  "DBusAdaptor {\n"
                  "  service: 'org.dbusqml.Acquire'\n"
                  "  path: '/AcquireA'\n"
                  "  iface: 'org.dbusqml.Acquire'\n"
                  "  allowReplacement: true\n"
                  "  function ping() { return 'a' }\n"
                  "}",
                  QUrl());
    QVERIFY2(compA.isReady(), qPrintable(compA.errorString()));
    // beginCreate: the spy must be connected BEFORE completion (the flagged
    // claim's nameAcquired fires during/just after componentComplete).
    QObject *a = compA.beginCreate(engine.rootContext());
    QVERIFY(a != nullptr);
    a->setProperty("allowReplacement", true);

    QSignalSpy acquiredA(a, SIGNAL(nameAcquired()));
    QSignalSpy lostA(a, SIGNAL(nameLost()));
    compA.completeCreate();
    QTRY_VERIFY_WITH_TIMEOUT(acquiredA.count() >= 1, 5000); // async flagged claim
    QCOMPARE(lostA.count(), 0);

    // B: custom connection, takes the name over.
    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *connB = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(connB != nullptr);

    QQmlComponent compB(&engine);
    compB.setData("import DBus 1.0\n"
                  "DBusAdaptor {\n"
                  "  service: 'org.dbusqml.Acquire'\n"
                  "  path: '/AcquireB'\n"
                  "  iface: 'org.dbusqml.Acquire'\n"
                  "  replaceExisting: true\n"
                  "  function ping() { return 'b' }\n"
                  "}",
                  QUrl());
    QVERIFY2(compB.isReady(), qPrintable(compB.errorString()));
    QObject *b = compB.beginCreate(engine.rootContext());
    QVERIFY(b != nullptr);
    b->setProperty("connection", QVariant::fromValue<DBusConnection *>(connB));
    b->setProperty("replaceExisting", true);

    QSignalSpy acquiredB(b, SIGNAL(nameAcquired()));
    compB.completeCreate();
    QTRY_VERIFY_WITH_TIMEOUT(acquiredB.count() >= 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(lostA.count() >= 1, 5000);

    // The name now routes to B.
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.Acquire"), QStringLiteral("/AcquireB"),
        QStringLiteral("org.dbusqml.Acquire"), QStringLiteral("ping"));
    // Async: the reply arrives on this connection while the custom
    // connection's socket (B's adaptor) is pumped by the main loop.
    QDBusPendingCallWatcher *route =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m));
    QSignalSpy routeSpy(route, &QDBusPendingCallWatcher::finished);
    QTRY_COMPARE_WITH_TIMEOUT(routeSpy.count(), 1, 5000);
    QVERIFY(!route->isError());
    QCOMPARE(route->reply().arguments().first().toString(), QStringLiteral("b"));
    delete route;

    delete b;
    delete connB;
    delete a;
}

// ==================== 0.9.0 standalone watcher elements (N1) ===============

class WatcherCatcher : public QObject {
    Q_OBJECT
public:
    QString lastMember;
    QVariantList lastArgs;
    int count = 0;
public slots:
    void onReceived(const QString &member, const QVariantList &args) {
        lastMember = member;
        lastArgs = args;
        ++count;
    }
};

// A signal watcher delivers a test adaptor's emitted signal with no proxy and
// no introspection requirement.
void TestDBusAdaptor::testSignalWatcherDelivers() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.SigWatch'\n"
                                        "  path: '/SigWatch'\n"
                                        "  iface: 'org.dbusqml.SigWatch'\n"
                                        "  function fire() {\n"
                                        "    emitSignal('StateChanged', ['/session/1', 2])\n"
                                        "  }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusSignalWatcher {\n"
                      "  service: 'org.dbusqml.SigWatch'\n"
                      "  path: '/SigWatch'\n"
                      "  iface: 'org.dbusqml.SigWatch'\n"
                      "  member: 'StateChanged'\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *watcher = component.create();
    QVERIFY(watcher != nullptr);

    WatcherCatcher catcher;
    QVERIFY(QObject::connect(watcher, SIGNAL(received(QString, QVariantList)), &catcher,
                             SLOT(onReceived(QString, QVariantList))));

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.SigWatch"), QStringLiteral("/SigWatch"),
        QStringLiteral("org.dbusqml.SigWatch"), QStringLiteral("fire"));
    QCOMPARE(QDBusConnection::sessionBus().call(m, QDBus::Block, 3000).type(),
             QDBusMessage::ReplyMessage);

    QTRY_COMPARE_WITH_TIMEOUT(catcher.count, 1, 5000);
    QCOMPARE(catcher.lastMember, QStringLiteral("StateChanged"));
    QCOMPARE(catcher.lastArgs.size(), 2);
    QCOMPARE(catcher.lastArgs.first().toString(), QStringLiteral("/session/1"));
    QCOMPARE(catcher.lastArgs.at(1).toInt(), 2);

    delete watcher;
}

// An empty member is a wildcard: every member of the interface is delivered.
void TestDBusAdaptor::testSignalWatcherWildcardMember() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.SigWatchW'\n"
                                        "  path: '/SigWatchW'\n"
                                        "  iface: 'org.dbusqml.SigWatchW'\n"
                                        "  function fireA() { emitSignal('SigA', []) }\n"
                                        "  function fireB() { emitSignal('SigB', []) }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusSignalWatcher {\n"
                      "  service: 'org.dbusqml.SigWatchW'\n"
                      "  path: '/SigWatchW'\n"
                      "  iface: 'org.dbusqml.SigWatchW'\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *watcher = component.create();
    QVERIFY(watcher != nullptr);

    WatcherCatcher catcher;
    QVERIFY(QObject::connect(watcher, SIGNAL(received(QString, QVariantList)), &catcher,
                             SLOT(onReceived(QString, QVariantList))));

    QDBusConnection bus = QDBusConnection::sessionBus();
    for (const char *member : {"fireA", "fireB"}) {
        QDBusMessage m = QDBusMessage::createMethodCall(
            QStringLiteral("org.dbusqml.SigWatchW"), QStringLiteral("/SigWatchW"),
            QStringLiteral("org.dbusqml.SigWatchW"), QString::fromLatin1(member));
        QCOMPARE(bus.call(m, QDBus::Block, 3000).type(), QDBusMessage::ReplyMessage);
    }

    QTRY_COMPARE_WITH_TIMEOUT(catcher.count, 2, 5000);
    QCOMPARE(catcher.lastMember, QStringLiteral("SigB"));
    delete watcher;
}

// The service watcher observes a service appearing and disappearing.
void TestDBusAdaptor::testServiceWatcherAppearDisappear() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusServiceWatcher {\n"
                      "  service: 'org.dbusqml.SvcWatch'\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *watcher = component.create();
    QVERIFY(watcher != nullptr);

    QCOMPARE(watcher->property("registered").toBool(), false);

    struct Change {
        QString oldOwner;
        QString newOwner;
    };
    QList<Change> changes;
    QObject::connect(static_cast<DBusServiceWatcher *>(watcher), &DBusServiceWatcher::ownerChanged,
                     watcher,
                     [&changes](const QString &o, const QString &n) { changes.append({o, n}); });

    // The adaptor appears.
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.SvcWatch'\n"
                                        "  path: '/SvcWatch'\n"
                                        "  iface: 'org.dbusqml.SvcWatch'\n"
                                        "  function ping() { return 'p' }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    QTRY_COMPARE_WITH_TIMEOUT(watcher->property("registered").toBool(), true, 5000);
    QVERIFY(!changes.isEmpty());
    QVERIFY(changes.last().newOwner.startsWith(QStringLiteral(":1.")));

    // And disappears.
    delete adaptor;
    QTRY_COMPARE_WITH_TIMEOUT(watcher->property("registered").toBool(), false, 5000);
    QVERIFY(changes.last().newOwner.isEmpty());
}

// ==================== 0.9.0 ObjectManager client (B6) ======================

// A test ObjectManager service built on a served adaptor: GetManagedObjects
// returns the inventory (declared a{oa{sv}} via _signatures); live changes
// are broadcast as InterfacesAdded/InterfacesRemoved.
void TestDBusAdaptor::testObjectManagerClient() {
    QObject *server = createQmlAdaptor(
        "import DBus 1.0\n"
        "import DBus 1.0 as DBusQML\n"
        "DBusAdaptor {\n"
        "  service: 'org.dbusqml.ObjMgr'\n"
        "  path: '/ObjMgr'\n"
        "  iface: 'org.freedesktop.DBus.ObjectManager'\n"
        "  _members: ({ GetManagedObjects: 'getManagedObjects' })\n"
        "  _signatures: ({ GetManagedObjects: 'a{oa{sv}}' })\n"
        "  property var objects: ({ '/org/obj/1': { 'org.dbusqml.Device': { name: 'one' } } })\n"
        "  function getManagedObjects() { return objects }\n"
        "  function add(path, iface, props) {\n"
        "    var next = {}\n"
        "    for (var k in objects) next[k] = objects[k]\n"
        "    var ifaces = {}\n"
        "    ifaces[iface] = props\n"
        "    next[path] = ifaces\n"
        "    objects = next\n"
        "    emitSignal('InterfacesAdded', [path, ifaces])\n"
        "  }\n"
        "  function remove(path, iface) {\n"
        "    var next = {}\n"
        "    for (var k in objects) if (k !== path) next[k] = objects[k]\n"
        "    objects = next\n"
        "    var list = [iface]\n"
        "    emitSignal('InterfacesRemoved', [path, list])\n"
        "  }\n"
        "}");
    QVERIFY(server != nullptr);

    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));
    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusObjectManager {\n"
                      "  service: 'org.dbusqml.ObjMgr'\n"
                      "  path: '/ObjMgr'\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *client = component.create();
    QVERIFY(client != nullptr);

    // Initial inventory.
    QTRY_COMPARE_WITH_TIMEOUT(client->property("ready").toBool(), true, 5000);
    const QVariantMap initial = client->property("managedObjects").toMap();
    QCOMPARE(initial.size(), 1);
    const QVariantMap obj1 = initial.value(QStringLiteral("/org/obj/1")).toMap();
    QVERIFY(obj1.contains(QStringLiteral("org.dbusqml.Device")));

    // Live add.
    struct Added {
        QString path;
        QVariantMap ifaces;
    };
    QList<Added> addedList;
    QObject::connect(static_cast<DBusObjectManager *>(client), &DBusObjectManager::interfacesAdded,
                     client, [&addedList](const QString &p, const QVariantMap &ifaces) {
                         addedList.append({p, ifaces});
                     });

    QDBusMessage add = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.ObjMgr"), QStringLiteral("/ObjMgr"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("add"));
    add.setArguments({QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/obj/2"))),
                      QVariant(QStringLiteral("org.dbusqml.Device")),
                      QVariant(QVariantMap{{QStringLiteral("name"), QStringLiteral("two")}})});
    QCOMPARE(QDBusConnection::sessionBus().call(add, QDBus::Block, 3000).type(),
             QDBusMessage::ReplyMessage);

    QTRY_COMPARE_WITH_TIMEOUT(addedList.count(), 1, 5000);
    QCOMPARE(addedList.first().path, QStringLiteral("/org/obj/2"));
    QVERIFY(addedList.first().ifaces.contains(QStringLiteral("org.dbusqml.Device")));
    const QVariantMap now = client->property("managedObjects").toMap();
    QCOMPARE(now.size(), 2);

    // Live remove.
    struct Removed {
        QString path;
        QStringList ifaces;
    };
    QList<Removed> removedList;
    QObject::connect(static_cast<DBusObjectManager *>(client),
                     &DBusObjectManager::interfacesRemoved, client,
                     [&removedList](const QString &p, const QStringList &ifaces) {
                         removedList.append({p, ifaces});
                     });

    QDBusMessage rem = QDBusMessage::createMethodCall(
        QStringLiteral("org.dbusqml.ObjMgr"), QStringLiteral("/ObjMgr"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("remove"));
    rem.setArguments({QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/obj/1"))),
                      QVariant(QStringLiteral("org.dbusqml.Device"))});
    QCOMPARE(QDBusConnection::sessionBus().call(rem, QDBus::Block, 3000).type(),
             QDBusMessage::ReplyMessage);

    QTRY_COMPARE_WITH_TIMEOUT(removedList.count(), 1, 5000);
    QCOMPARE(removedList.first().path, QStringLiteral("/org/obj/1"));
    const QVariantMap after = client->property("managedObjects").toMap();
    QCOMPARE(after.size(), 1);
    QVERIFY(after.contains(QStringLiteral("/org/obj/2")));

    delete client;
}

// ==================== 0.9.0 lossless 64-bit delivery (C2) ==================

// Lossless 64-bit round-trip: the client sends MAX_INT64 as a decimal string
// with a declared 'x'; the adaptor echoes; the value arrives back as the same
// full-precision decimal STRING (no double round-trip).
void TestDBusAdaptor::testInt64StringRoundTrip() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.I64'\n"
                                        "  path: '/I64'\n"
                                        "  iface: 'org.dbusqml.I64'\n"
                                        "  function echo(v) { return v }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);

    const QString maxI64 = QStringLiteral("9223372036854775807");
    DBusMessage m;
    m.setService(QStringLiteral("org.dbusqml.I64"));
    m.setPath(QStringLiteral("/I64"));
    m.setIface(QStringLiteral("org.dbusqml.I64"));
    m.setMember(QStringLiteral("echo"));
    m.setSignature(QStringLiteral("x"));
    m.setArguments({maxI64});
    DBusPendingReply *reply = conn->asyncCall(m);
    QSignalSpy spy(reply, &DBusPendingReply::finished);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
    QCOMPARE(reply->values().size(), 1);
    // The echoed value arrives as the full-precision decimal string.
    QCOMPARE(reply->values().first().toString(), maxI64);

    delete reply;
    delete conn;
}

// Values within 2^53 stay plain JS numbers (no churn for the common case):
// 42 sent as declared 'x' comes back as the number 42.
void TestDBusAdaptor::testInt64SmallValueStaysNumber() {
    QObject *adaptor = createQmlAdaptor("import DBus 1.0\n"
                                        "DBusAdaptor {\n"
                                        "  service: 'org.dbusqml.I64Small'\n"
                                        "  path: '/I64Small'\n"
                                        "  iface: 'org.dbusqml.I64Small'\n"
                                        "  function echo(v) { return v }\n"
                                        "}");
    QVERIFY(adaptor != nullptr);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);

    DBusMessage m;
    m.setService(QStringLiteral("org.dbusqml.I64Small"));
    m.setPath(QStringLiteral("/I64Small"));
    m.setIface(QStringLiteral("org.dbusqml.I64Small"));
    m.setMember(QStringLiteral("echo"));
    m.setSignature(QStringLiteral("x"));
    m.setArguments({QStringLiteral("42")});
    DBusPendingReply *reply = conn->asyncCall(m);
    QSignalSpy spy(reply, &DBusPendingReply::finished);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
    QCOMPARE(reply->values().first().toInt(), 42);

    delete reply;
    delete conn;
}

// ==================== 0.9.0 unix fd passing (h; B1/S3) =====================

// Test adaptor with fd-aware methods: ReceiveFd writes through the received
// fd; GiveFd returns the held fd (declared 'h' out-signature); ReceiveFdArray
// counts the fds in a declared 'ah' array.
class FdAdaptor : public DBusAdaptor {
    Q_OBJECT
public:
    explicit FdAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

    Q_INVOKABLE QString receiveFd(const QVariant &fd) {
        const int rawFd = fd.toInt();
        if (rawFd < 0)
            return QStringLiteral("bad-fd");
        QFile f;
        if (!f.open(rawFd, QIODevice::WriteOnly))
            return QStringLiteral("open-failed");
        if (f.write("hello-fd") != 8)
            return QStringLiteral("write-failed");
        f.close(); // we wrote; the fd still belongs to the caller
        return QStringLiteral("ok");
    }

    int heldFd = -1;
    Q_INVOKABLE int giveFd() { return heldFd; }

    Q_INVOKABLE int receiveFdArray(const QVariant &fdsV) {
        // 'ah' — array of fds; count valid ints. (The C++ dispatch path
        // passes call args as QVariant — convert inside.)
        const QVariantList fds = fdsV.toList();
        int valid = 0;
        for (const QVariant &fd : fds)
            if (fd.toInt() >= 0)
                ++valid;
        return valid;
    }
};

// fd round-trip, send side: a pipe write-end sent as declared 'h'; the
// adaptor writes through the received fd and the test reads the content.
void TestDBusAdaptor::testFdRoundTripSend() {
    FdAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.Fd"));
    adaptor.setPath(QStringLiteral("/Fd"));
    adaptor.setIface(QStringLiteral("org.dbusqml.Fd"));
    adaptor.classBegin();
    adaptor.componentComplete();

    int fds[2];
    QVERIFY(pipe(fds) == 0);

    DBusMessage m;
    m.setService(QStringLiteral("org.dbusqml.Fd"));
    m.setPath(QStringLiteral("/Fd"));
    m.setIface(QStringLiteral("org.dbusqml.Fd"));
    m.setMember(QStringLiteral("ReceiveFd"));
    m.setSignature(QStringLiteral("h"));
    QVariantList args;
    args.append(fds[1]); // int fd — the plain-int send shape
    m.setArguments(args);
    m.setTimeout(3000);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);
    DBusPendingReply *reply = conn->asyncCall(m);
    QSignalSpy spy(reply, &DBusPendingReply::finished);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
    QCOMPARE(reply->value().toString(), QStringLiteral("ok"));

    // Read what the adaptor wrote through the fd.
    close(fds[1]);
    char buf[16] = {};
    const ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    QCOMPARE(int(n), 8);
    QCOMPARE(QByteArray(buf, 8), QByteArrayLiteral("hello-fd"));
    close(fds[0]);

    delete reply;
    delete conn;
}

// fd round-trip, receive side: the adaptor's GiveFd returns its held fd
// (declared 'h' out via _signatures); the client reads the content through
// the received int fd, then closes it (receiver closes — the lifetime
// contract).
void TestDBusAdaptor::testFdRoundTripReceive() {
    FdAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.FdGive"));
    adaptor.setPath(QStringLiteral("/FdGive"));
    adaptor.setIface(QStringLiteral("org.dbusqml.FdGive"));
    adaptor.setSignatures(QVariantMap{{QStringLiteral("GiveFd"), QStringLiteral("h")}});
    adaptor.classBegin();
    adaptor.componentComplete();

    int fds[2];
    QVERIFY(pipe(fds) == 0);
    QVERIFY(write(fds[1], "give-fd", 7) == 7);
    close(fds[1]);
    adaptor.heldFd = fds[0]; // the adaptor "owns" the read end

    DBusMessage m;
    m.setService(QStringLiteral("org.dbusqml.FdGive"));
    m.setPath(QStringLiteral("/FdGive"));
    m.setIface(QStringLiteral("org.dbusqml.FdGive"));
    m.setMember(QStringLiteral("GiveFd"));
    m.setTimeout(3000);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);
    DBusPendingReply *reply = conn->asyncCall(m);
    QSignalSpy spy(reply, &DBusPendingReply::finished);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
    QCOMPARE(reply->values().size(), 1);
    const int receivedFd = reply->values().first().toInt();
    QVERIFY(receivedFd >= 0);

    // The client reads through the received fd, then closes it.
    char buf[16] = {};
    const ssize_t n = read(receivedFd, buf, sizeof(buf) - 1);
    QCOMPARE(int(n), 7);
    QCOMPARE(QByteArray(buf, 7), QByteArrayLiteral("give-fd"));
    close(receivedFd);
    close(fds[0]);

    delete reply;
    delete conn;
}

// Container-position fds: a declared 'ah' array arrives as a list of ints.
void TestDBusAdaptor::testFdContainerPosition() {
    FdAdaptor adaptor;
    adaptor.setService(QStringLiteral("org.dbusqml.FdArr"));
    adaptor.setPath(QStringLiteral("/Fd"));
    adaptor.setIface(QStringLiteral("org.dbusqml.Fd"));
    adaptor.classBegin();
    adaptor.componentComplete();

    int fds[2];
    QVERIFY(pipe(fds) == 0);

    DBusMessage m;
    m.setService(QStringLiteral("org.dbusqml.FdArr"));
    m.setPath(QStringLiteral("/Fd"));
    m.setIface(QStringLiteral("org.dbusqml.Fd"));
    m.setMember(QStringLiteral("ReceiveFdArray"));
    m.setSignature(QStringLiteral("ah"));
    QVariantList fdArgs;
    fdArgs.append(QVariant(QVariantList{fds[1], fds[1]})); // two fds
    m.setArguments(fdArgs);
    m.setTimeout(3000);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);
    DBusPendingReply *reply = conn->asyncCall(m);
    QSignalSpy spy(reply, &DBusPendingReply::finished);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
    QCOMPARE(reply->value().toInt(), 2);

    close(fds[1]);

    delete reply;
    delete conn;
}

// ==================== 0.9.0 fix cycle: cross-process fd + leak ==============

// Cross-process fd transfer: the helper service runs in a SEPARATE process
// (spawned binary), so fd numbers cannot coincide — no vacuous same-process
// pass. The test sends a pipe/file fd with a declared 'h' (client send
// direction), the helper writes through the received fd and replies with an
// fd of its own file (served reply direction); both directions are asserted
// byte-exact.
static void os_lseek_guard(int fd) {
    ::lseek(fd, 0, SEEK_SET);
}

void TestDBusAdaptor::testFdCrossProcess() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sendFilePath = dir.filePath(QStringLiteral("fd-send"));
    const QString replyFilePath = dir.filePath(QStringLiteral("fd-reply"));

    // The helper's reply file: created empty; the helper pre-writes the
    // reply payload into it at startup and returns its fd.
    {
        QFile f(replyFilePath);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }

    const QString service = QStringLiteral("org.dbusqml.FdXfer");
    const QString helperBin =
        QCoreApplication::applicationDirPath() + QStringLiteral("/fd_helper_service");

    QProcess helper;
    helper.setProgram(QStringLiteral("/bin/sh"));
    QStringList shArgs;
    shArgs << QStringLiteral("-c")
           << QStringLiteral("exec '%1' '%2' '%3'").arg(helperBin, replyFilePath, service);
    helper.setArguments(shArgs);
    helper.start();
    QVERIFY2(helper.waitForStarted(5000), "fd helper service failed to start");

    // Wait for the service to appear on the bus.
    QTRY_VERIFY_WITH_TIMEOUT(
        QDBusConnection::sessionBus().interface()->isServiceRegistered(service), 10000);

    // Open the send-side file empty, O_RDWR — the helper writes through the
    // fd we send.
    {
        QFile f(sendFilePath);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    const int sendFd = ::open(sendFilePath.toLocal8Bit().constData(), O_RDWR);
    QVERIFY(sendFd >= 0);

    DBusMessage m;
    m.setService(service);
    m.setPath(QStringLiteral("/FdXfer"));
    m.setIface(service);
    m.setMember(QStringLiteral("WriteThrough"));
    m.setSignature(QStringLiteral("h"));
    QVariantList args;
    args.append(sendFd);
    m.setArguments(args);
    m.setTimeout(5000);

    const QByteArray addr = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    DBusConnection *conn = DBusConnection::connectToBus(QString::fromLocal8Bit(addr));
    QVERIFY(conn != nullptr);
    DBusPendingReply *reply = conn->asyncCall(m);
    QSignalSpy spy(reply, &DBusPendingReply::finished);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 10000);
    QVERIFY2(!reply->isError(), qPrintable(reply->error().name()));
    QCOMPARE(reply->values().size(), 1);

    // Reply direction: the received fd IS the helper's reply fd (dup'd via
    // the daemon) — read the payload byte-exact.
    const int receivedFd = reply->values().first().toInt();
    QVERIFY2(receivedFd >= 0, "reply did not carry a usable fd");
    {
        char buf[64] = {};
        os_lseek_guard(receivedFd);
        const ssize_t n = ::read(receivedFd, buf, sizeof(buf) - 1);
        QCOMPARE(QByteArray(buf, int(n > 0 ? n : 0)), QByteArrayLiteral("fd-xfer-reply-ok\n"));
    }

    // Send direction: the helper wrote through OUR fd — read the file.
    {
        char buf[64] = {};
        os_lseek_guard(sendFd);
        const ssize_t n = ::read(sendFd, buf, sizeof(buf) - 1);
        QCOMPARE(QByteArray(buf, int(n > 0 ? n : 0)), QByteArrayLiteral("fd-xfer-send-ok\n"));
    }

    ::close(sendFd);
    ::close(receivedFd);
    delete reply;
    delete conn;
    helper.terminate();
    helper.waitForFinished(3000);
}

// ==================== Adaptor lifecycle (L1–L7, 0.7.0) ====================
//
// The LIFECYCLE axis (QML ownership/GC/destroy across dispatch). 0.6.0's JS
// dispatch path flipped the adaptor to CppOwnership unconditionally and
// permanently, making dynamically created per-call adaptors (the portal
// Request pattern — Component.createObject at a caller-chosen handle path)
// indestructible from QML and leaking one bus registration per call. The
// fixtures are deliberately QML-shaped: a QQmlEngine-hosted stage creating
// adaptors via Component.createObject — NOT C++ delete, which ignores QML
// ownership and is exactly why the 0.6.0 adversarial matrix (C++-shaped)
// could not see this bug.

// Capture Qt messages: the QML destroy() refusal ("Invalid attempt to
// destroy() an indestructible object") is a qmlError/qWarning, not a JS
// exception — try/catch in QML cannot see it. Messages are also forwarded to
// stderr for debugging.
class LifecycleMessageCapture {
public:
    LifecycleMessageCapture() : m_prior(qInstallMessageHandler(record)) { s_active = this; }
    ~LifecycleMessageCapture() {
        s_active = nullptr;
        qInstallMessageHandler(m_prior);
    }

    bool contains(const QString &needle) const {
        for (const QString &m : std::as_const(messages))
            if (m.contains(needle))
                return true;
        return false;
    }
    void clear() { messages.clear(); }

    QStringList messages;

private:
    static LifecycleMessageCapture *s_active;
    QtMessageHandler m_prior;
    static void record(QtMsgType, const QMessageLogContext &, const QString &msg) {
        if (s_active)
            s_active->messages.append(msg);
        std::fprintf(stderr, "%s\n", qPrintable(msg));
    }
};
LifecycleMessageCapture *LifecycleMessageCapture::s_active = nullptr;

// The lifecycle fixture stage: creates Request-style adaptors dynamically
// (initial properties applied before componentComplete, so attachment happens
// at the caller-chosen service/path — the portal pattern).
static const char kLifecycleStage[] = R"QML(
import DBus 1.0
import QtQml
QtObject {
    id: root
    property var last: null
    property Component reqComp: Component {
        DBusAdaptor {
            property var lastCall: ""
            function close() { lastCall = "close"; return 0 }
            function kill() { destroy() }
        }
    }
    // N1 fixture: the handler destroys the adaptor DIRECTLY (no Qt.callLater).
    property Component closeKillComp: Component {
        DBusAdaptor {
            function close() { destroy(); return 0 }
        }
    }
    // N4 fixture: the handler stresses the JS GC during its own dispatch —
    // the only JS reference to the adaptor is the dispatch's thisObj.
    property Component gcComp: Component {
        DBusAdaptor {
            function gcProbe() {
                var sink = []
                for (var i = 0; i < 100; i++) { sink.push({ n: i, s: "x" + i }) }
                gc()
                for (var j = 0; j < 500; j++) { var z = { k: j } }
                gc()
                gc()
                return "survived"
            }
        }
    }
    property Component pendingComp: Component {
        DBusAdaptor {
            property bool opened: false
            function openFile(handle, appId, parentWindow, title, options) {
                holdReply()
                opened = true
            }
            function kill() { destroy() }
        }
    }
    function spawn(service, path) {
        var a = reqComp.createObject(null, { service: service, path: path, iface: service })
        root.last = a
        return a
    }
    function spawnCloseKill(service, path) {
        var a = closeKillComp.createObject(null, { service: service, path: path, iface: service })
        root.last = a
        return a
    }
    function spawnGc(service, path) {
        var a = gcComp.createObject(null, { service: service, path: path, iface: service })
        root.last = a
        return a
    }
    function spawnPending(service, path) {
        var a = pendingComp.createObject(null, {
            service: service, path: path,
            iface: 'org.freedesktop.impl.portal.FileChooser'
        })
        root.last = a
        return a
    }
}
)QML";

static QObject *createLifecycleStage(QQmlEngine &engine) {
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    QQmlComponent component(&engine);
    component.setData(QByteArray(kLifecycleStage), QUrl());
    if (!component.isReady()) {
        qWarning() << "lifecycle stage errors:" << component.errorString();
        return nullptr;
    }
    return component.create();
}

static DBusAdaptor *lifecycleSpawn(QObject *stage, const QString &fn, const QString &service,
                                   const QString &path) {
    QMetaObject::invokeMethod(stage, fn.toLatin1().constData(), Q_ARG(QVariant, service),
                              Q_ARG(QVariant, path));
    return qobject_cast<DBusAdaptor *>(stage->property("last").value<QObject *>());
}

// Pump deferred deletes + event-loop turns (GC finalization, DBus delivery).
static void pumpLifecycle(int turns = 25) {
    for (int i = 0; i < turns; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }
}

static QDBusMessage lifecycleCall(const QString &service, const QString &path,
                                  const QString &member, const QVariantList &args = {}) {
    QDBusMessage m = QDBusMessage::createMethodCall(service, path, service, member);
    if (!args.isEmpty())
        m.setArguments(args);
    return QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
}

// The path is FREED: a subsequent wire call errors with an Unknown*-class
// error (UnknownObject / ServiceUnknown / UnknownMethod) and the service name
// is released.
static void assertPathFreed(const QString &service, const QString &path) {
    QDBusMessage r = lifecycleCall(service, path, QStringLiteral("close"));
    QVERIFY2(r.type() == QDBusMessage::ErrorMessage,
             qPrintable(QStringLiteral("path %1 is still serving (type %2, sig '%3')")
                            .arg(path)
                            .arg(int(r.type()))
                            .arg(r.signature())));
    QVERIFY2(r.errorName().contains(QStringLiteral("Unknown")), qPrintable(r.errorName()));
    QTRY_VERIFY_WITH_TIMEOUT(
        !QDBusConnection::sessionBus().interface()->isServiceRegistered(service), 3000);
}

// L1 — napkin repro: dynamically create a Request-style adaptor at a unique
// path, dispatch one method (remote caller), then QML destroy(). Must
// succeed (no "indestructible" error) and the path must be FREED (wire assert
// + service release + dispatcher registry back to baseline). 0.6.0:
// destroy() is refused and the path keeps serving — one leaked bus
// registration per call.
void TestDBusAdaptor::testLifecycleDestroyAfterDispatch() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawn"), QStringLiteral("org.dbusqml.LifecycleL1"),
                       QStringLiteral("/LifecycleL1"));
    QVERIFY(guard != nullptr);

    LifecycleMessageCapture capture;

    // One dispatch through the JS path (remote caller).
    QDBusMessage reply = lifecycleCall(QStringLiteral("org.dbusqml.LifecycleL1"),
                                       QStringLiteral("/LifecycleL1"), QStringLiteral("close"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.arguments().first().toInt(), 0);

    // QML-side destroy().
    QMetaObject::invokeMethod(guard.data(), "kill");
    pumpLifecycle();

    QVERIFY2(guard.isNull(),
             "destroy() after dispatch must succeed — 0.6.0 refuses (indestructible object)");
    QVERIFY2(!capture.contains(QStringLiteral("indestructible")),
             "no destroy() refusal is acceptable after dispatch");

    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleL1"), QStringLiteral("/LifecycleL1"));
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);

    delete stage;
}

// L2 — GC path: create dynamically, dispatch, drop all JS references, force
// gc() (+ event-loop turns) → adaptor collected → path freed (same wire
// asserts). 0.6.0: leaks forever.
void TestDBusAdaptor::testLifecycleGCCollectsAfterDispatch() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawn"), QStringLiteral("org.dbusqml.LifecycleL2"),
                       QStringLiteral("/LifecycleL2"));
    QVERIFY(guard != nullptr);

    QDBusMessage reply = lifecycleCall(QStringLiteral("org.dbusqml.LifecycleL2"),
                                       QStringLiteral("/LifecycleL2"), QStringLiteral("close"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);

    // Drop every JS reference (including the stage's `last` stash) and force
    // collection with event-loop turns.
    stage->setProperty("last", QVariant());
    for (int i = 0; i < 20 && guard != nullptr; ++i) {
        engine.collectGarbage();
        pumpLifecycle(4);
    }

    QVERIFY2(guard.isNull(),
             "GC must collect a dispatched dynamic adaptor — 0.6.0 leaks it forever");

    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleL2"), QStringLiteral("/LifecycleL2"));
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);

    delete stage;
}

// N2 — L3 INVERTED (0.8.0). The 0.7.0 semantics asserted here were: destroy()
// refused while a held reply is pending, settle first, then destroy. The
// owner blessed the consistent-retirement semantic change in the
// ownership-preserve-v0.8.0 plan: destroy()/GC while a reply is pending is
// ALLOWED, and the pending caller is errored by the destructor (exactly what
// unregister() already does — 0.7.0 shipped the inconsistency). The old
// "held replies block retirement" behavior is gone; this test now pins the
// new contract: destroy while pending succeeds (deferred deletion), the
// pending caller receives `org.freedesktop.DBus.Error.Failed` ("adaptor
// destroyed with reply pending", not a hang), the path is freed, and the
// registry returns to baseline.
void TestDBusAdaptor::testLifecycleDestroyWhilePendingErrorsCallers() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawnPending"),
                       QStringLiteral("org.dbusqml.LifecycleL3"), QStringLiteral("/LifecycleL3"));
    QVERIFY(guard != nullptr);

    LifecycleMessageCapture capture;

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.LifecycleL3"), QStringLiteral("/LifecycleL3"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);

    // The handler ran and is holding the reply.
    QTRY_VERIFY_WITH_TIMEOUT(guard->property("opened").toBool(), 3000);

    // While pending: QML destroy() SUCCEEDS (deferred deletion).
    QMetaObject::invokeMethod(guard.data(), "kill");
    pumpLifecycle();
    QVERIFY2(guard.isNull(), "destroy() while a reply is held must succeed (0.8.0)");
    QVERIFY2(!capture.contains(QStringLiteral("indestructible")),
             "no destroy() refusal is acceptable");

    // The pending caller is errored, not hung and not answered normally.
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    if (spy.count() == 1) {
        QVERIFY(watcher->isError());
        QCOMPARE(watcher->reply().type(), QDBusMessage::ErrorMessage);
        QCOMPARE(watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.Failed"));
        QCOMPARE(watcher->reply().errorMessage(),
                 QStringLiteral("adaptor destroyed with reply pending"));
    }

    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleL3"), QStringLiteral("/LifecycleL3"));
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);
    delete watcher;
    delete stage;
}

// N1 — in-handler destroy (the 0.7.0 smell, verbatim): the close() handler
// destroys the adaptor DIRECTLY (no Qt.callLater) and returns → the caller
// still gets the reply; the deferred deletion then runs; the object is gone
// and the path freed. 0.7.0: destroy() refused ("indestructible object") and
// the path kept serving.
void TestDBusAdaptor::testLifecycleInHandlerDestroy() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawnCloseKill"),
                       QStringLiteral("org.dbusqml.LifecycleN1"), QStringLiteral("/LifecycleN1"));
    QVERIFY(guard != nullptr);

    LifecycleMessageCapture capture;

    QDBusMessage reply = lifecycleCall(QStringLiteral("org.dbusqml.LifecycleN1"),
                                       QStringLiteral("/LifecycleN1"), QStringLiteral("close"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    // 0.7.0: the destroy() refusal aborts the JS handler mid-function — the
    // `return 0` never executes and the reply comes back EMPTY. The 0.8.0
    // contract: the handler completes and the reply carries the value.
    QCOMPARE(reply.arguments().size(), 1);
    if (reply.arguments().size() == 1)
        QCOMPARE(reply.arguments().first().toInt(), 0);

    pumpLifecycle();
    QVERIFY2(guard.isNull(), "in-handler destroy() must take effect after the dispatch (0.8.0)");
    QVERIFY2(!capture.contains(QStringLiteral("indestructible")),
             "no destroy() refusal is acceptable");

    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleN1"), QStringLiteral("/LifecycleN1"));
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);
    delete stage;
}

// N3 — abandoned-while-pending: a dynamic adaptor with a pending held reply
// and ZERO QML references is GC-collected; the destructor errors the pending
// caller; the path is freed. 0.7.0: leaks until process exit (the 0.7.0
// deferral kept CppOwnership while the reply was pending).
void TestDBusAdaptor::testLifecycleAbandonedWhilePendingCollected() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawnPending"),
                       QStringLiteral("org.dbusqml.LifecycleN3"), QStringLiteral("/LifecycleN3"));
    QVERIFY(guard != nullptr);

    LifecycleMessageCapture capture;

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.LifecycleN3"), QStringLiteral("/LifecycleN3"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);
    QTRY_VERIFY_WITH_TIMEOUT(guard->property("opened").toBool(), 3000);

    // Drop every QML reference (adaptor AND reply) and force collection.
    stage->setProperty("last", QVariant());
    for (int i = 0; i < 20 && guard != nullptr; ++i) {
        engine.collectGarbage();
        pumpLifecycle(4);
    }
    QVERIFY2(guard.isNull(), "GC must collect an abandoned adaptor with a pending reply");

    // The pending caller is errored, not hung.
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    if (spy.count() == 1) {
        QVERIFY(watcher->isError());
        QCOMPARE(watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.Failed"));
        QCOMPARE(watcher->reply().errorMessage(),
                 QStringLiteral("adaptor destroyed with reply pending"));
    }
    QVERIFY2(!capture.contains(QStringLiteral("indestructible")), "GC retirement must not warn");

    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleN3"), QStringLiteral("/LifecycleN3"));
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);
    delete watcher;
    delete stage;
}

// N4 — gc() inside the handler (the spike-(a) scenario as a permanent pin):
// a JS-owned adaptor whose ONLY reference during dispatch is the dispatch's
// thisObj must survive its own dispatch under GC pressure — the QJSValue is
// a GC root.
void TestDBusAdaptor::testLifecycleGcInsideHandler() {
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawnGc"), QStringLiteral("org.dbusqml.LifecycleN4"),
                       QStringLiteral("/LifecycleN4"));
    QVERIFY(guard != nullptr);
    QCOMPARE(QQmlEngine::objectOwnership(guard), QQmlEngine::JavaScriptOwnership);

    QDBusMessage reply = lifecycleCall(QStringLiteral("org.dbusqml.LifecycleN4"),
                                       QStringLiteral("/LifecycleN4"), QStringLiteral("gcProbe"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.arguments().first().toString(), QStringLiteral("survived"));

    QVERIFY2(guard != nullptr, "adaptor collected DURING its own dispatch");
    QCOMPARE(QQmlEngine::objectOwnership(guard), QQmlEngine::JavaScriptOwnership);

    delete stage;
}

// L4 — declarative pin: a declaratively-declared adaptor behaves exactly as
// today through repeated dispatch — ownership stays CppOwnership (flip+restore
// are no-ops), repeated calls answer, QML destroy() stays forbidden (unchanged
// pin), and C++ delete still tears the path down.
void TestDBusAdaptor::testLifecycleDeclarativeAdaptorUnchanged() {
    QQmlEngine engine;
    QDir binDir(QCoreApplication::applicationDirPath());
    engine.addImportPath(binDir.path());
    engine.addImportPath(binDir.filePath(QStringLiteral("DBus")));

    LifecycleMessageCapture capture;

    QQmlComponent component(&engine);
    component.setData("import DBus 1.0\n"
                      "DBusAdaptor {\n"
                      "  service: 'org.dbusqml.LifecycleL4'\n"
                      "  path: '/LifecycleL4'\n"
                      "  iface: 'org.dbusqml.LifecycleL4'\n"
                      "  function close() { return 0 }\n"
                      "  function kill() { destroy() }\n"
                      "}",
                      QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QPointer<DBusAdaptor> guard = qobject_cast<DBusAdaptor *>(component.create());
    QVERIFY(guard != nullptr);
    QCOMPARE(QQmlEngine::objectOwnership(guard), QQmlEngine::CppOwnership);

    for (int i = 0; i < 3; ++i) {
        QDBusMessage reply = lifecycleCall(QStringLiteral("org.dbusqml.LifecycleL4"),
                                           QStringLiteral("/LifecycleL4"), QStringLiteral("close"));
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.arguments().first().toInt(), 0);
    }
    QCOMPARE(QQmlEngine::objectOwnership(guard), QQmlEngine::CppOwnership);

    // QML destroy() on a declarative object stays forbidden (unchanged).
    QMetaObject::invokeMethod(guard.data(), "kill");
    pumpLifecycle();
    QVERIFY2(guard != nullptr, "declarative adaptors are not QML-destroyable (unchanged)");
    QVERIFY2(capture.contains(QStringLiteral("indestructible")),
             "declarative destroy() refusal is the unchanged pin");

    delete guard.data();
    pumpLifecycle();
    QVERIFY(guard.isNull());
    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleL4"), QStringLiteral("/LifecycleL4"));
}

// L5 — unregister(): deterministic retirement independent of GC timing. The
// path is freed immediately (wire assert), the object stays alive and inert,
// a second unregister() is a warned no-op, and GC afterwards collects
// cleanly.
void TestDBusAdaptor::testLifecycleUnregister() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawn"), QStringLiteral("org.dbusqml.LifecycleL5"),
                       QStringLiteral("/LifecycleL5"));
    QVERIFY(guard != nullptr);

    LifecycleMessageCapture capture;

    QDBusMessage reply = lifecycleCall(QStringLiteral("org.dbusqml.LifecycleL5"),
                                       QStringLiteral("/LifecycleL5"), QStringLiteral("close"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);

    QMetaObject::invokeMethod(guard.data(), "unregister");

    // Path freed immediately.
    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleL5"), QStringLiteral("/LifecycleL5"));
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);

    // The QObject is left alive for QML to drop whenever.
    QVERIFY2(guard != nullptr, "unregister() must leave the QObject alive");

    // Second unregister() is a warned no-op.
    capture.clear();
    QMetaObject::invokeMethod(guard.data(), "unregister");
    QVERIFY2(capture.contains(QStringLiteral("already detached")), "second unregister() must warn");
    QVERIFY(guard != nullptr);

    // GC afterwards collects cleanly (ownership was restored after dispatch).
    stage->setProperty("last", QVariant());
    for (int i = 0; i < 20 && guard != nullptr; ++i) {
        engine.collectGarbage();
        pumpLifecycle(4);
    }
    QVERIFY2(guard.isNull(), "GC must collect an unregistered adaptor");

    delete stage;
}

// L5 (held-reply interplay): unregister() with an outstanding held reply
// errors the caller with the same code the destructor uses, and the object is
// GC-collectable afterwards.
void TestDBusAdaptor::testLifecycleUnregisterErrorsHeldReply() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    QPointer<DBusAdaptor> guard =
        lifecycleSpawn(stage, QStringLiteral("spawnPending"),
                       QStringLiteral("org.dbusqml.LifecycleL5H"), QStringLiteral("/LifecycleL5H"));
    QVERIFY(guard != nullptr);

    QDBusPendingCallWatcher *watcher = asyncCallDeferred(
        QStringLiteral("org.dbusqml.LifecycleL5H"), QStringLiteral("/LifecycleL5H"),
        QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"),
        {QDBusObjectPath(QStringLiteral("/req/1")), QStringLiteral("app"), QStringLiteral(""),
         QStringLiteral("title"), QVariantMap{}});
    QSignalSpy spy(watcher, &QDBusPendingCallWatcher::finished);
    QTRY_VERIFY_WITH_TIMEOUT(guard->property("opened").toBool(), 3000);

    QMetaObject::invokeMethod(guard.data(), "unregister");

    // The outstanding held reply is errored, not left to time out.
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QVERIFY(watcher->isError());
    QCOMPARE(watcher->reply().type(), QDBusMessage::ErrorMessage);
    QCOMPARE(watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.Failed"));
    QCOMPARE(watcher->reply().errorMessage(),
             QStringLiteral("adaptor unregistered with reply pending"));

    assertPathFreed(QStringLiteral("org.dbusqml.LifecycleL5H"), QStringLiteral("/LifecycleL5H"));
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);

    QVERIFY(guard != nullptr);
    stage->setProperty("last", QVariant());
    for (int i = 0; i < 20 && guard != nullptr; ++i) {
        engine.collectGarbage();
        pumpLifecycle(4);
    }
    QVERIFY2(guard.isNull(), "GC must collect an unregistered adaptor with no pending replies");

    delete watcher;
    delete stage;
}

// L6 — leak-count regression: N create→dispatch→destroy cycles at distinct
// paths → the dispatcher registry returns to its baseline (no accumulation).
void TestDBusAdaptor::testLifecycleLeakRegression() {
    const int baseline = DBusPathDispatcher::liveCount();
    QQmlEngine engine;
    QObject *stage = createLifecycleStage(engine);
    QVERIFY(stage != nullptr);

    LifecycleMessageCapture capture;

    const int cycles = 5;
    for (int i = 0; i < cycles; ++i) {
        const QString service = QStringLiteral("org.dbusqml.LifecycleL6%1").arg(i);
        const QString path = QStringLiteral("/LifecycleL6/%1").arg(i);
        QPointer<DBusAdaptor> guard = lifecycleSpawn(stage, QStringLiteral("spawn"), service, path);
        QVERIFY(guard != nullptr);

        QDBusMessage reply = lifecycleCall(service, path, QStringLiteral("close"));
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);

        QMetaObject::invokeMethod(guard.data(), "kill");
        pumpLifecycle();
        QVERIFY2(guard.isNull(),
                 qPrintable(QStringLiteral("cycle %1: destroy must succeed").arg(i)));
    }

    // Registry back to baseline — every path and dispatcher torn down.
    QCOMPARE(DBusPathDispatcher::liveCount(), baseline);

    for (int i = 0; i < cycles; ++i) {
        assertPathFreed(QStringLiteral("org.dbusqml.LifecycleL6%1").arg(i),
                        QStringLiteral("/LifecycleL6/%1").arg(i));
    }
    QVERIFY2(!capture.contains(QStringLiteral("indestructible")),
             "no destroy() refusals across the whole soak");

    delete stage;
}

// ==================== Adversarial input matrix (0.6.0) ====================
//
// Hostile values at the adaptor's marshal exits (reply / properties /
// signals), pinned from the Phase 1 probe matrix. Outcome contract per cell:
// correct wire type, OR loud-fail + safe fallback, OR error reply / skip —
// never a crash, hang, or silent wrong type.

static const char kMatrixStage[] = R"QML(
import DBus 1.0
import QtQml
QtObject {
    id: root
    property var adv: DBusAdaptor {
        id: adv
        service: "org.dbusqml.AMatrix"
        path: "/AMatrix"
        iface: "org.dbusqml.AMatrix"
        property var pv: null
        function makeValue(id) {
            if (id === "null") return null
            if (id === "undefined") return undefined
            if (id === "qobject") return Qt.createQmlObject("import QtQml; QtObject {}", adv)
            if (id === "func") return (function() {})
            if (id === "date") return new Date()
            if (id === "qobject-map") return { k: Qt.createQmlObject("import QtQml; QtObject {}", adv) }
            if (id === "qobject-list") return [Qt.createQmlObject("import QtQml; QtObject {}", adv)]
            if (id === "func-map") return { k: (function() {}) }
            if (id === "cycle-obj") { var a = {}; a.self = a; return a }
            if (id === "cycle-arr") { var b = []; b.push(b); return b }
            if (id === "nan") return NaN
            if (id === "inf") return Infinity
            if (id === "neginf") return -Infinity
            if (id === "nul-str") return "a\u0000b"
            if (id === "big-str") { var s = "x"; for (var i = 0; i < 20; i++) s = s + s; return s }
            if (id === "empty-map") return ({})
            if (id === "empty-list") return []
            if (id === "deep12") {
                var d = 1
                for (var j = 0; j < 12; j++) d = [d]
                return d
            }
            if (id === "mixed") return [1, "a", {}]
            if (id === "date-list") return [new Date()]
            if (id === "value-dict") return { value: 42 }
            if (id === "value-struct") return { value: [0.1, 0.2] }
            return null
        }
        function replyM(id) { return makeValue(id) }
        function stashP(id) { pv = makeValue(id) }
        function emitM(id) { emitSignal("Sig", [makeValue(id)]) }
    }
}
)QML";

static QObject *matrixStage = nullptr;
static QObject *matrixAdaptor = nullptr;

static void ensureMatrixStage() {
    if (matrixStage)
        return;
    static QQmlEngine *engine = nullptr;
    if (!engine) {
        engine = new QQmlEngine;
        QDir binDir(QCoreApplication::applicationDirPath());
        engine->addImportPath(binDir.path());
        engine->addImportPath(binDir.filePath(QStringLiteral("DBus")));
    }
    QQmlComponent component(engine);
    component.setData(QByteArray(kMatrixStage), QUrl());
    if (!component.isReady())
        QFAIL(qPrintable(component.errorString()));
    matrixStage = component.create();
    QVERIFY(matrixStage != nullptr);
    for (QObject *ch : matrixStage->findChildren<QObject *>()) {
        if (ch->inherits("DBusAdaptor")) {
            matrixAdaptor = ch;
            break;
        }
    }
    QVERIFY(matrixAdaptor != nullptr);
    QTest::qWait(300);
}

static QDBusMessage matrixCall(const QString &member, const QVariantList &args = {}) {
    // GetAll/Get are Properties-interface calls; everything else targets the
    // adaptor's own interface.
    const QString iface = (member == QStringLiteral("GetAll") || member == QStringLiteral("Get"))
                              ? QStringLiteral("org.freedesktop.DBus.Properties")
                              : QStringLiteral("org.dbusqml.AMatrix");
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.dbusqml.AMatrix"),
                                                    QStringLiteral("/AMatrix"), iface, member);
    if (!args.isEmpty())
        m.setArguments(args);
    return QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
}

// E1 — the reply exit across the value classes.
void TestDBusAdaptor::testMatrixReplyValues() {
    ensureMatrixStage();
    struct Cell {
        const char *id;
        bool error;      // ErrorMessage (guard) vs ReplyMessage
        const char *sig; // expected reply signature (Void → "")
    };
    const Cell cells[] = {
        {"null", true, ""},
        {"undefined", false, ""},
        {"qobject", true, ""},
        {"func", true, ""},
        {"qobject-map", true, ""},
        {"func-map", true, ""},
        {"nan", false, "d"},
        {"inf", false, "d"},
        {"neginf", false, "d"},
        {"nul-str", false, "s"},
        {"big-str", false, "s"},
        {"empty-map", false, "a{sv}"},
        {"empty-list", false, "av"},
        {"deep12", false, "av"},
        {"mixed", false, "av"},
        {"cycle-obj", false, "a{sv}"},
        {"cycle-arr", false, "av"},
        {"date", false, "((iii)(iiii)i)"},
        {"value-dict", false, "a{sv}"},
        {"value-struct", false, "a{sv}"},
    };
    for (const Cell &c : cells) {
        QDBusMessage r = matrixCall(QStringLiteral("replyM"), {QString::fromLatin1(c.id)});
        if (c.error) {
            QVERIFY2(r.type() == QDBusMessage::ErrorMessage,
                     qPrintable(QStringLiteral("cell %1").arg(c.id)));
            QVERIFY2(r.errorName() == QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                     qPrintable(QStringLiteral("cell %1").arg(c.id)));
        } else {
            QVERIFY2(r.type() == QDBusMessage::ReplyMessage,
                     qPrintable(QStringLiteral("cell %1").arg(c.id)));
            QVERIFY2(r.signature() == QString::fromLatin1(c.sig),
                     qPrintable(QStringLiteral("cell %1 got sig '%2' err='%3'")
                                    .arg(c.id, r.signature(), r.errorName())));
        }
    }
}

// E4 — the GetAll exit: unmarshalable properties skipped, benign present.
void TestDBusAdaptor::testMatrixGetAllValues() {
    ensureMatrixStage();
    const char *poison[] = {"qobject", "func", "qobject-map", "func-map", "qobject-list"};
    const char *benign[] = {"nan",        "inf",   "nul-str",   "big-str",   "empty-map",
                            "empty-list", "mixed", "cycle-obj", "value-dict"};
    for (const char *id : poison) {
        QCOMPARE(matrixCall(QStringLiteral("stashP"), {QString::fromLatin1(id)}).type(),
                 QDBusMessage::ReplyMessage);
        QDBusMessage r =
            matrixCall(QStringLiteral("GetAll"), {QStringLiteral("org.dbusqml.AMatrix")});
        QVERIFY2(r.type() == QDBusMessage::ReplyMessage,
                 qPrintable(QStringLiteral("cell %1: %2").arg(id, r.errorName())));
        const QVariantMap props = unwrapDbus(r.arguments().first()).toMap();
        QVERIFY2(!props.contains(QStringLiteral("Pv")),
                 qPrintable(QStringLiteral("cell %1 must be skipped").arg(id)));
    }
    for (const char *id : benign) {
        QCOMPARE(matrixCall(QStringLiteral("stashP"), {QString::fromLatin1(id)}).type(),
                 QDBusMessage::ReplyMessage);
        QDBusMessage r =
            matrixCall(QStringLiteral("GetAll"), {QStringLiteral("org.dbusqml.AMatrix")});
        QVERIFY2(r.type() == QDBusMessage::ReplyMessage,
                 qPrintable(QStringLiteral("cell %1: %2").arg(id, r.errorName())));
        const QVariantMap props = unwrapDbus(r.arguments().first()).toMap();
        QVERIFY2(props.contains(QStringLiteral("Pv")),
                 qPrintable(QStringLiteral("cell %1 must be present").arg(id)));
    }
}

// E5 — the emitSignal exit: poison warned+skipped, benign delivered.
void TestDBusAdaptor::testMatrixSignalValues() {
    ensureMatrixStage();
    MatrixCatcher catcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.connect(QStringLiteral("org.dbusqml.AMatrix"), QStringLiteral("/AMatrix"),
                        QStringLiteral("org.dbusqml.AMatrix"), QStringLiteral("Sig"), &catcher,
                        SLOT(onSignal(QDBusMessage))));

    const char *skipped[] = {"null",        "undefined", "qobject",     "func",
                             "qobject-map", "func-map",  "qobject-list"};
    for (const char *id : skipped) {
        int before = catcher.count;
        QCOMPARE(matrixCall(QStringLiteral("emitM"), {QString::fromLatin1(id)}).type(),
                 QDBusMessage::ReplyMessage);
        QTest::qWait(200);
        QVERIFY2(catcher.count == before,
                 qPrintable(QStringLiteral("cell %1 must be skipped").arg(id)));
    }

    struct Cell {
        const char *id;
        const char *sig;
    };
    const Cell delivered[] = {
        {"nan", "d"},
        {"inf", "d"},
        {"neginf", "d"},
        {"nul-str", "s"},
        {"big-str", "s"},
        {"empty-map", "a{sv}"},
        {"empty-list", "av"},
        {"deep12", "av"},
        {"mixed", "av"},
        {"cycle-obj", "a{sv}"},
        {"cycle-arr", "av"},
        {"date-list", "av"},
        {"value-dict", "a{sv}"},
        {"value-struct", "a{sv}"},
    };
    for (const Cell &c : delivered) {
        int before = catcher.count;
        QCOMPARE(matrixCall(QStringLiteral("emitM"), {QString::fromLatin1(c.id)}).type(),
                 QDBusMessage::ReplyMessage);
        QTest::qWait(200);
        QVERIFY2(catcher.count == before + 1,
                 qPrintable(QStringLiteral("cell %1 must be delivered").arg(c.id)));
        QVERIFY2(catcher.last.signature() == QString::fromLatin1(c.sig),
                 qPrintable(QStringLiteral("cell %1").arg(c.id)));
    }
    bus.disconnect(QStringLiteral("org.dbusqml.AMatrix"), QStringLiteral("/AMatrix"),
                   QStringLiteral("org.dbusqml.AMatrix"), QStringLiteral("Sig"), &catcher,
                   SLOT(onSignal(QDBusMessage)));
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
