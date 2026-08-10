#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QProcess>
#include <QTest>

#include "dbusadaptor.h"
#include "dbusconnection.h"

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

QTEST_GUILESS_MAIN(TestDBusAdaptor)
#include "test_dbusadaptor.moc"
