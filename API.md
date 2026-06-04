# DBus QML API Documentation

This document describes the public API of the `DBus` QML module.

## Module Import

To use the module in QML, import it as follows:

```qml
import DBus 1.0
```

## Core Components

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

**Dynamic methods** — D-Bus methods become callable directly on the element, using their original PascalCase names:
```qml
DBus {
    id: mpris
    service: "org.mpris.MediaPlayer2.myplayer"
    path: "/org/mpris/MediaPlayer2"
    iface: "org.mpris.MediaPlayer2.Player"
}
mpris.PlayPause()          // → calls "PlayPause" on the D-Bus object
mpris.Seek(50000000)       // → calls "Seek" with argument
mpris.SetVolume(0.8)       // → calls "SetVolume" with argument
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

Properties auto-update via `PropertiesChanged` signals.

#### Configuration Properties

| Property | Type | Description |
| :--- | :--- | :--- |
| `service` | `string` | The D-Bus service name. |
| `path` | `string` | The D-Bus object path. |
| `iface` | `string` | The D-Bus interface name. |
| `connection` | `DBusConnection` | The connection associated with this proxy. |

#### Built-in Methods

| Method | Arguments | Description |
| :--- | :--- | :--- |
| `call(method, args)` | `string method`, `list args` | Generic fallback for calling any D-Bus method. |

#### Signals

| Signal | Arguments | Description |
| :--- | :--- | :--- |
| `serviceChanged` | | Emitted when the `service` property changes. |
| `pathChanged` | | Emitted when the `path` property changes. |
| `ifaceChanged` | | Emitted when the `iface` property changes. |
| `connectionChanged` | | Emitted when the `connection` property changes. |
| `signalReceived(name, args)` | `string name`, `list args` | Emitted when a D-Bus signal is received on this object. |
| `valueChanged(key, value)` | `string key`, `variant value` | Emitted when a dynamic property changes (from QQmlPropertyMap). |

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

`DBusConnection` provides methods for making asynchronous calls to the bus.

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

## Data Types

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

These types are used for explicit typing of D-Bus data. Access them through a module alias when `import DBus 1.0` (the element `DBus` shadows the module namespace):

```qml
import DBus 1.0 as DBusQML

DBusQML.uint32(42)
DBusQML.int64(21474836470)
DBusQML.string("hello")
DBusQML.boolean(true)
DBusQML.objectPath("/org/freedesktop/UPower")
DBusQML.variant("any value")
DBusQML.dict({ key: "value" })
```

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
