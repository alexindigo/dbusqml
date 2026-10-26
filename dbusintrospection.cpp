#include "dbusintrospection.h"

#include <QIODevice>
#include <QStringConverter>

static bool isXmlSpace(QChar c) {
    return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n';
}

// Only inspect the prolog. Markup-looking text in comments, PIs, quoted
// external identifiers and the document body is not a DTD declaration.
static bool declaresInternalSubset(QStringView xml) {
    qsizetype pos = !xml.isEmpty() && xml.front() == QChar::ByteOrderMark ? 1 : 0;
    while (pos < xml.size()) {
        if (isXmlSpace(xml[pos])) {
            ++pos;
        } else if (xml.sliced(pos).startsWith(QLatin1String("<!--"))) {
            const qsizetype end = xml.indexOf(QLatin1String("-->"), pos + 4);
            if (end < 0)
                return false; // malformed XML remains the parser's responsibility
            pos = end + 3;
        } else if (xml.sliced(pos).startsWith(QLatin1String("<?"))) {
            const qsizetype end = xml.indexOf(QLatin1String("?>"), pos + 2);
            if (end < 0)
                return false;
            pos = end + 2;
        } else if (xml.sliced(pos).startsWith(QLatin1String("<!DOCTYPE"))) {
            pos += 9;
            QChar quote;
            while (pos < xml.size()) {
                const QChar c = xml[pos++];
                if (!quote.isNull()) {
                    if (c == quote)
                        quote = QChar();
                } else if (c == u'\'' || c == u'"') {
                    quote = c;
                } else if (c == u'[') {
                    return true;
                } else if (c == u'>') {
                    break;
                }
            }
        } else {
            return false;
        }
    }
    return false;
}

// Match Qt's byte-input encoding detection, including an explicit encoding
// in the XML declaration. Decoding is bounded by the input cap and does not
// parse a DTD or resolve entities. The original bytes still go to Qt.
static QString decodeXmlForProlog(const QByteArray &bytes) {
    const auto encoding =
        QStringDecoder::encodingForData(bytes, u'<').value_or(QStringDecoder::Utf8);
    QStringDecoder decoder(encoding);
    QString text = decoder(bytes);
    QStringView xml(text);
    if (!xml.isEmpty() && xml.front() == QChar::ByteOrderMark)
        xml = xml.sliced(1);
    if (!xml.startsWith(QLatin1String("<?xml")) || xml.size() <= 5 || !isXmlSpace(xml[5]))
        return text;
    const qsizetype end = xml.indexOf(QLatin1String("?>"), 5);
    if (end < 0)
        return text;
    xml = xml.first(end);
    qsizetype pos = 5;
    while (pos < xml.size()) {
        while (pos < xml.size() && isXmlSpace(xml[pos]))
            ++pos;
        const qsizetype nameStart = pos;
        while (pos < xml.size() && !isXmlSpace(xml[pos]) && xml[pos] != u'=')
            ++pos;
        const QStringView name = xml.sliced(nameStart, pos - nameStart);
        while (pos < xml.size() && isXmlSpace(xml[pos]))
            ++pos;
        if (pos == xml.size() || xml[pos++] != u'=')
            break;
        while (pos < xml.size() && isXmlSpace(xml[pos]))
            ++pos;
        if (pos == xml.size() || (xml[pos] != u'\'' && xml[pos] != u'"'))
            break;
        const QChar quote = xml[pos++];
        const qsizetype valueStart = pos;
        pos = xml.indexOf(quote, pos);
        if (pos < 0)
            break;
        if (name == QLatin1String("encoding")) {
            QStringDecoder declared(xml.sliced(valueStart, pos - valueStart));
            if (declared.isValid())
                return declared(bytes);
            break; // Qt reports an unsupported encoding
        }
        ++pos;
    }
    return text;
}

DBusHardenedXmlReader::DBusHardenedXmlReader(const QString &xml) {
    if (qint64(xml.size()) * qint64(sizeof(QChar)) > sizeLimitBytes) {
        m_refusal = QStringLiteral("document exceeds the %1 MiB size limit")
                        .arg(sizeLimitBytes / 1024 / 1024);
        return;
    }
    if (declaresInternalSubset(xml)) {
        m_refusal = QStringLiteral("document declares an internal DTD subset");
        return;
    }
    m_reader.addData(xml);
}

DBusHardenedXmlReader::DBusHardenedXmlReader(QIODevice *device) {
    if (!device) {
        m_refusal = QStringLiteral("no input device");
        return;
    }
    if (!device->isSequential() && device->size() > sizeLimitBytes) {
        m_refusal = QStringLiteral("document exceeds the %1 MiB size limit")
                        .arg(sizeLimitBytes / 1024 / 1024);
        return;
    }
    // Snapshot every device before parsing, including non-sequential files:
    // Qt must never read unexamined bytes (or a later revision of the file).
    char chunk[16 * 1024];
    while (m_bounded.size() <= sizeLimitBytes) {
        const qint64 remaining = sizeLimitBytes + 1 - m_bounded.size();
        const qint64 count = device->read(chunk, qMin(remaining, qint64(sizeof(chunk))));
        // A short positive read is not EOF. Both 0 (no current data) and
        // -1 (error or permanent stream end) stop this non-blocking snapshot;
        // the accumulated bytes still go through the same XML validation.
        if (count <= 0)
            break;
        m_bounded.append(chunk, count);
    }
    if (m_bounded.size() > sizeLimitBytes) {
        m_refusal = QStringLiteral("document exceeds the %1 MiB size limit")
                        .arg(sizeLimitBytes / 1024 / 1024);
        m_bounded.clear();
        return;
    }
    if (declaresInternalSubset(decodeXmlForProlog(m_bounded))) {
        m_refusal = QStringLiteral("document declares an internal DTD subset");
        return;
    }
    m_reader.addData(m_bounded);
}

