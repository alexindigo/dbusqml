# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Sections per release, in canonical order: Added, Changed, Deprecated,
Removed, Fixed, Security, Quality (project extension for test and CI
infrastructure that materially affects consumer trust).

## [Unreleased]

## [1.0.0] - 2026-09-27

### Added

- **T1 owner-change delivery via main-thread relay** — the manager thread
  no longer touches adaptors: value-only notes go to a process-lifetime
  relay, and holders are re-resolved on the main thread.
- **Bus connection-loss handling** — `connected` property and
  `disconnected()` signal on connections; proxies flip `status=Error` and
  `serviceAvailable=false` with match teardown; adaptor claims emit
  `nameLost`. No automatic resubscribe (documented).
- **Caller identification** — `callerService()` on adaptor (dispatch only)
  and caller on `DBusHeldReply` (captured at hold time).
- **Served option whitelist** — `_options` per-method map with
  xdg-desktop-portal drop/error semantics; `InvalidArgs` on mistype.
- **Sender authorization** — `allowedSender` adaptor property;
  `AccessDenied` on mismatch (methods and Properties).
- **Failed-write notification** — `propertyWriteFailed(name, errorName,
  message)` signal on rejected writes; the proxy then re-fetches the
  property from the service (see Changed).
- **Concurrent Get/Set dedupe** — in-flight Get coalesced into one wire
  call; Set is latest-wins queued. No new surface.
- **Held-reply TTL** — `heldReplyTimeout` (ms, default 0 = off); expiry
  settles with `Failed` ("reply timed out") and warns; settle cancels the
  timer.
- **`captureSubtree`** — opt-in `SubPath` registration on `DBusAdaptor` so
  messages under the prefix are delivered to dbusqml in bus arrival order
  and routed to child adaptors (portal `Request` objects), including a
  capturing adaptor at `/`. Absent paths answer `UnknownObject` in order.
  Co-located adaptors at the path must agree; a mismatching latecomer is
  refused.

### Changed

- **BREAKING: Strict option typing** — the `_options` filter iterates the
  declared keys and delivers the typed (coerced) value; a declared key with
  a wrong-kind or non-convertible value is rejected with `InvalidArgs` and
  the handler never runs (was: a post-marshal same-kind check that coerced
  almost anything through, delivering the raw caller value).
- **BREAKING: Empty option allow-list denies all keys** —
  `_options: { Method: {} }` filters with zero declared keys: every caller
  key is dropped and the handler receives `{}` (was: collapsed to
  no-entry, allow-all).
- **BREAKING: Failed writes re-fetch from the service** — a rejected `Set`
  issues a `Properties.Get` for the key once the write chain settles, and
  the server's value lands in the map; the proxy never invents a value
  (was: local rollback inference). `propertyWriteFailed` fires unchanged as
  the consumer hook for gesture policy; a failed re-fetch keeps the current
  value with a loud warning.
- **BREAKING: Completed replies are handed to the JS GC** — at completion
  a `DBusPendingReply` is unparented and marked `JavaScriptOwnership`,
  synchronously in the completion's own stack frame. Fire-and-forget calls
  no longer accumulate reply objects on the connection; callers that keep
  a reference keep the reply. Pure C++ consumers (no engine) keep parented
  ownership; the promise-style `asyncCall` overload manages its reply
  C++-side (unchanged; it never crosses into JS).
- **Deferred-deletion window documented as defined behavior** (was:
  tracked ambiguity) — an adaptor serves until its destructor runs (pinned
  by test); API.md lifecycle note.
- **Invalid-fd sentinel unified on `-1`** (was: single `h` → JS
  `undefined`, `ah` element → `-1`) — one consumer check covers both.
- **`holdReply()`/`callerService()` documented JS-handler-only** — the C++
  `Q_INVOKABLE` fallback runs after the dispatch scope closes; C++ services
  are pointed at `QDBusConnection::registerObject` + `QDBusContext`.
- **Introspection stays public under `allowedSender`** (xdp posture) —
  the dispatcher answers Introspectable before the adaptor gate; a
  well-known `allowedSender` value now warns at attach (it can never match
  a unique name).
