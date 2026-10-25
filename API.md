# DBus QML API Documentation

This document describes the public API of the `DBus` QML module.

For complete, task-oriented QML files, start with the
[guides](docs/guides/README.md).
QML blocks below are complete files. JavaScript excerpts run inside a QML
handler with the named objects and module imports supplied by your application.

## Module Import

To use the module in QML, import it as follows:

```qml
import QtQuick
import DBus 1.0

Item { property var bus: SessionBus }
```

## Core Components

### `busType` (Enum)

```js
DBusQML.busType.Session // The session bus (personal applications)
DBusQML.busType.System  // The system bus (system services)
```

Use `import DBus 1.0 as DBusQML` for this qualified enum access.

### Bus Connections

The module provides singleton access to the standard D-Bus connections.

#### `SessionBus`
A singleton representing the Session Bus.

#### `SystemBus`
A singleton representing the System Bus.

Example usage:
```qml
import QtQuick
import DBus 1.0

Item {
    property var sessionBus: SessionBus
    property var systemBus: SystemBus
}
```

---

### `DBus` (Proxy Element)

Guides: [calling methods](docs/guides/calling-methods.md) and
[properties and signals](docs/guides/properties-and-signals.md).

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
import QtQuick
import DBus 1.0

Item {
    id: root
    property var endpoint: mpris
    DBus {
        id: mpris
        service: "org.mpris.MediaPlayer2.myplayer"
        path: "/org/mpris/MediaPlayer2"
        iface: "org.mpris.MediaPlayer2.Player"
    }
    function togglePlayback() {
        if (mpris.status === DBus.Ready)
            root.endpoint.playPause() // wire method PlayPause
    }
    function seekForward() {
        if (mpris.status === DBus.Ready)
            root.endpoint.seek(50000000) // wire method Seek
    }
}
```

These calls require completed introspection. MPRIS volume is a writable
property, not a `SetVolume` method: use `mpris.setProperty("Volume", 0.8)`.

**Dynamic properties** — D-Bus properties are exposed under QML camelCase names:
```qml
import QtQuick
import DBus 1.0

Item {
    readonly property var percentage: battery["percentage"]
    readonly property var isPresent: battery["isPresent"]
    readonly property var timeToEmpty: battery["timeToEmpty"]
    DBus {
        id: battery
        connection: SystemBus
        service: "org.freedesktop.UPower"
        path: "/org/freedesktop/UPower/devices/DisplayDevice"
        iface: "org.freedesktop.UPower.Device"
    }
}
```

The initial `Properties.GetAll` populates these values; reading a binding
does not issue a `Get` each time. Known properties start as `null` until
their values arrive. Properties auto-update via `PropertiesChanged`
signals. See Property Name Conventions below.

Writes through the proxy are **optimistic**: the value is visible immediately
and `Properties.Set` goes out on the wire. A **failed** write re-fetches the
property from the service (`Properties.Get`, once the write chain settles) and
the server's value lands in the map — the proxy never invents a value (there
is no local rollback). `propertyWriteFailed(name, errorName, message)` still
fires, so consumers can apply gesture policy. If the re-fetch itself fails
(service gone), the current value stays with a loud warning; connection-loss
handling owns the rest.

#### Configuration Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The D-Bus service name. |
| `path` | `string` | The D-Bus object path. |
| `iface` | `string` | The D-Bus interface name. |
| `connection` | `DBusConnection` | The connection associated with this proxy (default session bus). Its bus connection is recorded by value when configured; all traffic, including dynamically exposed methods, keeps using it after the QML wrapper is destroyed. |
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
| `propertyWriteFailed(name, errorName, message)` | `string`, `string`, `string` | Signal: a `Set` the proxy issued was rejected (the property is re-fetched from the service; the proxy never invents a value). |
| `emitSignal(name, args)` | `string name`, `list args` | — | Emit a D-Bus signal from this proxy's path/interface. |
| `connectToBus(address)` | `string address` | `DBusConnection` | C++ static factory, callable on a QML proxy instance; prefer `SessionBus.connectToBus(address)` in QML. Returns null on failure. |
| `reloadTypes()` | — | — | C++ static catalog operation, callable on a QML proxy instance. Re-scan after drop-in changes; see `docs/TYPES.md`. |

**Boundary errors:** arguments that cannot be marshaled (a
`QObject*` in a map, a JS function, an unregistered type) fail the call
**locally** — the returned `DBusPendingReply` completes with
`org.freedesktop.DBus.Error.Failed` and "argument N is not marshalable"
instead of the process aborting inside QtDBus. Property writes with
unmarshalable values are dropped with a warning; `emitSignal` skips the send.
Override-string parsing is separate from type validation. A nonempty proxy
`_signatures` entry that yields no complete types warns ("unparseable
declared signature") and uses inference; a split type that fails validation
instead fails the call locally. Partial parses and adaptor overrides have
different behavior — see [Shape Selection](#shape-selection).

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
| `propertyWriteFailed(name, errorName, message)` | `string`, `string`, `string` | Emitted when a property `Set` the proxy issued is rejected (the property is re-fetched from the service; see the optimistic-writes note above). |

#### Data Signals

**Decode failures:** incoming messages are decoded atomically: no partial
argument list or property update is delivered. A method reply that cannot
be decoded completes its `DBusPendingReply` with the local error
`org.freedesktop.DBus.Error.Failed`, message `"reply could not be decoded"`
(empty `value`/`values`); promise-style calls invoke `reject`. This includes
coalesced `getProperty` waiters. Undecodable signals (including
`PropertiesChanged`, ObjectManager notifications and `DBusSignalWatcher`)
are dropped with one warning per delivery. Internal property/inventory
fetches keep their existing data on decode failure and report the failure;
an initial proxy fetch enters `Error`, and an initial ObjectManager fetch
does not set `ready`.

**Sender binding:** the proxy subscribes to every discovered signal (and to
`PropertiesChanged`) with the configured `service` as the sender and accepts
a signal only from the service's **current owner**. The bus match rule names
the service, so the daemon itself resolves the well-known name to its owner
on every delivery; the proxy additionally tracks the owner (an initial
`GetNameOwner` plus an owner-change watch) and drops a signal whose sender
differs — e.g. one from a stale previous owner still queued across a
restart. A unique (`:1.…`) `service` compares directly. While the owner is
not yet known (right after subscribing) the daemon's match rule is the
filter. A peer that emits the same path/interface/member without owning the
service is never delivered. To listen to signals from **any** sender
(dbus-monitor shape, wildcard `service`/`member`), use
[`DBusSignalWatcher`](#dbussignalwatcher) — wildcard listening stays its
purpose.

| Signal | Arguments | Description |
| :--- | :--- | :--- |
| `signalReceived(name, args)` | `string name`, `list args` | Emitted when a D-Bus signal from the configured service's current owner is received. |
| `valueChanged(key, value)` | `string key`, `variant value` | Emitted when a dynamic property changes (from QQmlPropertyMap). |

#### Toggle Signals

| Signal | Description |
| :--- | :--- |
| `signalsEnabledChanged` | Emitted when `signalsEnabled` is toggled. |
| `propertiesEnabledChanged` | Emitted when `propertiesEnabled` is toggled. |

Example usage:

```qml
import QtQuick
import DBus 1.0

