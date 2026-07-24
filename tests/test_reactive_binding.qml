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

    // Intermediate readonly property — this is the pattern that should
    // be reactive but isn't (QQmlPropertyMap::insert() uses keyed
    // valueChanged signals, not per-property NOTIFY, so QML bindings
    // through an intermediate readonly property see 'undefined' at startup
    // and never re-evaluate when the D-Bus property later arrives).
    //
    // See TODO.md and dbus.cpp:fetchProperties() for the fix plan.
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
}
