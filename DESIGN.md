# dbusqml — D-Bus QML Bridge

## What

A standalone QML module that exposes D-Bus (session and system bus) to QML applications. Zero KDE dependencies — just `Qt6::DBus`.

Pattern follows `mpvqml`: a thin C++ Qt6 QML plugin that wraps existing Qt APIs into QML-friendly types.

## Why

The only existing D-Bus QML module is KDE's `org.kde.plasma.workspace.dbus`, which depends on `plasma-workspace` (55 MiB, half of KDE). Not suitable for standalone QML apps on Niri/Hyprland/etc.

## Module: `import DBus 1.0`

## Types

### `busType` (enum)

| Value | Description |
|-------|-------------|
| `busType.Session` | Session bus |
| `busType.System` | System bus |

### `DBusConnection`

A connection object. Used for raw `asyncCall()` when you don't need the rich object wrapper.

```qml
var conn = DBus.connectToBus("unix:path=/tmp/kvm-bus")
conn.asyncCall({...})
```

### `DBusMessage` (value type)

```qml
{ service, path, iface, member, signature, arguments }
```

Construct from a map or set properties individually.

### `DBusPendingReply`

```qml
var reply = SessionBus.asyncCall({...})
reply.finished.connect(() => {
    if (reply.isError) { /* reply.error.message */ }
    else { /* reply.value */ }
})
```

Also supports Promise-style: `asyncCall({...}, (result) => {}, (error) => {})`

### `DBus` — the rich proxy element

Set `service`, `path`, `iface` and optionally `connection` (defaults to session bus). When `iface` is set, introspection is called to discover:

**Properties** — become QML properties with the same name:
```qml
DBus {
    id: bat
    service: "org.freedesktop.UPower"
    path: "/org/freedesktop/UPower/devices/..."
    iface: "org.freedesktop.UPower.Device"
}
bat.percentage  // → Properties.Get(...)
```

Properties auto-update via `PropertiesChanged` signal subscription.

**Methods** — dynamically exposed as callable properties after introspection:
```qml
proxy.Play()                     // → asyncCall(member: "Play")
proxy.SetProfile("balanced")     // → asyncCall(member: "SetProfile", args: ["balanced"])
proxy.EchoString("hello")        // → asyncCall(member: "EchoString", args: ["hello"])
```

Methods retain their D-Bus casing (PascalCase). When D-Bus exposes `Play`, `EchoString`, etc., they become `proxy.Play()`, `proxy.EchoString()`.

Also available via generic `call(method, args)`:
```qml
proxy.call("Play")
proxy.call("SetProfile", ["balanced"])
```

**Signals** — received via `onSignalReceived`:
```qml
onSignalReceived: function(name, args) {
    if (name === "Seeked") { ... }
}
```

### `DBusError` (value type)

```qml
{ isValid: bool, name: string, message: string }
```

### D-Bus Type Wrappers

```qml
DBus.uint32(42)
DBus.uint64(18446744073)
DBus.string("hello")
DBus.boolean(true)
DBus.double(3.14)
DBus.objectPath("/org/freedesktop/UPower")
DBus.variant("any value")
DBus.dict({ key: "value" })
```

Used when Qt's auto-conversion doesn't produce the correct D-Bus type signature.

## Architecture

```
Bus (session or system)
  │
  ├── Connection unique name (:1.42)
  │
  ├── Well-known service (org.freedesktop.DBus)
  │     └── Object path (/org/freedesktop/DBus)
  │           └── Interface (org.freedesktop.DBus)
  │                 ├── Methods: ListNames, GetNameOwner, ...
  │                 └── Signals: NameOwnerChanged
  │
  ├── Well-known service (org.kde.kdeconnect)
  │     ├── Path (/modules/kdeconnect)
  │     │     └── Iface (org.kde.kdeconnect.daemon)
  │     │           ├── Methods: devices(), ...
  │     │           └── Properties: ...
  │     └── Path (/modules/kdeconnect/devices/{id})
  │           └── Iface (org.kde.kdeconnect.device)
  │                 ├── Methods: ...
  │                 └── Properties: name, reachable, ...
```

