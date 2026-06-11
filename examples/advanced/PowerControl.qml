import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0
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
                logind.suspend(true)
            }
        }

        Button {
            text: "❄  Hibernate"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            font.pixelSize: 16
            onClicked: {
                statusText.text = "Calling Hibernate..."
                logind.hibernate(true)
            }
        }

        Button {
            text: "↻  Reboot"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            font.pixelSize: 16
            onClicked: {
                statusText.text = "Calling Reboot..."
                logind.reboot(true)
            }
        }

        Button {
            text: "⏻  Power Off"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            font.pixelSize: 16
            onClicked: {
                statusText.text = "Calling Power Off..."
                logind.powerOff(true)
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
                var reply = sysd.nameHasOwner("org.freedesktop.login1")
                reply.finished.connect(function() {
                    statusText.text = reply.value
                        ? "logind is available"
                        : "logind is not available (try installing systemd-logind)"
                })
            }
        }
    }

    // Dynamic proxy — Suspend(), Hibernate(), Reboot(), PowerOff() are callable directly.
    DBus {
        id: logind
        service: "org.freedesktop.login1"
        path: "/org/freedesktop/login1"
        iface: "org.freedesktop.login1.Manager"
        connection: SystemBus
    }

    // Bus daemon proxy — NameHasOwner() is callable directly.
    DBus {
        id: sysd
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
        connection: SystemBus
    }
    CloseButton {}
    
}
