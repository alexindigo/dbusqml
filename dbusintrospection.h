#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

class QIODevice;

// One XML hardening policy for every document this library parses — peer
// Introspect() replies and local type-catalog files alike:
//
//   * an input larger than 4 MiB (its encoded size: UTF-16 bytes for
//     in-memory text, device bytes for files) is refused before parsing;
//   * a DOCTYPE with only an external id is allowed (real libdbus
//     Introspect() replies carry one), but an internal DTD subset is
//     refused by a prolog scan before any input is handed to Qt's parser.
//
// Qt parses DTD declarations (including attribute defaults and parameter
// entity references) inside readNext(), before returning the DTD token.
// The token-level entity check remains as defence in depth, not the gate.
//
// A refused document yields no data: callers check refused() after the
// parse loop (atEnd() covers refusal) and discard the whole document with
// one warning.
class DBusHardenedXmlReader {
public:
    static constexpr qint64 sizeLimitBytes = 4 * 1024 * 1024;

    explicit DBusHardenedXmlReader(const QString &xml);
    explicit DBusHardenedXmlReader(QIODevice *device);

    bool refused() const { return !m_refusal.isEmpty(); }
    QString refusalReason() const { return m_refusal; }

    QXmlStreamReader::TokenType readNext();
    bool atEnd() const { return refused() || m_reader.atEnd(); }

    bool isStartElement() const { return m_reader.isStartElement(); }
    bool isEndElement() const { return m_reader.isEndElement(); }
    QStringView name() const { return m_reader.name(); }
    QXmlStreamAttributes attributes() const { return m_reader.attributes(); }
    bool hasError() const { return m_reader.hasError(); }
    QString errorString() const { return m_reader.errorString(); }

private:
    QXmlStreamReader m_reader;
    QByteArray m_bounded; // bounded, pre-scanned snapshot of any device
    QString m_refusal;
};

// Parsed D-Bus introspection data for a single interface.
struct DBusIntrospectionData {
    QStringList methodNames;
    QStringList signalNames;
    QHash<QString, QStringList> methodArgTypes; // in-args only, declaration order
    QHash<QString, QStringList> methodOutTypes; // out-args only, declaration order
    QStringList propertyNames;                  // always collected
};

// Parse the introspection XML for a specific interface.
// Returns empty data if the interface is not found.
// Never hangs on truncated XML — all loops check atEnd().
DBusIntrospectionData parseDBusIntrospection(const QString &xml, const QString &iface);

// A14/D5: THE shared D-Bus-name→QML-name fold (one implementation, two
// documented modes). collapseRuns=false — the SERVER mode (wire name → QML
// member): first character lowercased only ("URLConfig" → "uRLConfig").
// collapseRuns=true — the CLIENT mode (property wire name → QML map key):
// leading upper RUNS collapse ("URLConfig" → "urlConfig", "XMLConfig" →
// "xmlConfig", "URL" → "url"). The two modes are NOT behaviorally unified
// (renaming client map keys would break consumers); LOOKUP tolerates both
// (candidate lists accept either fold).
QString dbusFoldName(const QString &name, bool collapseRuns);
