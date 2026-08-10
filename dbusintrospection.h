#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Parsed D-Bus introspection data for a single interface.
struct DBusIntrospectionData {
    QStringList methodNames;
    QStringList signalNames;
    QHash<QString, QStringList> methodArgTypes; // in-args only, declaration order
    QStringList propertyNames;                  // always collected
};

// Parse the introspection XML for a specific interface.
// Returns empty data if the interface is not found.
// Never hangs on truncated XML — all loops check atEnd().
DBusIntrospectionData parseDBusIntrospection(const QString &xml, const QString &iface);
