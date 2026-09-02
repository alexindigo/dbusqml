#pragma once

#include <QJSValue>
#include <QObject>
#include <QQmlEngine>
#include <qqmlregistration.h>

// QML singleton providing D-Bus value conversion utilities.
// Primary use: converting ay (byte array, arrives as ArrayBuffer in QML)
// to/from text for common cases like NetworkManager SSIDs.
class DBusUtils : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(DBusUtils)
    QML_SINGLETON

public:
    explicit DBusUtils(QObject *parent = nullptr);

    // Decode an ArrayBuffer (or number-array) as UTF-8 text.
    // Handles the canonical ay representation (ArrayBuffer) and the
    // legacy number-array form for migration.
    Q_INVOKABLE QString textFromBytes(const QJSValue &bytes);

    // Encode a string as UTF-8 bytes, returned as an ArrayBuffer.
    // The result can be passed directly to ay-typed D-Bus arguments
    // (the marshaller also accepts plain strings, but this is explicit).
    Q_INVOKABLE QJSValue bytesFromText(const QString &text);

    // Build a D-Bus error value to THROW from a served method handler: the
    // caller receives an error reply carrying exactly this name and message.
    // Any other thrown value becomes org.freedesktop.DBus.Error.Failed.
    Q_INVOKABLE QVariantMap error(const QString &name, const QString &message);
};
