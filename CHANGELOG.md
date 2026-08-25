# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.0] — 2026-08-25

### Added

- **Generic signature-walking marshaller (`writeBySignature`)** — the write
  side is now signature-general, mirroring the signature-general read side
  (`readBySignature`). Any producible D-Bus signature — nested maps, arrays
  of dicts (`aa{sv}`), arbitrary structs — is built from public QtDBus
  primitives.
- **Declared reply signatures on `DBusAdaptor`** — method replies marshal
  against the served interface's declared out-signature. Precedence:
  explicit `_signatures` override → bundled/user type catalog → stable
  inference. Serves `org.freedesktop.impl.portal.Settings` correctly
  (`ReadAll` → `a{sa{sv}}`, the shape xdg-desktop-portal requires).
- **`_signatures` override property** on both `DBus` (call-argument
  signatures) and `DBusAdaptor` (reply signatures). The underscore prefix
  keeps it outside the mirrored/served D-Bus namespace.
- **Underscore-private adaptor properties** — `_`-prefixed properties are
  never exported via `generateXml()` or `Properties.Get/GetAll/Set`, so
  consumers get private helper properties for free.
- **`aa{sv}` support** — arrays of dicts (NM `Ip4Config.AddressData` shape)
  marshal from plain JS arrays.
- **Bundled `org.freedesktop.impl.portal.Settings` type** — the backend
  interface catalog, so Settings backends serve correct reply signatures
  with no per-app override.
- **`v(struct)` limitation lifted** — a hand-built struct payload now
  marshals inside a variant, as a map/list value, and as a signal or call
  arg (spike-verified on Qt 6.11). The outermost struct carries the wire
  signature; inner structs compose naturally.
- **Deep map nesting** — `a{sa{sa{sv}}}` and arbitrary map depth work.

### Fixed

- **Declared signatures are no longer silently ignored** — an unproducible
  declared signature emits
  `qWarning("dbusqml: cannot produce declared signature …")` and falls back
  to inference, rather than emitting a different wire type with no notice.
- **Struct gadgets marshal via a writable `QDBusArgument`** — a
  `DBusQML.struct_` value previously emitted an empty struct `()` and
  dropped the bus connection when nested in a variant/map/signal; it now
  cross-marshals correctly in every position.
- **Unmarshalable payloads degrade gracefully** — a reply value that cannot
  be marshaled (e.g. a returned JS function) now produces a
  `org.freedesktop.DBus.Error.Failed` error reply and a warning, instead of
  silently dropping the bus connection. Unmarshalable signal args are warned
  and skipped.

## [0.3.1] — 2026-08-12

### Fixed

- **Adaptor case-folded method dispatch** — D-Bus PascalCase members
  (`ReadOne`) now dispatch to QML camelCase methods (`readOne`). QML
  forbids uppercase-initial method names; previously the call got
  `UnknownMethod`. Exact match tried first for C++ Q_INVOKABLEs.
- **Adaptor reply marshaling** — method replies now run through
  `toDbusVariant()`, so `DBusQML.variant(x)` returns marshal as real
  D-Bus variants. Previously the caller timed out. Void returns now
  send an empty reply instead of an invalid-Variant error.
- **Adaptor signal marshaling** — `emitSignal()` arguments now run
  through `toDbusVariant()` via `qjsValueToVariant()`. Portal
  `SettingChanged` emits `(ssv)` instead of `(ssi)` — xdg-desktop-portal
  no longer drops the signal.
- **Adaptor introspection XML** — `destroyed`/`objectNameChanged` no
  longer leak into the served XML. Signal args typed from
  `parameterTypes()` instead of hardcoded `v`.

### Added

- **`DBus::Struct` value type** (`struct_` in QML) — wraps a
  `QVariantList` and marshals via `beginStructure`/`endStructure`.
  Enables struct-typed D-Bus values like `(ddd)` accent-color and
  `(uu)` StateReason. In method replies and signal args the struct is
  emitted through a writable `QDBusArgument` (QtDBus can't register a
  fixed signature for a variable-member struct).
- **Nested gadget unwrap in plain maps** — `toDbusVariant` recurses
  into `QVariantMap` values, so QML object literals returned from
  adaptor methods (`{ ns: { key: new DBusQML.variant(1) } }`) marshal
  as proper `a{sa{sv}}` with variant payloads.

## [0.3.0] — 2026-08-12

### Breaking

