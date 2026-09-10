# DBus QML API Documentation

This document describes the public API of the `DBus` QML module.

## Module Import

To use the module in QML, import it as follows:

```qml
import DBus 1.0
```

## Core Components

### `busType` (Enum)

```qml
busType.Session      // The session bus (personal applications)
busType.System       // The system bus (system services)
```

### Bus Connections

The module provides singleton access to the standard D-Bus connections.

#### `SessionBus`
A singleton representing the Session Bus.

#### `SystemBus`
A singleton representing the System Bus.

Example usage:
```qml
import DBus 1.0

Item {
    property var sessionBus: SessionBus
    property var systemBus: SystemBus
}
```

---

### `DBus` (Proxy Element)

**`callTimeout`:** the proxy's `callTimeout` property (milliseconds, `-1` =
Qt default) governs ALL proxy traffic — `call()`, dynamic methods,
`getProperty`/`setProperty`, and the internal `Introspect`/`GetAll` startup
calls. Per-call `message.timeout` overrides on raw `asyncCall`. Absent-
service hangs under long/infinite timeouts are the domain of
`watchServiceStatus`/`status`.

**Fire-and-forget:** `send(method, args)` issues the method call with
`NO_REPLY_EXPECTED` implied — nothing comes back (the OSD `showText`
pattern). `DBusConnection.send(message)` is the message-object form.

The `DBus` element represents a D-Bus object. When `iface` is set, it introspects the remote object and discovers:

**Dynamic methods** — D-Bus methods become callable directly on the element. D-Bus method names are PascalCase; the QML surface exposes them in camelCase:
```qml
DBus {
    id: mpris
    service: "org.mpris.MediaPlayer2.myplayer"
    path: "/org/mpris/MediaPlayer2"
    iface: "org.mpris.MediaPlayer2.Player"
}
mpris.playPause()          // → calls "PlayPause" on the D-Bus object
mpris.seek(50000000)       // → calls "Seek" with argument
mpris.setVolume(0.8)       // → calls "SetVolume" with argument
```

**Dynamic properties** — D-Bus properties are exposed as QML properties with the same name:
```qml
DBus {
    id: bat
    service: "org.freedesktop.UPower"
    path: "/org/freedesktop/UPower/devices/battery_BAT0"
    iface: "org.freedesktop.UPower.Device"
}
bat.percentage             // → Properties.Get("...Device", "Percentage")
bat.isPresent              // → Properties.Get("...Device", "IsPresent")
bat.timeToEmpty            // → Properties.Get("...Device", "TimeToEmpty")
```

Properties auto-update via `PropertiesChanged` signals. Property names follow QML camelCase — see Property Name Conventions below.

#### Configuration Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The D-Bus service name. |
| `path` | `string` | The D-Bus object path. |
| `iface` | `string` | The D-Bus interface name. |
| `connection` | `DBusConnection` | The connection associated with this proxy. |
| `callTimeout` | `int` | Per-call timeout in milliseconds (`-1` = default). See §`DBus` (Proxy Element). |
| `_signatures` | `var` (object) | Explicit call-argument signatures, keyed by D-Bus member name (see Shape Selection). |

#### Runtime Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `status` | `enum` | Null (0), Loading (1), Ready (2), Error (3) — introspection state. |
| `serviceAvailable` | `bool` | Whether the remote service is currently on the bus (requires `watchServiceStatus`). |
| `signalsEnabled` | `bool` | Whether D-Bus signal subscription is active (default true). |
| `watchServiceStatus` | `bool` | Whether to monitor if the remote service is running (default false). |
| `propertiesEnabled` | `bool` | Whether to auto-fetch D-Bus properties after introspection (default true). |

#### Built-in Methods

| Method | Arguments | Returns | Description |
| :--- | :--- | :--- | :--- |
| `call(method, args)` | `string method`, `list args` | `DBusPendingReply` | Call any D-Bus method. Returns a pending reply for error/result feedback. |
| `send(method, args)` | `string method`, `list args` | — | Fire-and-forget call (`NO_REPLY_EXPECTED`); no reply object. |
| `getProperty(name)` | `string name` | `DBusPendingReply` | Read a single D-Bus property directly via `Properties.Get`. |
| `setProperty(name, value)` | `string name`, `variant value` | — | Write a D-Bus property directly via `Properties.Set`. |
| `propertyWriteFailed(name, errorName, message)` | `string`, `string`, `string` | Signal: a `Set` the proxy issued was rejected (P8 — the QML-visible value is rolled back). |
| `emitSignal(name, args)` | `string name`, `list args` | — | Emit a D-Bus signal from this proxy's path/interface. |
| `connectToBus(address)` | `string address` | `DBusConnection` | (static) Connect to a custom D-Bus address. Returns null on failure. |
| `reloadTypes()` | — | — | (static) Re-scan the type catalog after drop-in changes. See `docs/TYPES.md`. |

**Boundary errors:** arguments that cannot be marshaled (a
`QObject*` in a map, a JS function, an unregistered type) fail the call
**locally** — the returned `DBusPendingReply` completes with
`org.freedesktop.DBus.Error.Failed` and "argument N is not marshalable"
instead of the process aborting inside QtDBus. Property writes with
unmarshalable values are dropped with a warning; `emitSignal` skips the send.
A malformed `_signatures` value warns ("unparseable declared signature") and
falls back to inference.

