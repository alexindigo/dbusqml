# Diagnose a failed call

Start with the error name and message. Confirm the bus, service, path,
interface and wire member before changing argument types. A local failure
can complete the same reply object as a remote error.

## Print the actual error

This complete file deliberately calls a nonexistent bus-daemon method:

```qml
import QtQuick
import DBus 1.0

Item {
    Component.onCompleted: {
        const reply = SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "NoSuchMethod",
            arguments: [],
            timeout: 5000
        })
        reply.finished.connect(function() {
            if (reply.isError)
                console.error(reply.error.name + ": " + reply.error.message)
            else
                console.log(reply.values)
        })
    }
}
```

Expect an `org.freedesktop.DBus.Error.UnknownMethod` error. Use this pattern
in your application's error display too.

## Interpret the message

| Error or warning | Meaning | What to check |
| :--- | :--- | :--- |
| `argument N is not marshalable` / local `Failed` | A value has no D-Bus representation | Remove QObjects/functions from outgoing data; use plain containers and supported value types |
| `reply could not be decoded` / local `Failed` | The whole reply was rejected, not partially delivered | Check the peer's message shape and nesting; `value`/`values` are empty on this failure |
| A signal decode warning | An undecodable delivery was dropped | Do not expect partial property updates; inspect the emitting service |
| `arguments could not be decoded` / adaptor `InvalidArgs` | The request failed before the handler or property write | Correct the caller's payload; the server property was not changed |
| Signature validation/length warning | A declared type or aggregate message signature is invalid | Use complete types, balanced nonempty structs, dict entries only as array elements, basic dict keys and at most 255 encoded bytes |
| `unparseable declared signature` | A proxy override could not be split into argument types | Correct `_signatures`; inference is not a reliable substitute for the intended declaration |
| `cannot produce declared signature` | A valid declaration could not be marshaled from this value, or a resource/depth limit was reached | Compare the exact nested shape and types; consult the shape-selection limits |
| `PropertyReadOnly` | The served property's effective advertised access is `read` | Check catalog XML and QML/C++ writability; writable local storage does not override catalog access |
| `UnknownMethod` | That interface does not provide that member | Check wire casing, aliases, catalog and introspection; `_members` maps wire name to QML name |
| `UnknownInterface` / `UnknownObject` | Wrong interface or object path | Inspect the service's actual objects; an interface and a service name are different fields |
| `ServiceUnknown` / `NameHasNoOwner` | The destination is unavailable | Confirm the name on the selected bus and whether activation is expected |
| `AccessDenied` | Bus/service policy or an adaptor sender restriction refused the caller | Check permissions; `allowedSender` compares a unique bus name, not a well-known name |
| `NoReply` or timeout error | No answer arrived within the wait | Check the service and timeout; a timeout does not cancel its work |
| Error naming `DBusQML.unixFd` | An `h` position received a number, wrong value or closed descriptor | Send a valid owning `unixFd`, not `take()`'s numeric result |
| `openFd` warning and `valid === false` | Open failed | Supply a native path, existing readable file and appropriate mode |
| `readFd` / `writeFd` / `fdUrl` expects a `unixFd` | A number or flattened model row was passed | Keep the original value outside `ListModel` |
| `fd closed by garbage collector — call close()` | Cleanup was left to collection | Close on success, failure, cancellation and component teardown |
| A consumer cannot load an `fdUrl` | The fd is not a regular file, or has closed | The helper constructs a URL but does not validate file kind; use stream I/O for pipes/sockets and keep regular-file fds open during lazy loads |
| Held reply already settled | A callback tried to answer twice | Give cancellation and completion one settlement owner |

Signature validation permits at most 32 array levels and 32 struct levels
independently. Incoming decoding also caps total container depth at 64,
including variants. Outgoing walkers retain a depth-32 guard. Do not infer
that every spec-valid deeply nested value is producible by this version.

## When it loads but nothing updates

Wait for proxy introspection before calling dynamic methods. Known
properties start as `null`; `status === DBus.Ready` is separate from
`serviceAvailable`, which needs `watchServiceStatus: true`.

Use `Connections { target: proxy; … }` for handlers that call surrounding
QML functions. `DBus` inline handlers run in the C++ object's context.
Use a watcher with an empty `service` only when you deliberately want
any-sender listening; a proxy accepts only its configured owner's signals.

If the bus itself disconnects, dbusqml reports the loss but does not
reconnect automatically. Recreate the affected connection/proxies as part
of your application's connection-loss handling.

Reference: [boundary errors](../../API.md#built-in-methods),
[shape selection](../../API.md#shape-selection),
[known limitations](../../API.md#known-limitations),
[fd lifetime](file-descriptors.md).
