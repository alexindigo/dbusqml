# dbusqml

[![CI](https://github.com/alexindigo/dbusqml/actions/workflows/ci.yml/badge.svg)](https://github.com/alexindigo/dbusqml/actions/workflows/ci.yml)

**D-Bus in QML. No KDE.** Standalone Qt 6 QML plugin — `Qt6::DBus` is the only dependency. Requires **Qt 6.8** or newer.

The `DBus {}` element introspects a remote object and exposes its methods and
properties as native QML — call methods directly, bind to properties, get
`PropertiesChanged` updates for free.

```qml
import DBus 1.0

DBus {
    id: notifications
    service: "org.freedesktop.Notifications"
    path:    "/org/freedesktop/Notifications"
    iface:   "org.freedesktop.Notifications"
    connection: SessionBus
}

Button {
    text: "Notify"
    onClicked: {
        // D-Bus `Notify(...)` → QML `notify(...)`, camelCased automatically.
        var reply = notifications.notify(
            "dbusqml", 0, "",
            "Hello from QML", "Sent over the session bus.",
            [], ({}), 5000
        )
        reply.finished.connect(() => {
            if (reply.isError) console.log(reply.error.message)
            else               console.log("id: " + reply.value)
        })
    }
}
```

## Features

- **`DBus {}` proxy element** — methods and properties appear on the object as
  soon as introspection completes; PascalCase D-Bus names become camelCase QML.
- **Auto-updating properties** — bound to `PropertiesChanged` (interface-arg
  filtered; `invalidated_properties` re-fetched), no wiring needed.
- **Async replies** — every call returns a `DBusPendingReply` with `finished`,
  `isError`, `error`, `value`. Fire-and-forget `send(method, args)` plus
  `callTimeout`; `DBusConnection.send(message)` for message objects.
- **`DBusAdaptor`** — export QML objects onto the bus with methods, properties,
  and signals declared inline. PascalCase D-Bus members dispatch to camelCase
  QML functions; variant/struct return types marshal correctly (portal-grade
  serving — e.g. `org.freedesktop.impl.portal.Settings`).
- **Deferred replies** — `holdReply()` defers the reply; the returned
  `DBusHeldReply` settles later with `send(value)` / `sendError(name, msg)`.
  A throw after `holdReply()` settles the held reply with the error (never
  silent, never a double reply). Per-call `Request` adaptors + `unregister()`
  cover the portal cancel lifecycle.
- **Co-location** — multiple adaptors share one path/service; `_signals`
  declares signal arg types, `_members` aliases wire↔QML names, catalog XML
  fills in services with poor introspection (both scan paths: bundled +
  user drop-ins).
- **Service identity** — `DBusConnection` naming, well-known-name acquisition
  (`allowReplacement` / `replaceExisting` / `queueOnBusy`), `nameAcquired` /
  `nameLost`, `DBusServiceWatcher`, `DBusObjectManager`.
- **Call options** — `DBusConnection.asyncCall(message)` (+ promise-style
  resolve/reject), `DBusMessage` structured values, per-call `_signatures`
  overrides, `reloadTypes()` for the catalog.
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
  (0.3.0+); fd quartet + `fdUrl` (see above).
- **`SessionBus` and `SystemBus`** singletons; `connectToBus(address)` for
  peer/custom connections.
- **Value types** — `dbusVariant`, `dbusMessage`, `dbusError`, `bytes`,
  `struct_`, `objectPath`, `signature`, `int16`…`uint64`, `boolean`, `dict`,
  `variant`, etc., via `import DBus 1.0 as DBusQML`. 64-bit integers keep
  full precision on the JS path.
- **`DBusUtils`** — `textFromBytes()` / `bytesFromText()` for `ay` payloads
  (0.3.0+); `DBusUtils.error(name, message)` for named error throws.

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
scripts/install          # install into Qt's QML path
scripts/run-examples     # pick and run any bundled example
scripts/run-tests        # C++ and QML tests
```

## Examples

Thirteen runnable examples covering the common patterns:

| Tier          | Examples                                                         |
|---------------|------------------------------------------------------------------|
| simple        | ListNames · KeyboardLayout · NetworkMonitor · PortalSettings     |
| intermediate  | BatteryMonitor · Notify · ServiceMonitor · Caffeine (inhibit) · PortalDemo |
| advanced      | MprisPlayer · KDEConnect · PowerControl · SystemdManager         |

```sh
scripts/run-examples MprisPlayer   # or a category: `advanced`
```

## Docs

- [`API.md`](API.md) — full API reference
- [`DESIGN.md`](DESIGN.md) — architecture and rationale
- [`docs/TYPES.md`](docs/TYPES.md) — user-land type catalog
- [`docs/PARITY.md`](docs/PARITY.md) — design-time checklists for every future mechanism
- [`RELEASING.md`](RELEASING.md) — release gates and ceremony
- [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) — upstream Qt bugs and workarounds

## License

GPL-3.0 — see [`LICENSE`](LICENSE).