Item {
    id: root
    // A var reference lets static tooling handle the proxy's dynamic methods.
    property var endpoint: notifications

    DBus {
        id: notifications
        service: "org.freedesktop.Notifications"
        path: "/org/freedesktop/Notifications"
        iface: "org.freedesktop.Notifications"
        connection: SessionBus
        propertiesEnabled: false
        callTimeout: 5000
    }

    Connections {
        target: notifications
        function onIntrospectionCompleted() {
            // QML notify(...) calls the wire method Notify(...).
            const reply = root.endpoint.notify("app", 0, "", "Hello", "World", [], {}, 5000)
            reply.finished.connect(function() {
                if (reply.isError)
                    console.error(reply.error.name, reply.error.message)
                else
                    console.log("Notification ID:", reply.value)
            })
        }
        function onSignalReceived(name, args) {
            console.log("Signal received:", name, "with args:", args)
        }
        function onStatusChanged() {
            if (notifications.status === DBus.Error)
                console.error("The notifications interface could not be loaded")
        }
    }
}
```

This is a complete file; it needs a notification service on the session
bus. The callback is connected before the asynchronous reply completes.
Use `call("Notify", args)` with the wire name when you do not want to use
the dynamic method; signature discovery still requires introspection or
catalog data unless you supply an override.

---

### `DBusConnection`

Guides: [getting started](docs/guides/getting-started.md) and
[explicit messages and callbacks](docs/guides/calling-methods.md).

`DBusConnection` provides methods for making asynchronous calls to the bus.
Reuse `SessionBus` / `SystemBus`, or call `SessionBus.connectToBus(address)`
for a custom connection. The factory does not use the singleton's bus;
it opens the supplied address. A C++ static invokable is exposed through
QML **object instances**, not as `DBus.connectToBus` on the type name:

```js
// Use the built-in singletons
const message = {
    service: "org.freedesktop.DBus",
    path: "/org/freedesktop/DBus",
    iface: "org.freedesktop.DBus",
    member: "ListNames",
    arguments: []
}
SessionBus.asyncCall(message)
SystemBus.asyncCall(message)

