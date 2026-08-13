# D-Bus Recursive Demarshaling in dbusqml

**Status:** Resolved in v0.3.0 — signature-complete recursive demarshaller  
**Date:** 2026-08-11  
**Context:** dbusqml v0.2.5, Qt 6.8–6.11

## Resolution

v0.3.0 replaced the special-case approach with a fully signature-driven
recursive demarshaller. `readBySignature` handles all nested containers
(dicts, struct arrays, arrays of arrays, tuples) via
`beginStructure`/`beginMap`/`beginMapEntry`/`beginArray` recursion.
`operator>>(QDBusArgument, QVariant&)` — the libdbus crash path — is
eliminated from all nested contexts. `unwrapDbus` keeps only top-level
concrete-type fast paths and delegates everything else.

## The Problem (historical)

`dbusqml`'s `unwrapDbus()` converts D-Bus reply arguments into plain
`QVariant` containers so QML can traverse them as JavaScript objects.
The function uses `QDBusArgument`'s recursive descent API
(`beginArray`/`beginStructure`/`beginMap`/`atEnd`) to walk nested
containers.

The issue: `operator>>(QDBusArgument, QVariant)` — the free function Qt
provides for reading D-Bus elements as `QVariant` — **crashes inside
libdbus** when called on struct members or dict values at certain nesting
depths. The crash is in libdbus's internal state management, not in Qt's
wrapper code.

This means the recursive descent approach — which Qt's own documentation
presents as the canonical pattern — hits a wall at certain nesting
combinations. The crash is not in our code; it's in libdbus's type
tracking when a `QDBusArgument` is read with an untyped `QVariant` target
inside a nested container context.

## What We Know

### The crash

```
#0  libdbus-1.so.3            (crash inside libdbus)
#1  operator>>(QDBusArgument const&, QVariant&)   libQt6DBus
#2  unwrapDbus(QVariant const&)                   libdbusqml.so
```

Reproducible on:
- `a(ssssssb)` (fcitx5 AvailableInputMethods) — struct array
- `sssssssbsa{sv}` (fcitx5 CurrentInputMethodInfo) — mixed tuple
- `aa{sv}` (NetworkManager Ip4Config.AddressData) — array of dicts

The crash is deterministic and version-independent (tested Qt 6.8.2,
6.10.0, 6.11.1).

### What works

- Basic types read via specific C++ overloads (`arg >> QString`,
  `arg >> int`, etc.) — no crash
- Top-level arrays of basic types (`av`, `ao`, `as`, `ay`, `au`, etc.)
- Top-level dicts (`a{sv}`, `a{ss}`)
- Struct arrays (`a(...)`) — fixed in v0.2.3 via `readBySignature`
- Mixed tuples (`(...)`) — fixed in v0.2.3 via `readBySignature`
- Array of dicts (`aa{...}`) — fixed via `readBySignature` + `unwrapDbus`

### The pattern that crashes

```cpp
// Inside beginArray() or beginStructure() loop:
QVariant elem;
arg >> elem;  // operator>>(QDBusArgument, QVariant) — crashes inside libdbus
```

The `operator>>(QDBusArgument, QVariant)` is a free function that calls
into libdbus to read the current element. At deeper nesting depths (struct
members, dict values inside arrays), the internal libdbus iterator state
is incompatible with the QVariant read path.

## The Fix

`readBySignature()` dispatches on `QDBusArgument::currentSignature()` to
use the correct C++ `operator>>` overload for each element type:

```cpp
static QVariant readBySignature(const QDBusArgument &arg)
{
    const QString sig = arg.currentSignature();

    // Basic types — specific C++ overloads, no QVariant
    if (sig == "s") { QString v; arg >> v; return QVariant::fromValue(v); }
    if (sig == "i") { int v; arg >> v; return QVariant::fromValue(v); }
    // ... etc for all basic types

    // Complex types — recurse via unwrapDbus
    QVariant v = arg.asVariant();
    return unwrapDbus(v);
}
```

This avoids the `operator>>(QDBusArgument, QVariant)` crash path entirely.
The `asVariant()` call reads the element without crashing (it uses a
different internal code path), and `unwrapDbus` handles the recursion.

## Current State

The recursive descent handles all currently-known D-Bus types. The
`aa{...}` handler (array of dicts) is the latest addition, discovered
during arch-niri VM testing with NetworkManager.

**Open question:** Are there other nesting patterns that will crash?
The current approach uses `currentSignature()` dispatch + `asVariant()`
fallback, which should handle any combination. But we haven't proven
exhaustive coverage.

## What We Need From Experts

1. **Is there a Qt/libdbus API we're missing?** Is there a canonical way
   to read any D-Bus element into a QVariant without hitting the libdbus
   crash?

2. **Is the `asVariant()` fallback safe at all nesting depths?** We know
   `operator>>(QDBusArgument, QVariant)` crashes. Is `asVariant()` using
   the same code path or a different one?

3. **Should we use `QDBusArgument::beginStructure`/`beginArray`/`beginMap`
   at every level, or is there a simpler pattern?** The Qt docs show
   recursive descent as the pattern, but the crash suggests the pattern
   has a gap at deeper nesting.

4. **Is this a known Qt bug?** Should we file it upstream? The crash is
   reproducible and deterministic — it seems like a libdbus/Qt integration
   issue that should be tracked.

## Files

- `dbusconnection.cpp` — `unwrapDbus()` and `readBySignature()`
- `tests/test_dbusconnection.cpp` — existing test suite
- `tests/test_reactive_binding.qml` — reactive-binding reproducer

## References

- Qt 6 QDBusArgument docs: https://doc.qt.io/qt-6/qdbusargument.html
- Qt D-Bus Type System: https://doc.qt.io/qt-6/qdbustypesystem.html
- `KNOWN_ISSUES.md` — documents the `createQmlObject+destroy` crash
  (separate issue, fixed in Qt 6.11)