- **Adaptor identity is attach-time-only** — post-attach
  service/path/iface/connection mutation warns and is ignored (was: silent
  wire-identity split plus registry leak).
- **Declared top-level `v` wraps in `QDBusVariant`** (was: plain returns
  marshaled as their own type while XML advertised `v`).
- **`getProperty` sends the wire name** (was: the QML name — split
  `Version`/`version` failed where Set succeeded).
- **Strict signature gate at depth 32** (was: 64 while the walkers cap at
  32 — unmarshalable shapes burned process-global signature slots).

### Fixed

- **Engine teardown errors held replies** (was: a shell live-reload with a
  portal dialog open stranded the caller forever) — declarative adaptors
  hook the engine's destruction and settle outstanding held replies with
  `Failed` before the world disappears.
- **Held-reply error-name validation shared** — `sendError` runs the same
  grammar check and `Failed` fallback as the throw path (was: an invalid
  dotted name produced zero replies for the serial); send results are
  checked and loud; `send`/`expire` audited.
- **Idempotent `holdReply()`** — a second hold in one dispatch returns the
  same handle (was: a second settlement authority plus a second TTL timer —
  a double-reply surface).
- **Uncapped walker recursion** — `firstCompleteType` is depth-capped at
  32 like every other walker, with a strict re-check in `writeBySignature`
  (was: a 200k-deep `a…a` prefix in peer-supplied introspection XML
  SIGSEGV'd the process — remotely triggerable).
- **Map/array containers close on failure** — probe-then-commit staging
  (was: early `return` mid-`begin*` left a half-open `QDBusArgument`).
- **Loss-probe ping storm** — the connection probe re-arms through a
  3000 ms single-shot timer on both the connection and proxy paths (was:
  98 idle pings per second per connection; the proxy twin never re-armed
  after startup).
- **Promise-style `asyncCall` reply leak** — the reply is deleted after
  settle (was: unbounded growth on the immortal connection).
- **Stale re-fetch overwrite** — an epoch plus destination snapshot guards
  invalidated-property re-fetches (was: a same-service repoint could
  contaminate across interface/path).
- **Queued Set cross-fire** — the drain sends to the snapshot destination
  (was: a repoint mid-queue redirected an earlier value at the new
  service).
- **Post-coercion reply guard for declared `h`/`o`/`g`** — the marshaled
  value is re-guarded before send (was: the guard ran on the pre-marshal
  value only).
- **Second same-name holder `nameAcquired`** — a late joiner on an owned
  claim is notified directly (was: silent — no watch transition).
- **Nested int64 precision** — the dispatch-arg converter is recursive
  (was: nested > 2^53 values rounded through double).
- **Raw-dict `PropertiesChanged`** — the changed-dict is unwrapped before
  the cast (was: a `QDBusArgument`-shaped dict produced an empty map and a
  stale UI).
- **`emitSignal` claims the service once per value** (was: a blocking
  daemon round-trip per emission).
- **`SignalRelay` lifetime discipline** — `QPointer` plus null checks
  (was: raw pointer, unchecked).
- **Teardown targets the attach-time connection** — captured by value
  (was: re-resolved — a dead connection QML sibling made detach fall back
  to the session bus, leaking the claim).
- **Dangling pointer in option validation** — the meta-method is copied by
  value (was: a loop-local's address dereferenced after scope end).
- **Dead `advertisedName` ternary removed**.
- **Settle stops the TTL timer** — the timer is stored on the reply (was:
  the comment promised a cancel that never happened; the timer outlived
  its purpose).
- **Two real marshal leaks fixed, not suppressed** — the half-open struct
  branch and a `QVariant` string-literal retention, found when the ASan
  ODR mask was removed; tests now link the library.
- **Interface-less `Introspect` / `GetAll` on captured children** —
  Qt's internal filters serve these again (was: child fallback
  `UnknownMethod`).
- **Captured children now match plain adaptors for every Qt-handled shape**
  (interface-less `Get`/`Set`, unknown `Properties` members); parity is
  pinned table-wise.
- **Held reply warns and replies `Failed` when the adaptor is gone at
  `send()`** (was: silent skip, then settle — a swallowed reply if the
  path were reached). Unreachable through `~DBusAdaptor` (that path
  already `sendError`s parented held replies); pinned on the wire.
- **Child-path fallback under `captureSubtree`** — an unhandled call on a
  live child answers `UnknownMethod` / `UnknownInterface` (was: Qt's
  leftover-path `UnknownObject` for a path that exists).
- **Co-located children agreeing on `captureSubtree`** — mismatch compares
  the first attacher's request, not the applied mode (was: two children
  both requesting capture under a capturing root refused the second).
