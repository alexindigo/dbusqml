# D-Bus Interface Type Catalog

dbusqml can call D-Bus methods like `proxy.playPause()` because it knows what
methods each interface exposes. Normally it discovers them by calling
`org.freedesktop.DBus.Introspectable.Introspect()` on the remote service.

Some services return empty or incomplete XML from `Introspect()` (notably
Chromium-based apps for MPRIS). For those, dbusqml consults a **type catalog**
of interface descriptors loaded from XML files.

## Where the library looks

In priority order (highest first):

1. `$DBUSQML_TYPES_PATH` — colon-separated list of directories (env var, for
   tests / containers). Optional.
2. `$XDG_CONFIG_HOME/dbusqml/types/*.xml` — user's own definitions
   (usually `~/.config/dbusqml/types/`).
3. `$XDG_DATA_DIRS/*/dbusqml/types/*.xml` — system-wide extensions.
4. `qrc:/dbusqml/types/*.xml` — descriptors bundled with the library.

A user file for an interface always overrides the bundled version. Bundled
files are inside the library binary and are not overwritten by upgrades of
your own configuration.

## File format

Standard D-Bus introspection XML. You can copy any freedesktop.org spec
verbatim. Example (`~/.config/dbusqml/types/com.example.MyService.xml`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE node PUBLIC
    "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
    "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
    <interface name="com.example.MyService">
        <method name="DoTheThing">
            <arg direction="in" type="s" name="input"/>
            <arg direction="out" type="i" name="result"/>
        </method>
        <signal name="ThingHappened">
            <arg type="s" name="what"/>
        </signal>
    </interface>
</node>
```

A file may contain multiple `<interface>` blocks. Interface names are matched
independently of the filename.

## Bundled catalog

dbusqml ships descriptors for these interfaces:

- `org.mpris.MediaPlayer2` and `org.mpris.MediaPlayer2.Player`
- `org.freedesktop.Notifications`
- `org.freedesktop.ScreenSaver`
- `org.freedesktop.login1.Manager`
- `org.freedesktop.portal.Settings`
- `org.freedesktop.impl.portal.Settings`
- `org.freedesktop.impl.portal.FileChooser`
- `org.freedesktop.impl.portal.Request`
- `org.freedesktop.portal.NetworkMonitor`
- `org.freedesktop.UPower`
- `org.freedesktop.NetworkManager`
- `org.freedesktop.NetworkManager.Device`
- `org.freedesktop.NetworkManager.Device.Wired`
- `org.freedesktop.NetworkManager.Device.Wireless`
- `org.freedesktop.NetworkManager.Connection.Active`
- `org.freedesktop.NetworkManager.IP4Config`
- `org.freedesktop.NetworkManager.IP6Config`
- `org.freedesktop.NetworkManager.AccessPoint`
- `org.freedesktop.NetworkManager.Settings`
- `org.freedesktop.NetworkManager.Settings.Connection`

## Overriding a bundled interface

Drop an XML file at `~/.config/dbusqml/types/` declaring the same
`<interface name="...">`. Your file entirely replaces the bundled version for
that interface. You are responsible for keeping it current.

## Reloading during development

```qml
DBus.reloadTypes()
```

Rescans all search paths. Existing proxies are not re-introspected — you must
recreate them (or change `iface`) to pick up new methods.

## Merge with live introspection

When a proxy connects to a service, the catalog spec is unioned with whatever
the service reports via `Introspect()`. Where both name the same method, the
service's arg types are preferred (the server is authoritative when it
speaks); the catalog only fills in what the server didn't report.

## Serving side: declared reply signatures

The catalog is also consulted on the **serving** side. When a `DBusAdaptor`
serves an interface whose method declares `<arg direction="out">` types, those
declared signatures drive the reply marshaling. This is what lets a
`org.freedesktop.impl.portal.Settings` backend return `ReadAll` as
`a{sa{sv}}` — the shape xdg-desktop-portal requires — from a plain QML object
literal, with no per-app override. Explicit `_signatures` overrides beat the
catalog; only malformed signatures, invalid values, or pool exhaustion warn
and fall back to inference (every well-formed element signature is
producible via the signature-slot pool).