QXmlStreamReader::TokenType DBusHardenedXmlReader::readNext() {
    if (refused())
        return QXmlStreamReader::Invalid;
    const QXmlStreamReader::TokenType token = m_reader.readNext();
    // Defence in depth only: Qt has already parsed the internal subset by
    // this point, including attribute defaults and parameter references.
    if (token == QXmlStreamReader::DTD && !m_reader.entityDeclarations().isEmpty())
        m_refusal = QStringLiteral("document declares XML entities");
    return token;
}

// A14/D5: the shared fold (see dbusintrospection.h for the two modes). The
// client copy previously folded upper RUNS; the server copy folded the first
// character only — both duplicated across files, with doc comments claiming
// "same rule". One implementation, modes documented at the call sites,
// lookup tolerant of both (candidate lists carry both folds).
QString dbusFoldName(const QString &name, bool collapseRuns) {
    if (name.isEmpty())
        return name;
    if (!collapseRuns) {
        // SERVER mode: first character only ("URLConfig" → "uRLConfig").
        return name.at(0).toLower() + name.mid(1);
    }
    // CLIENT mode: leading upper RUNS collapse with the word-boundary
    // exception — the final uppercase before a lowercase letter starts the
    // next word: "XMLConfig" → "xmlConfig", "URL" → "url".
    int upper = 0;
    while (upper < name.size() && name[upper].isUpper())
        ++upper;
    if (upper <= 1)
        return name.at(0).toLower() + name.mid(1);
    if (upper < name.size())
        --upper;
    return name.left(upper).toLower() + name.mid(upper);
}

static QString dbusPropToQml(const QString &name) {
    return dbusFoldName(name, true);
}

// Only <arg direction="in"> or <arg> without a direction attribute.
static bool isDBusInArg(const QXmlStreamAttributes &attrs) {
    const auto dir = attrs.value(QStringLiteral("direction"));
    return dir.isEmpty() || dir == QLatin1String("in");
}

DBusIntrospectionData parseDBusIntrospection(const QString &xml, const QString &iface) {
    DBusIntrospectionData data;
    DBusHardenedXmlReader reader(xml);
    QString currentMethod;
    QStringList currentArgs;
    QStringList currentOutArgs;

    auto flushMethod = [&]() {
        if (currentMethod.isEmpty())
            return;
        data.methodArgTypes.insert(currentMethod, currentArgs);
        data.methodArgTypes.insert(dbusPropToQml(currentMethod), currentArgs);
        data.methodOutTypes.insert(currentMethod, currentOutArgs);
        data.methodOutTypes.insert(dbusPropToQml(currentMethod), currentOutArgs);
        currentMethod.clear();
        currentArgs.clear();
        currentOutArgs.clear();
    };

    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;

        if (reader.name() != QLatin1String("interface") ||
            reader.attributes().value(QStringLiteral("name")) != iface) {
            continue;
        }

        // Found the target interface. Walk its children.
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isEndElement() && reader.name() == QLatin1String("interface"))
                break;
            if (!reader.isStartElement())
                continue;

            const auto name = reader.name();

            if (name == QLatin1String("signal")) {
                // Flush pending method before starting a signal block
                // so signal <arg>s don't pollute the method's arg list.
                flushMethod();
                data.signalNames << reader.attributes().value(QStringLiteral("name")).toString();
            } else if (name == QLatin1String("method")) {
                flushMethod();
                currentMethod = reader.attributes().value(QStringLiteral("name")).toString();
                data.methodNames << currentMethod;
            } else if (name == QLatin1String("arg") && !currentMethod.isEmpty()) {
                const auto attrs = reader.attributes();
                const QString type = attrs.value(QStringLiteral("type")).toString();
                if (isDBusInArg(attrs))
                    currentArgs << type;
                else
                    currentOutArgs << type;
            } else if (name == QLatin1String("property")) {
                data.propertyNames << reader.attributes().value(QStringLiteral("name")).toString();
            }
        }

        // Flush the last method when the interface ends
        flushMethod();

        if (reader.hasError()) {
            qWarning("DBus: introspection XML error for %s: %s", qPrintable(iface),
                     qPrintable(reader.errorString()));
        }

        break; // only one target interface
    }

    // Refusal: no data from the document is used, one warning.
    if (reader.refused()) {
        qWarning("DBus: introspection XML refused for %s: %s", qPrintable(iface),
                 qPrintable(reader.refusalReason()));
        return {};
    }

    return data;
}
