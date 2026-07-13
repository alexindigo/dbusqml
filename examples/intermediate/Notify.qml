import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    visible: true
    width: 480
    height: 320
    title: "DBus — Send Notification"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label { text: "Send a desktop notification"; font.bold: true; font.pixelSize: 16 }

        TextField {
            id: summaryField
            Layout.fillWidth: true
            placeholderText: "Notification title"
            text: "Hello from DBus QML"
        }

        TextField {
            id: bodyField
            Layout.fillWidth: true
            placeholderText: "e.g. This notification was sent via D-Bus!"
        }

    Button {
        text: "Send Notification"
        Layout.alignment: Qt.AlignHCenter
        onClicked: {
            var appName = "dbusqml-example"
            var replacesId = 0
            var appIcon = ""
            var summary = summaryField.text || "Hello from DBus QML"
            var body = bodyField.text || "This notification was sent via D-Bus!"
            var actions = []
            var hints = ({})
            var expireTimeout = 5000

            statusText.text = "Sending notification..."
            var reply = notificationProxy.notify(
                appName, replacesId, appIcon,
                summary, body, actions,
                hints, expireTimeout
            )
            reply.finished.connect(function() {
                if (reply.isError) {
                    statusText.lastError = "Error: " + reply.error.message
                    statusText.text = statusText.lastError
                } else {
                    statusText.lastError = ""
                    statusText.text = "Notification sent (id: " + reply.value + ")"
                }
            })
        }
    }

        Text {
            id: statusText
            text: "Click the button to send a notification"
            color: "#666"
            Layout.fillWidth: true

            property string lastError: ""

            MouseArea {
                anchors.fill: parent
                cursorShape: statusText.text.indexOf("Error:") === 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    if (statusText.text.indexOf("Error:") === 0) {
                        clipBoard.text = statusText.text
                        clipBoard.selectAll()
                        clipBoard.copy()
                        statusText.text = "Copied!"
                        restoreTimer.start()
                    }
                }
            }

            Timer {
                id: restoreTimer
                interval: 2000
                onTriggered: {
                    if (statusText.lastError)
                        statusText.text = statusText.lastError
                }
            }
        }

    }

    DBus {
        id: notificationProxy
        service: "org.freedesktop.Notifications"
        path: "/org/freedesktop/Notifications"
        iface: "org.freedesktop.Notifications"
        connection: SessionBus

        onSignalReceived: function(name, args) {
            if (name === "NotificationClosed") {
                statusText.text = "Notification was closed"
            } else if (name === "ActionInvoked") {
                statusText.text = "Action invoked: " + args[1]
            }
        }
    }
    CloseButton {}
    TextEdit { id: clipBoard; x: -9999; y: -9999 }

}