#### Configuration Signals

| Signal | Arguments | Description |
| :--- | :--- | :--- |
| `serviceChanged` | | Emitted when the `service` property changes. |
| `pathChanged` | | Emitted when the `path` property changes. |
| `ifaceChanged` | | Emitted when the `iface` property changes. |
| `connectionChanged` | | Emitted when the `connection` property changes. |

#### Lifecycle Signals

| Signal | Arguments | Description |
| :--- | :--- | :--- |
| `statusChanged` | | Emitted when `status` changes (Null/Loading/Ready/Error). |
| `introspectionCompleted` | | Emitted after introspection finishes and dynamic methods/properties are ready. |
| `serviceAvailableChanged` | | Emitted when `serviceAvailable` changes (requires `watchServiceStatus`). |
| `propertyWriteFailed(name, errorName, message)` | `string`, `string`, `string` | Emitted when a property `Set` the proxy issued is rejected (P8 — the QML-visible value is rolled back). |

#### Data Signals

| Signal | Arguments | Description |
| :--- | :--- | :--- |
| `signalReceived(name, args)` | `string name`, `list args` | Emitted when a D-Bus signal is received. |
| `valueChanged(key, value)` | `string key`, `variant value` | Emitted when a dynamic property changes (from QQmlPropertyMap). |

#### Toggle Signals

| Signal | Description |
| :--- | :--- |
| `signalsEnabledChanged` | Emitted when `signalsEnabled` is toggled. |
| `propertiesEnabledChanged` | Emitted when `propertiesEnabled` is toggled. |

Example usage:

```qml
import DBus 1.0

DBus {
    id: notifications
    service: "org.freedesktop.Notifications"
    path: "/org/freedesktop/Notifications"
    iface: "org.freedesktop.Notifications"
    connection: SessionBus

    onSignalReceived: (name, args) => {
        console.log("Signal received:", name, "with args:", args)
    }

    Component.onCompleted: {
        // Dynamic method — calls D-Bus method "Notify" with all arguments
        Notify("app", 0, "", "Hello", "World", [], {}, 5000)
    }
}
```

---

### `DBusConnection`

`DBusConnection` provides methods for making asynchronous calls to the bus. Obtain one via `connectToBus()` or reuse the built-in `SessionBus`/`SystemBus`:

```qml
// Use the built-in singletons
SessionBus.asyncCall({...})
SystemBus.asyncCall({...})

// Connect to a custom bus address
var custom = DBus.connectToBus("unix:path=/tmp/kvm-bus")
custom.asyncCall({...})
```

#### Methods

| Method | Arguments | Description |
| :--- | :--- | :--- |
| `asyncCall(message)` | `dbusMessage message` | Returns a `DBusPendingReply`. |
| `asyncCall(message, resolve, reject)` | `dbusMessage message`, `function resolve`, `function reject` | Promise-style asynchronous call. |

#### Connection properties and signals (P5)

| Property / Signal | Type | Description |
| :--- | :--- | :--- |
| `connected` | `bool` | Liveness of the bus connection (false after the loss is observed). Never reconnects (documented). |
| `disconnected()` | signal | Emitted once when the bus connection drops. Proxies flip to `Error` + `serviceAvailable=false`; served claims emit `nameLost`. |

Example usage — pending reply style:
```qml
var reply = SessionBus.asyncCall({
    service: "org.freedesktop.DBus",
    path: "/",
    iface: "org.freedesktop.DBus",
    member: "ListNames",
})
reply.finished.connect(() => {
    if (reply.isError) {
        console.error("Error:", reply.error.message)
    } else {
        console.log("Reply:", reply.value)
    }
})
```

Example usage — promise/callback style:
```qml
SessionBus.asyncCall({
    service: "org.freedesktop.DBus",
    path: "/",
    iface: "org.freedesktop.DBus",
    member: "ListNames",
}, (result) => {
    console.log("Success:", result)
}, (error) => {
    console.error("Error:", error.message)
})
```

---

### `DBusSignalWatcher`

Subscribes to signals on any bus with no proxy and no introspection
requirement. Empty `service`/`member` fields are wildcards (the
dbus-monitor shape, including `NameOwnerChanged` on the daemon and
system-bus signals).

```qml
DBusSignalWatcher {
    connection: SystemBus          // or SessionBus / custom
    service: ""                    // any sender
    path: "/org/freedesktop/DBus"
    iface: "org.freedesktop.DBus"
    member: "NameOwnerChanged"
    enabled: true                  // toggling re-subscribes
    onReceived: (member, args) => console.log(member, args)
}
```

Delivery is the `received(member, args)` signal with unwrapped args.

### `DBusServiceWatcher`

Watches one well-known name for appear/disappear/owner change on any bus —
the standalone complement to the proxy's `watchServiceStatus`.

```qml
DBusServiceWatcher {
    service: "org.bluez"
    onOwnerChanged: (oldOwner, newOwner) => console.log("bluez:", oldOwner, "->", newOwner)
    onRegisteredChanged: console.log("registered:", registered)
}
```

