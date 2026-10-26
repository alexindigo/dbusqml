# dbusqml — D-Bus QML Bridge

## What

A standalone QML module that exposes D-Bus (session and system bus) to QML
applications. Zero KDE dependencies; uses Qt Core, Qml and DBus.

Pattern follows `mpvqml`: a thin C++ Qt6 QML plugin that wraps existing Qt APIs into QML-friendly types.

## Why

The only existing D-Bus QML module is KDE's `org.kde.plasma.workspace.dbus`, which depends on `plasma-workspace` (55 MiB, half of KDE). Not suitable for standalone QML apps on Niri/Hyprland/etc.

## Module: `import DBus 1.0`

QML blocks below are complete files. JavaScript excerpts run inside a QML
handler with the named proxy/imports supplied by the surrounding component.

## Types

### `busType` (enum)

| Value | Description |
|-------|-------------|
| `busType.Session` | Session bus |
| `busType.System` | System bus |

### `DBusConnection`

A connection object. Used for raw `asyncCall()` when you don't need the rich object wrapper.

```js
const conn = SessionBus.connectToBus("unix:path=/run/user/1000/example-bus")
if (conn) {
    const reply = conn.asyncCall({
        service: "org.freedesktop.DBus",
        path: "/org/freedesktop/DBus",
        iface: "org.freedesktop.DBus",
        member: "ListNames",
        arguments: []
    })
    reply.finished.connect(function() {
        if (reply.isError)
            console.error(reply.error.name, reply.error.message)
        else
            console.log(reply.value)
    })
}
```

### `DBusMessage` (value type)

```json
{
    "service": "org.freedesktop.DBus",
    "path": "/org/freedesktop/DBus",
    "iface": "org.freedesktop.DBus",
    "member": "NameHasOwner",
    "signature": "s",
    "arguments": ["org.freedesktop.DBus"]
}
```

Construct from a map or set properties individually.

### `DBusPendingReply`

```js
const reply = SessionBus.asyncCall({
    service: "org.freedesktop.DBus",
    path: "/org/freedesktop/DBus",
    iface: "org.freedesktop.DBus",
    member: "ListNames",
    arguments: []
})
reply.finished.connect(() => {
    if (reply.isError) console.error(reply.error.name, reply.error.message)
    else console.log(reply.value)
})
```

Also supports a promise-style callback overload, not a JavaScript Promise:
`asyncCall(message, resolve, reject)`. Resolve receives the first output
argument; a pending reply's `values` exposes every output argument.

### `DBus` — the rich proxy element

Set `service`, `path`, `iface` and optionally `connection` (defaults to session bus). When `iface` is set, introspection is called to discover:

**Properties** — become cached QML properties under camelCase names:
```qml
import QtQuick
import DBus 1.0

Item {
    readonly property var percentage: battery["percentage"]
    DBus {
        id: battery
        connection: SystemBus
        service: "org.freedesktop.UPower"
        path: "/org/freedesktop/UPower/devices/DisplayDevice"
        iface: "org.freedesktop.UPower.Device"
    }
}
```

The initial `Properties.GetAll` populates the cache; known properties start
as `null`. Properties auto-update via `PropertiesChanged`; reading a
binding does not issue a new Get each time.

**Methods** — dynamically exposed as callable properties after introspection.
For a proxy configured for the session-bus daemon:
```js
proxy.listNames()                           // wire method ListNames
proxy.nameHasOwner("org.freedesktop.DBus")   // wire method NameHasOwner
proxy.getNameOwner("org.freedesktop.DBus")   // wire method GetNameOwner
```

D-Bus method names are PascalCase; QML exposes them in camelCase. The
original PascalCase name is what gets sent on the wire.

Also available via generic `call(method, args)`:
```js
proxy.call("ListNames")
proxy.call("NameHasOwner", ["org.freedesktop.DBus"])
```

**Signals** — received from the configured service's current owner via
`signalReceived`; use `Connections` for surrounding QML-scope helpers:
```qml
import QtQuick
import DBus 1.0

Item {
    DBus {
        id: player
        service: "org.mpris.MediaPlayer2.myplayer"
        path: "/org/mpris/MediaPlayer2"
        iface: "org.mpris.MediaPlayer2.Player"
    }
    Connections {
        target: player
        function onSignalReceived(name, args) {
            if (name === "Seeked")
                console.log("New position:", args[0])
        }
    }
}
```

