# Quickshell Reactive-Binding Comparison Spike

Diagnostic harness: verifies whether upstream Quickshell exhibits the
same reactive-binding-with-`undefined` behavior we see in dbusqml when
an intermediate `readonly property` layer wraps an async-populated
D-Bus property.

## Background

dbusqml's `DBusProxy` extends `QQmlPropertyMap`. Properties arrive
async via `Properties.GetAll` after introspection. QML bindings
established before a property exists resolve to `undefined` and do
NOT re-evaluate when `insert()` later adds the key. See
`tests/test_reactive_binding.qml` and `SessionTransfer.md` for details.

Quickshell uses `qdbusxml2cpp`-generated compile-time proxies with real
`Q_PROPERTY` + `Q_OBJECT_BINDABLE_PROPERTY`. Hypothesis: this avoids the
bug by construction because the property exists at binding-eval time.

This spike verifies the hypothesis empirically.

## Prerequisites

- Devcontainer image built (`docker build .devcontainer/ -t dbusqml-dev:local`)
- `org.freedesktop.NetworkManager` running on host system bus
- Host has WirelessEnabled property (any bool value works)

## Running

```
bash tests/qs-comparison/run.sh
```

The script:

1. Verifies `quickshell` is installed inside the container.
2. Verifies host's system DBus is reachable and NM is present.
3. Runs `shell.qml` under `quickshell` for ~4 seconds.
4. Prints t+0 and t+2 property values.

## Expected outcomes

**Outcome X** — `wrapper` transitions `false → true`
: QS's typed `Q_PROPERTY` approach avoids the bug. Confirms the
architectural distinction. dbusqml would need pre-declared properties
(catalog pre-populate, or private-header interception) to match.

**Outcome Y** — `wrapper` stays `false` throughout
: QS has the same bug. Escalates the finding — Qt's reactive-binding
engine has a broader limitation than currently understood.

**Outcome Z** — crash, missing module, or other error
: Investigate. Update this README with findings.
