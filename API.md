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
| `getProperty(name)` | `string name` | `DBusPendingReply` | Read a single D-Bus property directly via `Properties.Get`. |
| `setProperty(name, value)` | `string name`, `variant value` | — | Write a D-Bus property directly via `Properties.Set`. |
| `emitSignal(name, args)` | `string name`, `list args` | — | Emit a D-Bus signal from this proxy's path/interface. |
| `connectToBus(address)` | `string address` | `DBusConnection` | (static) Connect to a custom D-Bus address. Returns null on failure. |

**Boundary errors (0.6.0+):** arguments that cannot be marshaled (a
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

**Naming convention:** D-Bus members are PascalCase (`ReadOne`, `ReadAll`),
but QML forbids uppercase-initial method names. The adaptor folds the first
character when dispatching — a D-Bus call to `ReadOne` invokes the QML
function `readOne`. This is the same convention the proxy side uses for
property names (`dbusPropToQml`). Exact-name matches still work for C++
`Q_INVOKABLE`s.

**C++ typed returns (0.6.0+):** a C++ `Q_INVOKABLE` whose return type is any
default-constructible type (`QString`, `int`, `QByteArray`, …) round-trips
into the D-Bus reply with its wire signature. Non-default-constructible
returns cannot be captured — declare a `QVariant` return (the C++ dispatch
path also caps at 5 parameters and warns beyond that).

**Return-value marshaling (0.3.1+):** return values are marshaled through
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
- A struct **inside** a variant (`DBusQML.variant(DBusQML.struct_(...))`) marshals as a real `v` with a struct payload (lifted in 0.4.0); return the struct directly when you need a bare `(...)` signature.

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The D-Bus service name to claim (optional — omit for unique-name-only registration). |
| `path` | `string` | The object path to register at. |
| `iface` | `string` | The interface name to expose. |
| `connection` | `DBusConnection` | The bus to register on (default session bus). |
| `_signatures` | `var` (object) | Explicit reply signatures, keyed by D-Bus member name (see Shape Selection). |

**Private properties:** any adaptor property whose name starts with `_` is
library meta-config (`_signatures`, or your own helpers) and is never served
over D-Bus — it is excluded from `generateXml()` and
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
guard covers **properties** (0.5.2+): a property whose value cannot be
marshaled (e.g. a `var` holding a `QObject*`, such as a stashed
`DBusHeldReply`) produces `InvalidArgs` on `Properties.Get` and is skipped
with a warning in `GetAll` — never an aborted process (before 0.5.2 such a
value killed the hosting process inside QtDBus marshaling, remotely
triggerable via routine introspection). Gadget-valued properties
(`DBusQML.variant`/`bytes`/`struct_`) marshal like method replies do — served
as a single-wrapped `v` carrying the gadget's payload — and properties with
no possible D-Bus representation (`QObject*`-derived) are never advertised in
introspection.

#### Methods

| Method | Arguments | Description |
| :--- | :--- | :--- |
| `emitSignal(name, args)` | `string name`, `list args` | Emit a D-Bus signal on this adaptor's path/interface. |
| `holdReply()` | — | Defer the current method call; returns a `DBusHeldReply` (see below). |
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
- There is no server-side timeout — the caller owns timeouts. (For
  xdg-desktop-portal backends that means honoring the frontend's
  `G_MAXINT`-timeout contract: the backend may hold indefinitely.)
- A caller on the **same `QDBusConnection`** as the adaptor cannot receive a
  deferred reply — QtDBus dispatches local-loop calls synchronously
  (`sendWithReplyLocal`) and reports `local-loop message cannot have delayed
  replies`. Use a separate connection when a process calls its own adaptor.

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

- **Interface-scoped calls** (0.5.1+) route only to the adaptor whose `iface`
  matches the message's interface. A call that carries an interface name
  reaches a matching adaptor and no other — it is no longer answered by a
  member-name match on an adaptor that declares a *different* interface. A
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

> **Migration from the 0.5.0 workaround:** on 0.5.0, a single adaptor could
> serve members of *several* interfaces because dispatch ignored the message
> interface. That no longer holds — with the interface name set, each call now
> routes to an adaptor declaring that interface. Split such an adaptor into
> co-located adaptors (one per interface, sharing `service` and `path`), which
> is the pattern this section enables.

#### Per-call adaptor lifecycle (0.7.0)

`DBusAdaptor` is safe to create dynamically — one instance per call at a
unique object path, the shape portal backends need (a Request object per
portal call):

```qml
// The Request pattern: create per call, retire per call.
property Component requestComp: Component {
    DBusAdaptor {
        function close() { /* caller cancelled */ }
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

Lifecycle semantics (changed in 0.7.0 — on 0.6.0 the first dispatch made such
an adaptor indestructible and leaked its bus registration):

- **`destroy()` works after dispatch.** The adaptor protects itself from the
  JS garbage collector only for the duration of a method dispatch. Once the
  dispatch ends (and no deferred reply is outstanding), ownership returns to
  what it was — for dynamically created objects that means `destroy()` from
  QML succeeds and the GC collects the object once QML drops all references.
  Either way the destructor frees the object path and releases the service
  name reference.
- **GC behavior.** A dynamically created adaptor with no QML references is
  collected on the next `gc()` and its path freed. Declaratively declared
  adaptors are unchanged: they are engine-managed, `destroy()` is refused on
  them, and repeated dispatch never alters that.
- **Held replies block retirement.** While a `holdReply()` reply is
  outstanding, the adaptor stays GC-protected and QML `destroy()` is refused
  (this is correct — the adaptor is the only object that can answer the
  caller). After the last held reply settles, destroy()/GC work again.
  Destroying or unregistering the adaptor with a reply still held errors the
  caller (`org.freedesktop.DBus.Error.Failed`) instead of hanging it.
- **`unregister()` — deterministic retirement.** Frees the object path and
  releases the service reference *immediately*, errors out outstanding held
  replies, and leaves the QObject alive for QML to drop whenever (destroy()
  and GC then work normally). One-way: re-registration after `unregister()`
  is not supported; a second `unregister()` warns and does nothing. Use it
  when the bus path must go away now — e.g. a caller-side `Close` on a portal
  Request — without waiting for GC timing.

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

A declared signature is never silently ignored. If dbusqml cannot produce a
declared signature (e.g. an array of anonymous structs, which needs a
registered carrier type), it logs a warning and falls back to inference —
never a different wire type with no notice.

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
| `DBus::Struct` | `struct_` | D-Bus struct — wraps a JS array of members, marshals via `beginStructure`. Use for struct-typed values like `(ddd)` accent-color or `(uu)` StateReason. Since 0.4.0 it works in every position — variant payloads, map/list values, signal args, call args. The outermost struct carries the wire signature; inner structs compose naturally. |

Since v0.3.0, when the method signature is known (from introspection or
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

## Known Limitations

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
