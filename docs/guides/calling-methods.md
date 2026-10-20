# Call methods with the right types

Use a `DBus` proxy when you repeatedly use one remote interface. Use
`SessionBus.asyncCall` or `SystemBus.asyncCall` for a one-off call or explicit
control over a message.

## Call through a proxy

This complete file calls the bus daemon through a proxy:

```qml
import QtQuick
import DBus 1.0

Item {
    id: root
    property var endpoint: daemon

    DBus {
        id: daemon
        connection: SessionBus
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
        propertiesEnabled: false
        callTimeout: 5000
        _signatures: ({ NameHasOwner: "s" })
    }

    Connections {
        target: daemon
        function onIntrospectionCompleted() {
            const reply = root.endpoint.call("NameHasOwner", ["org.freedesktop.DBus"])
            reply.finished.connect(function() {
                if (reply.isError)
                    console.error(reply.error.name, reply.error.message)
                else
                    console.log("Bus daemon owns its name:", reply.value)
            })
        }
    }
}
```

`call("NameHasOwner", args)` takes the exact wire method name and returns
a pending reply. The `var` reference above lets static tooling treat the
property-map-backed proxy as dynamic; it invokes the same method as
`daemon.call(...)`. Introspection also installs camelCase methods, so after
introspection the equivalent dynamic call is
`daemon.nameHasOwner("org.freedesktop.DBus")`. The explicit `call()` form
is useful for tooling that cannot see runtime-generated methods.

Wait for `introspectionCompleted` before using dynamic methods, especially
after configuring a proxy at runtime. Use `Connections` when a handler
needs functions from the surrounding QML scope; inline handlers on a
`DBus` element run in the C++ object's context.

## Use explicit messages and typed wrappers

This asks the bus daemon for a service owner's Unix user ID. The input is
one string (`s`); the output is an unsigned integer (`u`).

```qml
import QtQuick
import DBus 1.0
import DBus 1.0 as DBusQML

Item {
    Component.onCompleted: {
        const reply = SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "GetConnectionUnixUser",
            arguments: [new DBusQML.string("org.freedesktop.DBus")],
            signature: "s",
            timeout: 5000
        })
        reply.finished.connect(function() {
            if (reply.isError)
                console.error(reply.error.name, reply.error.message)
            else
                console.log("Unix user ID:", reply.value)
        })
    }
}
```

The string wrapper and `signature` are redundant here on purpose: either
can describe this simple input. For a raw message, declare its input
signature or use wrappers when plain JS types are ambiguous. For example,
`new DBusQML.uint32(42)` selects `u`, `objectPath("/some/path")` selects
`o`, and `variant(["one", "two"], "as")` selects a variant holding a string
array. Construct each with `new DBusQML.…` as in the file above.

On a proxy, the precedence is `_signatures` → introspection/catalog →
inference. `_signatures` maps **wire method names** to concatenated input
types. On an adaptor it instead describes **output** types.
Raw-message `signature` also describes inputs, not the reply.

| Input shape | Declaration | Why it matters |
| :--- | :--- | :--- |
| Empty string array | `as` | Plain `[]` infers `av` |
| Nested settings map | `a{sa{sv}}` | Plain objects infer `a{sv}`, not a guessed nested shape |
| Open descriptor | `h` and a `unixFd` value | A signature never turns a number into an fd |
| Large 64-bit integer | `x` or `t`, with a decimal string | JS numbers cannot preserve all integers above 2^53 |

See [shape selection](../../API.md#shape-selection) for limits and failure
behavior. Correct malformed declarations; do not rely on a warning's
inference fallback to preserve the intended wire type.

## Use promise-style callbacks

The three-argument overload calls `resolve(firstOutput)` or
`reject(error)`. It manages its own reply. It does **not** return a
JavaScript `Promise`; there is no `.then()` or `await` on its return value.
Use the pending-reply overload when you need every output argument.

```qml
import QtQuick
import DBus 1.0

Item {
    Component.onCompleted: {
        SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames",
            arguments: [],
            timeout: 5000
        }, function(names) {
            console.log(names.join(", "))
        }, function(error) {
            console.error(error.name, error.message)
        })
    }
}
```

## Bound the wait

`callTimeout` is in milliseconds and covers all proxy traffic, including
startup introspection, property fetches, method calls and writes. Raw
messages use `timeout`. `-1` selects Qt's default. A timeout is a failed
wait, not cancellation of the work already started by the service.

Use `proxy.send(method, args)` or `SessionBus.send(message)` only when you
intentionally want no reply. They imply `NO_REPLY_EXPECTED`; they cannot
report a remote error. Prefer a normal call for actions whose result
matters.

Reference: [`DBus`](../../API.md#dbus-proxy-element),
[`DBusConnection`](../../API.md#dbusconnection),
[value types](../../API.md#d-bus-value-types).
