import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    id: root
    visible: true
    width: 480
    height: 360
    title: "DBus — Network Monitor"
    minimumWidth: 460
    minimumHeight: 320

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        Label {
            text: "Network Status"
            font.bold: true
            font.pixelSize: 20
        }

        Label {
            text: "Reads connectivity via org.freedesktop.portal.NetworkMonitor"
            color: "#888"
            font.italic: true
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        GroupBox {
            title: "Connectivity"
            Layout.fillWidth: true

            GridLayout {
                columns: 2
                width: parent.width
                rowSpacing: 8
                columnSpacing: 16

                Text { text: "Available:"; font.bold: true }
                Text { text: net.available !== undefined ? (net.available ? "Yes" : "No") : "—" }

                Text { text: "Metered:"; font.bold: true }
                Text { text: net.metered !== undefined ? (net.metered ? "Yes" : "No") : "—" }

                Text { text: "Connectivity:"; font.bold: true }
                Text {
                    text: net.connectivity !== undefined
                        ? (["Local", "Limited", "Captive Portal", "Full"][net.connectivity] || "Unknown")
                        : "—"
                }
            }
        }

        RowLayout {
            spacing: 8

            TextField {
                id: hostInput
                Layout.fillWidth: true
                placeholderText: "Hostname (e.g. google.com)"
                text: "google.com"
            }

            Button {
                text: "Ping"
                onClicked: root.canReach(hostInput.text, 80)
            }
        }

        Text {
            id: pingResult
            color: "#888"
            font.italic: true
        }

        Label {
            id: statusLabel
            color: "red"
            visible: false
        }

        Item { Layout.fillHeight: true }
    }

    function canReach(host, port) {
        var reply = net.canReach(host, port)
        reply.finished.connect(function() {
            if (reply.isError) {
                pingResult.text = "Error: " + reply.error.message
                return
            }
            pingResult.text = host + ":" + port + " is " + (reply.value ? "reachable" : "not reachable")
        })
    }

    // D-Bus properties (available, metered, connectivity) auto-bind as QML properties
    // after fetchProperties() runs on introspection completion.
    // canReach is a method — called dynamically.
    DBus {
        id: net
        service: "org.freedesktop.portal.Desktop"
        path: "/org/freedesktop/portal/desktop"
        iface: "org.freedesktop.portal.NetworkMonitor"

        onStatusChanged: {
            if (status === 3) {
                statusLabel.text = "Network portal not available"
                statusLabel.visible = true
            }
        }
    }
    CloseButton {}

}
