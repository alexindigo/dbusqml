#include <QTest>
#include <QElapsedTimer>
#include <QBuffer>
#include <QStringConverter>
#include <algorithm>
#include <cstring>
#include "dbusintrospection.h"

class ChunkedXmlDevice : public QIODevice {
public:
    ChunkedXmlDevice(const QByteArray &bytes, qint64 terminalResult, qint64 chunkSize = 7)
        : m_bytes(bytes), m_terminalResult(terminalResult), m_chunkSize(chunkSize) {}
    bool isSequential() const override { return true; }
    bool waitForReadyRead(int) override {
        ++waitCalls;
        return false;
    }
    qsizetype consumed() const { return m_pos; }
    int waitCalls = 0;

protected:
    qint64 readData(char *data, qint64 maxSize) override {
        if (maxSize == 0)
            return 0;
        if (m_pos == m_bytes.size())
            return m_terminalResult;
        const qint64 count = std::min({maxSize, m_chunkSize, qint64(m_bytes.size() - m_pos)});
        std::memcpy(data, m_bytes.constData() + m_pos, size_t(count));
        m_pos += count;
        return count;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    QByteArray m_bytes;
    qint64 m_terminalResult;
    qint64 m_chunkSize;
    qsizetype m_pos = 0;
};

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
    // --- XML hardening: one policy for every parsed document ------------
    // Input > 4 MiB and internal DTD subsets are refused before parsing.
    // External-only DOCTYPEs (real libdbus replies) remain accepted.

    void testPrologBoundary_data() {
        QTest::addColumn<QString>("prolog");
        QTest::addColumn<bool>("internalSubset");

        QTest::newRow("entity") << QStringLiteral("<!DOCTYPE node [<!ENTITY t 's'>]>") << true;
        QTest::newRow("attribute-default-entity")
            << QStringLiteral("<!DOCTYPE node [<!ENTITY t 's'><!ATTLIST arg type CDATA '&t;&t;'>]>")
            << true;
        QTest::newRow("parameter-entity")
            << QStringLiteral(
                   "<!DOCTYPE node [<!ENTITY % p '<!ATTLIST arg type CDATA \"s\">'>%p;]>")
            << true;
        QTest::newRow("empty-subset") << QStringLiteral("<!DOCTYPE node []>") << true;
        QTest::newRow("attribute-default-no-entity")
            << QStringLiteral("<!DOCTYPE node [<!ATTLIST arg type CDATA 's'>]>") << true;
        QTest::newRow("prolog-before-subset")
            << QStringLiteral("<?xml version='1.0'?> \t\r\n<!-- <!DOCTYPE fake []> -->"
                              "<?probe <!DOCTYPE fake []>?>\n<!DOCTYPE node []>")
            << true;
        QTest::newRow("external-and-subset")
            << QStringLiteral("<!DOCTYPE node SYSTEM 'unused.dtd' []>") << true;
        QTest::newRow("quoted-close-before-subset")
            << QStringLiteral("<!DOCTYPE node SYSTEM 'unused>file.dtd' []>") << true;
        QTest::newRow("truncated-subset") << QStringLiteral("<!DOCTYPE node [") << true;
        QTest::newRow("no-doctype") << QString() << false;
        QTest::newRow("bare-doctype") << QStringLiteral("<!DOCTYPE node>") << false;
        QTest::newRow("comment-not-declaration")
            << QStringLiteral("<!-- <!DOCTYPE node [<!ENTITY t 's'>]> -->") << false;
        QTest::newRow("pi-not-declaration")
            << QStringLiteral("<?probe <!DOCTYPE node []>?>") << false;
        QTest::newRow("quoted-system-bracket-single")
            << QStringLiteral("<!DOCTYPE node SYSTEM 'unused[>\"file.dtd'>") << false;
        QTest::newRow("quoted-system-bracket-double")
            << QStringLiteral("<!DOCTYPE node SYSTEM \"unused[>'file.dtd\">") << false;
        QTest::newRow("freedesktop")
            << QStringLiteral(
                   "<!DOCTYPE node PUBLIC '-//freedesktop//DTD D-BUS Object Introspection 1.0//EN' "
                   "'http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd'>")
            << false;
        QTest::newRow("comment-then-external")
            << QStringLiteral("<?xml version='1.0'?>\n<!-- <!DOCTYPE fake []> -->"
                              "<?probe ignored?><!DOCTYPE node SYSTEM 'unused.dtd'>")
            << false;
    }

