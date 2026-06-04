import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
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
            placeholderText: "Notification body"
            text: "This notification was sent via D-Bus!"
        }

    Button {
        text: "Send Notification"
        Layout.alignment: Qt.AlignHCenter
        onClicked: {
            var appName = "dbusqml-example"
            var replacesId = 0
            var appIcon = ""
            var summary = summaryField.text
            var body = bodyField.text
            var actions = []
            var hints = ({})
            var expireTimeout = 5000

            statusText.text = "Sending notification..."
            notificationProxy.Notify(
                appName, replacesId, appIcon,
                summary, body, actions,
                hints, expireTimeout
            )
        }
    }

        Text {
            id: statusText
            text: "Click the button to send a notification"
            color: "#666"
        }

        Component.onCompleted: {
            notificationProxy.signalReceived.connect(function(name, args) {
                if (name === "NotificationClosed") {
                    statusText.text = "Notification was closed"
                } else if (name === "ActionInvoked") {
                    statusText.text = "Action invoked: " + args[1]
                }
            })
        }
    }

    DBus {
        id: notificationProxy
        service: "org.freedesktop.Notifications"
        path: "/org/freedesktop/Notifications"
        iface: "org.freedesktop.Notifications"
        connection: SessionBus
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
