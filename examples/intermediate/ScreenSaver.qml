import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0
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
            property string lastError: ""
            color: "#888"
            font.italic: true

            MouseArea {
                anchors.fill: parent
                cursorShape: parent.text.indexOf("Error:") === 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    if (parent.text.indexOf("Error:") === 0) {
                        clipBoard.text = parent.text; clipBoard.selectAll(); clipBoard.copy()
                        parent.text = "Copied!"
                        restoreTimer.start()
                    }
                }
            }
            Timer {
                id: restoreTimer
                interval: 2000
                onTriggered: { if (parent.lastError) parent.text = parent.lastError }
            }
        }
    }

    function inhibit() {
        var reply = screensaver.inhibit(
            "dbusqml-example",
            "Example demonstrating ScreenSaver API"
        )
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.lastError = "Failed to inhibit: " + reply.error.message; statusText.text = statusText.lastError
                return
            }
            inhibitCookie = reply.value
            inhibited = true
            statusText.text = "ScreenSaver inhibited (cookie: " + inhibitCookie + ")"
        })
    }

    function uninhibit() {
        screensaver.unInhibit(inhibitCookie)
        inhibitCookie = 0
        inhibited = false
        statusText.text = "ScreenSaver unhibited"
    }

    // Dynamic proxy — UnInhibit() is callable directly after introspection.
    DBus {
        id: screensaver
        service: "org.freedesktop.ScreenSaver"
        path: "/org/freedesktop/ScreenSaver"
        iface: "org.freedesktop.ScreenSaver"
        connection: SessionBus
    }
    CloseButton {}
    TextEdit { id: clipBoard; x: -9999; y: -9999 }
    

}
