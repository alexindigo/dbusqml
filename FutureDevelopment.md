# Future Development

## QDBusContext — Caller Identification in Adaptor

`QDBusContext` provides access to the caller's D-Bus message inside a slot/method handler. Injects `calledFromDBus()`, `message()`, and `sendErrorReply()` into your adaptor class.

**Pros:**
- Know who's calling (unique name like `:1.42`) for authorization
- Send custom D-Bus error replies (not just return values)
- Access the raw `QDBusMessage` for introspection

**Cons:**
- Not needed when using `QDBusVirtualObject::handleMessage()` — we already have the message
- Adds macro-based boilerplate via `Q_CLASSINFO`
- Nemo doesn't use it — they rely on handleMessage directly

**Obstacles:**
- Our `DBusAdaptor` already receives the `QDBusMessage` in `handleMessage()`. We have full access to `.service()` and other fields.
- To expose caller info to QML, we'd need to store the last message and expose it as a property (e.g., `adaptor.lastCallerService`)
- No real need — `handleMessage()` already provides everything QDBusContext does

---

## QDBusServer — Private Bus Hosting

`QDBusServer` hosts a private D-Bus bus daemon inside your process. Other processes connect via a custom address like `unix:path=/tmp/my-bus`.

**Pros:**
- Full control over bus security — no dependency on session/system bus
- No risk of interfering with other applications on the user's bus
- Useful for sandboxed communication between your own processes

**Cons:**
- Adds complexity — you must manage bus lifecycle and peer connections
- Requires out-of-band channel to share the bus address with peers
- Socket/file-system based — may conflict with containerization

**Obstacles:**
- `QDBusServer` is a server-side API; clients would need `DBus.connectToBus(address)` to connect (already implemented)
- Our `DBusProxy` and `DBusAdaptor` already support custom `connection` properties, so integrating with a private bus is straightforward
- No server-side message routing needed — `QDBusServer` handles that internally
- Could be exposed as a `privateBus` property or a static `createPrivateBus()` method

---

## QDBusUnixFileDescriptor — FD Passing

`QDBusUnixFileDescriptor` embeds a Unix file descriptor (socket, pipe, device FD) in a D-Bus message for transfer to another process. Available on Unix only.

**Pros:**
- Efficient — avoids serializing large data through D-Bus
- Enables passing real kernel objects (fds, pipes, shared memory) between processes
- Standard D-Bus feature — supported by all major D-Bus implementations

**Cons:**
- Unix-only — not available on Windows
- Complex lifetime management — the sender must keep the FD open until the message is sent
- Not directly usable from QML — requires C++ wrapper to create/extract FDs

**Obstacles:**
- Our `DBus::Variant` and other type wrappers would need new `unixFd` member to support `h` type signature
- `toDbusVariant()` in `dbusconnection.cpp` would need a case for `QDBusUnixFileDescriptor`
- QML can't create or consume file descriptors directly — users would need a C++ helper to prepare the FD on the sender side and handle it on the receiver side
- Example use case: a portal app that opens a device file and passes the FD to a sandboxed client
