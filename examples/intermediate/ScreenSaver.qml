import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 400
    height: 250
    title: "DBus — ScreenSaver Control"

    property var inhibitCookie: 0
    property bool inhibited: false

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            text: "ScreenSaver Inhibition"
            font.bold: true
            font.pixelSize: 20
        }

        Label {
            text: "Prevent the screen from blanking"
            color: "#888"
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            radius: 12
            color: inhibited ? "#fff3e0" : "#e8f5e9"
            border.color: inhibited ? "#ffb74d" : "#81c784"

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 4

                Text {
                    text: inhibited ? "⚠ Screen Blanking Inhibited" : "✓ Screen Blanking Active"
                    font.bold: true
                    font.pixelSize: 16
                    color: inhibited ? "#e65100" : "#2e7d32"
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: inhibited ? "Your screen will not turn off" : "Screen will turn off normally"
                    font.pixelSize: 12
                    color: "#888"
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }

        Button {
            text: inhibited ? "Uninhibit" : "Inhibit for 30 seconds"
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            onClicked: {
                if (inhibited) {
                    uninhibit()
                } else {
                    inhibit()
                }
            }
        }

        Text {
            id: statusText
            color: "#888"
            font.italic: true
        }
    }

    function inhibit() {
        var reply = screensaver.Inhibit(
            "dbusqml-example",
            "Example demonstrating ScreenSaver API"
        )
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.text = "Failed to inhibit: " + reply.error.message
                return
            }
            inhibitCookie = reply.value
            inhibited = true
            statusText.text = "ScreenSaver inhibited (cookie: " + inhibitCookie + ")"
        })
    }

    function uninhibit() {
        screensaver.UnInhibit(inhibitCookie)
        inhibitCookie = 0
        inhibited = false
        statusText.text = "ScreenSaver unhibited"
    }

    // Dynamic proxy — UnInhibit() is callable directly after introspection.
    DBusQML.DBus {
        id: screensaver
        service: "org.freedesktop.ScreenSaver"
        path: "/org/freedesktop/ScreenSaver"
        iface: "org.freedesktop.ScreenSaver"
        connection: DBusQML.SessionBus
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
