#include "dbusintrospection.h"

#include <QXmlStreamReader>

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
    QXmlStreamReader reader(xml);
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

    return data;
}
