# Code Quality Review & Remediation Plan

## Phase 1 — Runtime Bugs

### #1 Missing `clipBoard` declarations — `PortalDemo.qml` + `KDEConnect.qml`
- **What:** Both files call `clipBoard.text/.selectAll()/.copy()` in error-copy MouseArea handlers but never declare `TextEdit { id: clipBoard }`.
- **Impact:** `ReferenceError` when clicking an error message to copy. Every other copy-enabled example has the hidden TextEdit.
- **Approach:** Add `TextEdit { id: clipBoard; x: -9999; y: -9999 }` to both files.
- **Effort:** ~5 min

### #2 Unreliable `Clipboard` singleton in `ListNames.qml`
- **What:** `ListNames.qml:65` uses `Clipboard.text` from `import QtQml`. Every other copy-enabled file uses the hidden-`TextEdit` pattern. We previously confirmed `Clipboard` is unreliable across Qt builds.
- **Impact:** Copy-from-right-click may silently fail on some Qt configurations.
- **Approach:** Replace `Clipboard.text = model.name` with the standard `clipBoard.text + selectAll() + copy()` pattern. Remove `import QtQml`, add `TextEdit { id: clipBoard }` declaration.
- **Effort:** ~5 min

### #3 Dynamic method invocation broken for runtime-configured proxies
- **What:** `proxy.playPause()` / `proxy.previous()` fail with "Property ... is not a function" when the proxy's service/path/iface are set dynamically at runtime (e.g., MPRIS player selector). Works when configured statically in QML declarations.
- **Impact:** The headline "dynamic methods" feature doesn't work in its most common use case — connecting to a service discovered at runtime. Users must use `proxy.call("Method")` as a workaround.
- **Root cause hypothesis:** `QQmlPropertyMap` may not reliably expose `QJSValue` function objects stored via `insert()` as callable in all Qt 6.11 contexts. The functions are created via `engine->evaluate()` and stored in the property map, but when QML accesses them, they may not be recognized as functions.
- **Approach:**
  1. Add runtime debug logging to `setupDynamicMethods` to confirm functions are created and inserted
  2. Investigate whether storing in a `QQmlPropertyMap` vs directly on the QObject via `setProperty` makes a difference
  3. Try registering real `Q_INVOKABLE` meta-methods via `QMetaObjectBuilder` at runtime instead of JS closures
  4. Fallback: add a `Q_INVOKABLE void invoke(const QString &method, const QJSValue &args)` dispatcher and standardize examples on `call("Method")` for dynamic proxies
- **Effort:** ~2–4h

### #4 `DBusConnection::connectToBus` uses fixed connection name
- **What:** `dbusconnection.cpp:80` uses `QStringLiteral("dbusqml-custom")` as the connection name. A second call with a different address silently reuses the first connection (QtDBus returns existing connection for same name).
- **Impact:** User can't connect to two different private buses simultaneously.
- **Approach:** Use a counter-based name like `DBusProxy::connectToBus` does (`dbusqml-custom-%1`). Move the counter to static scope shared by both functions, or unify them.
- **Effort:** ~15 min

---

## Phase 2 — Library Hardening

### #5 Duplicate `Q_PROPERTY status` in `dbus.h`
- **What:** Lines 26 and 31 are identical: `Q_PROPERTY(Status status READ status NOTIFY statusChanged)`.
- **Impact:** No functional issue (Qt deduplicates), but confusing and wrong.
- **Approach:** Remove line 31.
- **Effort:** ~1 min

### #6 Adaptor method dispatch is fragile and injection-prone
- **What:** `dbusadaptor.cpp:345-359` builds JavaScript as a string and calls `engine->evaluate()`. This:
  - Escapes backslashes/quotes for strings, but numeric args bypass escaping entirely
  - Uses a fixed global name `__dbusAdaptor` that collides across multiple adaptor instances
  - Breaks for complex args (arrays/dicts) which `toString()` can't represent as valid JS literals
- **Impact:** Possible injection vector. Silent failure for methods with array/dict arguments.
- **Approach:** Replace with `QJSValue::callWithInstance()`:
  1. Get the QML function via `engine->newQObject(this).property(member)`
  2. Convert each arg via `engine->toScriptValue(arg)` (handles all types correctly)
  3. Call with `fn.callWithInstance(thisObj, jsArgs)`
- **Effort:** ~30 min

### #7 Promise-style `asyncCall` loses typed data
- **What:** `dbusconnection.cpp:115` resolve callback receives only `reply->value().toString()` — discards numeric, boolean, and structured return values. Reject callback passes `(name, message)` as two separate args, not a single error object.
- **Impact:** Users writing promise-style async calls get stringified/split results.
- **Approach:** Pass `reply->value()` directly to resolve. Pass `{ name: ... message: ... }` as a single QJSValue to reject.
- **Effort:** ~15 min

### #8 Unbounded growth on re-introspection
- **What:** Each call to `setupDynamicMethods()` creates a new `DbusMethodHelper` and stores it on the global object. Old helpers and `m_cachedFunctions` accumulate.
- **Impact:** Memory leak on repeated introspection (e.g., switching MPRIS players repeatedly).
- **Approach:** Before re-introspecting:
  1. Clear old helper from global object
  2. `m_cachedFunctions.clear()` (releases QJSValues)
  3. Clear old method entries from the property map