A D-Bus **service** owns a well-known name (like `org.freedesktop.portal.Desktop`). Under that service, there can be multiple **object paths** (like `/org/freedesktop/portal/desktop`). Each path can expose multiple **interfaces** (like `org.freedesktop.portal.Settings`).

Registering means:
1. **Service** (optional) — Claim a well-known name on the bus via `registerService()`. Without it, connections are reachable only by their unique name (`:1.42`).
2. **Path** — Register an object handler for a specific path via `registerObject()`. This is required to receive method calls.
3. **Interface** — Part of the registered object's introspection XML. Defines which methods, signals, and properties the object exposes.

The sender's unique name is included in every D-Bus message. A receiver can identify the caller via `QDBusMessage::sender()`, regardless of whether the sender owns a well-known name.

## Registration (server-side)

### `DBusAdaptor` (proposed)

Inverse of the `DBus` proxy element. Registers a D-Bus object on the bus and maps QML properties/functions/signals to D-Bus properties/methods/signals.

```qml
DBusAdaptor {
    id: portal
    service: "org.freedesktop.impl.portal.atmosphera"  // optional — omit for unique-name-only
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"

    // QML properties → D-Bus properties (read/write)
    property int preferredDarkMode: 0

    // QML functions → D-Bus methods
    function Read(namespace, key) {
        return settings[key] ?? ""
    }

    // QML signals → D-Bus signals
    signal settingChanged(string ns, string key, variant value)
}
```

The adaptor:
- Claims `service` via `registerService()` (if specified)
- Registers on `path` via `registerObject()` with `QDBusVirtualObject`
- Exposes `iface` methods from QML functions
- Exposes `iface` signals from QML signals
- Exposes `iface` properties from QML properties
- Handles incoming method calls: `QDBusVirtualObject::handleMethodCall()` dispatches to the matching QML function
- Handles property get/set: `QDBusVirtualObject::property()` or custom dispatch
- Emits signals via `QDBusConnection::send(QDBusMessage::createSignal(...))`

Multiple interfaces on the same path use multiple `DBusAdaptor` instances with the same `service` and `path` but different `iface`.

```
QML
  │
  ├── DBus (rich proxy)
  │     ├── Introspect on iface set → discover methods, signals, properties
  │     ├── Properties → QQmlPropertyMap + PropertiesChanged subscription
  │     ├── Methods   → dynamic QMetaMethod via qt_metacall override
  │     └── Signals   → dynamic QMetaSignal + QDBusConnection::connect
  │
  ├── DBusConnection (raw wrapper)
  │     └── asyncCall(message) → QDBusPendingCallWatcher
  │
  └── TypedValues → Q_GADGET value types with QML_CONSTRUCTIBLE_VALUE
```

## Non-Goals (v1)

- Exposing QML objects on D-Bus (no adaptor — would need `QDBusVirtualObject`)
- Qt5 / Qt7 support (single source, forkable)
- Service activation (caller uses existing D-Bus activation)

## Build

- CMake, `find_package(Qt6 REQUIRED COMPONENTS DBus Qml)`
- Standard Qt6 QML plugin (qmldir + .so)
- Installed to Qt's QML import path

## Dependencies

| Dependency | Why |
|------------|-----|
| `Qt6::DBus` | Core D-Bus types and connection |
| `Qt6::Qml` | QML engine integration |

Zero KDE deps. Zero other deps.

## License

GPLv3

## Implementation Order

1. `DBusMessage` (value type, wraps args + metadata)
2. `DBusConnection` + `SessionBus`/`SystemBus` + `connectToBus()`
3. `DBusPendingReply` (wraps `QDBusPendingCallWatcher`)
4. Typed wrappers (`uint32`, `string`, `variant`, etc.)
5. `DBusError`
6. `DBus` — introspection + dynamic methods/signals/properties via `qt_metacall`
7. Plugin registration + CMake build