- **Reactive bindings always on** — `DBUSQML_REACTIVE_BINDINGS` CMake
  option removed. Catalog/introspection pre-population of `null`
  placeholders is unconditional. Properties known from catalog or
  introspection read as `null` (not `undefined`) before the real D-Bus
  value arrives. `reactiveBindingsSupported` is hardwired `true`.
  `qt6-dbusqml-reactive` AUR package is obsolete — use `qt6-dbusqml`.
- **`ay` reads as `ArrayBuffer`** — byte arrays consistently arrive as
  JS `ArrayBuffer` (was inconsistent: string or array-like depending on
  the code path). Use `DBusUtils.textFromBytes(buf)` for UTF-8 text
  (e.g. SSIDs), or index via `new Uint8Array(buf)`.

### Added

- **Signature-driven argument marshaller** — method calls marshal JS
  values against the introspected/catalog arg signature. `a{sa{sv}}`
  (NM connection settings), `ay` (byte arrays from strings or number
  arrays), and nested containers work with plain JS objects — no wrapper
  types needed when the signature is known.
- **`DBusMessage.signature`** — optional per-arg signature for the
  low-level `asyncCall` path (e.g. `signature: "sa{sa{sv}}ay"`).
- **`DBusUtils` QML singleton** — `textFromBytes(buf)` /
  `bytesFromText(str)` for UTF-8 conversion of `ay` payloads.
- **`DBus.bytes` value type** — explicit `ay` override for
  signature-less calls.
- **GetAll fallback** — when `Introspect` fails or returns empty XML,
  the proxy falls back to `Properties.GetAll(iface)`. Reaches `Ready`
  with populated reactive properties. Covers NM's `Ip4Config`,
  `Connection.Active`, `AccessPoint` objects.
- **NM sub-interface catalog XMLs** — `Device`, `Device.Wired`,
  `Device.Wireless`, `Connection.Active`, `IP4Config`, `IP6Config`,
  `AccessPoint`, `Settings`, `Settings.Connection`. Synchronous
  placeholder pre-population and typed signatures for NM sub-objects.

### Fixed

- **`aa{...}` demarshaling** — arrays of dicts (NM `Ip4Config.AddressData`)
  no longer warn and return empty strings. Per-entry signature reads
  avoid the `operator>>(QDBusArgument, QVariant&)` libdbus crash.
- **Nested `DBus.dict` marshaling** — `toDbusVariant` recurses through
  `Dict` values and `Variant` payloads, fixing "type 'DBus::Dict' is not
  registered" marshaller crashes.
- **Signature-complete demarshaller** — `readBySignature` handles all
  nested containers recursively. `operator>>(QDBusArgument, QVariant&)`
  eliminated from all nested paths — the libdbus crash vector is
  eliminated structurally.

## [0.2.5] — 2026-08-10

### Fixed

- **Adaptor D-Bus Properties compliance** — `Get` wraps reply in
  `QDBusVariant` (spec requires `v`), `Set` unwraps value and sends empty
  reply (was missing — clients hung ~25s), wrong-iface and unknown-property
  calls return `InvalidArgs` errors. `GetAll` excludes internal properties.
- **Adaptor method arg unwrapping** — complex D-Bus args (`a{sv}`, nested
  structs) now arrive as traversable JS objects, not opaque QDBusArgument.
- **Adaptor introspection XML accuracy** — property types correct
  (`UInt` → `u`, `QVariantList` → `av`), every method parameter gets an
  `<arg direction="in">`, return types mapped.
- **Introspection parser robustness** — no infinite loop on truncated XML,
  signal args don't pollute method arg lists, only `direction="in"` args
  collected.
- **Property writes** — `updateValue` maps camelCase QML names back to
  D-Bus PascalCase names and wraps values in `QDBusVariant`. Writes via
  `proxy.someProp = value` now actually reach the service.
- **Reply/watcher leaks** — watchers auto-deleted on finished, replies
  handed to JS GC after `finished` is delivered. No unbounded growth.
- **Signal hook cleanup** — exact disconnect matches connect args;
  stale hooks no longer fire after iface/connection changes.
- **Introspection coalescing** — one call per config change instead of
  up to 4 (one per setter + componentComplete).
- **`propertiesEnabled: false`** — proxy now reaches Ready and emits
  `introspectionCompleted` instead of staying at Loading forever.
- **Custom bus connection safety** — `QPointer<DBusConnection>` prevents
  dangling references when JS drops the connection.
