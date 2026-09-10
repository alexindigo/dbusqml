# Known Issues

Upstream Qt bugs that affect dbusqml usage patterns, discovered while
building test coverage. The library floor is Qt 6.8: entries for older
Qt are pruned (floor-pruning policy — history lives in git, not here).
Most entries below are fixed in current Qt; one is a permanent Qt-layer
limitation (thrown-`undefined` undetectability — affects all Qt versions,
worked around, never fixable at this layer).

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