`registered` is read-only and tracks the name's presence.

### `DBusObjectManager`

Client for `org.freedesktop.DBus.ObjectManager`: fetches `GetManagedObjects`
and follows `InterfacesAdded`/`InterfacesRemoved`.

```qml
DBusObjectManager {
    service: "org.bluez"
    path: "/"
    onManagedObjectsChanged: console.log(Object.keys(managedObjects))
    onInterfacesAdded: (objectPath, ifaces) => console.log("+", objectPath, Object.keys(ifaces))
    onInterfacesRemoved: (objectPath, ifaces) => console.log("-", objectPath, ifaces)
}
```

`ready` turns true after the initial `GetManagedObjects` completes;
`managedObjects` maps object path → { interface → { property → value }}.

### `DBusAdaptor` (Server Element)

The `DBusAdaptor` element registers a D-Bus object on the bus and responds to incoming method calls. It is the server-side counterpart of the `DBus` proxy element.

```qml
DBusAdaptor {
    id: adaptor
    service: "org.freedesktop.impl.portal.atmosphera"
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"

    // QML properties become D-Bus properties (readable via Properties.Get/GetAll)
    property int colorScheme: 0

    // QML functions become D-Bus methods (callable from other processes)
    function readSetting(ns, key) {
        if (ns === "org.freedesktop.appearance" && key === "color-scheme")
            return colorScheme
        return ""
    }
}
```

When the adaptor is registered on the bus, other processes can call `Get`, `GetAll`, `Set` on its properties, and invoke its QML functions as D-Bus methods.

**Naming: the explicit → declared → fold ladder.** D-Bus members are
PascalCase (`ReadOne`, `ReadAll`), but QML forbids uppercase-initial names.
The adaptor resolves names in three tiers — the first that declares a name
wins, and the same resolution drives dispatch, served introspection XML,
`GetAll` keys, and `PropertiesChanged` names, so the wire and the XML can
never disagree:

1. **Explicit** — a `_members` map (`{ "Delete": "doDelete" }`: wire name →
   QML name) makes reserved-word or colliding members servable, and a
   `_signals` map (`{ "StateChanged": "oa{sv}" }`: wire signal name →
   concatenated arg signature) declares wire signals that are emitted via
   `emitSignal` but not expressible as QML `signal` declarations.
2. **Declared** — the interface's catalog XML (bundled types, the
   freedesktop-standard `<data>/dbus-1/interfaces/` directory, or
   user-supplied XML). Catalog method in-arg types are also served.
3. **Stable inference** — the deterministic first-character fold
   (`readOne` ⇄ `ReadOne`). The same fold in reverse is the advertised-name
   fallback: an undeclared QML member is advertised wire-cased on the bus.
   On the client the property-map fold collapses leading uppercase RUNS with
   the word-boundary exception (`URLConfig` → `urlConfig`, `XMLConfig` →
   `xmlConfig`, `URL` → `url`); the two folds are the two documented modes of
   one shared implementation, and lookups accept BOTH folds (a client-folded
   name resolves the same member).

`_signals` is introspection-only — `emitSignal` remains the send path.
`_signatures` (reply out-signatures) is unchanged.

**Property casing:** `Properties.Get`/`Set` accept both the exact QML
property name and its wire-cased form (`Get("iface", "Version")` finds
`property int version`). A `Set` whose value cannot be converted replies
`InvalidArgs` with the property unchanged.

**Property change notifications:** property changes emit the standard
`org.freedesktop.DBus.Properties.PropertiesChanged` signal (with the
advertised name; unmarshalable values report via `invalidated_properties`).
Property notify signals are never broadcast as `fooChanged` bus signals.

**Built-in names:** `service`, `path`, `iface`, and `connection` cannot be
shadowed from QML (load-time error). A QML method folding onto a library
mechanism name (`unregister`, `holdReply`, `emitSignal`) warns at load and
points at `_members`.

**Error replies:** a handler that throws
`DBusQML.DBusUtils.error(name, message)` (or any value carrying the same
`{dbusError, name, message}` shape with a dotted `name`) produces an error
reply with exactly that name; any other thrown value produces
`org.freedesktop.DBus.Error.Failed` with the exception message.

**Name acquisition:** `allowReplacement`, `replaceExisting`, and
`queueOnBusy` map onto the D-Bus `RequestName` flags (all default false).
`nameAcquired`/`nameLost` signals report acquisition (including after
queueing) and loss to another owner.

**C++ typed returns:** a C++ `Q_INVOKABLE` whose return type is any
default-constructible type (`QString`, `int`, `QByteArray`, …) round-trips
into the D-Bus reply with its wire signature. Non-default-constructible
returns cannot be captured — declare a `QVariant` return (the C++ dispatch
path also caps at 5 parameters and warns beyond that).

**Return-value marshaling:** return values are marshaled through
`toDbusVariant`, so `DBusQML.variant(x)` produces a real D-Bus variant
(`v`), and `DBusQML.dict(...)` produces `a{sv}`. A function that returns
nothing sends an empty reply. Nested object literals with gadget values
(`{ ns: { key: new DBusQML.variant(1) } }`) marshal correctly at any
depth.

