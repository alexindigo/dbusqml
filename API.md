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

**Limitations:**
- Function names must follow QML camelCase convention (lowercase first letter)
- Signal forwarding to D-Bus requires explicit `emitSignal()` calls — QML `signal` declarations are not automatically forwarded
- Maximum 5 parameters for forwarded signals

#### Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The D-Bus service name to claim (optional — omit for unique-name-only registration). |
| `path` | `string` | The object path to register at. |
| `iface` | `string` | The interface name to expose. |
| `connection` | `DBusConnection` | The bus to register on (default session bus). |

#### Methods

| Method | Arguments | Description |
| :--- | :--- | :--- |
| `emitSignal(name, args)` | `string name`, `list args` | Emit a D-Bus signal on this adaptor's path/interface. |

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
| `DBus::Variant` | `variant` | D-Bus variant. |

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
