#include <QTest>
#include <QElapsedTimer>
#include "dbusintrospection.h"

// Warning capture for the refusal tests: a refused document must produce
// exactly one warning (nothing silently swallowed, no warning storm).
static QStringList g_warnings;
static void captureWarnings(QtMsgType type, const QMessageLogContext &, const QString &msg) {
    if (type == QtWarningMsg)
        g_warnings.append(msg);
}

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

    void testOutArgsCollected() {
        const QString xml = QStringLiteral(R"(
            <node>
              <interface name="org.test.Foo">
                <method name="GetInfo">
                  <arg type="s" direction="in"/>
                  <arg type="a{sa{sv}}" direction="out"/>
                </method>
                <method name="ReadAll">
                  <arg type="as" direction="in"/>
                  <arg type="a{sa{sv}}" direction="out"/>
                </method>
              </interface>
            </node>
        )");

        auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        QCOMPARE(data.methodOutTypes.value(QStringLiteral("GetInfo")),
                 QStringList({QStringLiteral("a{sa{sv}}")}));
        QCOMPARE(data.methodOutTypes.value(QStringLiteral("ReadAll")),
                 QStringList({QStringLiteral("a{sa{sv}}")}));
        // CamelCase lookup mirror also populated
        QCOMPARE(data.methodOutTypes.value(QStringLiteral("readAll")),
                 QStringList({QStringLiteral("a{sa{sv}}")}));
        // In-args still separated
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("ReadAll")),
                 QStringList({QStringLiteral("as")}));
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
    // --- XML hardening (P5): one policy for every parsed document -------
    // Input > 4 MiB is refused before parsing; a DOCTYPE is allowed (real
    // libdbus Introspect() replies carry one) but ANY entity declaration
    // refuses the document at the DTD token — before any content, so
    // before any entity expansion can happen.

    void testEntityDeclarationsRefused_data() {
        QTest::addColumn<QString>("xml");

        // Billion-laughs shape: nested entity references in an attribute.
        const QString bomb = QStringLiteral(
                                 R"(<!DOCTYPE node [
<!ENTITY a "%1">
<!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;">
<!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;">
<!ENTITY d "&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;">
]>
<node>
  <interface name="org.test.Foo">
    <method name="Bar">
      <arg type="&d;" direction="in"/>
    </method>
  </interface>
</node>)")
                                 .arg(QString(64, u'x'));
        QTest::newRow("nested-bomb-attribute") << bomb;

        // Entities individually under Qt's per-entity expansion limit,
        // referenced many times — the aggregate blowup that limit does
        // not bound. Refused by declaration, not by expansion cost.
        QStringList methods;
        methods.reserve(3000);
        for (int i = 0; i < 3000; ++i)
            methods << QStringLiteral(
                           "<method name=\"M%1\"><arg type=\"&pad;\" direction=\"in\"/></method>")
                           .arg(i);
        const QString many =
            QStringLiteral("<!DOCTYPE node [ <!ENTITY pad \"%1\"> ]>\n"
                           "<node><interface name=\"org.test.Foo\">%2</interface></node>")
                .arg(QString(1000, u'y'), methods.join(QString()));
        QTest::newRow("many-references-under-limit") << many;
    }

    void testEntityDeclarationsRefused() {
        QFETCH(QString, xml);

        g_warnings.clear();
        const QtMessageHandler prev = qInstallMessageHandler(captureWarnings);
        QElapsedTimer timer;
        timer.start();
        const DBusIntrospectionData data =
            parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        const qint64 elapsed = timer.elapsed();
        qInstallMessageHandler(prev);

        // Refused fast — no expansion work is ever done.
        QVERIFY2(elapsed < 1000, qPrintable(QStringLiteral("parse took %1 ms").arg(elapsed)));
        // No data from a refused document is used.
        QVERIFY(data.methodNames.isEmpty());
        QVERIFY(data.signalNames.isEmpty());
        QVERIFY(data.propertyNames.isEmpty());
        QVERIFY(data.methodArgTypes.isEmpty());
        QVERIFY(data.methodOutTypes.isEmpty());
        // Exactly one warning, naming the refusal.
        QCOMPARE(g_warnings.size(), 1);
        QVERIFY(g_warnings.first().contains(QStringLiteral("refused")));
        QVERIFY(g_warnings.first().contains(QStringLiteral("declares XML entities")));
    }

    void testOversizeDocumentRefused_data() {
        QTest::addColumn<QString>("xml");
        QTest::addColumn<bool>("accepted");

        // The cap is 4 MiB of the document as parsed (UTF-16 code units).
        const qsizetype limitBytes = 4 * 1024 * 1024;
        const QString prefix = QStringLiteral("<node><!--");
        const QString suffix =
            QStringLiteral("--><interface name=\"org.test.Foo\"><method name=\"Bar\">"
                           "<arg type=\"s\" direction=\"in\"/></method></interface></node>");
        const qsizetype padAtLimit = limitBytes / 2 - prefix.size() - suffix.size();
        QTest::newRow("at-4MiB-accepted") << prefix + QString(padAtLimit, u'x') + suffix << true;
        QTest::newRow("over-4MiB-refused")
            << prefix + QString(padAtLimit + 1, u'x') + suffix << false;
    }

    void testOversizeDocumentRefused() {
        QFETCH(QString, xml);
        QFETCH(bool, accepted);

        g_warnings.clear();
        const QtMessageHandler prev = qInstallMessageHandler(captureWarnings);
        const DBusIntrospectionData data =
            parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        qInstallMessageHandler(prev);

        if (accepted) {
            QCOMPARE(data.methodNames, QStringList({QStringLiteral("Bar")}));
            QCOMPARE(data.methodArgTypes.value(QStringLiteral("Bar")),
                     QStringList({QStringLiteral("s")}));
            QCOMPARE(g_warnings.size(), 0);
        } else {
            QVERIFY(data.methodNames.isEmpty());
            QVERIFY(data.signalNames.isEmpty());
            QVERIFY(data.propertyNames.isEmpty());
            QCOMPARE(g_warnings.size(), 1);
            QVERIFY(g_warnings.first().contains(QStringLiteral("refused")));
            QVERIFY(g_warnings.first().contains(QStringLiteral("exceeds the 4 MiB size limit")));
        }
    }

    void testRealLibdbusDoctypeAccepted() {
        // A real libdbus Introspect() reply starts with this DOCTYPE and
        // declares no entities — it must keep parsing in full.
        const QString xml = QStringLiteral(
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.freedesktop.DBus\">\n"
            "    <method name=\"Hello\">\n"
            "      <arg direction=\"out\" type=\"s\"/>\n"
            "    </method>\n"
            "    <method name=\"RequestName\">\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"in\" type=\"u\"/>\n"
            "      <arg direction=\"out\" type=\"u\"/>\n"
            "    </method>\n"
            "    <signal name=\"NameOwnerChanged\">\n"
            "      <arg type=\"s\"/>\n"
            "      <arg type=\"s\"/>\n"
            "      <arg type=\"s\"/>\n"
            "    </signal>\n"
            "    <signal name=\"NameLost\">\n"
            "      <arg type=\"s\"/>\n"
            "    </signal>\n"
            "  </interface>\n"
            "  <interface name=\"org.test.HasProps\">\n"
            "    <property name=\"Enabled\" type=\"b\" access=\"readwrite\"/>\n"
            "    <property name=\"Version\" type=\"u\" access=\"read\"/>\n"
            "  </interface>\n"
            "</node>");

        g_warnings.clear();
        const QtMessageHandler prev = qInstallMessageHandler(captureWarnings);
        const DBusIntrospectionData data =
            parseDBusIntrospection(xml, QStringLiteral("org.freedesktop.DBus"));
        const DBusIntrospectionData props =
            parseDBusIntrospection(xml, QStringLiteral("org.test.HasProps"));
        qInstallMessageHandler(prev);

        QCOMPARE(data.methodNames,
                 QStringList({QStringLiteral("Hello"), QStringLiteral("RequestName")}));
        QCOMPARE(data.signalNames,
                 QStringList({QStringLiteral("NameOwnerChanged"), QStringLiteral("NameLost")}));
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("Hello")), QStringList());
        QCOMPARE(data.methodOutTypes.value(QStringLiteral("Hello")),
                 QStringList({QStringLiteral("s")}));
        QCOMPARE(data.methodArgTypes.value(QStringLiteral("RequestName")),
                 QStringList({QStringLiteral("s"), QStringLiteral("u")}));
        QCOMPARE(data.methodOutTypes.value(QStringLiteral("RequestName")),
                 QStringList({QStringLiteral("u")}));
        QCOMPARE(props.propertyNames,
                 QStringList({QStringLiteral("Enabled"), QStringLiteral("Version")}));
        QVERIFY(props.methodNames.isEmpty());
        QCOMPARE(g_warnings.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestIntrospection)
#include "test_introspection.moc"