- **Effort:** ~20 min

### #9 Signal handler context limitation
- **What:** Inline signal handlers on DBus elements (`onIntrospectionCompleted: someFunc()`) run in the C++ object's context where JS functions from the QML scope are NOT accessible. Only id references and Q_PROPERTY values work.
- **Impact:** Every QML file had to work around this by using `Component.onCompleted: proxy.signal.connect(func)` or `Connections { target: proxy; function onSignal() { ... } }`.
- **Approach:** Document this clearly as a known limitation. No code fix — inherent to Qt 6.
- **Effort:** ~10 min (docs only)

---

## Phase 3 — Documentation Accuracy

### #10 API.md method naming is wrong (PascalCase → camelCase)
- **What:** API.md consistently shows `proxy.ListNames()`, `mpris.PlayPause()`, etc. The implementation lowercases method names to camelCase (`listNames`, `playPause`).
- **Impact:** Every method-call example in the docs would fail if copy-pasted.
- **Approach:** Global search-and-replace in API.md and DESIGN.md: `ListNames` → `listNames`, `PlayPause` → `playPause`, etc. Also update explanatory text claiming methods "retain PascalCase."
- **Effort:** ~20 min

### #11 DESIGN.md treats DBusAdaptor as proposed/non-goal
- **What:** DESIGN.md:148 says "proposed" and DESIGN.md:199-201 lists it as a non-goal. It's fully implemented and documented in API.md.
- **Approach:** Update DESIGN.md to reflect current reality: adaptor is implemented and shipped.
- **Effort:** ~10 min

### #12 DESIGN.md uses shadowed `DBus.uint32()` namespace
- **What:** DESIGN.md uses `DBus.uint32(42)` — broken because `DBus` is also the element name. API.md correctly uses `DBusQML.uint32(...)` (aliased import).
- **Approach:** Fix all `DBus.typeName()` references in DESIGN.md to use `DBusQML.typeName()`.
- **Effort:** ~5 min

### #13 DESIGN.md architecture section is inaccurate
- **What:** Claims dynamic methods use `qt_metacall` override and dynamic `QMetaSignal` generation. Actual implementation uses JS closures stored in `QQmlPropertyMap` and a single fixed slot.
- **Approach:** Rewrite the relevant DESIGN.md sections to describe the actual architecture.
- **Effort:** ~15 min

### #14 Document the signal-handler context limitation
- **What:** Inline signal handlers on DBus elements can't call JS functions. Not documented anywhere.
- **Approach:** Add a "Known Limitations" section to API.md.
- **Effort:** ~10 min

---

## Phase 4 — Example Cleanup & Consistency

### #15 Remove dead `import DBus 1.0 as DBusQML` (5 files)
- **Files:** `KeyboardLayout.qml`, `ServiceMonitor.qml`, `KDEConnect.qml`, `SystemdManager.qml`, `PowerControl.qml`
- **Approach:** Remove the unused import line. Verify no code uses the alias.
- **Effort:** ~10 min

### #16 Fix `parent.width` in BatteryMonitor delegate
- **What:** `BatteryMonitor.qml:93,100,128` use `parent.width` inside ListView delegate → risky in Qt 6.
- **Approach:** Replace with `deviceList.width`.
- **Effort:** ~5 min

### #17 Replace inline close buttons with shared `CloseButton`
- **Files:** `KDEConnect.qml`, `PortalDemo.qml`
- **Approach:** Replace the inline `Text { ... MouseArea { ... onClicked: Qt.quit() } }` with `CloseButton {}`.
- **Effort:** ~15 min

### #18 Add missing `status === 3` comments
- **Files:** `MprisPlayer.qml`, `NetworkMonitor.qml`
- **Approach:** Add `// Error` comment after each `status === 3` check.
- **Effort:** ~5 min

### #19 Clean up leftovers
- **Files:**
  - `ListNames.qml:92` — remove `console.error` debug line
  - `KeyboardLayout.qml:183-185` — remove empty `Component.onCompleted` with only a comment
  - Various files: trailing blank lines before `}`
- **Approach:** Simple edits per file.
- **Effort:** ~10 min

### #20 Standardize copy-pattern across all examples
- **What:** Some files use slightly different indentation or formatting of the copy pattern.
- **Approach:** Ensure every file with click-to-copy uses the same 3-line pattern and has `TextEdit { id: clipBoard }`.
- **Effort:** ~10 min

---

## Summary

| Phase | Items | Risk | Effort |
|-------|-------|------|--------|
| 1 — Runtime Bugs | #1–4 | High | ~3–6h |
| 2 — Library Hardening | #5–9 | Medium | ~1.5h |
| 3 — Docs | #10–14 | Low (docs) | ~1h |
| 4 — Example Cleanup | #15–20 | Low (cosmetic) | ~1h |
| **Total** | | | **~6.5–9.5h** |
