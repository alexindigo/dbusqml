# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.9.0] — 2026-09-02

### Added

- **fd quartet + `fdUrl` on `DBusUtils`.** QML had no fd I/O — the `ay`
  codec gap again, this time for file descriptors. `openFd`/`writeFd`/
  `readFd`/`closeFd` make a received `h` usable from QML (receiver-closes
  is now dischargeable via `closeFd`), and `fdUrl` maps a regular-file fd
  to `file:///proc/self/fd/N` for path-based consumers. Documented caveats:
  `fdUrl` is regular-file-only (streams use `readFd`/`writeFd`); the URL is
  valid only while the fd stays open.

### Changed

- **Truthful served surface — the naming ladder.** Served introspection XML,
  `GetAll` keys, `PropertiesChanged` names, and method in-arg types now
  resolve through explicit (`_signals`, `_members`) → declared (catalog) →
  stable inference (the deterministic first-character fold,
  `readOne` ⇄ `ReadOne`). **Wire change:** undeclared QML members are
  advertised wire-cased on the bus (`ReadOne`, not `readOne`), and
  QML-declared plain signals broadcast under their folded wire names
  (`SomethingHappened`, not `somethingHappened`). Subscribers matching the
  old lowercase names must update their match rules.
- **`PropertiesChanged` replaces the accidental notify-signal relays.**
  Property changes now emit the standard
  `org.freedesktop.DBus.Properties.PropertiesChanged`; the `fooChanged`
  broadcast signals are gone (unmarshalable values report via
  `invalidated_properties`). dbusqml clients (which always subscribed to
  `PropertiesChanged`) now see dbusqml adaptors' property changes —
  reactivity works end-to-end; consumers that polled as a workaround can
  stop.
- **Shadowing a built-in property errors at load.** `service`, `path`,
  `iface`, `connection` are `FINAL` — a QML `property string path` on an
  adaptor is a load-time error instead of a silently broken adaptor.
- **64-bit values above 2^53 are delivered as full-precision decimal
  strings.** QML's JS engine has no BigInt (spike-confirmed); a qint64/
  quint64 that doesn't round-trip through a double would silently lose
  precision as a JS number. Values within 2^53 stay plain numbers. The
  send path accepts decimal strings with declared `x`/`t`, making the
  round-trip lossless end-to-end.

### Added

- **`_signals` / `_members`** — explicit wire-name declarations:
  `_signals` maps wire signal names to concatenated arg signatures (served
  in introspection; `emitSignal` remains the send path); `_members` aliases
  wire member names to QML names, making reserved-word (`Delete`) and
  collision (`Unregister`) members servable.
- **Catalog-served signals and types** — the freedesktop-standard
  `<data>/dbus-1/interfaces/` directory is scanned at lowest precedence;
  bundled `org.freedesktop.impl.portal.Inhibit.xml`; user-supplied XML
  (`~/.config/dbusqml/types/`, `DBUSQML_TYPES_PATH`) documented as the
  first-class custom-interface path, highest precedence. Malformed XML
  files in scanned directories are discarded whole.
- **Call options** — per-call `message.timeout` (ms, `-1` = Qt default),
  `interactiveAuthorization` and `autoStart` message flags; the proxy's
  `callTimeout` governs all proxy traffic including the internal
  Introspect/GetAll startup calls; `send(method, args)` and
  `DBusConnection.send(message)` for fire-and-forget.
- **Named error replies from handlers** — throwing
  `DBusQML.DBusUtils.error(name, message)` produces that exact error reply;
  other thrown values produce `org.freedesktop.DBus.Error.Failed` with the
  exception message (previously the JS error silently re-ran the handler
  and replied EMPTY).
- **Service-name acquisition** — `allowReplacement`, `replaceExisting`,
  `queueOnBusy` (RequestName flags; all default false — previous behavior
  preserved) with `nameAcquired`/`nameLost` signals driven by daemon owner
  changes.
- **Standalone watcher elements** — `DBusSignalWatcher` (any-bus signal
  subscription with wildcard fields, no proxy/introspection required) and
  `DBusServiceWatcher` (appear/disappear/owner change for one name).
- **`DBusObjectManager`** — ObjectManager client: `GetManagedObjects`
  inventory plus live `InterfacesAdded`/`InterfacesRemoved` signals with
  unwrapped args (BlueZ / KDE-Connect / Valent class).
- **Unix fd (`h`) passing** — fds in both directions as plain integers
  (dbus-next + Nemo shape) with the receiver-closes lifetime; send via a
  plain int fd + declared `h`; container positions (`ah`) covered.

### Fixed

- **Spec-cased property dispatch** — `Properties.Get`/`Set` accept the
  wire-cased property name (`Get("iface", "Version")` finds
  `property int version`), mirroring the method dispatch's exact→folded
  dual lookup.
- **`Set` write guard** — a value that cannot be converted for the property
  replies `InvalidArgs` with the property unchanged, instead of a silent
  empty success.
