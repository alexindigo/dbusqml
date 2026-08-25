#include "dbusintrospection.h"

#include <QXmlStreamReader>

// Convert D-Bus PascalCase property name to QML camelCase.
// Handles abbreviations: "Percentage" → percentage, "URL" → url, "XMLConfig" → xmlConfig
static QString dbusPropToQml(const QString &name) {
    if (name.isEmpty())
        return name;
    int upper = 0;
    while (upper < name.size() && name[upper].isUpper())
        ++upper;
    if (upper <= 1)
        return name.at(0).toLower() + name.mid(1);
    return name.left(upper).toLower() + name.mid(upper);
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