Example — serving `org.freedesktop.impl.portal.Settings` (the portal
backend pattern):

```qml
DBusAdaptor {
    service: "org.freedesktop.impl.portal.MyShell"
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"

    // D-Bus ReadOne(ss) → v — called by xdg-desktop-portal as "ReadOne"
    function readOne(ns, key) {
        if (ns === "org.freedesktop.appearance" && key === "color-scheme")
            return new DBusQML.variant(1)              // prefer-dark
        if (ns === "org.freedesktop.appearance" && key === "accent-color")
            return new DBusQML.struct_([0.2, 0.5, 0.9]) // (ddd) on the wire
        return new DBusQML.variant("")
    }

    // D-Bus ReadAll(as) → a{sa{sv}}
    function readAll(namespaces) {
        return {
            "org.freedesktop.appearance": {
                "color-scheme": new DBusQML.variant(1)
            }
        }
    }

    function onThemeChanged() {
        // Emits SettingChanged with signature (ssv) — portal-compliant
        emitSignal("SettingChanged",
            ["org.freedesktop.appearance", "color-scheme", new DBusQML.variant(0)])
    }
}
```

**Limitations:**
- QML `signal` declarations are automatically forwarded as D-Bus signals (maximum 5 parameters)
- Static `DBus.emitSignal(service, path, iface, name, args)` always uses the session bus
- Instance `emitSignal(name, args)` intentionally attempts `registerService(service)` so the signal appears to originate from that name (portal-style signals)
- `emitSignal(name, args)` arguments are marshaled through `toDbusVariant` — use `DBusQML.variant(x)` for variant-typed signal args (e.g. the portal `SettingChanged` `(ssv)` signature)
- A struct **inside** a variant (`DBusQML.variant(DBusQML.struct_(...))`) marshals as a real `v` with a struct payload; return the struct directly when you need a bare `(...)` signature.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The D-Bus service name to claim (optional — omit for unique-name-only registration). |
| `path` | `string` | The object path to register at. |
| `iface` | `string` | The interface name to expose. |
| `connection` | `DBusConnection` | The bus to register on (default session bus). |
| `_signatures` | `var` (object) | Explicit reply signatures, keyed by D-Bus member name (see Shape Selection). |
| `_options` | `var` (object) | Per-method served option whitelist `{ Method: { key: sig } }` applied to the method's last `a{sv}` in-arg (P10a: unknown keys dropped, mistyped keys → `InvalidArgs`). |
| `allowedSender` | `string` | Unique bus name allowed to call this adaptor (empty = open); mismatch → `AccessDenied` before any handler runs (P10b). The value is compared against the caller's UNIQUE name (`msg.service()`): a well-known name never matches, so setting one denies everyone — attach-time warning. Introspection (`org.freedesktop.DBus.Introspectable`) is answered by the dispatcher before the adaptor gate runs, so it stays public metadata (xdp posture) regardless of `allowedSender`. |
| `heldReplyTimeout` | `int` | Max held-reply lifetime in ms (`0` = disabled default); expiry settles with `Failed` ("reply timed out"). |

**Private properties:** any adaptor property whose name starts with `_` is
library meta-config (`_signatures`, `_options`, or your own helpers) and is
never served over D-Bus — it is excluded from `generateXml()` and
`Properties.Get/GetAll/Set`.

**Declared reply signatures:** when the served interface is in the bundled or
user type catalog, its declared out-args drive reply marshaling. A
`org.freedesktop.impl.portal.Settings` backend therefore returns `ReadAll` as
`a{sa{sv}}` (the shape xdg-desktop-portal requires) from a plain object
literal, with no override needed.

**Robustness:** a method return value that cannot be marshaled (e.g. a
returned JS function) produces a D-Bus `org.freedesktop.DBus.Error.Failed`
error reply — never a dropped bus connection. An unmarshalable signal arg is
warned about and skipped (signals have no error-reply channel). The same
guard covers **properties**: a property whose value cannot be
marshaled (e.g. a `var` holding a `QObject*`, such as a stashed
`DBusHeldReply`) produces `InvalidArgs` on `Properties.Get` and is skipped
with a warning in `GetAll` — never an aborted process. Gadget-valued properties
(`DBusQML.variant`/`bytes`/`struct_`) marshal like method replies do — served
as a single-wrapped `v` carrying the gadget's payload — and properties with
no possible D-Bus representation (`QObject*`-derived) are never advertised in
introspection.

#### Methods

| Method | Arguments | Description |
| :--- | :--- | :--- |
| `emitSignal(name, args)` | `string name`, `list args` | Emit a D-Bus signal on this adaptor's path/interface. |
| `holdReply()` | — | Defer the current method call; returns a `DBusHeldReply` (see below). |
| `callerService()` | — | Unique bus name of the current caller (dispatch only; empty + warn outside). Held replies capture it at hold time. |
| `unregister()` | — | Retire this adaptor's bus registration immediately (see *Per-call adaptor lifecycle* below). |

#### Deferred replies

