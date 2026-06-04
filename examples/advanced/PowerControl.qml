import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 400
    height: 300
    title: "DBus — Power Control"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        Label {
            text: "System Power Control"
            font.bold: true
            font.pixelSize: 20
        }

        Label {
            text: "Uses org.freedesktop.login1 on the system bus"
            color: "#888"
            font.italic: true
        }

        Item { Layout.fillHeight: true }

        Button {
            text: "⏻  Suspend"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            font.pixelSize: 16
            onClicked: {
                statusText.text = "Calling Suspend..."
                logind.Suspend(true)
            }
        }

        Button {
            text: "❄  Hibernate"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            font.pixelSize: 16
            onClicked: {
                statusText.text = "Calling Hibernate..."
                logind.Hibernate(true)
            }
        }

        Button {
            text: "↻  Reboot"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            font.pixelSize: 16
            onClicked: {
                statusText.text = "Calling Reboot..."
                logind.Reboot(true)
            }
        }

        Button {
            text: "⏻  Power Off"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            font.pixelSize: 16
            onClicked: {
                statusText.text = "Calling Power Off..."
                logind.PowerOff(true)
            }
        }

        Item { Layout.fillHeight: true }

        Text {
            id: statusText
            color: "#666"
            font.italic: true
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        Button {
            text: "Check if logind is available"
            Layout.alignment: Qt.AlignHCenter
            onClicked: {
                var reply = sysd.NameHasOwner("org.freedesktop.login1")
                reply.finished.connect(function() {
                    statusText.text = reply.value
                        ? "logind is available"
                        : "logind is not available (try installing systemd-logind)"
                })
            }
        }
    }

    // Dynamic proxy — after introspection, Suspend(), Hibernate(),
    // Reboot(), and PowerOff() are callable directly.

    // Bus daemon proxy (system bus) — NameHasOwner() is callable directly.
    DBusQML.DBus {
        id: sysd
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
        connection: DBusQML.SystemBus
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