- **Every reply/error send is checked** — `checkedSend` at remaining
  adaptor, held-reply, and dispatcher sites (was: four unchecked `send`s).
- **`DBus::Struct` / `DBusAsArray` demarshal via the signature walker**
  (was: `operator>>(QDBusArgument, QVariant)` inside the container — the
  libdbus crash class).
- **Method miss on a served interface now answers `UnknownMethod`** (was
  `UnknownInterface` — a lie about an advertised interface; consumer leg
  8a). Roots and captured children alike; Qt's text; NO_REPLY_EXPECTED
  honored.
- **C++-path methods with more than five arguments answer `Failed`**
  instead of declining.
- **Plugin loads clean under ASan's ODR check** — the backing library no
  longer carries a dead duplicate of the generated QML plugin class (its
  copy of the class's `staticMetaObject` tripped the odr-violation abort
  when a process had both DSOs loaded, e.g. an ASan-instrumented consumer).
- **Owner-change acquisition can no longer be missed** — a flagged claim's
  initial state comes from the RequestName reply, so a late or suppressed
  `NameOwnerChanged` signal no longer wedges `nameAcquired` (the G1
  capture). `nameAcquired` may now arrive earlier (from the reply). And
  tearing down a flagged adaptor always withdraws its name request, even
  when the name was only queued at the daemon or held after a takeover —
  previously such a claim stayed queued and the daemon could grant the name
  later to a connection with nobody serving it (a ghost owner answering
  `UnknownObject`, blocking the legitimate next requester). One blocking
  `ReleaseName` now happens at every flagged last-detach (was: only when
  owned). The deprecated `serviceOwnerChanged` warning is gone (the
  subscription moved to the public bus-signal API). No public API change.

From the 0.9.1 fix train (never released separately):

- **Declared container elements are producible** (the `a(...)` hole is
  closed): unproducible array/map element signatures mint assign-once
  signature-slot pool registrations at first encounter and stream through
  the existing walkers — `a(sa{sv})` (BindShortcuts response / signal),
  `aa{ss}`, `a{sas}`, struct arrays like `a(ii)`. No marshall operators,
  no libdbus, no private Qt ABI; pool exhaustion fails loud.
- **Dispatcher never blocks under the registry lock** (two proven deadly
  embraces): detach releases `ReleaseName` outside the lock with
  ownership gating, tombstone adoption, and NOT_OWNER-aware logging;
  attach registers outside the lock with a pending-state claim. A
  debug-only guard asserts the lock is clear before any blocking bus call.
- **Relayed signal args are guarded** (the last unguarded send site):
  unmarshalable values warn + skip; send failures are logged; the 0.5.2
  kill-class is closed.
- **Aliased handlers dispatch through the JS path** — thrown
  `DBusUtils.error()` on an aliased member is a named error reply (was:
  silent empty success), and 64-bit in-args keep precision.
