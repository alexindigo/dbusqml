import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 420
    height: 300
    title: "DBus — Network Monitor"

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
                Text { id: availableText; text: "—" }

                Text { text: "Metered:"; font.bold: true }
                Text { id: meteredText; text: "—" }

                Text { text: "Connectivity:"; font.bold: true }
                Text { id: connectivityText; text: "—" }
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
                onClicked: canReach(hostInput.text, 80)
            }
        }

        Text {
            id: pingResult
            color: "#888"
            font.italic: true
        }

        Item { Layout.fillHeight: true }

        Button {
            text: "Refresh"
            Layout.alignment: Qt.AlignHCenter
            onClicked: fetchStatus()
        }
    }

    function fetchStatus() {
        var r1 = net.GetAvailable()
        r1.finished.connect(function() {
            if (r1.isError) return
            availableText.text = r1.value ? "Yes" : "No"
        })

        var r2 = net.GetMetered()
        r2.finished.connect(function() {
            if (r2.isError) return
            meteredText.text = r2.value ? "Yes" : "No"
        })

        var r3 = net.GetConnectivity()
        r3.finished.connect(function() {
            if (r3.isError) return
            var labels = ["Local", "Limited", "Captive Portal", "Full"]
            connectivityText.text = labels[r3.value] || "Unknown (" + r3.value + ")"
        })
    }

    function canReach(host, port) {
        var reply = net.CanReach(host, port)
        reply.finished.connect(function() {
            if (reply.isError) {
                pingResult.text = "Error: " + reply.error.message
                return
            }
            pingResult.text = host + ":" + port + " is " + (reply.value ? "reachable" : "not reachable")
        })
    }

    Component.onCompleted: fetchStatus()

    // Dynamic proxy — GetAvailable(), GetMetered(), etc. are callable directly.
    DBusQML.DBus {
        id: net
        service: "org.freedesktop.portal.Desktop"
        path: "/org/freedesktop/portal/desktop"
        iface: "org.freedesktop.portal.NetworkMonitor"
    }
    Text {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        text: "✕"
        font.pixelSize: 16
        color: mouseArea.containsMouse ? "black" : "#999"

        MouseArea {
            id: mouseArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: Qt.quit()
        }
    }

}
