#pragma once

#include <QDBusConnection>
#include <QJSValue>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <qqmlregistration.h>

#include "dbusmessage.h"
#include "dbuspendingreply.h"

QVariant toDbusVariant(const QVariant &v);

// Nested-slot half of the slot-aware conversion pair: for values destined to a
// slot that already provides the D-Bus variant wrapper (an a{sv} dict value,
// or an explicit QDBusVariant wrap). A DBus::Variant contributes its payload
// directly, guaranteeing single-wrap semantics.
QVariant toDbusVariantNested(const QVariant &v);

// Parse one complete D-Bus type from `sig` starting at `pos`. Returns the
// type's signature substring and advances pos past it. Returns empty on parse
// failure. Used to split concatenated signatures (override strings, catalog
// out-arg lists) into per-argument signatures.
QString firstCompleteType(const QString &sig, int &pos);

// Marshal a JS-supplied QVariant against a known D-Bus signature.
// Produces a QVariant with the correct C++ type for QtDBus to marshal
// to the wire format matching `sig`. DBus.* wrapper types take priority
// over the signature. Falls back to toDbusVariant when sig is empty or
// unrecognized.
QVariant marshalBySignature(const QString &sig, const QVariant &value);

// Generic signature-walking marshaller. Builds a writable QDBusArgument for
// any producible D-Bus signature via public QtDBus primitives and returns it
// wrapped in a QVariant (cross-marshaled by QtDBus as a request or reply
// argument). Returns an invalid QVariant when the signature cannot be
// produced — callers must fail loudly rather than emit a different wire type.
QVariant writeBySignature(const QString &sig, const QVariant &value);

// Recursively unwrap QDBusVariant / QDBusArgument values into plain QVariant
// containers (QVariantMap, QVariantList) so QML can traverse them as
// JavaScript objects. Handles nested a{sv}, a{ss}, av, as, ao, etc.
QVariant unwrapDbus(const QVariant &v);
class QQmlEngine;
class QJSValue;
QJSValue precisionSafeToScriptValue(QQmlEngine *engine, const QVariant &v);

// Determine whether a QVariant can be handed to QtDBus for wire marshaling
// without corrupting the connection. Catches the unregistered/invalid class
// (QJSValue, QObject*, unregistered gadgets) that QtDBus reports as "not
// registered with D-Bus". Containers recurse — one unmarshalable element
// poisons the whole. Shared by the adaptor (reply/signal/property) and the
// client (call/message/set) exits.
bool wireMarshalable(const QVariant &v);

// Convert a QVariant into a native JS value, recursively unwrapping lists
// and maps so the JS side receives real Array / Object instances (with a
// working Array.isArray and iterable/spread semantics), not the array-like
// QVariantList wrappers QQmlEngine::toScriptValue produces by default.
QJSValue variantToJs(QQmlEngine *engine, const QVariant &v);

class busType {
    Q_GADGET
    QML_NAMED_ELEMENT(busType)
    QML_UNCREATABLE("Enum type")
public:
    enum Type { Session, System };
    Q_ENUM(Type)
};

class DBusConnection : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DBusConnection)
    // P5 (features train, Phase 3): connection-liveness surface. The
    // detector is a failing-call observation (a daemon-facing
    // NameHasOwner probe that surfaces QDBusError::Disconnected when
    // the socket breaks) — QtDBus consumes Local.Disconnected
    // internally and never delivers it to match rules, so the Nemo
    // connection.cpp:89-109 subscription shape cannot work here (see
    // onPendingCallFinished).
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    explicit DBusConnection(const QDBusConnection &conn, const QString &name,
                            QObject *parent = nullptr);
    ~DBusConnection() override;

    Q_INVOKABLE static DBusConnection *connectToBus(const QString &address);

    Q_INVOKABLE DBusPendingReply *asyncCall(const DBusMessage &message);
    Q_INVOKABLE void asyncCall(const DBusMessage &message, const QJSValue &resolve,
                               const QJSValue &reject);

    // Fire-and-forget: sends the message with NO_REPLY_EXPECTED implied —
    // nothing comes back (the OSD showText pattern).
    Q_INVOKABLE void send(const DBusMessage &message);

    // P5: liveness of the underlying bus connection. False after the
    // loss is observed (see onDisconnected); connections never
    // reconnect (documented — Nemo's reconnect() deliberately NOT
    // copied).
    bool isConnected() const { return m_connected; }

    operator QDBusConnection() const { return m_connection; }

Q_SIGNALS:
    // P5: emitted once when the bus connection drops (the daemon died or
    // the socket broke — observed via a failing call, NOT
    // Local.Disconnected, which QtDBus consumes internally and never
    // delivers to match rules). Proxies flip to Error +
    // serviceAvailable=false; adaptor claims emit nameLost (see
    // DBusPathDispatcher).
    void disconnected();
    void connectedChanged();

private Q_SLOTS:
    // P5 loss probe: any finished call carrying a Disconnected error
    // observes connection death (QtDBus fails all pending calls with
    // QDBusError::Disconnected when the socket breaks — the observable
    // equivalent of Nemo's connection.cpp:89-109 Disconnected handler,
    // which lives one layer down in libdbus where Qt already consumes
    // it).
    void onPendingCallFinished(QDBusPendingCallWatcher *w);

private:
    // Spies one call per connection-moment: the daemon-facing
    // NameHasOwner ping doubles as the loss detector (it is answered by
    // the daemon itself, so it fails if and only if the connection is
    // dead). Re-armed after every completion while connected; torn down
    // on disconnect (dead on a dead bus).
    void armLossProbe();

    QDBusConnection m_connection;
    QString m_connectionName;
    bool m_connected = true;
    QDBusPendingCallWatcher *m_lossProbe = nullptr;
};

class SessionBusConnection : public DBusConnection {
    Q_OBJECT
    QML_NAMED_ELEMENT(SessionBus)
    QML_SINGLETON

public:
    explicit SessionBusConnection(QObject *parent = nullptr);
};

class SystemBusConnection : public DBusConnection {
    Q_OBJECT
    QML_NAMED_ELEMENT(SystemBus)
    QML_SINGLETON

public:
    explicit SystemBusConnection(QObject *parent = nullptr);
};