    void testPrologBoundary() {
        QFETCH(QString, prolog);
        QFETCH(bool, internalSubset);
        const QString xml =
            prolog +
            QStringLiteral(
                "<node><!-- <!DOCTYPE fake []> --><![CDATA[<!DOCTYPE fake []>]]>"
                "<interface name='org.test.Foo'><method name='Bar'><arg type='s'/></method>"
                "</interface></node>");
        const QString refusal = QStringLiteral("document declares an internal DTD subset");
        auto checkReader = [&](DBusHardenedXmlReader &reader) {
            // The refusal is established by construction, before even the
            // synthetic StartDocument token: no DTD parsing/expansion can run.
            QCOMPARE(reader.refused(), internalSubset);
            if (internalSubset) {
                QCOMPARE(reader.refusalReason(), refusal);
                QVERIFY(reader.atEnd());
                QVERIFY(!reader.hasError());
                QCOMPARE(reader.readNext(), QXmlStreamReader::Invalid);
                QVERIFY(!reader.isStartElement());
            } else {
                QStringList methods;
                while (!reader.atEnd()) {
                    reader.readNext();
                    if (reader.isStartElement() && reader.name() == QLatin1String("method"))
                        methods << reader.attributes().value("name").toString();
                }
                QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
                QVERIFY(!reader.refused());
                QCOMPARE(methods, QStringList{QStringLiteral("Bar")});
            }
        };
        DBusHardenedXmlReader textReader(xml);
        checkReader(textReader);

        // Cover both device paths and the Unicode encodings Qt autodetects,
        // with/without BOM. A byte-only search must not miss a wide DOCTYPE.
        for (auto encoding :
             {QStringConverter::Utf8, QStringConverter::Utf16LE, QStringConverter::Utf16BE,
              QStringConverter::Utf32LE, QStringConverter::Utf32BE}) {
            for (auto flags : {QStringConverter::Flags{},
                               QStringConverter::Flags{QStringConverter::Flag::WriteBom}}) {
                QStringEncoder encoder(encoding, flags);
                QByteArray bytes = encoder(xml);
                QBuffer device(&bytes);
                QVERIFY(device.open(QIODevice::ReadOnly));
                DBusHardenedXmlReader deviceReader(&device);
                checkReader(deviceReader);
                ChunkedXmlDevice sequential(bytes, 0, bytes.size());
                QVERIFY(sequential.open(QIODevice::ReadOnly));
                DBusHardenedXmlReader sequentialReader(&sequential);
                checkReader(sequentialReader);
            }
        }

        g_warnings.clear();
        const QtMessageHandler prev = qInstallMessageHandler(captureWarnings);
        const auto data = parseDBusIntrospection(xml, QStringLiteral("org.test.Foo"));
        qInstallMessageHandler(prev);
        if (internalSubset) {
            QVERIFY(data.methodNames.isEmpty());
            QVERIFY(data.methodArgTypes.isEmpty());
            QCOMPARE(g_warnings.size(), 1);
            QVERIFY(g_warnings.first().contains(refusal));
        } else {
            QCOMPARE(data.methodNames, QStringList{QStringLiteral("Bar")});
            QCOMPARE(data.methodArgTypes.value(QStringLiteral("Bar")),
                     QStringList{QStringLiteral("s")});
            QVERIFY(g_warnings.isEmpty());
        }
    }

