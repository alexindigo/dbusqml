# Keep results without keeping every call

Keep a pending reply when you need to read it later. Drop the reference
when you no longer need it. You do not need to destroy QML-facing replies
manually.

## Capture a reply safely

This complete file retains the latest completed reply and reads its first
output argument through a binding:

```qml
import QtQuick
import QtQuick.Controls
import DBus 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 440
    height: 180
    property var latestReply: null

    Label {
        anchors.centerIn: parent
        text: !root.latestReply || !root.latestReply.isFinished ? "Waiting…"
              : root.latestReply.isError ? root.latestReply.error.message
              : "Bus names: " + root.latestReply.value.length
    }

    Component.onCompleted: {
        const reply = SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames",
            arguments: []
        })
        root.latestReply = reply
        reply.finished.connect(function() {
            if (reply.isError)
                console.error(reply.error.name, reply.error.message)
            else
                console.log("Names:", reply.value)
        })
    }
}
```

While a call is pending, its creator owns the reply. At completion, a
QML-facing reply is handed to the JavaScript garbage collector. A retained
reference keeps it alive; dropping all references allows collection.
`value`, `values` and `error` are cached and remain readable after completion.

In 1.1.0, `finished()` is one-shot and takes **no arguments**. After delivery
all its receivers are disconnected. A handler that captures `reply`, as
above, is safe: that connection no longer keeps a closure/reply cycle alive.
Connect immediately after making the call. A connection made after
delivery does not run; check `isFinished` if inspecting an older reply.

Bindings on reply properties update once at completion. A settled reply
does not change again. The promise-style `asyncCall(message, resolve,
reject)` overload owns and deletes its private reply automatically, with
or without callbacks. Pure C++ consumers without a QML engine retain
creator-parented ownership.

## Close resources, not just references

A reply containing an fd owns the `unixFd` in its cached values. Reading
it gives you a shared-state copy. Keeping either the reply or one of those
copies can keep the fd open. Call `close()` as soon as the resource's
purpose ends; dropping a reply is not a timely close strategy.

For held replies, the adaptor owns the handle while it is pending, even
if JavaScript drops every reference. Settle it with `send` or `sendError`.
While the QML engine is alive, a settled held reply is unparented and
collectable. Clear application queues/maps after settling. With no live
engine, it stays adaptor-owned. Held incoming arguments are released at
dispatch exit; retain a specific argument yourself if later work needs it,
and explicitly close any retained fd when finished.

## Store value types without flattening them

Qt's [ListModel documentation](https://doc.qt.io/qt-6/qml-qtqml-models-listmodel.html)
describes its dictionary-to-model conversion and nested models. `ListModel`
is not a general JavaScript object store: dbusqml value types are copied
property by property into plain model data. They lose their type and
methods. Qt values such as `Qt.point(1, 2)` behave similarly.

A `unixFd` stored directly in a row becomes a snapshot such as
`{valid: true}`. It holds no descriptor. The original value can be collected
and closed while the row still says `true`. `dynamicRoles` does not make
that row a value-preserving store.

Store a key in the model and keep the actual value in a separate map:

```qml
import QtQuick
import QtQml.Models
import DBus 1.0

Item {
    id: root
    property var handles: ({})
    ListModel { id: rows }

    Component.onCompleted: {
        const fd = DBusUtils.openFd("/etc/hostname", "r")
        if (!fd.valid) {
            console.error("Could not open /etc/hostname")
            return
        }
        root.handles = { hostname: fd }
        rows.append({ handleKey: "hostname", label: "Host name" })
        const retained = root.handles[rows.get(0).handleKey]
        console.log(DBusUtils.textFromBytes(DBusUtils.readFd(retained, 256)))
        retained.close()
        rows.clear()
        root.handles = {}
    }
}
```

A `property var`, plain JS array or object retains value types. JS arrays
also work as view models. Replace the property with a new array/object
when the UI needs notification of a structural change; mutating an array
with `push()` does not itself notify a QML property binding.

Reference: [pending-reply lifetime](../../API.md#lifetime),
[ListModel and dbusqml values](../../API.md#listmodel-and-dbusqml-values).