### `DBusError` (value type)

```json
{
    "isValid": true,
    "name": "org.freedesktop.DBus.Error.Failed",
    "message": "The operation failed"
}
```

### D-Bus Type Wrappers

The module is imported once for the `DBus` element and again with an alias
for the value-type constructors, because the plain module name is shadowed
by the element:

```qml
import QtQuick
import DBus 1.0
import DBus 1.0 as DBusQML

Item {
    property var examples: [
        new DBusQML.uint32(42),
        new DBusQML.uint64(18446744073),
        new DBusQML.string("hello"),
        new DBusQML.boolean(true),
        new DBusQML.double(3.14),
        new DBusQML.objectPath("/org/freedesktop/UPower"),
        new DBusQML.variant("any value"),
        new DBusQML.dict({ key: "value" })
    ]
    Component.onCompleted: {
        const fd = DBusUtils.openFd("/etc/hostname", "r")
        if (!fd.valid) return
        const number = fd.take()
        if (number >= 3) {
            const copy = new DBusQML.unixFd(number) // owns another duplicate
            DBusUtils.closeFd(number)
            copy.close()
        }
        fd.close()
    }
}
```

Used when Qt's auto-conversion doesn't produce the correct D-Bus type signature.

`DBusQML.unixFd` is the exception: it is the only fd value, both directions.
It owns its descriptor through a state shared by every copy — `close()` is
the contract, the last copy's destruction (GC for values that reached JS)
is the fallback, and a process-wide pressure valve queues a collection when
wrapper-owned fds pile up. A number is never inferred as an fd from a
declared `h`: every send site goes through one fd rule (`wireFd`, the send
boundary reports `DBusFdFailure`), and every decoded `h` becomes a fresh
`unixFd` holding its own `dup()`.
Qt can construct a wrapper at an application-declared `unixFd`-typed
property/parameter, and constructor-only engines are not registered for
the pressure valve. These accepted limits are detailed in
[the fd reference](API.md#qt-construction-and-pressure-valve-scope).

Qt's `ListModel` copies these wrappers' exposed properties into model data;
it does not preserve their type, methods or, for `unixFd`, descriptor. See
API.md "ListModel and dbusqml values" for the storage alternatives.

## Architecture

```
Bus (session or system)
  │
  ├── Connection unique name (:1.42)
  │
  ├── Well-known service (org.freedesktop.DBus)
  │     └── Object path (/org/freedesktop/DBus)
  │           └── Interface (org.freedesktop.DBus)
  │                 ├── Methods: ListNames, GetNameOwner, ...
  │                 └── Signals: NameOwnerChanged
  │
  ├── Well-known service (org.kde.kdeconnect)
  │     ├── Path (/modules/kdeconnect)
  │     │     └── Iface (org.kde.kdeconnect.daemon)
  │     │           ├── Methods: devices(), ...
  │     │           └── Properties: ...
  │     └── Path (/modules/kdeconnect/devices/{id})
  │           └── Iface (org.kde.kdeconnect.device)
  │                 ├── Methods: ...
  │                 └── Properties: name, reachable, ...
```

A D-Bus **service** owns a well-known name (like `org.freedesktop.portal.Desktop`). Under that service, there can be multiple **object paths** (like `/org/freedesktop/portal/desktop`). Each path can expose multiple **interfaces** (like `org.freedesktop.portal.Settings`).

Registering means:
1. **Service** (optional) — Claim a well-known name on the bus. Without it, connections are reachable only by their unique name (`:1.42`).
2. **Path** — Register an object handler for a specific path. This is required to receive method calls.
3. **Interface** — Part of the registered object's introspection XML. Defines which methods, signals, and properties the object exposes.

The sender's unique name is included in every D-Bus message. On an incoming
message, Qt exposes it through `QDBusMessage::service()`; the adaptor's QML
dispatch API exposes `callerService()`.

## Registration (server-side)

### `DBusAdaptor`

Inverse of the `DBus` proxy element. Registers a D-Bus object on the bus and maps QML properties/functions/signals to D-Bus properties/methods/signals. Shipped and covered by tests.

```qml
import DBus 1.0
import DBus 1.0 as DBusQML

DBusAdaptor {
    id: portal
    service: "org.freedesktop.impl.portal.atmosphera"  // optional — omit for unique-name-only
    path: "/org/freedesktop/portal/desktop"
    iface: "org.freedesktop.impl.portal.Settings"

    // Property access comes from the catalog, then QML writability.
    property int preferredDarkMode: 0
    property var _settings: ({ "color-scheme": 1 })

    // QML functions → D-Bus methods (lowercase-initial: QML forbids
    // uppercase-initial function names)
    function readOne(ns, key) {
        return new DBusQML.variant(portal._settings[key] ?? "")
    }

    // QML signals → D-Bus signals
    // The relay takes QVariant parameters; the catalog supplies wire ssv.
    signal settingChanged(ns: var, key: var, value: var)
}
```

The adaptor:
- Claims `service` via the dispatcher service-claim registry (first claimant
  wins; co-located adaptors share the name; `allowReplacement` /
  `replaceExisting` / `queueOnBusy` opt into daemon-mediated transfer)
- Registers on `path` via `registerVirtualObject()` with a shared
  `DBusPathDispatcher` (one virtual object per path; co-located adaptors
  route through it in attach order)
- Exposes `iface` methods from QML functions
- Exposes `iface` signals from QML signals
- Exposes `iface` properties from QML properties
- Handles incoming method calls: `QDBusVirtualObject::handleMessage()`
  dispatches to the matching QML function (explicit `_members` alias →
  exact → first-char fold)
- Handles property get/set via the Properties interface dispatch in
  `handleMessage()` (Get/GetAll/Set with catalog-typed XML)
- Resolves property identity, catalog type/access and advertised name once per
  operation through a shared served-property enumeration. The catalog key is
  the advertised wire name (including `_members` aliases); privacy, library
  configuration and `objectName` exclusions have one owner. XML, Get, GetAll,
  Set and notify relays consume that representation. Catalog `read` refuses
  Set; `write` refuses Get and never appears in GetAll or PropertiesChanged.
- Emits signals via `QDBusConnection::send(QDBusMessage::createSignal(...))`

Multiple interfaces on the same path use multiple `DBusAdaptor` instances with the same `service` and `path` but different `iface`.

```
QML
  │
  ├── DBus (rich proxy — subclass of QQmlPropertyMap)
  │     ├── Introspect on iface set → discover methods, signals, properties
  │     ├── Method dispatch
  │     │     ├── Introspection XML gives {name → [arg-signatures]}.
  │     │     ├── Empty XML? The user-land catalog (types/*.xml + XDG
  │     │     │   drop-ins) fills in the gap.
  │     │     ├── For each method, an engine-side JS closure is created
  │     │     │   and stored in the property map under a camelCased name.
  │     │     │   The closure forwards to a per-proxy DbusMethodHelper
  │     │     │   (Q_INVOKABLE callMethod), which coerces each argument
  │     │     │   through toTypedDbusVariant and issues asyncCall.
  │     │     └── On re-introspection, prior method keys are cleared from
  │     │         the property map before the new set is installed, so an
  │     │         iface switch doesn't leave stale callables behind.
  │     ├── Properties → fetched via Properties.GetAll and inserted into
  │     │                 the property map; PropertiesChanged updates in
  │     │                 place. Nested a{sv}/a{ss} unwrapped to
  │     │                 QVariantMap/QVariantList so JS can traverse them.
  │     └── Signals   → QDBusConnection::connect on each discovered signal
  │                       name → emitted through signalReceived(name, args).
  │
  ├── DBusAdaptor (server-side, subclass of QDBusVirtualObject)
  │     ├── Builds introspection XML from Q_PROPERTY / method / signal
  │     │   metadata of the enclosing QML component (declared out-arg
  │     │   types via `declaredOutTypes`: explicit `_signatures` →
  │     │   catalog declaration → stable inference; catalog in-arg
  │     │   types; `_signals` signal arg types; `_members` wire-name
  │     │   aliases; folded signal wire names).
  │     ├── handleMessage dispatches to matching QML method via
  │     │   QJSValue::callWithInstance (arguments passed as native JS
  │     │   values through precision-safe conversion; throw-after-hold
  │     │   settles the held reply; RAII dispatch-context stack for
  │     │   re-entrant dispatches; send results checked).
  │     ├── Deferred replies via holdReply() → DBusHeldReply (send /
  │     │   sendError; exactly-one-reply-per-serial structural).
  │     ├── Co-located adaptors share one path dispatcher + service
  │     │   claim (tombstone/pending-state teardown markers; per-call
  │     │   Request adaptors + unregister() for the cancel lifecycle).
  │     └── Properties.Get / GetAll / Set served from QMetaProperty
  │         (+ PropertiesChanged emission on notify; declared signal
  │         types applied at emission; relay guard on relayed args).
  │
  ├── DBusConnection (raw wrapper)
  │     └── asyncCall(message) → QDBusPendingCallWatcher
  │           Promise overload: resolve receives the native reply value
  │           (containers unwrapped); reject receives { name, message }.
  │
  └── TypedValues → Q_GADGET value types with QML_CONSTRUCTIBLE_VALUE.
```

### Why JS closures instead of runtime QMetaMethod?

Attempting to synthesize new `Q_INVOKABLE` slots at runtime via
`QMetaObjectBuilder` was tried and rejected: the property-map integration
becomes fragile, and Qt's dynamic-meta machinery is private and unstable
across point releases. JS closures stored in `QQmlPropertyMap` are
callable from QML with no meta-object surgery and survive engine
lifecycle events cleanly.

### Why user-land catalog instead of introspection-only?

Some services (Chromium-based MPRIS players, minimal system daemons)
return an empty `<node></node>` from `Introspect()` but still implement
their documented interface. The catalog lets dbusqml call those services
by name without forcing the user to fall back to `proxy.call(...)`.
Two scan paths: descriptors bundled with the library (`qrc:/dbusqml/types/`)
plus user/system drop-ins (`$XDG_CONFIG_HOME/dbusqml/types/`,
`$XDG_DATA_DIRS/*/dbusqml/types/`, `$DBUSQML_TYPES_PATH` override) —
see `docs/TYPES.md`.

### Decode boundary

All incoming argument consumers share `decodeMessage`: the recursive
signature reader and native-container unwrapping carry one failure state and
the same protocol depth budget. A failed read stops the enclosing walk and
discards the whole message's decoded arguments. Replies settle as local
`Failed` errors, signals warn once and are dropped, and adaptor calls answer
`InvalidArgs` before dispatch or property mutation. `DBus::Struct` and the
string-array demarshaller reuse that reader rather than maintaining separate
container loops.

### Send boundary

All outgoing argument producers share `checkSendable`: per-argument
marshalability plus the aggregate message signature against the spec's
255-byte cap, evaluated before a message reaches QtDBus. Signature strings
themselves (introspected in-args, `_signatures` overrides, `variant(x,
sig)`, slot-pool registrations) are validated against the full "Valid
Signatures" rules — type codes, balanced non-empty structs, dict entries
only as array elements with basic-type keys, per-kind nesting ≤ 32,
encoded length ≤ 255 — by `validateSignature` at proxy type validation
and generic-writer/registration boundaries. Override splitting happens
before validation: an unsplittable proxy override warns and infers;
an invalid parsed proxy input type fails locally. Raw/adaptor marshalling
can warn and infer after a container writer refuses a type. The send
boundary checks the resulting values. See
[Shape Selection](API.md#shape-selection) for the exact path-specific rules.

Basic wire values have one producer, shared by the probe, staged writer,
recursive signature writer and top-level marshaller. It validates `g`
contents with the message-signature validator (including the 255-byte cap)
and `o` contents with one ASCII object-path predicate before constructing
Qt wrappers. Staged writes consume the exact produced types. Inferred
typed leaves use the same producer before a raw Struct can stream them.
A basic-value refusal propagates through containers as a non-marshalable
failure, never an inference fallback or a void reply. Empty `g` contents
are valid nested values; only an empty top-level `g` stays refused.

### XML boundary

Both XML parsers — the peer-introspection parser
(`parseDBusIntrospection`) and the local catalog loader — go through one
hardened reader (`DBusHardenedXmlReader`). Inputs larger than 4 MiB are
refused before parsing; a DTD is allowed (real libdbus `Introspect()`
replies carry one) but any entity declaration refuses the document at
the DTD token — before any content is scanned, so no entity expansion
can run. A refused document yields no data: the introspection parser
returns empty (the catalog/inference tiers take over), the catalog
loader discards the file whole; each path warns once.

## Dependencies (`DBus` proxy + `DBusAdaptor`)

| Dependency | Why |
|------------|-----|
| `Qt6::DBus` | Core D-Bus types and connection |
| `Qt6::Qml` | QML engine integration |

Zero KDE deps. Zero other deps.

## License

GPLv3
