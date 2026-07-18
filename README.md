# dbusqml

[![CI](https://github.com/alexindigo/dbusqml/actions/workflows/ci.yml/badge.svg)](https://github.com/alexindigo/dbusqml/actions/workflows/ci.yml)

**D-Bus in QML. No KDE.** Standalone Qt 6 QML plugin — `Qt6::DBus` is the only dependency.

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
- **Auto-updating properties** — bound to `PropertiesChanged`, no wiring needed.
- **Async replies** — every call returns a `DBusPendingReply` with `finished`,
  `isError`, `error`, `value`.
- **`DBusAdaptor`** — export QML objects onto the bus with methods, properties,
  and signals declared inline.
- **User-land type catalog** — drop XML descriptors into
  `$XDG_CONFIG_HOME/dbusqml/types/` for services that don't publish
  introspection (Chromium-based MPRIS players, for example). See [`docs/TYPES.md`](docs/TYPES.md).
- **Nested container unmarshaling** — `a{sv}` / `a{ss}` / arrays arrive as
  `QVariantMap` / `QVariantList` you can traverse directly in JS.
- **`SessionBus` and `SystemBus`** singletons; `connectToBus(address)` for
  peer/custom connections.
- **Value types** — `dbusVariant`, `dbusMessage`, `dbusError`, etc., via
  `import DBus 1.0 as DBusQML`.

## Build & install

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
| intermediate  | BatteryMonitor · Notify · ServiceMonitor · Caffeine · PortalDemo |
| advanced      | MprisPlayer · KDEConnect · PowerControl · SystemdManager         |

```sh
scripts/run-examples MprisPlayer   # or a category: `advanced`
```

## Docs

- [`API.md`](API.md) — full API reference
- [`DESIGN.md`](DESIGN.md) — architecture and rationale
- [`docs/TYPES.md`](docs/TYPES.md) — user-land type catalog
- [`FutureDevelopment.md`](FutureDevelopment.md) — roadmap

## License

GPL-3.0 — see [`LICENSE`](LICENSE).
