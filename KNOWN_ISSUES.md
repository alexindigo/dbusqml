# Known Issues

Upstream Qt bugs that affect dbusqml usage patterns, discovered while
building test coverage against a matrix of Qt versions. Neither is
caused by dbusqml, and both are fixed in current Qt; they are recorded
here so users on older Qt know what to avoid.

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

## Qt bug: `new ValueType({...})` throws `Type error` for `QVariantMap`-arg constructors

**Affects:** Qt 6.5.x through 6.7.x
**Fixed in:** Qt 6.8
**Symptom:** `Uncaught exception: Type error` on the JS `new` expression.

Reproducer:

```qml
import DBus 1.0 as DBusQML
var msg = new DBusQML.dbusMessage({    // Type error on Qt 6.5–6.7
    service: "org.freedesktop.DBus",
    path: "/",
    iface: "org.freedesktop.DBus",
    member: "ListNames",
})
```

Qt 6.8 added the implicit JS-object → `QVariantMap` coercion at the
value-type constructor dispatch site.

### Workaround

Use QML structured-value assignment instead of JS `new`. Any function
or property that takes a `dbusMessage` accepts a plain JS object with
matching keys, converted implicitly via `QML_STRUCTURED_VALUE`:

```qml
// Works on Qt 6.5+:
DBusQML.SessionBus.asyncCall({
    service: "org.freedesktop.DBus",
    path: "/",
    iface: "org.freedesktop.DBus",
    member: "ListNames",
})
```

For a standalone value, assign to a typed property:

```qml
property DBusQML.dbusMessage msg: ({
    service: "...", path: "...", iface: "...", member: "..."
})
```

---

## Reporting upstream

Both bugs have minimal reproducers (above). Filing on
https://bugreports.qt.io would help other Qt/QML users. If you file
one, please link it here.
