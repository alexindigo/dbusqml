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
- `org.freedesktop.portal.NetworkMonitor`
- `org.freedesktop.UPower`

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
