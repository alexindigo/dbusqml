# Pass an open file descriptor

In 1.1.0, every D-Bus `h` is an owning `DBusQML.unixFd` value. Send that
value, retain it while you need the resource, and call `close()` when done.
A number is never implicitly treated as an fd, even with a declared `h`.

## Send a file to a portal

This complete file sends a readable local file to the OpenURI portal.
Change `localPath` to an existing regular file. The path is a native path,
not a `file:` URL. A desktop portal frontend and suitable backend are
needed on the session bus.

```qml
import QtQuick
import DBus 1.0

Item {
    property string localPath: "/etc/hostname"

    Component.onCompleted: {
        const fd = DBusUtils.openFd(localPath, "r")
        if (!fd.valid) {
            console.error("Could not open", localPath)
            return
        }
        const reply = SessionBus.asyncCall({
            service: "org.freedesktop.portal.Desktop",
            path: "/org/freedesktop/portal/desktop",
            iface: "org.freedesktop.portal.OpenURI",
            member: "OpenFile",
            arguments: ["", fd, {}],
            signature: "sha{sv}",
            timeout: 5000
        })
        // Sending has captured its own descriptor; release our copy now.
        fd.close()
        reply.finished.connect(function() {
            if (reply.isError)
                console.error(reply.error.name, reply.error.message)
            else
                console.log("Portal request handle:", reply.value)
        })
    }
}
```

The empty `parent_window` means the interaction has no parent window. The
reply is a **request object path**, not confirmation that an application
opened the file. The portal's later `org.freedesktop.portal.Request.Response`
signal reports the interaction result. Applications that need that result
must subscribe before the call using the portal's `handle_token` convention;
see the [Request protocol](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Request.html).
The [runnable examples](../../examples/README.md) include window-based
patterns for using descriptors.

Sending does not consume your `unixFd`. Closing it after `asyncCall` or
`proxy.call` returns releases only your side; the outgoing message has
already captured what it needs. This also prevents a leak when the remote
call later fails.

## Hold a received descriptor

logind's `Inhibit("sleep", who, why, "block")` returns an fd. Keeping it
open holds the inhibitor; closing it releases the inhibitor. This complete
file acquires one on startup and releases it after two seconds. It does
not request sleep.

```qml
import QtQuick
import DBus 1.0

Item {
    id: root
    property var inhibitor: null

    Component.onCompleted: {
        const reply = SystemBus.asyncCall({
            service: "org.freedesktop.login1",
            path: "/org/freedesktop/login1",
            iface: "org.freedesktop.login1.Manager",
            member: "Inhibit",
            arguments: ["sleep", "dbusqml-guide", "Demonstrate fd lifetime", "block"],
            signature: "ssss",
            timeout: 5000
        })
        reply.finished.connect(function() {
            if (reply.isError) {
                console.error(reply.error.name, reply.error.message)
                return
            }
            const fd = reply.value
            if (!fd || !fd.valid) {
                console.error("logind returned an invalid descriptor")
                return
            }
            root.inhibitor = fd
            console.log("Sleep inhibitor held")
            releaseTimer.start()
        })
    }

    Timer {
        id: releaseTimer
        interval: 2000
        onTriggered: {
            root.inhibitor.close()
            root.inhibitor = null
            console.log("Sleep inhibitor released")
        }
    }

    Component.onDestruction: {
        if (root.inhibitor)
            root.inhibitor.close()
    }
}
```

logind policy may deny the request; report that error rather than treating
the inhibitor as active. A toggle should keep the same owning value while
on and call `close()` when switched off; see the
[runnable examples](../../examples/README.md) for window-based patterns.

## Close, copy or hand off

| Operation | Ownership |
| :--- | :--- |
| `openFd(path, "r")` | Returns an owning value; check `valid` |
| Read `reply.value`, assign to a `property var` or JS array | Copies share the same state; closing one closes all those copies |
| Receive the same signal in two watchers | Each listener gets an independent duplicate |
| `fd.close()` | Closes now, for all shared copies; safe to repeat |
| `fd.take()` | Returns a caller-owned duplicate number, or `-1` on failure; **leaves the wrapper open** |
| `new DBusQML.unixFd(number)` | Duplicates a number owned by another API; you still own that number |
| `DBusUtils.closeFd(number)` | Closes a numeric duplicate, such as one from `take()`; refuses 0–2 and wrapper values |

Use `DBusUtils.readFd(fd, maxBytes)` for an `ArrayBuffer`,
`writeFd(fd, bytesOrText)` for output, and `textFromBytes` for UTF-8 text.
These helpers also accept pipes/sockets, but I/O can block; they are not an
asynchronous stream API. `fdUrl(fd)` is for regular files only and is valid
only while the descriptor stays open. A lazy image loader must not outlive
it. Access mode travels with the descriptor: use an appropriate open mode
for what the receiver must do.

Garbage collection closes a forgotten fd as a fallback and warns
`fd closed by garbage collector — call close()`. A process-wide fd-pressure
threshold can request collection, but it does not make cleanup timely.
Close explicitly on success, errors and cancellation.

Do not store a wrapper directly in `ListModel`: it becomes a plain map
without the descriptor or methods. Keep it in a `property var`, JS array
or keyed map instead. See [lifetime and memory](lifetime-and-memory.md).

## Migrate from 1.0 plain integers

| 1.0 code | 1.1.0 code |
| :--- | :--- |
| Send a number with signature `h` | Send a `unixFd` from `openFd` or `new DBusQML.unixFd(n)` |
| Check a received fd against `-1` | Check `fd.valid` |
| `DBusUtils.closeFd(receivedFd)` | `receivedFd.close()` |
| `readFd` / `writeFd` / `fdUrl` with a number | Pass the owning value |
| Give a received number to a native consumer | Use `take()`, then close that duplicate according to the consumer's ownership contract |

Reference: [Unix file descriptors](../../API.md#unix-file-descriptors-h).
