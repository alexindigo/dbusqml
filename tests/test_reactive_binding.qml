import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import DBus 1.0

Window {
    visible: true
    width: 440
    height: 220
    title: "DBus — Reactive Binding Test (NM WirelessEnabled)"

    // Intermediate readonly property — the pattern that historically
    // broke with QQmlPropertyMap (auto-created properties resolved to
    // undefined and never re-evaluated when the real DBus value arrived).
    // Fixed since 0.3.0 (always-on): catalog/introspection pre-population
    // inserts null placeholders before QML bindings evaluate, so
    // QQmlPropertyMap's built-in reactivity handles subsequent updates.
    readonly property bool wifiEnabled: nm.wirelessEnabled === true

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        Label {
            text: "Reactive Property Binding Test"
            font.bold: true
            font.pixelSize: 16
        }

        Label {
            text: "Service: org.freedesktop.NetworkManager"
            color: "#888"
            font.italic: true
        }

        // Direct binding — works today (QML evaluates each access).
        Label {
            text: "Direct  : wirelessEnabled = " + nm.wirelessEnabled
            color: nm.wirelessEnabled ? "#4caf50" : "#e53935"
            font.family: "monospace"
            font.pixelSize: 14
        }

        // Intermediate readonly property — the bug pattern.
        Label {
            text: "Wrapper : wifiEnabled = " + wifiEnabled
            color: wifiEnabled ? "#4caf50" : "#e53935"
            font.family: "monospace"
            font.pixelSize: 14
        }

        // Status
        RowLayout {
            spacing: 4
            Label {
                text: nm.status === 1 ? "Introspecting..." :
                      nm.status === 2 ? "Ready" :
                      nm.status === 3 ? "Error" :
                      "Waiting..."
                color: "#888"
                font.italic: true
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "Both labels should match when Ready."
                color: "#aaa"
                font.pixelSize: 11
            }
        }
    }

    DBus {
        id: nm
        service: "org.freedesktop.NetworkManager"
        path: "/org/freedesktop/NetworkManager"
        iface: "org.freedesktop.NetworkManager"
        connection: SystemBus
    }

    Timer {
        interval: 2000; running: true
        onTriggered: {
            var direct = nm.wirelessEnabled
            var wrapper = wifiEnabled
            var ok = (direct === true && wrapper === true)
            console.warn("reactive-binding test: direct=" + direct +
                         " wrapper=" + wrapper +
                         " " + (ok ? "PASS" : "FAIL"))
            console.warn("  DBusProxy.reactiveBindingsSupported = " +
                         nm.reactiveBindingsSupported)
            if (!ok)
                console.error("Expected both direct and wrapper to be true")
            Qt.quit()
        }
    }
}
