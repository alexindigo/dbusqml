# Follow properties and signals

Bind to a proxy property when your UI needs current state. Listen to
`signalReceived` for interface events. Use a standalone watcher when you
need signals without a proxy or from any sender.

## Bind to remote properties

This window follows NetworkManager's `WirelessEnabled` property on the
system bus. It needs NetworkManager; a machine using another network
service will not provide this interface.

```qml
import QtQuick
import QtQuick.Controls
import DBus 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 480
    height: 180
    title: "Wireless state"

    DBus {
        id: network
        connection: SystemBus
        service: "org.freedesktop.NetworkManager"
        path: "/org/freedesktop/NetworkManager"
        iface: "org.freedesktop.NetworkManager"
        watchServiceStatus: true
        callTimeout: 5000
    }

    Label {
        anchors.centerIn: parent
        text: !network.serviceAvailable ? "NetworkManager is unavailable"
              : network.status !== DBus.Ready ? "Loading wireless state…"
              : network["wirelessEnabled"] === null ? "Wireless state is unknown"
              : network["wirelessEnabled"] === true ? "Wireless enabled" : "Wireless disabled"
    }

    Connections {
        target: network
        function onPropertyWriteFailed(name, errorName, message) {
            console.error(name, errorName, message)
        }
    }
}
```

The dynamic property can also be read as `network.wirelessEnabled`.
Bracket access keeps static lint from treating a runtime property as a
missing C++ member. The catalog or introspection pre-populates known
properties with `null` until their values arrive.

Names use QML camelCase: `WirelessEnabled` becomes `wirelessEnabled`, and
`URLConfig` becomes `urlConfig`. Initial `GetAll` populates the map.
`PropertiesChanged` updates it automatically, filtered by interface.
Invalidated names are re-read with `Properties.Get`; a failed re-read
keeps the previous value and warns.

For a one-off read, `network.getProperty("WirelessEnabled")` returns a
pending reply; handle its completion as in [getting started](getting-started.md).
For a write, `network.setProperty("WirelessEnabled", true)` or a dynamic
property assignment sends `Properties.Set`. Do this only in response to an
intentional user action. Writes are optimistic; if the service rejects one,
`propertyWriteFailed` fires and the proxy re-fetches the server's value.
The proxy never invents a rollback value.

`status` describes loading, not service ownership. `serviceAvailable`
requires `watchServiceStatus: true`. The signal sender check below is
independent of that setting.

## Listen to interface events

This file logs service-name changes from the bus daemon:

```qml
import QtQuick
import DBus 1.0

Item {
    DBus {
        id: daemon
        connection: SessionBus
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
        propertiesEnabled: false
    }

    Connections {
        target: daemon
        function onSignalReceived(name, args) {
            if (name === "NameOwnerChanged")
                console.log(args[0], "owner:", args[1], "->", args[2])
        }
    }
}
```

The proxy discovers interface signals through introspection/catalog data.
In 1.1.0, it binds each subscription, including `PropertiesChanged`, to
the configured service and accepts signals only from its **current owner**.
A peer using the same path, interface and member without owning the name
does not reach the proxy. A queued signal from an old owner is discarded
after an ownership change. This does not turn the proxy into a wildcard
monitor.

## Watch without introspection

Use `DBusSignalWatcher` for a known signal or monitoring-style subscription.
An empty `service` means any sender; an empty `member` means any member.
`path` and `iface` must still be set.

```qml
import QtQuick
import DBus 1.0

Item {
    DBusSignalWatcher {
        connection: SessionBus
        service: ""
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
        member: "NameOwnerChanged"
        enabled: true
        onReceived: function(member, args) {
            console.log(member, args[0], args[1], args[2])
        }
    }
}
```

Choose a nonempty `service` when only that service's signals are relevant.
Changing a watcher's connection or filter removes its exact old
subscription before creating the new one. `enabled: false` unsubscribes.

## Discover objects with ObjectManager

For services implementing `org.freedesktop.DBus.ObjectManager`, start with
the inventory, then follow interface additions and removals. This file
uses BlueZ on the system bus:

```qml
import QtQuick
import DBus 1.0

Item {
    DBusObjectManager {
        id: devices
        connection: SystemBus
        service: "org.bluez"
        path: "/"
        onReadyChanged: console.log("Initial inventory ready:", devices.ready)
        onManagedObjectsChanged: console.log(Object.keys(devices.managedObjects))
        onInterfacesAdded: function(objectPath, ifaces) {
            console.log("Added:", objectPath, Object.keys(ifaces))
        }
        onInterfacesRemoved: function(objectPath, ifaces) {
            console.log("Removed:", objectPath, ifaces)
        }
    }
}
```

`managedObjects` is a map of path → interface → property → value. Its
property names are wire names, not the proxy's camelCase names. It follows
`InterfacesAdded` / `InterfacesRemoved`; use a per-object `DBus` proxy for
ongoing property changes. `ready` becomes true after the initial inventory
arrives. Fetch/decode failures warn; an initial failure does not set it.
Changing service, path or connection resubscribes and refetches.

Reference: [proxy data signals](../../API.md#data-signals),
[`DBusSignalWatcher`](../../API.md#dbussignalwatcher),
[`DBusObjectManager`](../../API.md#dbusobjectmanager).
