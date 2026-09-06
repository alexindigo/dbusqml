#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

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