- **`NO_REPLY_EXPECTED` honored** — no reply (success or error) is
  constructed or sent when the caller didn't ask for one.
- **`signaturesChanged` no longer leaks into served introspection** (nor
  the new `_signalsChanged`/`_membersChanged`).

## [0.8.0] — 2026-08-29

### Changed

- **`destroy()` (and GC) now work everywhere, including inside a dispatched
  handler, and retirement while a reply is pending is allowed.** In 0.7.0
  the adaptor stayed GC-protected while a `holdReply()` reply was
  outstanding — QML `destroy()` was refused until the last held reply
  settled, and a `destroy()` attempted inside the dispatched handler had to
  be deferred (`Qt.callLater`). Both restrictions are gone: the dispatch no
  longer alters the adaptor's ownership (it now merely neutralizes
  `newQObject()`'s JavaScriptOwnership side effect and restores immediately),
  so dynamically created adaptors keep their QML ownership through dispatch.
  Retiring an adaptor while a reply is pending — via `destroy()`, GC, or
  `unregister()` — is consistent: the pending caller is errored
  (`org.freedesktop.DBus.Error.Failed`, "adaptor destroyed with reply
  pending") instead of the retirement being refused. This also closes the
  corner where an adaptor abandoned with a pending reply and zero QML
  references leaked until process exit. Internal: the 0.7.0
  restore-deferral machinery (`maybeRestoreOwnership`, the held-reply
  settle notification) is deleted; GC-rooting of the adaptor during its own
  dispatch is spike-verified (the dispatch's `QJSValue` is a GC root).

## [0.7.0] — 2026-08-28

### Fixed

- **Per-call adaptors are no longer indestructible after their first
  dispatch.** The method-dispatch path flipped the adaptor to
  `CppOwnership` unconditionally and permanently — a dynamically created
  `DBusAdaptor` (the portal Request pattern: one instance per call via
  `Component.createObject` at the caller-chosen handle path) could never be
  `destroy()`ed from QML again ("Invalid attempt to destroy() an
  indestructible object") and leaked one bus registration per call. The
  JS-GC protection is now scoped to the dispatch itself: the pre-dispatch
  ownership is saved and restored afterwards — deferred while any
  `DBusHeldReply` is outstanding (the adaptor must outlive the calls it can
  still answer; the last settle performs the restore). Declarative adaptors
  are unaffected (their ownership was already `CppOwnership`; the flip and
  restore are no-ops there). A lifecycle test axis (L1–L6: destroy, GC,
  held-reply interplay, declarative pin, `unregister()`, leak-count
  regression) now pins this dimension permanently.

### Added

- **`DBusAdaptor.unregister()`** — deterministic retirement of a dynamically
  created adaptor, independent of GC timing: frees the object path and
  releases the service reference immediately, errors out outstanding held
  replies (same code the destructor runs), and leaves the QObject alive for
  QML to drop whenever. One-way: re-registration after `unregister()` is not
  supported; a second call warns and does nothing. Covers the "free the bus
  path NOW, collect the object later" case — e.g. a caller-side `Close` on a
  portal Request object.

## [0.6.0] — 2026-08-28

### Changed

- **The `{value:}` flattening heuristic is removed.** `qjsValueToVariant`
  guessed that any map with a single `value` key was a flattened gadget (and
  an all-doubles payload a struct), silently marshaling a legitimate
  `{value: 42}` dict as `v(i)` and `{value: [0.1, 0.2]}` as `(dd)`. Such
  values now marshal as the real dicts they are (`a{sv}`). **Migration:** if
  you relied on the guess, write the value type explicitly —
  `new DBusQML.variant(x)` / `new DBusQML.struct_([...])` — the documented
  contract since 0.3.x/0.4.0. Gadgets themselves were never affected (they
  survive `QJSValue` conversion intact).

### Fixed

- **Client-side marshal exits are guarded.** `proxy.call`,
  `DBusConnection.asyncCall`, property writes and proxy signal sends validate
  converted arguments: an argument with no wire representation (a `QObject*`
  in a map, a JS function) fails the call **locally** (`DBusPendingReply`
  completes with `org.freedesktop.DBus.Error.Failed`, "argument N is not
  marshalable") instead of aborting the caller inside QtDBus marshaling.
  Property writes and signals warn and drop.
- **Typed C++ method returns round-trip.** The adaptor's C++ dispatch path
  captured `QVariant` returns only — a `QString`/`int`/`QByteArray`-returning
  `Q_INVOKABLE` silently sent an EMPTY reply. Any default-constructible
  return type is now captured and served with its wire type; the >5-argument
  C++ dispatch cap warns by name.
- **Malformed declared signatures can no longer abort the process.**
  `variant(x, "(")` (or any unbalanced/empty container signature reaching the
  signature-driven marshaller) built a `QDBusArgument` that libdbus rejects
  with an assertion abort. Malformed signatures are rejected before building
  and loud-fail to inference; unparseable `_signatures` values warn instead
  of being silently inert.

### Added

- **Adversarial input matrix** — hostile value classes (null, objects,
  functions, NaN/±Infinity, embedded-NUL and 1 MB strings, empty and deeply
  nested containers, cyclic objects/arrays, malformed signatures) swept
  across every marshal exit and pinned as permanent regression tests.

## [0.5.2] — 2026-08-28

### Fixed

- **`Properties.Get`/`GetAll` no longer abort the process on non-marshalable
  property values** (remotely triggerable via routine introspection —
  `busctl introspect` populates property values through `GetAll`). A property
  whose value has no D-Bus wire signature (e.g. a QML `var` holding a
  `QObject*`) now produces an `InvalidArgs` error reply on `Get` and is
  skipped (with a warning) in `GetAll`, instead of killing the hosting
  process inside QtDBus container writing. Gadget-valued properties
  (`DBusQML.variant`/`bytes`/`struct_`) now marshal correctly through the
  library's slot-aware conversion — served as a single-wrapped `v` with the
  gadget's payload rather than skipped — and `generateXml` no longer
  advertises properties it could never serve (`QObject*`-derived).
- **A failed path registration no longer releases a shared service name.** An
  adaptor whose `registerVirtualObject` failed (a foreign object already
  occupied its path) never took the service-name reference, but its destructor
  still ran the detach path and could decrement another adaptor's
  `(connection, service)` claim to zero — silently stealing the bus name from
  a healthy, still-serving adaptor. The destructor now only detaches
  registrations that were actually attached.

## [0.5.1] — 2026-08-27

### Fixed

- **Multiple `DBusAdaptor` instances can now serve the same path.** Two or more
  adaptors that share a `service` and `path` but differ in `iface` (the
  standard multi-interface D-Bus shape, e.g. serving
  `org.freedesktop.impl.portal.Settings` and
  `org.freedesktop.impl.portal.FileChooser` together at
  `/org/freedesktop/portal/desktop`) now register, serve, and introspect
  correctly. Co-located adaptors share path registration through an internal
  dispatcher, and share the service name through a reference count, so
  destroying one adaptor no longer drops the shared path or service name for
  the survivors. Introspection of a shared path merges every attached
  adaptor's interface block.

### Changed (documented retroactively in 0.5.2)

- **Method calls that carry an interface name now route only to an adaptor
  declaring that interface** — the multi-adaptor dispatcher routes
  interface-scoped calls by interface match (and `Properties.Get/GetAll/Set`
  by the interface argument), whereas interface-less calls keep the
  member-name dispatch across adaptors. On 0.5.0 a single adaptor served
  members of *several* interfaces by ignoring the message interface; that
  workaround must be split into co-located adaptors (the pattern 0.5.1
  enables), or its foreign-interface members become unreachable when called
  with the interface name set.

## [0.5.0] — 2026-08-25

### Added

- **Deferred method replies (`holdReply()` + `DBusHeldReply`)** — a
  `DBusAdaptor` handler can hold the D-Bus call open and answer later with
  `reply.send(value)` / `reply.sendError(name, message)`. Explicit and
  opt-in: the handler calls `holdReply()` synchronously during dispatch
  (Qt's `QDBusContext::setDelayedReply` precedent). Deferred replies route
  through the same reply tail as synchronous replies, so declared
  out-signatures, error degradation, and multi-out splitting come for free.
- **Bundled `org.freedesktop.impl.portal.FileChooser` type** — the backend
  interface catalog (`OpenFile`/`SaveFile`/`SaveFiles`, each
  `(o s s s a{sv}) → (u a{sv})`), so FileChooser backends serve correct
  multi-out reply signatures with no per-app override.
- **Bundled `org.freedesktop.impl.portal.Request` type** — the shared
  `Close()` request interface.
- **Typed variant payloads (`DBusQML.variant(value, signature)`)** — the
  variant value type gains an optional payload-signature argument, so a
  variant's payload can be declared (`new DBusQML.variant(paths, "as")` →
  `v(as)`). Routed through the existing `marshalBySignature` engine and
  generic (subsumes `as`, `ay`, `au`, `(ddd)`, `aa{sv}`, anything the engine
  produces). Unproducible signatures warn and fall back to inference — never
  a silent wrong type. Closes the variant-payload hole that made a plain QML
  `uris` array marshal as `av` and silently vanish inside xdg-desktop-portal.

### Fixed

- **Variant values nested in dicts/lists no longer double-wrap.** A
  `DBusQML.variant(x)` inside an `a{sv}` dict or `av` list value previously
  marshaled as `v(v(x))` (a nested variant) because the value carried its own
  `QDBusVariant` on top of the container's `v`. GLib's concrete-type lookups —
  the standard idiom `g_variant_lookup(vardict, "key", "as", …)` — silently
  returned nothing for such values. Variant conversion is now slot-aware: a
  variant in a variant-providing slot contributes its payload directly, while
  `variant(variant(x))` still produces an explicit inner variant. Fixes 0.4.0
  behavior.

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
