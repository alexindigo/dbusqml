#include <QTest>
#include "dbusintrospection.h"

class TestIntrospection : public QObject {
    Q_OBJECT

private slots:
    void testBasicInterface() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="Bar">
                  <arg type="s" direction="in"/>
                  <arg type="i" direction="in"/>
                </method>
                <signal name="Baz"/>
                <property name="Enabled" type="b" access="readwrite"/>
              </interface>
            </node>
        )");

        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        QCOMPARE(data.methodNames, QStringList({QStringLiteral("Bar")}));
        QCOMPARE(data.signalNames, QStringList({QStringLiteral("Baz")}));
        QCOMPARE(data.propertyNames, QStringList({QStringLiteral("Enabled")}));
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("Bar")),
                 QStringList({QStringLiteral("s"), QStringLiteral("i")}));
    }

    void testTruncatedXml() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="Bar">
                  <arg type="s" direction="in"/>
                </method>
                <signal name="Baz"/>
                <!-- truncated: missing </interface> and </node> -->
        )");

        // Must not hang — parser returns partial data
        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        QVERIFY(data.methodNames.contains(QStringLiteral("Bar")));
        QVERIFY(data.signalNames.contains(QStringLiteral("Baz")));
    }

    void testSignalArgsDontPolluteMethod() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="DoIt">
                  <arg type="s" direction="in"/>
                </method>
                <signal name="Done">
                  <arg type="b"/>
                </signal>
              </interface>
            </node>
        )");

        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        // Signal <arg> must not leak into method arg list
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("DoIt")),
                 QStringList({QStringLiteral("s")}));
    }

    void testOutArgsFiltered() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="GetInfo">
                  <arg type="s" direction="in"/>
                  <arg type="s" direction="out"/>
                  <arg type="i" direction="out"/>
                </method>
              </interface>
            </node>
        )");

        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        // Only in-args collected
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("GetInfo")),
                 QStringList({QStringLiteral("s")}));
    }

    void testNoDirectionTreatedAsIn() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="DoIt">
                  <arg type="s"/>
                  <arg type="i" direction="in"/>
                </method>
              </interface>
            </node>
        )");

        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("DoIt")),
                 QStringList({QStringLiteral("s"), QStringLiteral("i")}));
    }

    void testWrongIfaceReturnsEmpty() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="Bar"/>
              </interface>
            </node>
        )");

        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Missing"));
        QVERIFY(data.methodNames.isEmpty());
        QVERIFY(data.signalNames.isEmpty());
        QVERIFY(data.propertyNames.isEmpty());
    }

    void testMultipleMethods() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="First"><arg type="s" direction="in"/></method>
                <method name="Second"><arg type="i" direction="in"/></method>
              </interface>
            </node>
        )");

        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        QCOMPARE(data.methodNames.size(), 2);
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("First")),
                 QStringList({QStringLiteral("s")}));
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("Second")),
                 QStringList({QStringLiteral("i")}));
    }
};

QTEST_GUILESS_MAIN(TestIntrospection)
#include "test_introspection.moc"
