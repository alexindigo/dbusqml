# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] — 2026-07-18

Initial release.

### Added

- `DBus` proxy element with dynamic method dispatch. D-Bus methods
  discovered via `Introspect()` are exposed on the element under
  camelCased names and forward through a per-proxy helper.
- Automatic property discovery via `Properties.GetAll` and
  `PropertiesChanged` subscription. Nested `a{sv}` / `a{ss}` containers
  arrive at QML as `QVariantMap` / `QVariantList` (not opaque
  `QDBusArgument`).
- `DBusAdaptor` (server-side): expose a QML component as a D-Bus object.
  Method dispatch goes through `QJSValue::callWithInstance` so container
  arguments round-trip natively.
- User-land type catalog. Drop XML descriptors into
  `$XDG_CONFIG_HOME/dbusqml/types/` (or use bundled defaults for MPRIS,
  Notifications, ScreenSaver, login1.Manager, portal.Settings,
  portal.NetworkMonitor, UPower) so proxies can call methods on services
  that return empty `Introspect()` (e.g., Chromium-based MPRIS players).
  Documented in [`docs/TYPES.md`](docs/TYPES.md).
- Promise-style `asyncCall(message, resolve, reject)`. `resolve` receives
  the reply value as a native JS value (`Array.isArray` returns `true` for
  array replies; objects for `a{sv}`); `reject` receives a single
  `{ name, message }` error object.
- `SessionBus` and `SystemBus` singletons; `connectToBus(address)` for
  peer / custom connections. Each call produces a distinct QtDBus
  connection name — no more silent handle reuse.
- Value types (`import DBus 1.0 as DBusQML`): `uint32`, `int32`, `uint64`,
  `int64`, `uint16`, `int16`, `bool`, `double`, `byte`, `string`,
  `objectPath`, `signature`, `dict`, `variant`.
- 13 runnable examples (`simple/*`, `intermediate/*`, `advanced/*`) with a
  shared `CloseButton` and click-to-copy error messages.
- CMake package config: downstream projects can now
  `find_package(dbusqml 0.1 REQUIRED)` and link to
  `dbusqml::dbusqml`.
- GitHub Actions CI: matrix build on Qt 6.5.3 and 6.8.2, running C++
  tests and QML tests on every push and PR.

### Fixed

- `unwrapDbus` no longer crashes on concrete-type D-Bus arrays. `ao`
  (object-path arrays returned by UPower `EnumerateDevices`, logind
  `ListSessions`, etc.) previously segfaulted inside libdbus. Common
  array signatures (`a{s*}`, `av`, `ao`, `as`, `ay`) are demarshaled
  through their proper C++ target types and flattened for JS.
- Re-introspection no longer leaks stale method callables. Switching a
  proxy's `iface` at runtime removes the previous iface's method keys
  from the property map, clears cached `QJSValue`s, and drops the old
  helper QObject from the engine global.
- `BatteryMonitor` example delegate widths are bound to the enclosing
  `ListView` explicitly instead of `parent.width`, avoiding the Qt 6
  ListView-delegate `parent`-during-creation footgun.
- `DBusAdaptor::handleMessage` no longer builds a JS source string to
  dispatch to QML methods (container arguments would arrive stringified,
  the global name collided across instances).

### Docs

- `README.md` with tagline, feature bullets, quick-start snippet, and
  build instructions.
- `API.md` full API reference, including a "Known Limitations" section
  covering the C++-context signal-handler pitfall and the
  runtime-configured proxy async pattern.
- `DESIGN.md` architecture rewrite: describes the actual dispatch
  (JS closures in `QQmlPropertyMap`, per-proxy `DbusMethodHelper`,
  catalog fallback) and the trade-offs behind the design.
- `docs/TYPES.md` documenting the user-land catalog.
- `FutureDevelopment.md` (replaces legacy `DBUS_w_QML.md` scratch notes).