- **Catalog precedence** — user data dirs now correctly override system
  dirs (was reversed).
- **Method name injection** — dynamic methods installed via shared JS
  factory, not string-interpolated evaluate.

### Changed

- `DBusPendingReply::value`/`values` return `QJSValue` with real JS
  `Array`/`Object` instances (working `Array.isArray`, `.map`, `.filter`).
  C++ consumers use `valueVariant()`/`valuesVariant()` for raw data.

## [0.2.4] — 2026-08-10

### Fixed

- **Nested containers as real JS Arrays** — `DBusPendingReply`'s
  `value`/`values` Q_PROPERTYs now return `QJSValue` with nested
  containers converted via `variantToJs()`. QML receives real JS
  `Array`/`Object` instances with working `Array.isArray`, `.map`,
  `.filter`, etc. C++ consumers use new `valueVariant()`/`valuesVariant()`
  accessors for raw QVariant data.
- **Struct array grouping** — per-struct member lists wrapped with
  `QVariant::fromValue()` to prevent `QList::append` concatenation.
- **Late-subscriber race on `finished`** — emitted via
  `Qt::QueuedConnection` so synchronous `reply.finished.connect()` after
  `call()` always lands before the signal fires.

## [0.2.3] — 2026-08-09

### Fixed

- **SIGSEGV on struct/tuple D-Bus calls** — `operator>>(QDBusArgument,
  QVariant)` crashes inside libdbus when reading struct members. Replaced
  with `currentSignature()` dispatch to the correct C++ `operator>>`
  overload per member type. Complex types use `asVariant()` + recursion.
  Fixes fcitx5 `AvailableInputMethods` (`a(ssssssb)`) and
  `CurrentInputMethodInfo` (`sssssssbsa{sv}`) crashes.

## [0.2.2] — 2026-08-09

### Fixed

- **SIGSEGV at client teardown** — `QPointer<QDBusPendingCallWatcher>`
  auto-nulls when the watcher is deleted, preventing use-after-free
  when the parent proxy is destroyed while a pending call is in flight.
- **Flaky `finished` delivery** — watcher stays alive until the parent
  destroys it, no premature deletion.
- **Generic array demarshaling** — all basic D-Bus array types
  (`ay`, `ab`, `an`, `aq`, `ai`, `au`, `ax`, `at`, `ad`, `as`, `ao`,
  `ag`, `av`) now handled via template dispatch. Removes
  "unsupported signature au" warnings from NetworkManager properties.

## [0.2.1] — 2026-08-09

### Fixed

- **Recursive `unwrapDbus`** for struct arrays (`a(...)`), mixed tuples
  (`(...)`), and generic dicts (`a{...}`). Covers fcitx5's `a(ssssssb)`
  and `sssssssbsa{sv}` patterns. Replaces silent fallthrough with
  `qWarning()` naming the unsupported signature.
- **Watcher cleanup** — `QDBusPendingCallWatcher` auto-deleted on
  `finished()` via `deleteLater()`. Eliminates per-call memory leaks.
- **Reply self-containment** — `DBusPendingReply` caches reply data in
  `onFinished()` and clears the watcher pointer, preventing SIGSEGV when
  QML accesses the reply after the watcher is deleted.

## [0.2.0] — 2026-07-25

### Added

- `DBUSQML_REACTIVE_BINDINGS` CMake option (default `OFF`). When enabled,
  DBusProxy pre-populates `null` placeholders for catalog-declared
  properties BEFORE QML bindings evaluate, so `QQmlPropertyMap`'s
  built-in reactivity handles subsequent D-Bus value updates. Fixes the
  long-standing bug where intermediate `readonly property` layers
  wrapping DBusProxy properties resolved to `false` forever.
- `reactiveBindingsSupported` QML property on every `DBus` element
  (`bool`, read-only, constant). Returns `true` when the build includes
  the reactive-bindings fix.
- `types/org.freedesktop.NetworkManager.xml` catalog descriptor with 26
  property declarations.

### Changed

- `DBusCatalog::InterfaceSpec` gains `properties` (`QStringList`) field.
  Catalog XML parser now extracts `<property name="..."/>` elements.
- `dbusqmlConfig.cmake.in` exposes `dbusqml_REACTIVE_BINDINGS` variable
  to downstream CMake consumers.

## [0.1.0] — 2026-07-18

Initial release. Requires Qt 6.8 or newer. See
[`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) for two upstream Qt bugs users
on older Qt should be aware of.

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