// Connect to a custom bus address
const custom = SessionBus.connectToBus("unix:path=/run/user/1000/example-bus")
if (custom) custom.asyncCall(message)
```

#### Methods

| Method | Arguments | Description |
| :--- | :--- | :--- |
| `asyncCall(message)` | `dbusMessage message` | Returns a `DBusPendingReply`. |
| `asyncCall(message, resolve, reject)` | `dbusMessage message`, `function resolve`, `function reject` | Promise-style asynchronous call. |

The callback overload calls `resolve` with the **first output argument** or
`reject` with the error, and manages its reply internally. It does not
return a JavaScript `Promise`. Use the pending-reply overload and `values`
when you need all output arguments.

#### Connection properties and signals (P5)

| Property / Signal | Type | Description |
| :--- | :--- | :--- |
| `connected` | `bool` | Liveness of the bus connection (false after the loss is observed). Never reconnects (documented). |
| `disconnected()` | signal | Emitted once when the bus connection drops. Proxies flip to `Error` + `serviceAvailable=false`; served claims emit `nameLost`. |

Example usage — pending reply style:
```js
var reply = SessionBus.asyncCall({
    service: "org.freedesktop.DBus",
    path: "/org/freedesktop/DBus",
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
```js
SessionBus.asyncCall({
    service: "org.freedesktop.DBus",
    path: "/org/freedesktop/DBus",
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

Guide: [watch without introspection](docs/guides/properties-and-signals.md#watch-without-introspection).

Subscribes to signals on any bus with no proxy and no introspection
requirement. Empty `service`/`member` fields are wildcards (the
dbus-monitor shape, including `NameOwnerChanged` on the daemon and
system-bus signals).

```qml
import DBus 1.0

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

All properties are runtime-writable: changing `connection`, `service`,
`path`, `iface`, `member` or `enabled` first drops the live subscription
with the exact connection and filter it was made with, then resubscribes —
signals matching only the old filter or arriving on the old connection are
never delivered after the change, and the new subscription delivers each
signal exactly once.

The bus connection (default session bus) is recorded by value when configured.
Destroying its `DBusConnection` wrapper does not move later subscriptions to
the session bus when a filter changes or the watcher is re-enabled.

### `DBusServiceWatcher`

Watches one well-known name for appear/disappear/owner change on any bus —
the standalone complement to the proxy's `watchServiceStatus`.

```qml
import DBus 1.0

DBusServiceWatcher {
    connection: SystemBus
    service: "org.bluez"
    onOwnerChanged: (oldOwner, newOwner) => console.log("bluez:", oldOwner, "->", newOwner)
    onRegisteredChanged: console.log("registered:", registered)
}
```

`registered` is read-only and tracks the name's presence.

The bus connection (default session bus) is recorded by value when configured.
After its `DBusConnection` wrapper is destroyed, changing `service` keeps both
the initial presence lookup and subsequent owner-change watch on that bus.

### `DBusObjectManager`

Guide: [discover objects](docs/guides/properties-and-signals.md#discover-objects-with-objectmanager).

Client for `org.freedesktop.DBus.ObjectManager`: fetches `GetManagedObjects`
and follows `InterfacesAdded`/`InterfacesRemoved`.

```qml
import DBus 1.0

DBusObjectManager {
    connection: SystemBus
    service: "org.bluez"
    path: "/"
    onManagedObjectsChanged: console.log(Object.keys(managedObjects))
    onInterfacesAdded: (objectPath, ifaces) => console.log("+", objectPath, Object.keys(ifaces))
    onInterfacesRemoved: (objectPath, ifaces) => console.log("-", objectPath, ifaces)
}
```

`ready` turns true after the initial `GetManagedObjects` completes;
`managedObjects` maps object path → { interface → { property → value }}.
These are wire property names, not proxy camelCase names. The inventory
follows interface additions/removals, not ongoing `PropertiesChanged`
updates; use per-object proxies for live property bindings.

Changing `service`, `path` or `connection` at runtime resubscribes on the
new target (the old subscription is dropped with the exact connection and
service/path it was made with) and refetches the inventory; live
`InterfacesAdded`/`InterfacesRemoved` then arrive from the new target only.

The bus connection (default session bus) is recorded by value when configured,
independently of signal-subscription success. Destroying the `DBusConnection`
wrapper does not reroute inventory fetches or later service/path refetches to
the session bus.

### `DBusAdaptor` (Server Element)

Guide: [serving interfaces](docs/guides/serving-interfaces.md).

The `DBusAdaptor` element registers a D-Bus object on the bus and responds to incoming method calls. It is the server-side counterpart of the `DBus` proxy element.

```qml
import DBus 1.0
import DBus 1.0 as DBusQML

DBusAdaptor {
    id: adaptor
    service: "org.freedesktop.impl.portal.atmosphera"
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"
    captureSubtree: true

    // QML properties become D-Bus properties (readable via Properties.Get/GetAll)
    property int colorScheme: 0

    // QML functions become D-Bus methods (callable from other processes)
    function readOne(ns, key) {
        if (ns === "org.freedesktop.appearance" && key === "color-scheme")
            return new DBusQML.variant(adaptor.colorScheme)
        return new DBusQML.variant("")
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
   concatenated arg signature) declares served signal names and types,
   including names not expressible as QML `signal` declarations.
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

`_signals` supplies the served introspection declarations **and the types
used for emission marshaling**, ahead of catalog signal declarations.
Setting the map does not emit anything: call `emitSignal(name, args)` to
send a declared wire signal. Ordinary QML signals are relayed automatically;
their emission also uses the resolved declarations. `_signatures` instead
declares method reply out-signatures.

**Property casing:** `Properties.Get`/`Set` accept both the exact QML
property name and its wire-cased form (`Get("iface", "Version")` finds
`property int version`). A `Set` whose value cannot be converted replies
`InvalidArgs` with the property unchanged.

**Property access enforcement:** the access the introspection XML advertises
is the access the bus enforces — one resolution drives both. A `Set` on a
property whose effective access is `read` (declared by the interface's
catalog XML, or a read-only `CONSTANT` QML/C++ property) is refused with
`org.freedesktop.DBus.Error.PropertyReadOnly` and the value is left
unchanged.

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

An incoming method call whose arguments cannot be decoded is answered with
`org.freedesktop.DBus.Error.InvalidArgs` (`"arguments could not be decoded"`)
before the handler runs. This also covers `Properties.Set`: its property is
left unchanged. As with other adaptor errors, `NO_REPLY_EXPECTED` suppresses
the reply.

**Name acquisition:** `allowReplacement`, `replaceExisting`, and
`queueOnBusy` map onto the D-Bus `RequestName` flags (all default false).
`nameAcquired`/`nameLost` signals report acquisition (including after
queueing) and loss to another owner.

**`captureSubtree`:** attach-time (later writes warn and are ignored). Registers the path with QtDBus as `SubPath` so every
message under the prefix is delivered to dbusqml in bus arrival order.
Child adaptors are written as today (`DBusAdaptor { path: handle }`) and
are auto-routed when their path lies under a capturing prefix on the same
connection. Use it whenever handlers create child adaptors that callers
may address immediately — portal `Request` objects are the canonical case.
A captured child answers every request exactly as an uncaptured adaptor
would (same reply, same error name and text); the only difference capture
makes is *when* — in arrival order. Sole exception: dbusqml never replies
to NO_REPLY_EXPECTED calls, where stock Qt's fallback does (PARITY §3).

Rules:

- All co-located adaptors at the path must agree. A mismatching latecomer
  is refused and not registered (no `nameAcquired`; its iface is absent
  from Introspect).
- Refused if paths are already registered beneath the prefix — create the
  capturing adaptor before its children.
- Nested capture (`captureSubtree` on a path already under a capturing
  ancestor) is ignored with a warning; routing stays with the ancestor.
- Absent paths under a captured prefix answer
  `org.freedesktop.DBus.Error.UnknownObject` with Qt's exact text
  (`No such object path '…'`), in arrival order. Debug logging:
  `QT_LOGGING_RULES="dbusqml.dispatch.debug=true"`.

**Peer:** `org.freedesktop.DBus.Peer` (`Ping`, `GetMachineId`) is answered
by the D-Bus transport library (libdbus) for every path on every
connection, before Qt dispatch — as GDBus and sd-bus do in their
libraries. dbusqml neither serves nor shadows it. Callers on the same
connection as the adaptor (in-process loopback) bypass the transport and
receive Qt's `UnknownInterface`; see KNOWN_ISSUES.

**C++ typed returns:** a C++ `Q_INVOKABLE` whose return type is any
default-constructible type (`QString`, `int`, `QByteArray`, …) round-trips
into the D-Bus reply with its wire signature. Non-default-constructible
returns cannot be captured — declare a `QVariant` return (the C++ dispatch
path also caps at 5 parameters and warns beyond that).

**Return-value marshaling:** return values are marshaled through
`toDbusVariant`, so `DBusQML.variant(x)` produces a real D-Bus variant
(`v`), and `DBusQML.dict(...)` produces `a{sv}`. A function that returns
nothing sends an empty reply. Nested object literals with gadget values
(`{ ns: { key: new DBusQML.variant(1) } }`) marshal correctly within the
documented nesting limits.

Example — serving `org.freedesktop.impl.portal.Settings` (the portal
backend pattern):

```qml
import DBus 1.0
import DBus 1.0 as DBusQML

DBusAdaptor {
    service: "org.freedesktop.impl.portal.MyShell"
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"
    captureSubtree: true

    // D-Bus ReadOne(ss) → v — called by xdg-desktop-portal as "ReadOne"
    function readOne(ns, key) {
        if (ns === "org.freedesktop.appearance" && key === "color-scheme")
            return new DBusQML.variant(1)              // prefer-dark
        if (ns === "org.freedesktop.appearance" && key === "accent-color")
            return new DBusQML.struct_([0.2, 0.5, 0.9]) // v holding (ddd)
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
- Auto-forwarded QML signals use `var` parameters (maximum 5); `_signals` or catalog declarations supply their wire types. The current relay's Qt slots take `QVariant`: native typed parameters such as `string` do not connect to those slots and produce an incompatible sender/receiver warning. Use `var` or explicit `emitSignal`.
- The C++ static five-argument `DBusProxy::emitSignal(service, path, iface, name, args)` overload always uses the session bus. From QML it is callable through a proxy object instance, not the `DBus` type name.
- `DBusAdaptor.emitSignal(name, args)` sends on its recorded attach connection, using the adaptor's existing registration. The **proxy's** instance `emitSignal` is the API that attempts `registerService(service)`, once per service value.
- `emitSignal(name, args)` arguments are marshaled through `toDbusVariant` — use `DBusQML.variant(x)` for variant-typed signal args (e.g. the portal `SettingChanged` `(ssv)` signature)
- A struct **inside** a variant (`DBusQML.variant(DBusQML.struct_(...))`) marshals as a real `v` with a struct payload; return the struct directly when you need a bare `(...)` signature.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The D-Bus service name to claim (optional — omit for unique-name-only registration). |
| `path` | `string` | The object path to register at. |
| `iface` | `string` | The interface name to expose. |
| `connection` | `DBusConnection` | The bus to register on (default session bus). Attach-time only (post-attach changes warn and are ignored). Every send is bound to the recorded attach connection: broadcasts (auto-forwarded QML signals, `emitSignal`, `PropertiesChanged`) go out on it, and each method reply — immediate or held — travels on the connection the call arrived on. Destroying this object after attach does not reroute any of it to the session bus. |
| `_signatures` | `var` (object) | Explicit reply signatures, keyed by D-Bus member name (see Shape Selection). |
| `_options` | `var` (object) | Per-method served option whitelist `{ Method: { key: sig } }` applied to the method's last `a{sv}` in-arg (xdp `xdp_filter_options` semantics): the filter iterates the **declared** keys — unknown caller keys are silently dropped (forward-compat); a declared key present with a matching/coercible-same-kind value delivers the **typed** (coerced) value; a declared key with a wrong-kind (scalar-for-list, list-for-map, container-for-scalar) or non-convertible value gets `org.freedesktop.DBus.Error.InvalidArgs` and the handler never runs. An **empty entry** `{ Method: {} }` is a real rule: filter ON with zero declared keys — every caller key is dropped and the handler receives `{}` (deny-all-keys; no runtime notice). The key may be the QML method name or its advertised wire name (the name introspection shows); the policy then applies to **every accepted spelling** of that method — exact, folded, or client-folded — because dispatch resolves the handler first and looks the whitelist up by the resolved identity. |
| `allowedSender` | `string` | Unique bus name allowed to call this adaptor (empty = open); mismatch → `AccessDenied` before any handler runs (P10b). The value is compared against the caller's UNIQUE name (`msg.service()`): a well-known name never matches, so setting one denies everyone — attach-time warning. Introspection (`org.freedesktop.DBus.Introspectable`) is answered by the dispatcher before the adaptor gate runs, so it stays public metadata (xdp posture) regardless of `allowedSender`. |
| `heldReplyTimeout` | `int` | Max held-reply lifetime in ms (`0` = disabled default); expiry settles with `Failed` ("reply timed out"). |

**Private properties:** any adaptor property whose name starts with `_` is
library meta-config (`_signatures`, `_options`, or your own helpers) and is
never served over D-Bus — it is excluded from `generateXml()` and
`Properties.Get/GetAll/Set`. Privacy is checked on the resolved property,
and a `_members` alias targeting a private property is refused with a
warning whenever the map is set — attach-time or runtime — so an alias can
never launder a private property into a public wire name.

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

This complete example answers on a timer. A real dialog uses the same
handle, with one settlement owner and dialog callbacks disconnected when
the interaction finishes.

```qml
import QtQuick
import DBus 1.0

Item {
    id: root
    property var pending: []
    DBusAdaptor {
        id: delayed
        service: "com.example.Delayed"
        path: "/com/example/Delayed"
        iface: "com.example.Delayed"
        _signatures: ({ ReadLater: "s" })
        function readLater() {
            root.pending.push(delayed.holdReply())
            answerTimer.start()
        }
    }
    Timer {
        id: answerTimer
        interval: 250
        onTriggered: {
            const batch = root.pending
            root.pending = []
            for (const reply of batch)
                reply.send("Ready")
        }
    }
}
```

`reply.send(value)` runs through the same reply tail as a synchronous
`return`: declared multi-out signatures (FileChooser's `(u, a{sv})`) split
automatically, and a bare `send()` sends an empty (void) reply.
`sendError(name, message)` sends an error reply; a `name` without a `.` is
treated as the message with the generic
`org.freedesktop.DBus.Error.Failed` name.

While pending, the adaptor owns the held reply, so dropping all JavaScript
references cannot collect it before it answers. Once settled, with its QML
engine still alive, it is unparented and handed to the JavaScript garbage
collector: keeping a reference keeps the handle; dropping every reference
allows it to be collected without waiting for the adaptor to die. If the
engine has already died (or there is no engine), it retains adaptor ownership
and is destroyed with that adaptor. The held message releases its incoming
arguments when the dispatch that called `holdReply()` ends; reply correlation
headers and the captured `callerService()` remain available.

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
- `holdReply()`/`callerService()` are **QML-handler facilities**. C++
  services should register with `QDBusConnection::registerObject` and
  inherit `QDBusContext` (`setDelayedReply`) — the native mechanism for
  Qt-dispatched objects (`QDBusContext` only populates under Qt's own
  dispatcher). Subclassing this adaptor puts handlers under the QML
  dispatch model — a C++-subclass handler calling either gets the
  outside-dispatch warning path (null/empty).

#### Multiple interfaces on one path

A D-Bus object path can serve several interfaces from separate `DBusAdaptor`
instances that share the same `service` and `path` but differ in `iface` — the
standard multi-interface D-Bus shape every real service has. Co-located
adaptors share the path registration and the service name:

```qml
import QtQuick
import DBus 1.0

Item {
    DBusAdaptor {
        service: "com.example.Shared"
        path: "/com/example/Shared"
        iface: "com.example.Greeter"
        _signatures: ({ Greet: "s" })
        function greet(name) { return "Hello, " + name }
    }
    DBusAdaptor {
        id: counter
        service: "com.example.Shared"
        path: "/com/example/Shared"
        iface: "com.example.Counter"
        property int count: 0
        _signatures: ({ ReadCount: "i" })
        function readCount() { return counter.count }
    }
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
import QtQuick
import DBus 1.0
import DBus 1.0 as DBusQML

Item {
    id: root
    property var activeRequest: null
    property int requestId: 0
    DBusAdaptor {
        service: "com.example.Requests"
        path: "/com/example/Requests"
        iface: "com.example.RequestFactory"
        captureSubtree: true // on every co-located prefix adaptor
        _signatures: ({ CreateRequest: "o" })
        function createRequest() {
            if (root.activeRequest)
                throw DBusUtils.error("com.example.Requests.Busy", "Close the current request first")
            const handle = "/com/example/Requests/r" + (++root.requestId)
            root.activeRequest = requestComp.createObject(root, {
                path: handle,
                _onRetire: function() { root.activeRequest = null }
            })
            return new DBusQML.objectPath(handle)
        }
    }
    Component {
        id: requestComp
        DBusAdaptor {
            id: request
            iface: "org.freedesktop.impl.portal.Request"
            property var _onRetire
            function close() {
                request.unregister()
                request._onRetire()
                request.destroy()
            }
        }
    }
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

Guides: [typed calls](docs/guides/calling-methods.md) and
[troubleshooting](docs/guides/troubleshooting.md).

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
import DBus 1.0

// Proxy — declare the in-arg signature for a call:
DBus {
    service: "com.example.Service"
    path: "/com/example/Service"
    iface: "com.example.Service"
    _signatures: ({ AddThing: "a{sa{sv}}oo" })   // a{sa{sv}}, o, o
}
```

```qml
import DBus 1.0

// Adaptor — declare the out-arg signature for a reply:
DBusAdaptor {
    service: "com.example.Service"
    path: "/com/example/Service"
    iface: "com.example.Service"
    _signatures: ({ ReadAll: "a{sa{sv}}" })
}
```

**Override parsing is not type validation.** The QML property is
`_signatures` on both elements (`signatures()` / `setSignatures()` are its
C++ accessors). The override splitter extracts a prefix of complete types;
it does not validate the entire string against the D-Bus rules below.
Behavior at this step is:

| Override | Proxy call arguments | Adaptor reply arguments |
| :--- | :--- | :--- |
| Empty string | Uses inference, without an unparseable-signature warning | Declares a void reply; a marshalable returned value is dropped with a warning |
| Nonempty but no type can be split, e.g. `"("` | Warns `unparseable declared signature`, then uses inference for the arguments | Warns `breaks mid-parse — partial out-args used`; the empty prefix is treated as a declared void reply |
| A parsed prefix followed by an unparseable suffix, e.g. `"s("` | Keeps the prefix (`s` here), infers remaining arguments, and does not emit the `unparseable declared signature` warning for this case | Warns `breaks mid-parse — partial out-args used` and uses only the parsed prefix as its out-argument declaration |

An override entry wins even in these cases: parsing failure does **not**
restore the catalog/introspection declaration for that method.

**Proxy type validation:** each nonempty input type obtained from the
override, introspection or catalog is then validated. A type the splitter
can extract but the validator rejects, such as `"()"` or `"a{vs}"`, fails
the value, warns, and completes the call locally with
`org.freedesktop.DBus.Error.Failed`; it is not replaced by an inferred type.
`send()` drops the invalid call with a warning instead of returning a reply.

**Other marshaling paths:** the signature-driven container writer also
rejects invalid types before constructing or registering them. Its caller
can handle that refusal differently: `marshalBySignature` (used for raw
message arguments, adaptor replies and declared signal arguments) warns
`cannot produce declared signature` and falls back to inference when its
generic container writer fails, including for an invalid container type.
An unrecognized type code reaching that marshaller also warns and infers.
A bad `h` value is an exception: it fails rather than inferring, on every
path. The send boundary then checks the actual marshaled values and their
aggregate signature; calls/replies fail or one-way messages are dropped
when that check fails.

A valid type that the generic container writer cannot produce from the
value (including depth-limit or signature-slot exhaustion) likewise warns
and falls back to inference, except for fd failures. Correct declarations
and warnings rather than relying on any fallback. Unregistered valid
container shapes use assign-once signature-slot pool registrations (see
`dbussignatureslots.h`).

**Signature limits** (D-Bus "Valid Signatures", enforced before any
signature is registered or used): a signature is a list of complete types
built only from the type codes, parentheses, and curly braces; structs
are balanced and non-empty; a dict entry appears only as an array's
element type, has exactly two members, and its key is a basic type;
array nesting and struct nesting are each capped at 32 levels; and the
encoded signature is at most 255 bytes. The aggregate message signature —
the concatenation of a message's argument signatures — is likewise capped
at 255 bytes, checked with the per-argument marshalability guard at one
send boundary shared by every send path (calls, fire-and-forget sends,
proxy and adaptor signals, adaptor replies, held replies): a message that
would exceed the cap fails locally (`Failed` error reply / `InvalidArgs`)
or is dropped with a warning on paths without a reply channel, instead of
aborting inside libdbus.

**Introspection and catalog XML limits** (one policy for both parsers):
an XML document larger than 4 MiB (encoded size) is refused before
parsing, and a document whose DTD declares any entity (`<!ENTITY …>`) is
refused at the DTD token — before any content is scanned, so no entity
expansion can ever run. A plain DOCTYPE without entity declarations
(the kind every real libdbus `Introspect()` reply starts with) parses
normally. A refused introspection reply is treated as if the interface
were absent from the XML (the catalog/inference tiers take over), and a
refused catalog file is discarded whole — each with exactly one warning.

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

Can be constructed from a property map:
`new DBusQML.dbusMessage({service: "...", path: "...", ...})`, using
`import DBus 1.0 as DBusQML` for the value type. `asyncCall` also accepts
the property map directly.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The destination's well-known or unique bus name. |
| `path` | `string` | The object path. |
| `iface` | `string` | The interface name. |
| `member` | `string` | The method name, in wire case. |
| `arguments` | `list` | The arguments of the message. |
| `signature` | `string` | Optional concatenated input-argument signature, not the reply signature. |

---

### `DBusPendingReply` (Object)

Guide: [reply lifetime and memory](docs/guides/lifetime-and-memory.md).

Represents the result of an asynchronous D-Bus call.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `isFinished` | `bool` | Whether the reply has arrived. |
| `isError` | `bool` | Whether the call resulted in an error. |
| `isValid` | `bool` | Whether the reply is valid. |
| `error` | `dbusError` | The error if `isError` is true. |
| `value` | `variant` | The first output argument of the call. |
| `values` | `list` | All output arguments, in order. |

#### Signals

| Signal | Description |
| :--- | :--- |
| `finished` | Emitted exactly once, when the reply arrives (or the call fails locally). |

`finished` is one-shot: right after it is delivered, every receiver is
disconnected. Connect your handler right after the call — it is delivered on
a later event-loop turn, so a synchronous `finished.connect(...)` always lands
first, even for a call that fails locally (on any connection, including one
from `connectToBus`). A `finished.connect(...)` made
after delivery never fires; check `isFinished` instead. `finished` is also the
change signal of every reply property, so bindings on `reply.value` etc.
update once on completion — a settled reply never changes again.

#### Lifetime

A reply's data is cached at completion, so `value`/`values`/`error` stay
readable after `finished` — the reply is self-contained. Ownership: while the
call is in flight the reply is parented to its creator; **at completion it is
handed to the JavaScript garbage collector** — on every completion path
(`call`, `asyncCall`, `getProperty` including coalesced duplicates, and local
failures). Keep a reference and the reply stays alive as long as you hold it;
drop every reference and the next `gc()` collects it — fire-and-forget calls
do not accumulate on the connection. A handler that captures its reply
(`var r = ...; r.finished.connect(() => use(r.value))`, the example above) is
safe: the one-shot disconnect releases the handler, so it does not keep the
reply alive. A pure C++ consumer (no QML engine) keeps the reply parented to
its creator. The promise-style `asyncCall(message, resolve, reject)` manages
its own reply and deletes it once it settles, with or without callbacks.

A reply owns its fds: a received `h` in its values is a
[`DBusQML.unixFd`](#unix-file-descriptors-h) that closes when the reply and
every copy read from it are gone (or at once, on `close()`).

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
import QtQuick
import DBus 1.0
import DBus 1.0 as DBusQML    // alias for value types

Item {
    property var bus: SessionBus
    property var examples: [
        new DBusQML.uint32(42),
        new DBusQML.int64(21474836470),
        new DBusQML.string("hello"),
        new DBusQML.boolean(true),
        new DBusQML.objectPath("/org/freedesktop/UPower"),
        new DBusQML.variant("any value"),
        new DBusQML.variant(["file:///home/user/example.txt"], "as"),
        new DBusQML.dict({ key: "value" })
    ]
}
```

Ordinary data usually needs no wrapper: plain JS strings, numbers and
booleans cover common cases. Wrappers select explicit types when needed
(for example, `uint32` rather than `int32`). File descriptors are different:
an owning `unixFd` is required, even when the `h` signature is known.

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

| C++ Type | QML Type | Description |
| :--- | :--- | :--- |
| `DBus::Bytes` | `bytes` | Byte array (`ay`). |
| `DBus::Struct` | `struct_` | D-Bus struct — wraps a JS array of members, marshals via `beginStructure`. Use for struct-typed values like `(ddd)` accent-color or `(uu)` StateReason. It works in every position — variant payloads, map/list values, signal args, call args. The outermost struct carries the wire signature; inner structs compose naturally. |
| `DBus::UnixFd` | `unixFd` | An owned Unix file descriptor (`h`) — see [Unix file descriptors](#unix-file-descriptors-h). The only way to send an fd; every received fd arrives as one. |

When the method signature is known (from introspection or catalog), plain
JS values suffice for ordinary data. File descriptors still require an
owning `unixFd`; a number never becomes an fd because of a declaration.
Value types remain as explicit overrides for signature-less calls.

### Byte Arrays (`ay`)

Byte array properties arrive as JS `ArrayBuffer`. Use `DBusUtils` for
text conversion:

JavaScript excerpt with `accessPoint` and `proxy` provided by the application
and both DBus imports above:

```js
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

Guide: [sending, receiving and closing file descriptors](docs/guides/file-descriptors.md).

A file descriptor crosses D-Bus only as a **`DBusQML.unixFd`** — a value that
**owns** its descriptor (prior art: Node `FileHandle`, Go `os.File`, Qt
`QDBusUnixFileDescriptor`, dbus-python `UnixFd`). This works in both
directions, over session, system and custom buses (the ScreenCast/Camera
`OpenPipeWireRemote` scenario).

- **Sending.** An `h` position (top level, `ah`, struct member, dict value,
  variant payload) accepts only a `unixFd` (C++: also a
  `QDBusUnixFileDescriptor`). A plain number fails the call **locally**, with
  an error naming `DBusQML.unixFd`, and nothing is sent. This holds whatever
  declared the `h`: peer introspection, the catalog, `_signatures`, a message
  signature, or `variant(x, "h")`. On the served side, a reply or signal arg
  declared `h` that receives a number becomes an error reply or a skipped
  signal (with a warning). A closed `unixFd` cannot be sent either. Sending
  never consumes the sender's `unixFd`: it stays open until you `close()` it.
- **Receiving.** Every received `h` (and every `ah` element) arrives as a
  `unixFd` holding **its own** duplicate of the descriptor. Two listeners on
  one signal (two watchers, a watcher and a proxy, …) each get an
  independent fd, so one listener's `close()` never closes another's. An fd
  that arrives invalid is a `unixFd` with `valid === false`. An option
  declared `h` in `_options` takes only a received fd; a number there is an
  `InvalidArgs` type mismatch.

JavaScript excerpt for a proxy declaring `Consume(h)` and `OpenThing() → h`:

```js
// Send: wrap the fd you want to pass (the wrapper dups it; you keep yours).
var f = DBusUtils.openFd("/run/user/1000/doc.txt", "r")
proxy.call("Consume", [f])          // declared h: the fd travels
f.close()                           // done with it on our side

// Receive: a unixFd you own.
var r = proxy.call("OpenThing", [])
r.finished.connect(function() {
    if (r.isError) {
        console.error(r.error.name, r.error.message)
        return
    }
    var fd = r.value
    if (!fd.valid) return
    var bytes = DBusUtils.readFd(fd, 4096)
    fd.close()
})
```

### `DBusQML.unixFd`

| Member | Description |
| :--- | :--- |
| `valid` | `true` while the fd is open. `false` after `close()`, after a failed `openFd`, and for a received invalid fd |
| `close()` | Closes the fd now, for **every copy** of the value. Idempotent |
| `take()` | Returns a **duplicate** fd number (at least 3) that you own; close it with `DBusUtils.closeFd(n)`. The `unixFd` itself stays open. Use it to hand the fd to an API that wants a number |
| `toString()` | `"DBusQML.unixFd(valid)"` / `"DBusQML.unixFd(invalid)"`. It never shows the number |
| `new DBusQML.unixFd(n)` | Wraps a duplicate of fd number `n`; you keep (and close) `n` |

Copies share one state: `var b = a`, a `property var` holding it, a JS
array element and the `reply.value` you read again all refer to the same fd.
A unixFd converts to no number (`Number(fd)` is `NaN`).

**Closing.** Call `close()` when you are done. A `unixFd` that reached
JavaScript and is collected while still open is closed by the garbage
collector, which warns `dbusqml: fd closed by garbage collector — call
close()`. Engine destruction closes every fd its JS still holds. A **pressure
valve** watches the wrapper-owned fds of the whole process. When their
number crosses `clamp(RLIMIT_NOFILE soft / 4, 64, 1024)`, it queues a garbage
collection on registered engines that received a library-delivered `unixFd`,
and warns once per process. Constructor-only engines are outside that
registration scope (details below). It is a fallback for forgotten
`close()` calls, not a substitute for them.

**A reply owns its fds.** The fds in a `DBusPendingReply`'s values belong
to the reply. `reply.value` hands out copies of the same `unixFd`, and the
fds close when the reply and every copy are gone. Fds nobody reads therefore
close with the reply. Examples: the values after the first in a
promise-style `asyncCall`, a promise without callbacks, a rejected
`Properties.Set`, an option the whitelist drops.

### Qt construction and pressure-valve scope

`unixFd` is a `QML_CONSTRUCTIBLE_VALUE`. If **the application** declares a
`DBusQML.unixFd`-typed property or function parameter, Qt can construct a
wrapper from an assigned/passed number before the library sees it. That
local type declaration is an explicit application choice, distinct from a
peer declaring an `h` argument. Library helpers and call arguments accept
untyped values and refuse a bare number at an `h` boundary; they do not
perform that implicit numeric construction.

The pressure valve queues collection only for engines registered when the
library delivered a valid `unixFd` (for example, through `openFd`, a reply
or a signal). An engine whose fds come **solely** from
`new DBusQML.unixFd(n)` is not registered by that constructor, so it is not
a pressure-triggered collection target unless it also receives a
library-delivered fd. Constructor-created values still close with their
last copy on garbage collection and warn when collected open. In every
case, explicit `close()` remains the timely cleanup contract.

C++ consumers receive the same `DBus::UnixFd` in `QVariant`s. It converts to
a `QDBusUnixFileDescriptor` that shares its descriptor
(`v.value<QDBusUnixFileDescriptor>()`), and `fileDescriptor()` borrows the
number while the value stays open. A `unixFd` is not for concurrent use from
several threads.

### fd helpers (`DBusUtils`)

QML has no fd I/O — the same gap the `ay` codec hit (0.3.0 shipped
`textFromBytes`/`bytesFromText` *with* `ay` support because QML lacked text
codecs). These helpers fill it. `openFd` returns a `unixFd`, and
`readFd` / `writeFd` / `fdUrl` require one: a plain number is refused with a
warning, never read, written or turned into a URL. `closeFd` is the separate
numeric-duplicate cleanup helper described below.

- `DBusUtils.openFd(path, mode) → unixFd` — fopen-style `mode` (`"r"`,
  `"w"`, `"rw"`); an invalid `unixFd` + warning on failure.
- `DBusUtils.writeFd(unixFd, ArrayBuffer|string) → int` — bytes written;
  −1 + warning on failure.
- `DBusUtils.readFd(unixFd, maxBytes) → ArrayBuffer` — empty on error/EOF.
- `DBusUtils.fdUrl(unixFd) → string` — `"file:///proc/self/fd/N"`, so
  regular-file fds flow into path-based QML consumers (`Image`,
  Quickshell `FileView`, …) with no extra I/O code.
- `DBusUtils.closeFd(n)` — closes a number you got from `take()`. It refuses
  0–2 (stdin/stdout/stderr) and a `unixFd` (call its `close()`).

Caveats:

- **fd taxonomy.** `readFd`/`writeFd` work on any fd — regular files,
  pipes, sockets. These are synchronous I/O helpers and can block.
  `fdUrl` is useful for **regular-file-backed consumers only**: pipes, sockets, and
  anon inodes (the ScreenShot2 pipe, the `OpenPipeWireRemote` socket)
  have no usable path — use `readFd`/`writeFd` for streams. The helper
  checks that the owning value is open, but does not validate file kind.
- **Access mode travels with the fd.** A transferred fd keeps its access
  mode; a receiver cannot read through a write-only fd — open `rw` when
  the other side must read.
- **fdUrl lifetime.** The URL is valid only while the `unixFd` stays open —
  lazy/async loaders must not outlive it.

### Migrating from 0.9.0 / 1.0 plain-int fds

| Before | Now |
| :--- | :--- |
| send an fd number with a declared `h` | send `DBusUtils.openFd(...)` or `new DBusQML.unixFd(n)` |
| a received fd is an `int`, `-1` if invalid | a received fd is a `unixFd`, `valid === false` if invalid |
| `DBusUtils.closeFd(fd)` on a received fd | `fd.close()` |
| `openFd(...)` returns an `int`, `-1` on failure | `openFd(...)` returns a `unixFd`; check `valid` |
| `readFd`/`writeFd`/`fdUrl(int)` | the same helpers with a `unixFd` |

## ListModel and dbusqml values

Guide: [value-preserving storage](docs/guides/lifetime-and-memory.md#store-value-types-without-flattening-them).

Qt's [`ListModel`](https://doc.qt.io/qt-6/qml-qtqml-models-listmodel.html)
keeps QObjects, URLs, dates, functions and simple values (strings, numbers,
booleans, nested lists). It copies **any other object property by property**
into a plain map. Qt's own value types behave the same: `Qt.point(1, 2)`
stored in a `ListModel` comes back as a plain `{x, y}` object.

For dbusqml this means every value wrapper (`objectPath`, `variant`,
`uint64`, …, `unixFd`) loses its type and methods inside a `ListModel` row.
For a `unixFd` the copy is a frozen `{valid: true}` that holds **no
descriptor**: the real fd closes when the original `unixFd` is collected
(with the GC warning), and the helpers refuse the copy.

Use instead:

- a `property var` or a JS array / object. JS arrays work directly as view
  models (`model: myArray`);
- Quickshell's `ScriptModel` where available;
- a key in the `ListModel` row, with the value looked up in a `property var`
  map.

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
- **Decode nesting follows the D-Bus limits:** up to 32 array levels and 32
  struct levels per signature, independently, and up to 64 total container
  levels in a message value (including variants and dict entries). A variant
  starts a new signature but retains the total-depth budget. These follow
  libdbus's `DBUS_MAXIMUM_TYPE_RECURSION_DEPTH` (32) and its doubled body-depth
  limit. The send-side signature walkers currently retain their depth-32
  guard; exceeding a decode limit takes the failure paths above, never a
  non-advancing loop. Signature **validation** (Shape Selection) uses the
  spec's own per-kind caps, so a spec-valid shape deeper than 32 total
  levels is accepted by the validator but remains unproducible by the
  depth-32 walkers and fails loud at marshal time.

### Signal handlers on `DBus` elements run in C++-object context

Inline signal handlers declared on a `DBus` element (`onIntrospectionCompleted: ...`, `onSignalReceived: ...`, `onStatusChanged: ...`) evaluate in the C++ object's context, not in the enclosing QML scope. Consequences:

- `id` references and `Q_PROPERTY` values are accessible.
- JavaScript functions defined in the QML scope are **not** callable from these handlers.

Workarounds:

```qml
import QtQuick
import DBus 1.0

Item {
    id: root
    function refreshPlayers() { console.log("Refresh requested") }
    DBus { id: proxy }
    Connections {
        target: proxy
        function onIntrospectionCompleted() { root.refreshPlayers() }
    }
}
```

This is inherent to Qt 6; there is no library-side fix.

### Runtime-configured proxies: dynamic methods are async

Introspection is asynchronous even when `service`, `path` and `iface` are
set inline in QML. Do not assume dynamic methods exist in
`Component.onCompleted` or before the remote introspection reply arrives.

For a proxy whose properties are assigned at runtime (typically via `onActivePlayerChanged`, a selector, etc.), introspection is asynchronous — dynamic method callbacks are **not** yet installed in the same event-loop tick as the assignment.

Wait for `introspectionCompleted` (or check `status === DBus.Ready`) before
calling dynamic methods on either an inline or runtime-configured proxy:

The complete notification example in the proxy section uses this pattern;
the [calling-methods guide](docs/guides/calling-methods.md) also shows a
complete call to the bus daemon.