Interactive methods (file choosers, dialogs, screenshares) must hold the
D-Bus call open and answer only when the user finishes. Call `holdReply()`
synchronously inside the handler to mark the call as deferred; it returns a
`DBusHeldReply` that settles the call later with `send(value)` or
`sendError(name, message)`:

```qml
import DBus 1.0 as DBusQML   // alias for value types

DBusAdaptor {
    iface: "org.freedesktop.impl.portal.FileChooser"
    function openFile(handle, appId, parentWindow, title, options) {
        const reply = holdReply()
        dialog.accepted.connect(paths => reply.send([0, { uris: new DBusQML.variant(paths, "as") }]))
        dialog.cancelled.connect(() => reply.send([1, {}]))
    }
}
```

`reply.send(value)` runs through the same reply tail as a synchronous
`return`: declared multi-out signatures (FileChooser's `(u, a{sv})`) split
automatically, and a bare `send()` sends an empty (void) reply.
`sendError(name, message)` sends an error reply; a `name` without a `.` is
treated as the message with the generic
`org.freedesktop.DBus.Error.Failed` name.

Caveats:

- `holdReply()` is valid **only synchronously** during the handler — before
  any event-loop spin. Outside dispatch it warns and returns `null`.
- The handler's return value is ignored when `holdReply()` was called (a
  warning is emitted if one is supplied).
- Each `DBusHeldReply` settles **once**; a second `send()`/`sendError()`
  warns and does nothing.
- If the adaptor is destroyed while a reply is still held, the caller gets an
  `org.freedesktop.DBus.Error.Failed` error reply instead of hanging.
  The same tail runs when the adaptor's `QQmlEngine` is destroyed first
  (shell live-reload: engine teardown orphans declarative adaptors
  without running their destructors — the adaptor hooks the engine's
  `destroyed()` signal at attach and errors pending held replies +
  detaches, so a reload with a portal dialog open never hangs the
  caller; pinned by `testEngineTeardownErrorsHeldCaller`).
- There is no server-side timeout **by default** — the caller owns timeouts. (For
  xdg-desktop-portal backends that means honoring the frontend's
  `G_MAXINT`-timeout contract: the backend may hold indefinitely. The
  xdp reality behind the opt-in: their synchronous `Close` blocks the
  calling thread up to 25 s waiting for the response — a consumer that
  cannot afford that block sets a TTL so the held reply errors out
  instead of hanging a thread.)
  Opt in per-adaptor with `heldReplyTimeout` (ms, `0` = disabled): on expiry
  the held reply settles with `Failed` ("reply timed out") + warn; settle
  cancels the timer.
- A thrown `undefined` after `holdReply()` is **undetectable** (sole
  exception — see Known issues): `callWithInstance` reports a thrown
  `undefined` identically to a returned `undefined` (the normal void
  return of a handler that only called `holdReply()`), so it completes
  silently with no reply — the caller times out. Throw a named error or
  a primitive instead; the TTL fence (`heldReplyTimeout`) bounds the
  wait when the shape cannot be controlled.
- A caller on the **same `QDBusConnection`** as the adaptor cannot receive a
  deferred reply — QtDBus dispatches local-loop calls synchronously
  (`sendWithReplyLocal`) and reports `local-loop message cannot have delayed
  replies`. Use a separate connection when a process calls its own adaptor.
- `holdReply()`/`callerService()` are **JS-handler-only**: the C++
  `Q_INVOKABLE` fallback runs after the dispatch scope closes, so a
  C++-subclass handler calling either gets the outside-dispatch warning
  path (null/empty) — document, don't extend (D2). JS handlers are the
  served surface; C++ handlers use synchronous returns.

#### Multiple interfaces on one path

A D-Bus object path can serve several interfaces from separate `DBusAdaptor`
instances that share the same `service` and `path` but differ in `iface` — the
standard multi-interface D-Bus shape every real service has. Co-located
adaptors share the path registration and the service name:

```qml
DBusAdaptor {
    service: "org.freedesktop.impl.portal.MyShell"
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"
    function readOne(ns, key) { /* ... */ }
}
DBusAdaptor {
    service: "org.freedesktop.impl.portal.MyShell"
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.FileChooser"
    function openFile(handle, appId, parentWindow, title, options) { /* ... */ }
}
```

Routing:

- **Interface-scoped calls** route only to the adaptor whose `iface`
  matches the message's interface. A call that carries an interface name
  reaches a matching adaptor and no other; a member-name match on an
  adaptor that declares a *different* interface does not answer. A
  duplicate `iface` on one path warns at load time and the first-attached
  adaptor wins.
- **`Properties.Get/GetAll/Set`** route by the interface *argument*, so
  `GetAll("…Settings")` returns only the Settings adaptor's properties.
- **Empty-interface calls** (which D-Bus allows) route by member name across
  the attached adaptors in attach order. `Properties.GetAll` with an **empty
  interface argument** degenerates the same way: the first attached adaptor
  answers with its own properties.
- **Introspection** of a shared path merges every attached adaptor's interface
  block, so callers see all interfaces at once.

Destroying any co-located adaptor leaves the path and the service name
registered for the survivors; only the last adaptor to be destroyed releases
them.

