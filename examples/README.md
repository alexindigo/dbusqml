# DBus QML Examples

To run an example, use `qml6` with the `DBus` module's build directory in the import path:

```bash
export QML_IMPORT_PATH=/path/to/dbusqml/build-release
qml6 examples/simple/ListNames.qml
```

If the module is installed system-wide, the import path can be omitted:

```bash
qml6 examples/simple/ListNames.qml
```

## Examples by Scenario

### Simple

| Example | What it demonstrates |
| :--- | :--- |
| `simple/ListNames.qml` | `SessionBus.asyncCall()` + `DBusPendingReply` to list all registered D-Bus services |
| `simple/KeyboardLayout.qml` | `SystemBus.asyncCall()` + `org.freedesktop.DBus.Properties.GetAll` to read the current X11 keyboard layout from `org.freedesktop.locale1` |
| `simple/PortalSettings.qml` | `SessionBus.asyncCall()` with `org.freedesktop.portal.Settings.ReadOne` to read the system color scheme (dark/light mode) and accent color preference — no shell commands needed |
| `simple/NetworkMonitor.qml` | `SessionBus.asyncCall()` with `org.freedesktop.portal.NetworkMonitor` — check network availability, metered status, connectivity level, and host reachability — replaces `ping`/`curl` subprocess calls |

### Intermediate

| Example | What it demonstrates |
| :--- | :--- |
| `intermediate/Notify.qml` | `DBus` proxy element, `call()`, `signalReceived` signal for `ActionInvoked`/`NotificationClosed` |
| `intermediate/ServiceMonitor.qml` | `ListNames` + `NameHasOwner` + `NameOwnerChanged` signal monitoring — recreates the noctalia service discovery pattern natively |
| `intermediate/BatteryMonitor.qml` | `SystemBus.asyncCall()`, `org.freedesktop.DBus.Properties.GetAll`, UPower device enumeration — recreates the noctalia UPower battery monitoring pattern natively |
| `intermediate/ScreenSaver.qml` | `org.freedesktop.ScreenSaver.Inhibit`/`UnInhibit` — prevent screen blanking. Replaces the `dbus-send --type=signal` and gnome-screensaver-command subprocess approach |

### Advanced

| Example | What it demonstrates |
| :--- | :--- |
| `advanced/KDEConnect.qml` | Multi-step device discovery, per-device property queries, method calls (ring, share, pair/unpair) — recreates the noctalia KDE Connect pattern natively |
| `advanced/MprisPlayer.qml` | Player discovery via `ListNames`, dynamic `DBusProxy` properties, playback controls, `PropertiesChanged` signal handling |
| `advanced/PowerControl.qml` | `SystemBus.asyncCall()` with `org.freedesktop.login1` for suspend, hibernate, reboot, power off — recreates the noctalia power management pattern natively |
| `advanced/SystemdManager.qml` | `SystemBus.asyncCall()` with `org.freedesktop.systemd1.Manager` — look up units, check ActiveState/SubState/LoadState, start/stop/restart services. Replaces `systemctl` subprocess calls |

## What makes these examples different from noctalia

Noctalia's QML code makes all D-Bus calls by spawning subprocess commands (`busctl`, `gdbus`, `dbus-send`, `dbus-monitor`) and parsing their output. These examples do the same thing using the `DBus` modules native QML API — no subprocesses, no parsing, just direct type-safe D-Bus calls from QML.
