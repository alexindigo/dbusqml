#include <QTest>
#include <QDebug>

#include "../dbustypes.h"
#include "../dbuserror.h"
#include "../dbusmessage.h"

class TestDBusTypes : public QObject {
    Q_OBJECT

private slots:
    void testUint32()
    {
        DBus::Uint32 v(42);
        QCOMPARE(v.value, 42u);
        QCOMPARE(v.toString(), QStringLiteral("42"));

        DBus::Uint32 vDefault;
        QCOMPARE(vDefault.value, 0u);

        uint raw = v;
        QCOMPARE(raw, 42u);

        QVariant asVar = v;
        QVERIFY(asVar.canConvert<DBus::Uint32>());
    }

    void testInt32()
    {
        DBus::Int32 v(-42);
        QCOMPARE(v.value, -42);
        QCOMPARE(v.toString(), QStringLiteral("-42"));
    }

    void testUint16()
    {
        DBus::Uint16 v(42);
        QCOMPARE(v.value, static_cast<ushort>(42));
    }

    void testInt16()
    {
        DBus::Int16 v(-42);
        QCOMPARE(v.value, static_cast<short>(-42));
    }

    void testUint64()
    {
        DBus::Uint64 v(Q_UINT64_C(18446744073));
        QCOMPARE(v.value, Q_UINT64_C(18446744073));
    }

    void testInt64()
    {
        DBus::Int64 v(Q_INT64_C(-42));
        QCOMPARE(v.value, Q_INT64_C(-42));
    }

    void testBool()
    {
        DBus::Bool vTrue(true);
        DBus::Bool vFalse(false);
        QCOMPARE(vTrue.value, true);
        QCOMPARE(vFalse.value, false);
        QCOMPARE(vTrue.toString(), QStringLiteral("true"));

        bool b = vTrue;
        QCOMPARE(b, true);

        QVariant asVar = vTrue;
        QVERIFY(asVar.canConvert<DBus::Bool>());
    }

    void testDouble()
    {
        DBus::Double v(3.14);
        QCOMPARE(v.value, 3.14);
        QCOMPARE(v.toString(), QStringLiteral("3.14"));

        double d = v;
        QCOMPARE(d, 3.14);
    }

    void testByte()
    {
        DBus::Byte v(255);
        QCOMPARE(v.value, static_cast<uchar>(255));
        QVERIFY(!v.toString().isEmpty());
    }

    void testString()
    {
        DBus::String v("hello");
        QCOMPARE(v.value, QStringLiteral("hello"));
        QCOMPARE(v.toString(), QStringLiteral("hello"));

        QString s = v;
        QCOMPARE(s, QStringLiteral("hello"));

        DBus::String vEmpty;
        QVERIFY(vEmpty.value.isEmpty());
    }

    void testObjectPath()
    {
        DBus::ObjectPath v("/org/freedesktop/UPower");
        QCOMPARE(v.value.path(), QStringLiteral("/org/freedesktop/UPower"));
        QCOMPARE(v.toString(), QStringLiteral("/org/freedesktop/UPower"));

        DBus::ObjectPath vDefault;
        QVERIFY(vDefault.value.path().isEmpty());

        QDBusObjectPath qpath = v;
        QCOMPARE(qpath.path(), QStringLiteral("/org/freedesktop/UPower"));
    }

    void testSignature()
    {
        DBus::Signature v("ssss");
        QCOMPARE(v.value.signature(), QStringLiteral("ssss"));
        QCOMPARE(v.toString(), QStringLiteral("ssss"));

        DBus::Signature vDefault;
        QVERIFY(vDefault.value.signature().isEmpty());
    }

    void testVariant()
    {
        DBus::Variant v(QStringLiteral("test"));
        QCOMPARE(v.value.variant().toString(), QStringLiteral("test"));
        QCOMPARE(v.toString(), QStringLiteral("test"));

        DBus::Variant vNum(QJSValue(42));
        QCOMPARE(vNum.value.variant().toInt(), 42);

        QVariant qv = v;
        QCOMPARE(qv.toString(), QStringLiteral("test"));

        DBus::Variant vDefault;
        QVERIFY(!vDefault.value.variant().isValid());
    }

    void testDict()
    {
        QVariantMap map;
        map["key1"] = "value1";
        map["key2"] = 42;
        DBus::Dict d(map);
        QCOMPARE(d.value["key1"].toString(), QStringLiteral("value1"));
        QCOMPARE(d.value["key2"].toInt(), 42);

        const QVariantMap &m = d.value;
        QCOMPARE(m["key1"].toString(), QStringLiteral("value1"));

        DBus::Dict dEmpty;
        QVERIFY(dEmpty.value.isEmpty());
    }

    void testDBusMessageDefaults()
    {
        DBusMessage msg;
        QVERIFY(msg.service().isEmpty());
        QVERIFY(msg.path().isEmpty());
        QVERIFY(msg.iface().isEmpty());
        QVERIFY(msg.member().isEmpty());
        QVERIFY(msg.arguments().isEmpty());
        QVERIFY(msg.signature().isEmpty());
    }

    void testDBusMessageSetters()
    {
        DBusMessage msg;
        msg.setService("org.freedesktop.DBus");
        msg.setPath("/org/freedesktop/DBus");
        msg.setIface("org.freedesktop.DBus");
        msg.setMember("ListNames");
        msg.setSignature("s");

        QVariantList args;
        args << QStringLiteral("test");
        msg.setArguments(args);

        QCOMPARE(msg.service(), QStringLiteral("org.freedesktop.DBus"));
        QCOMPARE(msg.path(), QStringLiteral("/org/freedesktop/DBus"));
        QCOMPARE(msg.iface(), QStringLiteral("org.freedesktop.DBus"));
        QCOMPARE(msg.member(), QStringLiteral("ListNames"));
        QCOMPARE(msg.signature(), QStringLiteral("s"));
        QCOMPARE(msg.arguments().size(), 1);
        QCOMPARE(msg.arguments()[0].toString(), QStringLiteral("test"));
    }

    void testDBusMessageFromMap()
    {
        QVariantMap map;
        map["service"] = "org.freedesktop.login1";
        map["path"] = "/org/freedesktop/login1";
        map["iface"] = "org.freedesktop.login1.Manager";
        map["member"] = "Inhibit";
        map["signature"] = "ssss";
        QVariantList args = {"sleep", "app", "Screen locked", "delay"};
        map["arguments"] = args;

        DBusMessage msg(map);
        QCOMPARE(msg.service(), QStringLiteral("org.freedesktop.login1"));
        QCOMPARE(msg.member(), QStringLiteral("Inhibit"));
        QCOMPARE(msg.signature(), QStringLiteral("ssss"));
        QCOMPARE(msg.arguments().size(), 4);
    }

    void testDBusMessageFromEmptyMap()
    {
        QVariantMap empty;
        DBusMessage msg(empty);
        QVERIFY(msg.service().isEmpty());
        QVERIFY(msg.arguments().isEmpty());
    }

    void testDBusError()
    {
        DBusError err;
        QCOMPARE(err.isValid(), false);
        QVERIFY(err.name().isEmpty());
        QVERIFY(err.message().isEmpty());

        QDBusError qerr(QDBusError::AccessDenied, "permission denied");
        DBusError wrapped(qerr);
        QCOMPARE(wrapped.isValid(), true);
        QCOMPARE(wrapped.name(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QCOMPARE(wrapped.message(), QStringLiteral("permission denied"));
    }
};

QTEST_MAIN(TestDBusTypes)
#include "test_dbustypes.moc"