Threading: attach/detach for a given (connection, service) are
consumer-serialized — in practice both run on the QML/main thread. Driving
attach/detach for the SAME name from two threads concurrently is
unsupported (see `docs/PARITY.md`, threading contract).
Ownership notifications (`nameAcquired`/`nameLost`) are delivered on the
main thread via a process-lifetime relay — never on QtDBus's manager thread
(T1: no adaptor pointer ever crosses a thread). Adaptors on foreign threads
are unsupported (loud warning, main-thread delivery).

#### Per-call adaptor lifecycle

`DBusAdaptor` is safe to create dynamically — one instance per call at a
unique object path, the shape portal backends need (a Request object per
portal call):

```qml
// The Request pattern: create per call, retire per call.
property Component requestComp: Component {
    DBusAdaptor {
        function close() { destroy(); /* caller cancelled */ }
    }
}

function newRequest(handle) {
    return requestComp.createObject(null, {
        service: "org.freedesktop.impl.portal.MyShell",
        path: handle,
        iface: "org.freedesktop.impl.portal.Request"
    })
}
```

Lifecycle semantics:

- **`destroy()` works anywhere** — after a dispatch, and *inside* the
  dispatched handler itself (e.g. the `Close` handler destroying its own
  Request). The dispatch no longer changes the adaptor's ownership at all:
  QML's `destroy()` is deferred by design, so deletion lands after the
  current script block returns — the reply still goes out first, then the
  path is freed.
- **GC behavior.** A dynamically created adaptor with no QML references is
  collected on the next `gc()` and its path freed. A dispatched adaptor is
  never collected *during* its own dispatch (the dispatch holds a GC root);
  under GC pressure inside a handler the adaptor survives. Declaratively
  declared adaptors are unchanged: they are engine-managed, `destroy()` is
  refused on them, and repeated dispatch never alters that.
