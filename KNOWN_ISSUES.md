# Known Issues

Upstream Qt bugs that affect dbusqml usage patterns, discovered while
building test coverage. The library floor is Qt 6.8: entries for older
Qt are pruned (floor-pruning policy — history lives in git, not here).
Most entries below are fixed in current Qt; the rest are permanent
Qt-layer limitations (thrown-`undefined` undetectability; uncaptured
child-path registration races; Peer on loopback) or ecosystem-normal
behavior (Close after settle).

---

## Thrown-`undefined` after `holdReply()` is undetectable (permanent Qt-layer limitation)

**Affects:** all Qt versions (not version-gated — the indistinguishability
is in `QJSEngine::callWithInstance` reporting, not a versioned bug).

A handler that calls `holdReply()` and then **throws `undefined`**
produces no reply — the caller times out. `callWithInstance` reports a
thrown `undefined` identically to a returned `undefined`, and a returned
`undefined` is the normal void return of a handler that only called
`holdReply()` (proof pointer: `dbusadaptor.cpp` dispatch, thrown-vs-
returned discrimination comment — thrown primitives/arrays/objects and
the error shape are classified; bare `undefined` cannot be, covered by
the for-all-times P0 matrix).

Workarounds: throw a named error (`DBusQML.DBusUtils.error(name, msg)`)
or a primitive instead of bare `undefined`; fence with
`heldReplyTimeout` (TTL expiry settles with `Failed`) when the throw
shape cannot be controlled.

---

## Peer on loopback (permanent Qt-layer limitation)

**Affects:** all Qt versions (QtDBus in-process short-circuit).

On the wire, `org.freedesktop.DBus.Peer` is answered by libdbus for
every path before Qt sees the message. A caller on the *same* connection
as the object (in-process) is short-circuited by QtDBus and never reaches
libdbus; it receives `UnknownInterface`. Stock Qt behavior; dbusqml does
not paper over it.

---

## Calls to a child path registered inside a handler race its registration (permanent Qt-layer limitation)

**Affects:** all Qt versions that dispatch D-Bus on a manager thread
(QtDBus). GDBus backends are immune (single-threaded in-order dispatch).

QtDBus resolves object paths on its manager thread at receive time
(`qdbusintegrator.cpp` `handleObjectCall`), copies the node by value into
the activation event, and never re-looks-up. A `DBusAdaptor` created
inside a handler (the xdg-desktop-portal `impl.portal.Request` pattern)
is registered later, on the main thread. A call to that child arriving
in between is answered `org.freedesktop.DBus.Error.UnknownObject` by
QtDBus before dbusqml sees it. xdg-desktop-portal forwards that error to
the client's `Close` verbatim and keeps the request exported → orphaned
dialog.

Trigger: prompt Close (timeouts, Esc, automation) racing the child's
attach. No consumer-side workaround exists: the child path arrives in
the very call that races.

**Remedy:** `captureSubtree: true` on the prefix adaptor. QtDBus then
hands every message under the prefix to dbusqml in arrival order;
dbusqml routes to child adaptors from its own table. If your handlers
create child adaptors that callers may address immediately, set the
flag on the parent.

**For the record:** the Qt reference backend xdg-desktop-portal-kde has
the same latent race — `filechooser.cpp` registers its `Request`
(`request.cpp`: `registerVirtualObject(handle, SubPath)`) inside the
`OpenFile` handler on the main thread; only human cancel latency hides
it.

---

## Close after settle hits an absent path (ecosystem-normal, not a dbusqml defect)

A Close arriving after the backend settled and destroyed its Request
adaptor legitimately hits an absent path. Both verifiable reference
backends do exactly this — GTK unexports the Request on completion
(`filechooser.c send_response`), KDE parents the Request to the dialog
so deleting the dialog unregisters the path — and a late Close yields
an error (`UnknownMethod` / `UnknownObject`) that the daemon forwards
and clients already tolerate (libportal ignores Close failures).

Guidance: destroy the Request adaptor on settle; make `Close` on a live
request idempotent (settle-with-cancelled once); do NOT keep stubs or
grace timers; do not wait for a "daemon unexported" signal (none
exists). With `captureSubtree`, dbusqml answers that late Close as
`UnknownObject` in order and logs it under `dbusqml.dispatch` (debug).

---

## Qt bug: `Qt.createQmlObject(...).destroy()` crashes on `QQmlPropertyMap` subclasses

**Affects:** Qt 6.5.x through 6.10.x
**Fixed in:** Qt 6.11
**Symptom:** SIGSEGV inside `QV4::QObjectWrapper::virtualResolveLookupGetter`, address `0x0`, immediately on `.destroy()`.

Minimum reproducer (any subclass of `QQmlPropertyMap` registered as a
QML element will do; the `DBus` element from this module is one such):

```qml
import DBus 1.0

TestCase {
    function test_crash() {
        var p = Qt.createQmlObject('import DBus 1.0; DBus {}', this)
        p.destroy()   // SIGSEGV on Qt 6.5–6.10
    }
}
```

The bug is present with no `service`/`path`/`iface`, no introspection,
no dynamic methods installed, no calls made. Bare create + destroy on a
`QQmlPropertyMap` subclass created via `Qt.createQmlObject` is enough.

Plain `QtObject`, `Item`, and other non-`QQmlPropertyMap` types are
unaffected on the same Qt versions.

### Workarounds

**Prefer inline declarative form.** For most use cases, `DBus {}`
declared inline in a QML tree (a `Window`, a `Component`, etc.) does not
hit this bug — destruction is managed by the QML component teardown, not
by the JS `destroy()` handler.

```qml
Window {
    DBus {
        id: proxy
        service: "..."; path: "..."; iface: "..."
    }
    // proxy is torn down with the Window; no explicit .destroy() needed.
}
```

**If you must create dynamically**, parent the created object to a QML
object that will outlive it and let that parent destroy it, rather than
calling `.destroy()` yourself:

```qml
var p = Qt.createQmlObject('import DBus 1.0; DBus {...}', someParent)
// ...use p...
// Do NOT call p.destroy(). Let someParent's own destruction clean up.
```

**Test suites** should declare shared proxies at `TestCase` scope
instead of building a fresh one per test with `Qt.createQmlObject +
destroy`. dbusqml's own `tests/test_api.qml` uses this pattern.

---

## Resolved-history section (pruned per floor policy)

The `new ValueType({...})` entry (Qt 6.5–6.7, fixed in 6.8 — below the
library floor) was removed; its workaround (QML structured-value
assignment) is the normal form everywhere in `API.md`. The reactive
bindings note below stays: it documents a behavior change users
upgrading across 0.3.0 still hit.

---

## Reporting upstream

The remaining entry has a minimal reproducer (above). Filing on
https://bugreports.qt.io would help other Qt/QML users. If you file
one, please link it here.

---

## Reactive Property Bindings — RESOLVED in 0.3.0

**Affects:** builds before v0.3.0 without `-DDBUSQML_REACTIVE_BINDINGS=ON`.

Since v0.3.0, reactive bindings are always enabled. The
`DBUSQML_REACTIVE_BINDINGS` flag no longer exists. Properties known
from catalog or introspection are pre-populated as `null` placeholders,
so bindings (including through `readonly property` layers) re-evaluate
when real values arrive. `reactiveBindingsSupported` is hardwired `true`.
