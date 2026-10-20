# Make your first call

List the names registered on your session bus. This uses the bus daemon
itself, so you do not need a notification server, portal or media player.

## Install and import

On Arch Linux, install `qt6-dbusqml` from the AUR, or `qt6-dbusqml-git` for
the development version. For other systems, follow the
[source installation instructions](../../README.md#build-from-source).
Install Qt Quick Controls too if you want to run these window examples.

Use `import DBus 1.0` for elements and singletons. Add
`import DBus 1.0 as DBusQML` when constructing value types such as
`new DBusQML.uint32(42)`. The alias avoids a name collision with the `DBus`
proxy element. Library version 1.1.0 still uses import version 1.0.

## Call, wait, then read

Save this as `Main.qml`:

```qml
import QtQuick
import QtQuick.Controls
import DBus 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 560
    height: 420
    title: "Session-bus names"

    property string statusText: "Loading…"

    ScrollView {
        anchors.fill: parent
        anchors.margins: 20
        TextArea {
            text: String(root.statusText)
            readOnly: true
            wrapMode: TextEdit.Wrap
        }
    }

    Component.onCompleted: {
        const reply = SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames",
            arguments: [],
            timeout: 5000
        })
        reply.finished.connect(function() {
            if (reply.isError) {
                root.statusText = reply.error.name + ": " + reply.error.message
                return
            }
            root.statusText = reply.value.join("\n")
        })
    }
}
```

Run `qml6 Main.qml`. If you are using an uninstalled build, point
`QML_IMPORT_PATH` at the build directory first:

```sh
QML_IMPORT_PATH=/path/to/dbusqml/build-release qml6 Main.qml
```

`SessionBus.asyncCall(message)` returns a `DBusPendingReply` immediately.
Connect `finished` right after creating it. Read `isError` in that handler,
then either `error.name` / `error.message` or the result.

`value` is the first output argument. Here it is an array of strings.
`values` is the list of **all output arguments**; here it contains that
array as its first element. A void method has no output arguments.

## Choose the bus and destination

| Field | Meaning | Example |
| :--- | :--- | :--- |
| Connection | Which bus to use | `SessionBus` for desktop apps, `SystemBus` for logind |
| `service` | The destination's well-known or unique name | `org.freedesktop.DBus` |
| `path` | An object within that service | `/org/freedesktop/DBus` |
| `iface` | The interface containing the method | `org.freedesktop.DBus` |
| `member` | The wire method name, including its case | `ListNames` |

Errors can come from the service, the bus, or local argument validation.
Always display both the error name and message. A system-bus method can
also require permission; switching buses does not grant it.

Next: [calling methods](calling-methods.md), or the complete
[`DBusPendingReply` reference](../../API.md#dbuspendingreply-object).