- **Retirement while a reply is pending is allowed.** Destroying (or GC-
  collecting, or unregistering) an adaptor while a `holdReply()` reply is
  still outstanding works: the destructor errors the pending caller with
  `org.freedesktop.DBus.Error.Failed` ("adaptor destroyed with reply
  pending"). Callers get an error, never a hang.
- **`unregister()` — deterministic retirement.** Frees the object path and
  releases the service reference *immediately*, errors out outstanding held
  replies, and leaves the QObject alive for QML to drop whenever (destroy()
  and GC then work normally). One-way: re-registration after `unregister()`
  is not supported; a second `unregister()` warns and does nothing. Use it
  when the bus path must go away now — e.g. a caller-side `Close` on a portal
  Request — without waiting for GC timing.
- **Deferred-deletion window (defined behavior).** QML `destroy()` is
  deferred by design: between the `destroy()` call and the destructor
  running, the adaptor is still attached and still serves. A call arriving
  inside that window gets a valid reply; the path is freed when the
  destructor runs. This is consistent with the 0.8.0 lifecycle contract
  (deletion lands after the current script block) and is PINNED by
  `testDeferredDeletionWindowServesThenFrees` — not a defect, not a race
  to fix.

---

## Shape Selection

When dbusqml marshals a JS value to the wire, the D-Bus signature comes from
declarations, never from guessing the data's shape. Inference is stable and
boring: `QVariantMap` → `a{sv}`, always. Anything fancier must be declared.

Precedence, on both the **proxy** (call arguments it sends) and the
**adaptor** (replies it returns):

1. **Explicit `_signatures` override** — the author corrects wrong or
   missing introspection.
2. **Declared signature** — the proxy's introspected/cataloged in-args; the
   adaptor's cataloged out-args.
3. **Stable inference** — `QVariantMap` → `a{sv}`, etc.

The `_signatures` property is a JS object mapping D-Bus member names to a
concatenated signature string, split per-argument by the parser:

```qml
// Proxy — declare the in-arg signature for a call:
DBus {
    iface: "com.example.Service"
    _signatures: ({ AddThing: "a{sa{sv}}oo" })   // a{sa{sv}}, o, o
}

// Adaptor — declare the out-arg signature for a reply:
DBusAdaptor {
    iface: "com.example.Service"
    _signatures: ({ ReadAll: "a{sa{sv}}" })
}
```

A declared signature is never silently ignored. Every well-formed element
signature is producible (unregistered container shapes mint assign-once
signature-slot pool registrations — see `dbussignatureslots.h`); only
malformed signatures, invalid values, or pool exhaustion log a warning and
fall back to inference — never a different wire type with no notice.

---

## Data Types

### Property Name Conventions

D-Bus property names use PascalCase (`Percentage`, `IsPresent`). When exposed as QML properties, the first letter is lowercased automatically:

| D-Bus name | QML property |
|:---|:---|
| `Percentage` | `percentage` |
| `IsPresent` | `isPresent` |
| `TimeToEmpty` | `timeToEmpty` |
| `X11Layout` | `x11Layout` |

For consecutive uppercase prefixes (abbreviations), the entire prefix is lowercased:

| D-Bus name | QML property |
|:---|:---|
| `URL` | `url` |
| `XMLConfig` | `xmlConfig` |
| `DBusAddress` | `dbusAddress` |

### `DBusMessage` (Structured Value)

Call options ride on the message: `timeout` (ms, `-1` = Qt default — used
by `asyncCall`), `interactiveAuthorization` and `autoStart` (message
flags, applied on send).

Represents a D-Bus message.

Can be constructed from a property map: `new DBus.dbusMessage({service: "...", path: "...", ...})`.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The sender service. |
| `path` | `string` | The object path. |
| `iface` | `string` | The interface name. |
| `member` | `string` | The method or signal name. |
| `arguments` | `list` | The arguments of the message. |
| `signature` | `string` | The D-Bus signature. |

---

### `DBusPendingReply` (Object)

Represents the result of an asynchronous D-Bus call.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `isFinished` | `bool` | Whether the reply has arrived. |
| `isError` | `bool` | Whether the call resulted in an error. |
| `isValid` | `bool` | Whether the reply is valid. |
| `error` | `dbusError` | The error if `isError` is true. |
| `value` | `variant` | The result value of the call. |
| `values` | `list` | The result values of the call. |

#### Signals

| Signal | Description |
| :--- | :--- |
| `finished` | Emitted when the reply arrives. |

---

### `DBusError` (Structured Value)

Represents a D-Bus error.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `isValid` | `bool` | Whether the error is valid. |
| `name` | `string` | The error name. |
| `message` | `string` | The error message. |

---

### D-Bus Value Types

These types are used for explicit typing of D-Bus data when Qt's auto-conversion doesn't produce the correct D-Bus type signature. Access them through a module alias — the `DBus` element shadows the module namespace, so value types use a separate import:

```qml
import DBus 1.0
import DBus 1.0 as DBusQML    // alias for value types

// Without alias: element is DBus {}, singletons are SessionBus, etc.
// With alias: value types are DBusQML.uint32(), DBusQML.string(), etc.
DBusQML.uint32(42)
DBusQML.int64(21474836470)
DBusQML.string("hello")
DBusQML.boolean(true)
DBusQML.objectPath("/org/freedesktop/UPower")
DBusQML.variant("any value")
DBusQML.variant(["file:///tmp/x"], "as")   // variant whose payload has signature "as"
DBusQML.dict({ key: "value" })
```

Most examples don't need value types — plain JS strings/numbers/booleans work for common cases. Value types are only needed when you must force a specific D-Bus signature (e.g., distinguish `uint32` from `int32`).

| C++ Type | QML Type | Description |
| :--- | :--- | :--- |
| `DBus::Uint32` | `uint32` | Unsigned 32-bit integer. |
| `DBus::Int32` | `int32` | Signed 32-bit integer. |
| `DBus::Uint16` | `uint16` | Unsigned 16-bit integer. |
| `DBus::Int16` | `int16` | Signed 16-bit integer. |
| `DBus::Uint64` | `uint64` | Unsigned 64-bit integer. |
| `DBus::Int64` | `int64` | Signed 64-bit integer. |
| `DBus::Bool` | `boolean` | Boolean. |
| `DBus::Double` | `double` | Double-precision floating point. |
| `DBus::Byte` | `byte` | Unsigned 8-bit integer. |
| `DBus::String` | `string` | String. |
| `DBus::ObjectPath` | `objectPath` | D-Bus object path. |
| `DBus::Signature` | `signature` | D-Bus signature. |
| `DBus::Dict` | `dict` | D-Bus dictionary (map). |
| `DBus::Variant` | `variant` | D-Bus variant. Optional second argument declares the payload's wire signature — `DBusQML.variant(paths, "as")` produces a variant whose payload is an `as` string array (a plain JS array inside a variant infers `av`, which strict receivers such as xdg-desktop-portal reject for `uris`). Empty signature = inference. |

A `variant` nested inside a dict or list value is the value's payload (the
container supplies the `v`); `variant(variant(x))` produces an explicit inner
variant.
| `DBus::Bytes` | `bytes` | Byte array (`ay`). |
| `DBus::Struct` | `struct_` | D-Bus struct — wraps a JS array of members, marshals via `beginStructure`. Use for struct-typed values like `(ddd)` accent-color or `(uu)` StateReason. It works in every position — variant payloads, map/list values, signal args, call args. The outermost struct carries the wire signature; inner structs compose naturally. |

When the method signature is known (from introspection or
catalog), plain JS values are marshaled correctly — no wrapper types needed.
Value types remain as explicit overrides for signature-less calls.

### Byte Arrays (`ay`)

Byte array properties arrive as JS `ArrayBuffer`. Use `DBusUtils` for
text conversion:

```qml
import DBus 1.0

// SSID arrives as ArrayBuffer
var ssidText = DBusUtils.textFromBytes(accessPoint.ssid)

// Marshal a string as ay (UTF-8) — signature-driven, no wrapper needed:
proxy.call("SomeMethod", ["MyWifi"])  // if the arg is ay, string → UTF-8 bytes

// Or explicitly:
var bytes = new DBusQML.bytes("MyWifi")
```

---

## 64-bit integers (`x` / `t`)