- **Client PropertiesChanged protocol complete**: interface-argument filter
  (a co-located adaptor's signals no longer contaminate the map);
  `invalidated_properties` re-Gets each name (stale kept + one warning on
  error); introspected-property placeholders record the wire name
  immediately (no wrong-name Set window).
- **Emission truthfulness**: private-property notify signals no longer
  leak onto the bus (privacy exclusion moved to relay attach);
  `_signals`/catalog-declared signal names are relayed under the declared
  wire name; declared signal TYPES are applied at emission (relay +
  `emitSignal`), mismatched payloads warn + skip; >5-param signals are
  neither relayed nor advertised; `qml*`-prefixed signals are never
  relayed.
- **Served out-args are declared-truthful**: `declaredOutTypes` gains a
  found/not-found distinction; declared-void methods carry no out-arg
  (the napkin's phantom `result v` dies); a declared-void handler that
  returns a value is warned and the value dropped (declaration wins);
  catalog in-args match the resolved wire name (alias + exact tiers);
  catalog property `type`/`access` reach the served XML.
- **Walker parity**: nested `av`/`ao` read (was silently dropped); `ay`
  number-array coercion at every position; invalid fds rejected at
  marshal (`h`) and at the send boundary; the declared signature wins
  over a mismatched gadget's own type at every position (D4); bare
  `o`/`g` carriers unwrap to strings.
- **Robustness**: recursion depth cap 32 in the recursive walkers (warn +
  loud fail); a failed C++ method invocation is a Failed error reply
  (never a silent empty success); every silent malformed-signature path
  warns; error names are grammar-validated before the reply; QML-declared
  `emitSignal` is skipped by dispatch AND the XML (the load warning is
  now true); `holdReply`/`unregister` are never advertised.
- **Hygiene**: the client/server folds share one implementation with the
  two documented modes (both accepted on lookup — `xmlConfig` resolves
  `XMLConfig`); `setProperty` converts before the guard and resolves
  names through the recorded map; alias-to-private-property refused at
  attach with a warning; `_signatures` keyed by the aliased QML name
  honored; QObject\*-typed properties never attach relays; co-located
  empty-iface adaptors dedupe in merged introspection; dead residues
  removed; user-callback exceptions in asyncCall are logged.

### Quality

- **The TSan gate is a verdict-layer classifier** — complete-report
  parsing with fail-closed ambiguity; an identity register for
  investigated Qt-internal noise matched on four layers (report type →
  stack arrangement → site offsets → binary BuildId), evidence-blocked,
  inert pending revalidation on unvalidated Qt builds; a per-job canary
  battery (pure race, mixed-instrumentation race, known-foreign class)
  plus an independent cross-check; the CI verdict is owned by the
  classifier, with a racy canary that must be caught before silence
  counts.
- **Sanitizer suppressions are typed, symbol-named, and evidence-blocked**
  (LSan and TSan); the ASan ODR mask is gone (tests link the library).
- **Wire oracle observes both directions** — its own `ReplyCount` plus a
  caller-mode `ReceivedCount` driven in-suite through the held-reply pins,
  with a deliberate double-send sensitivity self-test.
- **Fuzzers exercise the real symbols** with megabyte-scale inputs and
  hostile corpora (the remote-crash class is fuzzer-visible pre-fix).
- **Zero `QEXPECT_FAIL` remain** — the engine-teardown expected-fail pin
  became a real green test when the fix landed; the four exploratory
  campaigns' one open finding is resolved. PARITY.md gained rewritten
  axioms 6, 9b, 9c.
- **Test hygiene** — late-joiner `nameAcquired` pin; per-test statics
  reset.
- **Wire oracle counts error replies** — the exactly-one-reply referee's
  caller mode used a blocking libdbus call that returns NULL for error
  replies, so a single error read as 0 and a double as 1; replaced with a
  pending call + steal, remote replies of either type counted once; error
  sensitivity self-tests (1 → 1, 2 → 2, none → 0).
- **Wire oracle gains `CallAdaptorNoReply`** — observes replies to calls
  that asked for none.
- **Peer tests observe the transport from a separate connection** — the
  loopback-only Peer serving was removed as unreachable on the wire.
- **Oracle pins no longer stall** — the harness called the wire
  oracle with a blocking `QDBus::Block` while the oracle called back into
  the same thread; every pin waited out the oracle's 5 s timeout. Now async
  with a pumped wait; each pin bounds its own wall time (< 2 s), the
  deliberate no-reply case pins the timeout path.
- **Referee poll-to-expected with a declared bound** — `oracleSettledCount`
  waits for the expected count then a 500 ms quiet window (PARITY §5);
  not a proof of finality.
- **T1 spawn/ping barrier** — a blocking `Peer.Ping` on the worker
  connection replaces the blind 200 ms sleep; both legs assert handler
  time vs ping-sent.
- **Dead oracle `DoubleSend` member removed** — the suite's double-send
  service is QtDBus-side.
- **H6c `Failed` is wire-pinned** — real method-call, name + count legs.
- **Test teardown is explicit** — the suites close their named connections,
  stop the oracle, and delete their static engines before exit (LSan-clean
  on CI's Qt 6.8.2 save libdbus's process-lifetime register residue);
  the TSan gate's foreign canary now accepts any registered bus-bind-family
  class; the owner-change storm's survivor settle is proportional to the
  storm's measured duration.

### Docs

- **The two child-path races** — QtDBus manager-thread lookup vs
  main-thread registration (remedy: `captureSubtree`) is distinct from
  the settle race (Close after the Request is destroyed: ecosystem
  behavior, GTK and KDE both destroy on completion). API.md, KNOWN_ISSUES,
  PARITY threading contract, README portal snippet.

## [0.9.0] - 2026-09-02

### Added

- **fd quartet + `fdUrl` on `DBusUtils`** — QML had no fd I/O:
  `openFd`/`writeFd`/`readFd`/`closeFd` make a received `h` usable from
  QML (receiver-closes is now dischargeable via `closeFd`), and `fdUrl`
  maps a regular-file fd to `file:///proc/self/fd/N` for path-based
  consumers. Caveats: `fdUrl` is regular-file-only (streams use
  `readFd`/`writeFd`); the URL is valid only while the fd stays open.
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

### Changed

- **Truthful served surface — the naming ladder** — served introspection
  XML, `GetAll` keys, `PropertiesChanged` names, and method in-arg types
  now resolve through explicit (`_signals`, `_members`) → declared
  (catalog) → stable inference (the deterministic first-character fold,
  `readOne` ⇄ `ReadOne`). **Wire change:** undeclared QML members are
  advertised wire-cased on the bus (`ReadOne`, not `readOne`), and
  QML-declared plain signals broadcast under their folded wire names
  (`SomethingHappened`, not `somethingHappened`). Subscribers matching the
  old lowercase names must update their match rules.
- **`PropertiesChanged` replaces the accidental notify-signal relays** —
  property changes now emit the standard
  `org.freedesktop.DBus.Properties.PropertiesChanged`; the `fooChanged`
  broadcast signals are gone (unmarshalable values report via
  `invalidated_properties`). dbusqml clients (which always subscribed to
  `PropertiesChanged`) now see dbusqml adaptors' property changes —
  reactivity works end-to-end; consumers that polled as a workaround can
  stop.
- **Shadowing a built-in property errors at load** — `service`, `path`,
  `iface`, `connection` are `FINAL`: a QML `property string path` on an
  adaptor is a load-time error instead of a silently broken adaptor.
- **64-bit values above 2^53 are delivered as full-precision decimal
  strings** — QML's JS engine has no BigInt (spike-confirmed); a qint64/
  quint64 that doesn't round-trip through a double would silently lose
  precision as a JS number. Values within 2^53 stay plain numbers. The
  send path accepts decimal strings with declared `x`/`t`, making the
  round-trip lossless end-to-end.

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

## [0.8.0] - 2026-08-29

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

## [0.7.0] - 2026-08-28

### Added

- **`DBusAdaptor.unregister()`** — deterministic retirement of a dynamically
  created adaptor, independent of GC timing: frees the object path and
  releases the service reference immediately, errors out outstanding held
  replies (same code the destructor runs), and leaves the QObject alive for
  QML to drop whenever. One-way: re-registration after `unregister()` is not
  supported; a second call warns and does nothing. Covers the "free the bus
  path NOW, collect the object later" case — e.g. a caller-side `Close` on a
  portal Request object.

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

## [0.6.0] - 2026-08-28

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

### Quality

- **Adversarial input matrix** — hostile value classes (null, objects,
  functions, NaN/±Infinity, embedded-NUL and 1 MB strings, empty and deeply
  nested containers, cyclic objects/arrays, malformed signatures) swept
  across every marshal exit and pinned as permanent regression tests.

## [0.5.2] - 2026-08-28

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

## [0.5.1] - 2026-08-27

### Changed

- **Interface-scoped method routing** — calls that carry an interface name
  now route only to an adaptor declaring that interface (and
  `Properties.Get/GetAll/Set` route by the interface argument), whereas
  interface-less calls keep the member-name dispatch across adaptors. On
  0.5.0 a single adaptor served members of *several* interfaces by ignoring
  the message interface; that workaround must be split into co-located
  adaptors (the pattern 0.5.1 enables), or its foreign-interface members
  become unreachable when called with the interface name set. (Documented
  retroactively in 0.5.2.)

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

## [0.5.0] - 2026-08-25

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

## [0.4.0] - 2026-08-25

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

## [0.3.1] - 2026-08-12

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

## [0.3.0] - 2026-08-12

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

### Changed

- **BREAKING: Reactive bindings always on** — `DBUSQML_REACTIVE_BINDINGS`
  CMake option removed. Catalog/introspection pre-population of `null`
  placeholders is unconditional. Properties known from catalog or
  introspection read as `null` (not `undefined`) before the real D-Bus
  value arrives. `reactiveBindingsSupported` is hardwired `true`.
  The `qt6-dbusqml-reactive` AUR package is obsolete — use `qt6-dbusqml`.
- **BREAKING: `ay` reads as `ArrayBuffer`** — byte arrays consistently
  arrive as JS `ArrayBuffer` (was inconsistent: string or array-like
  depending on the code path). Use `DBusUtils.textFromBytes(buf)` for UTF-8
  text (e.g. SSIDs), or index via `new Uint8Array(buf)`.

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

## [0.2.5] - 2026-08-10

### Changed

- **Reply values are real JS values** — `DBusPendingReply::value`/`values`
  return `QJSValue` with real JS `Array`/`Object` instances (working
  `Array.isArray`, `.map`, `.filter`). C++ consumers use
  `valueVariant()`/`valuesVariant()` for raw data.

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

## [0.2.4] - 2026-08-10

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

## [0.2.3] - 2026-08-09

### Fixed

- **SIGSEGV on struct/tuple D-Bus calls** — `operator>>(QDBusArgument,
  QVariant)` crashes inside libdbus when reading struct members. Replaced
  with `currentSignature()` dispatch to the correct C++ `operator>>`
  overload per member type. Complex types use `asVariant()` + recursion.
  Fixes fcitx5 `AvailableInputMethods` (`a(ssssssb)`) and
  `CurrentInputMethodInfo` (`sssssssbsa{sv}`) crashes.

## [0.2.2] - 2026-08-09

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

## [0.2.1] - 2026-08-09

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

## [0.2.0] - 2026-07-25

### Added

- **`DBUSQML_REACTIVE_BINDINGS` CMake option** (default `OFF`) — when
  enabled, DBusProxy pre-populates `null` placeholders for catalog-declared
  properties BEFORE QML bindings evaluate, so `QQmlPropertyMap`'s
  built-in reactivity handles subsequent D-Bus value updates. Fixes the
  long-standing bug where intermediate `readonly property` layers
  wrapping DBusProxy properties resolved to `false` forever.
- **`reactiveBindingsSupported` QML property** on every `DBus` element
  (`bool`, read-only, constant) — returns `true` when the build includes
  the reactive-bindings fix.
- **`types/org.freedesktop.NetworkManager.xml` catalog descriptor** with
  26 property declarations.

### Changed

- **`DBusCatalog::InterfaceSpec` gains `properties`** (`QStringList`
  field) — the catalog XML parser now extracts `<property name="..."/>`
  elements.
- **`dbusqmlConfig.cmake.in` exposes `dbusqml_REACTIVE_BINDINGS`** to
  downstream CMake consumers.

## [0.1.0] - 2026-07-18

Initial release. Requires Qt 6.8 or newer. See
[`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) for two upstream Qt bugs users
on older Qt should be aware of.

### Added

- **`DBus` proxy element with dynamic method dispatch** — D-Bus methods
  discovered via `Introspect()` are exposed on the element under
  camelCased names and forward through a per-proxy helper.
- **Automatic property discovery** via `Properties.GetAll` and
  `PropertiesChanged` subscription — nested `a{sv}` / `a{ss}` containers
  arrive at QML as `QVariantMap` / `QVariantList` (not opaque
  `QDBusArgument`).
- **`DBusAdaptor` (server-side)** — expose a QML component as a D-Bus
  object; method dispatch goes through `QJSValue::callWithInstance` so
  container arguments round-trip natively.
- **User-land type catalog** — drop XML descriptors into
  `$XDG_CONFIG_HOME/dbusqml/types/` (or use bundled defaults for MPRIS,
  Notifications, ScreenSaver, login1.Manager, portal.Settings,
  portal.NetworkMonitor, UPower) so proxies can call methods on services
  that return empty `Introspect()` (e.g., Chromium-based MPRIS players).
  Documented in [`docs/TYPES.md`](docs/TYPES.md).
- **Promise-style `asyncCall(message, resolve, reject)`** — `resolve`
  receives the reply value as a native JS value (`Array.isArray` returns
  `true` for array replies; objects for `a{sv}`); `reject` receives a
  single `{ name, message }` error object.
- **`SessionBus` and `SystemBus` singletons; `connectToBus(address)`** for
  peer / custom connections — each call produces a distinct QtDBus
  connection name, no silent handle reuse.
- **Value types** (`import DBus 1.0 as DBusQML`): `uint32`, `int32`,
  `uint64`, `int64`, `uint16`, `int16`, `bool`, `double`, `byte`, `string`,
  `objectPath`, `signature`, `dict`, `variant`.
- **13 runnable examples** (`simple/*`, `intermediate/*`, `advanced/*`)
  with a shared `CloseButton` and click-to-copy error messages.
- **CMake package config** — downstream projects can
  `find_package(dbusqml 0.1 REQUIRED)` and link to `dbusqml::dbusqml`.
- **GitHub Actions CI** — matrix build on Qt 6.5.3 and 6.8.2, running C++
  tests and QML tests on every push and PR.

### Fixed

- **`unwrapDbus` no longer crashes on concrete-type D-Bus arrays** — `ao`
  (object-path arrays returned by UPower `EnumerateDevices`, logind
  `ListSessions`, etc.) previously segfaulted inside libdbus. Common
  array signatures (`a{s*}`, `av`, `ao`, `as`, `ay`) are demarshaled
  through their proper C++ target types and flattened for JS.
- **Re-introspection no longer leaks stale method callables** — switching
  a proxy's `iface` at runtime removes the previous iface's method keys
  from the property map, clears cached `QJSValue`s, and drops the old
  helper QObject from the engine global.
- **`BatteryMonitor` example delegate widths** are bound to the enclosing
  `ListView` explicitly instead of `parent.width`, avoiding the Qt 6
  ListView-delegate `parent`-during-creation footgun.
- **`DBusAdaptor::handleMessage` no longer builds a JS source string** to
  dispatch to QML methods (container arguments would arrive stringified,
  the global name collided across instances).

### Quality

- **README.md** — tagline, feature bullets, quick-start snippet, and
  build instructions.
- **API.md** — full API reference, including a Known Limitations section
  covering the C++-context signal-handler pitfall and the
  runtime-configured proxy async pattern.
- **DESIGN.md** — architecture rewrite: describes the actual dispatch
  (JS closures in `QQmlPropertyMap`, per-proxy `DbusMethodHelper`,
  catalog fallback) and the trade-offs behind the design.
- **docs/TYPES.md** — documents the user-land catalog.
- **FutureDevelopment.md** — replaced legacy `DBUS_w_QML.md` scratch
  notes (later removed when its content was fully absorbed).

[Unreleased]: https://github.com/alexindigo/dbusqml/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/alexindigo/dbusqml/compare/v0.9.0...v1.0.0
[0.9.0]: https://github.com/alexindigo/dbusqml/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/alexindigo/dbusqml/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/alexindigo/dbusqml/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/alexindigo/dbusqml/compare/v0.5.2...v0.6.0
[0.5.2]: https://github.com/alexindigo/dbusqml/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/alexindigo/dbusqml/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/alexindigo/dbusqml/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/alexindigo/dbusqml/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/alexindigo/dbusqml/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/alexindigo/dbusqml/compare/v0.2.5...v0.3.0
[0.2.5]: https://github.com/alexindigo/dbusqml/compare/v0.2.4...v0.2.5
[0.2.4]: https://github.com/alexindigo/dbusqml/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/alexindigo/dbusqml/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/alexindigo/dbusqml/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/alexindigo/dbusqml/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/alexindigo/dbusqml/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/alexindigo/dbusqml/releases/tag/v0.1.0