    void testDeviceDeclaredEncoding_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<bool>("internalSubset");
        for (auto encoding :
             {QStringConverter::Utf8, QStringConverter::Latin1, QStringConverter::Utf16LE,
              QStringConverter::Utf16BE, QStringConverter::Utf32LE, QStringConverter::Utf32BE}) {
            for (bool subset : {false, true}) {
                const QByteArray name = QStringConverter::nameForEncoding(encoding);
                const QString xml =
                    QStringLiteral("<?xml version='1.0' encoding='%1'?>"
                                   "<!-- \u00e9 -->%2<node/>")
                        .arg(QString::fromLatin1(name),
                             subset ? QStringLiteral("<!DOCTYPE node []>") : QString());
                QStringEncoder encoder(encoding);
                QTest::newRow((name + (subset ? "-subset" : "-accepted")).constData())
                    << QByteArray(encoder(xml)) << subset;
            }
        }
    }

    void testDeviceDeclaredEncoding() {
        QFETCH(QByteArray, bytes);
        QFETCH(bool, internalSubset);
        QBuffer device(&bytes);
        QVERIFY(device.open(QIODevice::ReadOnly));
        DBusHardenedXmlReader reader(&device);
        QCOMPARE(reader.refused(), internalSubset);
        if (internalSubset) {
            QCOMPARE(reader.refusalReason(),
                     QStringLiteral("document declares an internal DTD subset"));
            QCOMPARE(reader.readNext(), QXmlStreamReader::Invalid);
        } else {
            bool sawRoot = false;
            while (!reader.atEnd()) {
                reader.readNext();
                if (reader.isStartElement() && reader.name() == QLatin1String("node"))
                    sawRoot = true;
            }
            QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
            QVERIFY(sawRoot);
        }
    }

    void testChunkedDeviceSnapshot_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<qint64>("terminalResult");
        QTest::addColumn<QString>("outcome");
        const QByteArray body("<node><method name='Later'/></node>");
        const QByteArray prolog("<?xml version='1.0'?><!-- ignored -->");
        const QByteArray suffix("--><method name='Later'/></node>");
        QByteArray atCap("<node><!--");
        atCap +=
            QByteArray(DBusHardenedXmlReader::sizeLimitBytes - atCap.size() - suffix.size(), 'x');
        atCap += suffix;
        for (qint64 terminal : {qint64(0), qint64(-1)}) {
            const QByteArray ending = terminal == 0 ? "-zero" : "-minus-one";
            QTest::newRow(("valid" + ending).constData())
                << prolog + "<!DOCTYPE node SYSTEM 'unused[>file.dtd'>" + body << terminal
                << QStringLiteral("valid");
            QTest::newRow(("later-subset" + ending).constData())
                << prolog + "<!DOCTYPE node [<!ATTLIST method name CDATA 'Default'>]>" + body
                << terminal << QStringLiteral("subset");
            QTest::newRow(("at-cap" + ending).constData())
                << atCap << terminal << QStringLiteral("valid");
            QTest::newRow(("over-cap" + ending).constData())
                << atCap + QByteArray(100, ' ') << terminal << QStringLiteral("oversize");
            QTest::newRow(("truncated" + ending).constData())
                << prolog + "<node><method name='Later'/>" << terminal
                << QStringLiteral("truncated");
        }
    }

    void testChunkedDeviceSnapshot() {
        QFETCH(QByteArray, bytes);
        QFETCH(qint64, terminalResult);
        QFETCH(QString, outcome);
        ChunkedXmlDevice device(bytes, terminalResult);
        QVERIFY(device.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
        DBusHardenedXmlReader reader(&device);
        QCOMPARE(device.waitCalls, 0);
        if (outcome == QLatin1String("subset") || outcome == QLatin1String("oversize")) {
            QVERIFY(reader.refused());
            QCOMPARE(reader.refusalReason(),
                     outcome == QLatin1String("subset")
                         ? QStringLiteral("document declares an internal DTD subset")
                         : QStringLiteral("document exceeds the 4 MiB size limit"));
            QCOMPARE(reader.readNext(), QXmlStreamReader::Invalid);
            QVERIFY(!reader.isStartElement());
        } else {
            QVERIFY(!reader.refused());
            QStringList methods;
            while (!reader.atEnd()) {
                reader.readNext();
                if (reader.isStartElement() && reader.name() == QLatin1String("method"))
                    methods << reader.attributes().value("name").toString();
            }
            QCOMPARE(reader.hasError(), outcome == QLatin1String("truncated"));
            QCOMPARE(methods, QStringList{QStringLiteral("Later")});
        }
        // The snapshot drains all currently readable bytes, but never asks
        // for a byte beyond the cap's single oversize sentinel.
        QCOMPARE(device.consumed(),
                 std::min(qint64(bytes.size()), DBusHardenedXmlReader::sizeLimitBytes + 1));
    }

    void testDeviceSizeBoundary_data() {
        QTest::addColumn<bool>("sequential");
        QTest::addColumn<bool>("oversize");
        QTest::newRow("random-at-cap") << false << false;
        QTest::newRow("random-over-cap") << false << true;
        QTest::newRow("sequential-at-cap") << true << false;
        QTest::newRow("sequential-over-cap") << true << true;
    }

    void testDeviceSizeBoundary() {
        QFETCH(bool, sequential);
        QFETCH(bool, oversize);
        QByteArray bytes("<node><!--");
        bytes += QByteArray(DBusHardenedXmlReader::sizeLimitBytes - bytes.size() - 10, 'x');
        bytes += "--></node>";
        if (oversize)
            bytes += ' ';
        QBuffer random(&bytes);
        ChunkedXmlDevice stream(bytes, 0, bytes.size());
        QIODevice *device = sequential ? static_cast<QIODevice *>(&stream) : &random;
        QVERIFY(device->open(QIODevice::ReadOnly));
        DBusHardenedXmlReader reader(device);
        QCOMPARE(reader.refused(), oversize);
        if (oversize) {
            QVERIFY(
                reader.refusalReason().contains(QStringLiteral("exceeds the 4 MiB size limit")));
            QCOMPARE(reader.readNext(), QXmlStreamReader::Invalid);
        } else {
            while (!reader.atEnd())
                reader.readNext();
            QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        }
    }

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
        QVERIFY(g_warnings.first().contains(QStringLiteral("declares an internal DTD subset")));
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