QML's JS engine has no BigInt, so a 64-bit value above 2^53 cannot be a JS
number without precision loss. dbusqml delivers losslessly: 64-bit values
that round-trip through a double (|value| ≤ 2^53) arrive as plain numbers;
magnitudes above 2^53 arrive as **full-precision decimal strings**. The
send path accepts decimal strings with a declared `x`/`t` signature (or
through `DBusQML.int64(value)` / `uint64(value)`), making the round-trip
lossless end-to-end.

## Unix file descriptors (`h`)

fds pass in both directions as plain integers with a declared `h`
signature (or an `ah` array). The **receiver** closes the fd; the sender
keeps ownership of its own end. Works over session/system/custom buses on
unix sockets (the ScreenCast/Camera `OpenPipeWireRemote` scenario).

### The fd quartet + `fdUrl` (`DBusUtils`)

QML has no fd I/O — the same gap the `ay` codec hit (0.3.0 shipped
`textFromBytes`/`bytesFromText` *with* `ay` support because QML lacked
text codecs). The fd quartet fills it the same way, so a received fd is
usable from the script layer, and the receiver-closes contract is
dischargeable from QML via `closeFd`:

- `DBusUtils.openFd(path, mode) → int` — fopen-style `mode` (`"r"`,
  `"w"`, `"rw"`); −1 + warning on failure.
- `DBusUtils.writeFd(fd, ArrayBuffer|string) → int` — bytes written;
  −1 + warning on failure.
- `DBusUtils.readFd(fd, maxBytes) → ArrayBuffer` — empty on error/EOF.
- `DBusUtils.closeFd(fd)` — no-op warning on an invalid fd, never a crash.
- `DBusUtils.fdUrl(fd) → string` — `"file:///proc/self/fd/N"`, so
  regular-file fds flow into path-based QML consumers (`Image`,
  Quickshell `FileView`, …) with no extra I/O code.

Caveats:

- **fd taxonomy.** `readFd`/`writeFd` work on any fd — regular files,
  pipes, sockets. `fdUrl` is **regular-file-only**: pipes, sockets, and
  anon inodes (the ScreenShot2 pipe, the `OpenPipeWireRemote` socket)
  have no usable path — use `readFd`/`writeFd` for streams.
- **Access mode travels with the fd.** A transferred fd keeps its access
  mode; a receiver cannot read through a write-only fd — open `rw` when
  the other side must read.
- **fdUrl lifetime.** The URL is valid only while the fd stays open in
  this process — lazy/async loaders must not outlive it. Close with
  `closeFd` when done (that is the receiver-closes contract).

## Known Limitations

Language/type-system constraints only — everything buildable is served
(see the sections above for the full surface: truthful introspection,
`PropertiesChanged`, named errors, name acquisition, call options,
watchers, ObjectManager, fds, lossless 64-bit).

- **Dict keys are strings.** JS object keys are strings by definition, so
  demarshaled dict keys stringify (an `i`-keyed dict arrives with string
  keys). The declared-signature send path coerces keys back correctly.
- **Empty container inference.** `[]` infers `av` and `{}` infers `a{sv}`
  — stable, deterministic inference. Declared signatures are the answer
  when a receiver needs a concrete element type.
- **Connection loss is reported, never reconnected (P5).** A `DBusConnection`
  exposes `connected` + `disconnected()`; proxies flip to `Error` +
  `serviceAvailable=false` and tear down match subscriptions; served claims
  emit `nameLost`. There is no automatic resubscribe/reconnect (session-bus
  death usually ends the session; custom-bus consumers rebuild on their own
  signal). If the bus ITSELF dies and returns, destroy and recreate the
  proxy/connection.
- **Signature recursion depth is capped at 32.** Hostile or pathological
  signatures (remote-influenced) fail loud with a warning instead of
  exhausting the stack.

### Signal handlers on `DBus` elements run in C++-object context

Inline signal handlers declared on a `DBus` element (`onIntrospectionCompleted: ...`, `onSignalReceived: ...`, `onStatusChanged: ...`) evaluate in the C++ object's context, not in the enclosing QML scope. Consequences:

- `id` references and `Q_PROPERTY` values are accessible.
- JavaScript functions defined in the QML scope are **not** callable from these handlers.

Workarounds:

```qml
// Won't work — refreshPlayers() is not visible in the C++ context
DBus {
    id: proxy
    onIntrospectionCompleted: refreshPlayers()
}

// Works — Connections re-binds the target in the QML scope
DBus { id: proxy }
Connections {
    target: proxy
    function onIntrospectionCompleted() { refreshPlayers() }
}
```

This is inherent to Qt 6; there is no library-side fix.

### Runtime-configured proxies: dynamic methods are async

For a `DBus { }` element declared with all of `service`, `path`, and `iface` set inline in QML, introspection completes before user interaction is possible, and `proxy.methodName()` works immediately.

For a proxy whose properties are assigned at runtime (typically via `onActivePlayerChanged`, a selector, etc.), introspection is asynchronous — dynamic method callbacks are **not** yet installed in the same event-loop tick as the assignment.

Wait for `introspectionCompleted` (or check `status === DBus.Ready`) before calling dynamic methods on a runtime-configured proxy:

```qml
Connections {
    target: playerProxy
    function onIntrospectionCompleted() {
        playerProxy.playPause()   // safe here
    }
}
```
