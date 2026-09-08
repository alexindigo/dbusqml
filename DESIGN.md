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
proxy.play()                     // → asyncCall(member: "Play")
proxy.setProfile("balanced")     // → asyncCall(member: "SetProfile", args: ["balanced"])
proxy.echoString("hello")        // → asyncCall(member: "EchoString", args: ["hello"])
```

D-Bus method names are PascalCase; QML exposes them in camelCase. The
original PascalCase name is what gets sent on the wire.

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

The module is imported once for the `DBus` element and again with an alias
for the value-type constructors, because the plain module name is shadowed
by the element:

```qml
import DBus 1.0
import DBus 1.0 as DBusQML

// ...
DBusQML.uint32(42)
DBusQML.uint64(18446744073)
DBusQML.string("hello")
DBusQML.boolean(true)
DBusQML.double(3.14)
DBusQML.objectPath("/org/freedesktop/UPower")
DBusQML.variant("any value")
DBusQML.dict({ key: "value" })
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
1. **Service** (optional) — Claim a well-known name on the bus. Without it, connections are reachable only by their unique name (`:1.42`).
2. **Path** — Register an object handler for a specific path. This is required to receive method calls.
3. **Interface** — Part of the registered object's introspection XML. Defines which methods, signals, and properties the object exposes.

The sender's unique name is included in every D-Bus message. A receiver can identify the caller via `QDBusMessage::sender()`, regardless of whether the sender owns a well-known name.

## Registration (server-side)

### `DBusAdaptor`

Inverse of the `DBus` proxy element. Registers a D-Bus object on the bus and maps QML properties/functions/signals to D-Bus properties/methods/signals. Shipped and covered by tests.

```qml
DBusAdaptor {
    id: portal
    service: "org.freedesktop.impl.portal.atmosphera"  // optional — omit for unique-name-only
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"

    // QML properties → D-Bus properties (read/write)
    property int preferredDarkMode: 0

    // QML functions → D-Bus methods (lowercase-initial: QML forbids
    // uppercase-initial function names)
    function read(namespace, key) {
        return settings[key] ?? ""
    }

    // QML signals → D-Bus signals
    signal settingChanged(string ns, string key, variant value)
}
```

The adaptor:
- Claims `service` via the dispatcher service-claim registry (first claimant
  wins; co-located adaptors share the name; `allowReplacement` /
  `replaceExisting` / `queueOnBusy` opt into daemon-mediated transfer)
- Registers on `path` via `registerVirtualObject()` with a shared
  `DBusPathDispatcher` (one virtual object per path; co-located adaptors
  route through it in attach order)
- Exposes `iface` methods from QML functions
- Exposes `iface` signals from QML signals
- Exposes `iface` properties from QML properties
- Handles incoming method calls: `QDBusVirtualObject::handleMessage()`
  dispatches to the matching QML function (explicit `_members` alias →
  exact → first-char fold)
- Handles property get/set via the Properties interface dispatch in
  `handleMessage()` (Get/GetAll/Set with catalog-typed XML)
- Emits signals via `QDBusConnection::send(QDBusMessage::createSignal(...))`

Multiple interfaces on the same path use multiple `DBusAdaptor` instances with the same `service` and `path` but different `iface`.

```
QML
  │
  ├── DBus (rich proxy — subclass of QQmlPropertyMap)
  │     ├── Introspect on iface set → discover methods, signals, properties
  │     ├── Method dispatch
  │     │     ├── Introspection XML gives {name → [arg-signatures]}.
  │     │     ├── Empty XML? The user-land catalog (types/*.xml + XDG
  │     │     │   drop-ins) fills in the gap.
  │     │     ├── For each method, an engine-side JS closure is created
  │     │     │   and stored in the property map under a camelCased name.
  │     │     │   The closure forwards to a per-proxy DbusMethodHelper
  │     │     │   (Q_INVOKABLE callMethod), which coerces each argument
  │     │     │   through toTypedDbusVariant and issues asyncCall.
  │     │     └── On re-introspection, prior method keys are cleared from
  │     │         the property map before the new set is installed, so an
  │     │         iface switch doesn't leave stale callables behind.
  │     ├── Properties → fetched via Properties.GetAll and inserted into
  │     │                 the property map; PropertiesChanged updates in
  │     │                 place. Nested a{sv}/a{ss} unwrapped to
  │     │                 QVariantMap/QVariantList so JS can traverse them.
  │     └── Signals   → QDBusConnection::connect on each discovered signal
  │                       name → emitted through signalReceived(name, args).
  │
  ├── DBusAdaptor (server-side, subclass of QDBusVirtualObject)
  │     ├── Builds introspection XML from Q_PROPERTY / method / signal
  │     │   metadata of the enclosing QML component (declared out-arg
  │     │   types via `declaredOutTypes`: explicit `_signatures` →
  │     │   catalog declaration → stable inference; catalog in-arg
  │     │   types; `_signals` signal arg types; `_members` wire-name
  │     │   aliases; folded signal wire names).
  │     ├── handleMessage dispatches to matching QML method via
  │     │   QJSValue::callWithInstance (arguments passed as native JS
  │     │   values through precision-safe conversion; throw-after-hold
  │     │   settles the held reply; RAII dispatch-context stack for
  │     │   re-entrant dispatches; send results checked).
  │     ├── Deferred replies via holdReply() → DBusHeldReply (send /
  │     │   sendError; exactly-one-reply-per-serial structural).
  │     ├── Co-located adaptors share one path dispatcher + service
  │     │   claim (tombstone/pending-state teardown markers; per-call
  │     │   Request adaptors + unregister() for the cancel lifecycle).
  │     └── Properties.Get / GetAll / Set served from QMetaProperty
  │         (+ PropertiesChanged emission on notify; declared signal
  │         types applied at emission; relay guard on relayed args).
  │
  ├── DBusConnection (raw wrapper)
  │     └── asyncCall(message) → QDBusPendingCallWatcher
  │           Promise overload: resolve receives the native reply value
  │           (containers unwrapped); reject receives { name, message }.
  │
  └── TypedValues → Q_GADGET value types with QML_CONSTRUCTIBLE_VALUE.
```

### Why JS closures instead of runtime QMetaMethod?

Attempting to synthesize new `Q_INVOKABLE` slots at runtime via
`QMetaObjectBuilder` was tried and rejected: the property-map integration
becomes fragile, and Qt's dynamic-meta machinery is private and unstable
across point releases. JS closures stored in `QQmlPropertyMap` are
callable from QML with no meta-object surgery and survive engine
lifecycle events cleanly.

### Why user-land catalog instead of introspection-only?

Some services (Chromium-based MPRIS players, minimal system daemons)
return an empty `<node></node>` from `Introspect()` but still implement
their documented interface. The catalog lets dbusqml call those services
by name without forcing the user to fall back to `proxy.call(...)`.
Two scan paths: descriptors bundled with the library (`qrc:/dbusqml/types/`)
plus user/system drop-ins (`$XDG_CONFIG_HOME/dbusqml/types/`,
`$XDG_DATA_DIRS/*/dbusqml/types/`, `$DBUSQML_TYPES_PATH` override) —
see `docs/TYPES.md`.

## Dependencies (`DBus` proxy + `DBusAdaptor`)

| Dependency | Why |
|------------|-----|
| `Qt6::DBus` | Core D-Bus types and connection |
| `Qt6::Qml` | QML engine integration |

Zero KDE deps. Zero other deps.

## License

GPLv3
