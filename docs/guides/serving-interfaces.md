# Serve an interface from QML

Declare a `DBusAdaptor` to register an object and handle calls. Put the
public contract in catalog XML so clients see the same names, signatures
and property access that your service enforces.

## Publish methods, properties and signals

For this example, save the following catalog as
`~/.config/dbusqml/types/com.example.Greeter.xml` **before** starting QML:

```xml
<node>
  <interface name="com.example.Greeter">
    <method name="Greet">
      <arg name="person" type="s" direction="in"/>
      <arg name="greeting" type="s" direction="out"/>
    </method>
    <method name="Configure">
      <arg name="options" type="a{sv}" direction="in"/>
    </method>
    <method name="Delete"/>
    <property name="Greeting" type="s" access="readwrite"/>
    <property name="Version" type="s" access="read"/>
    <signal name="Greeted"><arg name="person" type="s"/></signal>
  </interface>
</node>
```

Then save and run this complete QML file. Keep it running while another
process calls it.

```qml
import QtQuick
import DBus 1.0
import DBus 1.0 as DBusQML

Item {
    DBusAdaptor {
        id: greeter
        connection: SessionBus
        service: "com.example.Greeter"
        path: "/com/example/Greeter"
        iface: "com.example.Greeter"

        property string greeting: "Hello"
        property string version: "1.0"
        property int _calls: 0

        _members: ({ Delete: "resetGreeting" })
        _signatures: ({ Greet: "s" })
        _options: ({ Configure: { greeting: "s" } })
        _signals: ({ Greeted: "s" })

        onNameAcquired: console.log("Serving com.example.Greeter")
        onNameLost: console.error("Greeter lost its service name")

        function greet(person) {
            if (person.length === 0)
                throw DBusQML.DBusUtils.error("com.example.Greeter.EmptyName", "Supply a name")
            greeter._calls += 1
            greeter.emitSignal("Greeted", [person])
            return greeter.greeting + ", " + person
        }

        function configure(options) {
            if (options.greeting !== undefined)
                greeter.greeting = options.greeting
        }

        function resetGreeting() {
            greeter.greeting = "Hello"
        }
    }
}
```

From a second terminal on the same session bus:

```sh
busctl --user call com.example.Greeter /com/example/Greeter com.example.Greeter Greet s Ada
busctl --user get-property com.example.Greeter /com/example/Greeter com.example.Greeter Version
```

The first command returns `s "Hello, Ada"`; the second returns `s "1.0"`.

### Keep the wire contract explicit

| Declaration | Effect in this service |
| :--- | :--- |
| QML `function greet` | Handles wire method `Greet` |
| QML `property string greeting` | Serves `Greeting` through `Properties.Get/GetAll/Set` |
| Catalog `Version` with `access="read"` | A bus `Set` returns `org.freedesktop.DBus.Error.PropertyReadOnly`, even though the QML property is writable locally |
| `_members` | Maps wire `Delete` to QML `resetGreeting`; useful for reserved or colliding names |
| `_signatures` | Declares reply types; `Greet: "s"` is redundant with this catalog, but shows the override syntax |
| `_options` | Keeps only the declared keys from Configure's final `a{sv}` argument and type-checks their values before running the handler |
| `_signals` | Declares a wire signal's argument types; `emitSignal` sends it |
| `_calls` | Private helper property; never exported |

Unknown option keys are dropped. A declared key of the wrong kind or an
unconvertible value causes `InvalidArgs` before the handler runs. An empty
entry such as `Configure: {}` drops **every** key; omitting the method's
entry disables filtering. The policy applies to the resolved handler, so
using another accepted spelling cannot bypass it.

Property changes send standard `PropertiesChanged` notifications under the
advertised wire name. Property notify signals do not also become custom
bus signals. You can instead declare ordinary QML signals (for example,
`signal greeted(person: var)`) and emit them normally; they are relayed
automatically, with a maximum of five parameters. Use `var` parameters for
the current QVariant-based relay, and let the catalog or `_signals` declare
the wire types. Native typed parameters such as `string` do not connect to
that relay. Do not send the same event both ways.

## Answer later with holdReply

Call `holdReply()` synchronously inside a method handler, then retain the
handle until you can answer. This standalone service batches pending calls
on a timer. It does not need catalog XML because `_signatures` declares its
single string reply and the method takes no inputs.

```qml
import QtQuick
import DBus 1.0

Item {
    id: root
    property var pending: []

    DBusAdaptor {
        id: delayed
        connection: SessionBus
        service: "com.example.Delayed"
        path: "/com/example/Delayed"
        iface: "com.example.Delayed"
        _signatures: ({ ReadLater: "s" })

        function readLater() {
            const reply = delayed.holdReply()
            root.pending.push(reply)
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

Call it from another process with
`busctl --user call com.example.Delayed /com/example/Delayed com.example.Delayed ReadLater`.
Expect `s "Ready"` after a short delay.

Use `reply.sendError("com.example.Delayed.Failed", "Reason")` on failure;
use `reply.send()` for a void reply. Each handle settles once. Do not also
return a result from a held handler. The adaptor owns pending handles;
settled handles become collectable when their live QML engine has no
references to them. Drop your references after answering.

There is no server timeout by default. Set `heldReplyTimeout` (milliseconds,
`0` disables it) if your service needs one, and coordinate cancellation of
your own asynchronous work so it does not try to answer again. Destroying
the adaptor or its engine errors outstanding held calls. Never throw
`undefined` after holding: Qt cannot distinguish it from a normal void
return, so the caller may wait until its timeout.

A deferred reply cannot round-trip on QtDBus's same-connection local loop.
Use a second process, as above, or a distinct custom connection for a
client that lives in the same process.

## Claim a name and choose a lifetime

`service` requests a well-known name; omitting it serves only on the
connection's unique name. Watch `nameAcquired` / `nameLost` to report
ownership. `allowReplacement`, `replaceExisting` and `queueOnBusy` control
the RequestName flags and all default to false. Only advertise readiness
after acquisition when a well-known name is required.

Set service, path, interface, connection and `captureSubtree` before
attachment. Later identity writes warn and are ignored. For several
interfaces at one path, use one adaptor per interface with the same
service/path/connection. For dynamically created request children, see
[`captureSubtree`](../../API.md#dbusadaptor-server-element) and the
[per-call lifecycle](../../API.md#per-call-adaptor-lifecycle). `unregister()`
retires a registration immediately and cannot be reversed on that adaptor.

Reference: [`DBusAdaptor`](../../API.md#dbusadaptor-server-element),
[deferred replies](../../API.md#deferred-replies), [catalogs](../TYPES.md).
