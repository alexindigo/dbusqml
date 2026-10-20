# dbusqml

[![CI](https://github.com/alexindigo/dbusqml/actions/workflows/ci.yml/badge.svg)](https://github.com/alexindigo/dbusqml/actions/workflows/ci.yml)

**D-Bus in QML. No KDE.** Standalone Qt 6 QML plugin using Qt Core, Qml
and DBus. Requires **Qt 6.8** or newer.

The `DBus {}` element introspects a remote object and exposes its methods and
properties as native QML — call methods directly, bind to properties, get
`PropertiesChanged` updates for free.

```qml
import QtQuick
import QtQuick.Controls
import DBus 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 360
    height: 180
    property var endpoint: notifications

    DBus {
        id: notifications
        service: "org.freedesktop.Notifications"
        path: "/org/freedesktop/Notifications"
        iface: "org.freedesktop.Notifications"
        connection: SessionBus
        callTimeout: 5000
    }

    Button {
        anchors.centerIn: parent
        text: "Notify"
        enabled: notifications.status === DBus.Ready
        onClicked: {
            // D-Bus Notify(...) becomes the dynamic QML method notify(...).
            const reply = root.endpoint.notify(
                "dbusqml", 0, "", "Hello from QML",
                "Sent over the session bus.", [], {}, 5000
            )
            reply.finished.connect(function() {
                if (reply.isError)
                    console.error(reply.error.name, reply.error.message)
                else
                    console.log("Notification ID:", reply.value)
            })
        }
    }
}
```

Portal backends whose handlers spawn per-call `Request` adaptors capture
the subtree so a Close racing attach is delivered in order:

```qml
import DBus 1.0

DBusAdaptor {
    service: "org.freedesktop.impl.portal.MyShell"
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.FileChooser"
    captureSubtree: true   // on EVERY co-located adaptor at this path
}
```

## Features

- **`DBus {}` proxy element** — methods and properties appear on the object as
  soon as introspection completes; PascalCase D-Bus names become camelCase QML.
- **Auto-updating properties** — bound to `PropertiesChanged` (interface-arg
   filtered; `invalidated_properties` re-fetched), no wiring needed.
  Rejected optimistic writes emit `propertyWriteFailed` and re-fetch the
  server's value instead of inventing a rollback.
- **Owner-bound proxy signals** — interface signals and `PropertiesChanged`
  are accepted only from the configured service's current owner.
  `DBusSignalWatcher` provides explicit any-sender monitoring instead.
- **Async replies** — every call returns a `DBusPendingReply` with `finished`,
   `isError`, `error`, `value`. Completion is one-shot; its handlers are
  disconnected after delivery and completed QML replies become collectable.
  Fire-and-forget `send(method, args)` plus
  `callTimeout`; `DBusConnection.send(message)` for message objects.
- **`DBusAdaptor`** — export QML objects onto the bus with methods, properties,
  and signals declared inline. PascalCase D-Bus members dispatch to camelCase
  QML functions; variant/struct return types marshal correctly (portal-grade
  serving — e.g. `org.freedesktop.impl.portal.Settings`). Set
  `captureSubtree: true` on a prefix whose handlers create child adaptors
  that callers may address immediately (portal `Request` objects).
- **Deferred replies** — `holdReply()` defers the reply; the returned
  `DBusHeldReply` settles later with `send(value)` / `sendError(name, msg)`.
  A throw after `holdReply()` settles the held reply with the error (never
  silent, never a double reply — sole exception: thrown `undefined`, see
  Known issues). Per-call `Request` adaptors + `unregister()` cover the
  portal cancel lifecycle. Portal backends: `captureSubtree: true` on every
  co-located adaptor at `/org/freedesktop/portal/desktop`.
- **Co-location** — multiple adaptors share one path/service; `_signals`
   declares signal arg types, `_members` aliases wire↔QML names, catalog XML
   fills in services with poor introspection (bundled descriptors,
   user/system drop-ins and the freedesktop interface registry).
- **Served property access** — catalog access and QML/C++ writability drive
  both introspection and enforcement. Writes to an effectively read-only
   property return `PropertyReadOnly` without changing its value.
- **Adaptor policies** — `_options` filters and validates a method's final
  options map; `allowedSender` can restrict calls to one unique sender.
  `heldReplyTimeout` optionally bounds deferred replies.
- **Service identity** — `DBusConnection` naming, well-known-name acquisition
  (`allowReplacement` / `replaceExisting` / `queueOnBusy`), `nameAcquired` /
   `nameLost`, `DBusServiceWatcher`, `DBusObjectManager`.
- **Connection-loss reporting** — connections expose `connected` and
  `disconnected()`; proxies and claims report loss. There is no automatic
  reconnection.
- **Call options** — `DBusConnection.asyncCall(message)` (+ promise-style
   resolve/reject callbacks), `DBusMessage` structured values and per-message
   input signatures; proxy input/adaptor reply `_signatures` overrides;
   `reloadTypes()` for the catalog.
- **Reactive property bindings** — catalog/introspection pre-population makes
  intermediate `readonly property` layers reactive out of the box (0.3.0+).
- **Signature-driven marshaling** — method args marshal against the
  introspected signature; `a{sa{sv}}`, `ay`, and nested containers work with
  plain JS objects (0.3.0+). Declared container elements are producible via
  the signature-slot pool (0.9.1+); recursion depth capped at 32.
- **User-land type catalog** — drop XML descriptors into
  `$XDG_CONFIG_HOME/dbusqml/types/` for services that don't publish
  introspection (Chromium-based MPRIS players, for example). See [`docs/TYPES.md`](docs/TYPES.md).
- **Nested container unmarshaling** — `a{sv}` / `aa{sv}` / `a{sa{sv}}` /
  struct arrays arrive as real JS `Array` / `Object` instances you can
  traverse directly.
- **`DBusUtils`** — `textFromBytes()` / `bytesFromText()` for `ay` payloads
   (0.3.0+), `error(name, message)` for named error throws, and fd helpers
   (`openFd`/`readFd`/`writeFd`/`fdUrl`; see API.md).
- **Owning file descriptors** — sending an `h` requires `DBusQML.unixFd`,
  and received fds arrive as owning values. Call `close()` when done;
  `take()` supplies a caller-owned duplicate number, cleaned up with
  `DBusUtils.closeFd`. The 1.1.0 migration
  and Qt construction/pressure-valve limits are documented in
  [the fd reference](API.md#unix-file-descriptors-h).
- **`SessionBus` and `SystemBus`** singletons; `connectToBus(address)` for
  peer/custom connections.
- **Value types** — `dbusMessage`, `dbusError`, `bytes`, `unixFd`,
  `struct_`, `objectPath`, `signature`, `int16`…`uint64`, `boolean`, `dict`,
  `variant`, etc., via `import DBus 1.0 as DBusQML`. 64-bit integers keep
  full precision on the JS path.
- **Task guides and runnable examples** — [complete QML recipes](docs/guides/README.md)
  cover calls, properties/signals, adaptors, fds, reply lifetime and errors.

## Install

### Arch Linux (AUR)

```bash
yay -S qt6-dbusqml           # release
yay -S qt6-dbusqml-git       # git master
```

If AUR is unavailable, install directly from the GitHub mirrors:

```bash
git clone https://github.com/alexindigo/aur-qt6-dbusqml.git
cd aur-qt6-dbusqml && makepkg -si
```

### Build from source

Reactive property bindings (through intermediate `readonly property`
layers, e.g. `readonly property bool wifiEnabled: nm.wirelessEnabled === true`)
are always enabled — there is no build flag.

```sh
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
cmake --install build-release   # installs the QML plugin + bundled type XMLs
```

Or use the helper scripts:

```sh
scripts/build            # release build
BUILD_DIR=build-release scripts/install # install the release build
BUILD_DIR=build-release scripts/run-examples # run every example sequentially
scripts/run-tests        # C++ and QML tests
```

## Examples

Runnable examples cover the common patterns:

| Tier          | Examples                                                         |
|---------------|------------------------------------------------------------------|
| simple        | ListNames · KeyboardLayout · NetworkMonitor · PortalSettings     |
| intermediate  | BatteryMonitor · Notify · ServiceMonitor · Caffeine · PortalDemo · SleepInhibitor · OpenFileWithPortal |
| advanced      | MprisPlayer · KDEConnect · PowerControl · SystemdManager         |

```sh
BUILD_DIR=build-release scripts/run-examples MprisPlayer # or a category: advanced
```

## Documentation

- [`docs/guides/`](docs/guides/README.md) — task-oriented guides: first calls, properties/signals, serving interfaces, file descriptors and lifetime
- [`API.md`](API.md) — full API reference
- [`DESIGN.md`](DESIGN.md) — architecture and rationale
- [`docs/TYPES.md`](docs/TYPES.md) — user-land type catalog
- [`docs/PARITY.md`](docs/PARITY.md) — design-time checklists for every future mechanism
- [`RELEASING.md`](RELEASING.md) — release gates and ceremony
- [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) — upstream Qt bugs and workarounds

## Known issues

Thrown `undefined` after `holdReply()` is undetectable (a thrown
`undefined` is identical to the normal void return at the dispatch
layer), so it completes silently with no reply — the caller times out.
Fence with `heldReplyTimeout`, throw a named error or a primitive
instead. Full entry in [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).

## License

GPL-3.0 — see [`LICENSE`](LICENSE).
