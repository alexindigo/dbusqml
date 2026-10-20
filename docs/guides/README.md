# Use D-Bus from QML

Start with a call to the bus daemon. Then choose the task your application
needs. These guides target dbusqml 1.1.0 and use Qt 6.11+ QML syntax. The
QML import version remains `1.0`.

| Task | Guide | What you will build |
| :--- | :--- | :--- |
| Make your first call | [Getting started](getting-started.md) | A window that lists session-bus names and reports errors |
| Send the right arguments | [Calling methods](calling-methods.md) | Proxy calls, typed values, signature overrides and callbacks |
| Follow remote state | [Properties and signals](properties-and-signals.md) | Property bindings, signal subscriptions and object discovery |
| Provide a service | [Serving interfaces](serving-interfaces.md) | Methods, properties, signals and deferred replies |
| Transfer an open file | [File descriptors](file-descriptors.md) | Owning `unixFd` values, explicit close and migration |
| Keep and release results | [Lifetime and memory](lifetime-and-memory.md) | Reply ownership and value-preserving storage |
| Diagnose a failure | [Troubleshooting](troubleshooting.md) | Error names, warnings and the next thing to check |

Every `qml` code block in these guides is a complete file. Save it as, for
example, `Main.qml`, then run `qml6 Main.qml` (some distributions name the
runner `qml`). Service-specific examples need the named service on the
specified bus. A custom service example also needs its accompanying catalog
XML when stated.

Use the [API reference](../../API.md) for the complete surface and the
[runnable examples](../../examples/README.md) for larger windows. The
[type catalog guide](../TYPES.md) explains custom interface declarations.
