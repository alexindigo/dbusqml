import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    id: root
    visible: true
    width: 520
    height: 380
    title: "DBus — Caffeine (Prevent Sleep)"

    property var inhibitCookie: 0
    property bool inhibited: false
    property int remaining: 30
    property int selectedDuration: 30
    property var durations: [30, 60, 180, 300, 600, 900, 1800, 3600, 7200]
    property var labels: ["30s", "1m", "3m", "5m", "10m", "15m", "30m", "1h", "2h"]

    Timer {
        id: countdown
        interval: 1000
        repeat: true
        running: inhibited
        onTriggered: {
            remaining -= 1
            if (remaining <= 0) uninhibit()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            text: "Caffeine"
            font.bold: true
            font.pixelSize: 22
        }

        Label {
            text: "Prevent the screen from turning off and system from sleeping"
            color: "#888"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            radius: 12
            color: inhibited ? "#fff3e0" : "#e8f5e9"
            border.color: inhibited ? "#ffb74d" : "#81c784"

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 6

                Text {
                    text: inhibited ? "\u2615 Staying Awake" : "\u2705 Screen Saver Active"
                    font.bold: true
                    font.pixelSize: 16
                    color: inhibited ? "#e65100" : "#2e7d32"
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: "Inhibit expires in " + remaining + "s"
                    visible: inhibited
                    font.pixelSize: 12
                    color: "#888"
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }

        ColumnLayout {
            spacing: 4
            Layout.fillWidth: true

            Flow {
                spacing: 6
                visible: !inhibited
                Layout.alignment: Qt.AlignHCenter

                Repeater {
                    model: root.durations.length

                    Button {
                        required property int index
                        text: root.labels[index]
                        flat: true
                        checkable: true
                        checked: selectedDuration === root.durations[index]
                        onClicked: {
                            selectedDuration = root.durations[index]
                            inhibit()
                        }
                        background: Rectangle {
                            radius: 6
                            color: parent.checked ? "#e0e0e0" : "transparent"
                            border.color: parent.checked ? "#bbb" : "#ddd"
                        }
                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 12
                            color: parent.checked ? "#333" : "#666"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Layout.preferredHeight: 32
                        Layout.preferredWidth: 56
                    }
                }
            }

            Button {
                text: inhibited ? "Stop Early" : "Stay Awake for " + formatDuration(selectedDuration)
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                onClicked: {
                    if (inhibited) {
                        uninhibit()
                    } else {
                        inhibit()
                    }
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

    function formatDuration(secs) {
        if (secs < 60) return secs + "s"
        if (secs < 3600) return Math.floor(secs / 60) + "m"
        return Math.floor(secs / 3600) + "h"
    }

    function inhibit() {
        var reply = ssinh.inhibit(
            "dbusqml-caffeine",
            "User requested temporary sleep prevention (" + formatDuration(selectedDuration) + ")"
        )
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.lastError = "Failed to inhibit: " + reply.error.message; statusText.text = statusText.lastError
                return
            }
            inhibitCookie = reply.value
            inhibited = true
            remaining = selectedDuration
            statusText.text = "Staying awake for " + formatDuration(selectedDuration)
        })
    }

    function uninhibit() {
        inhibited = false
        if (inhibitCookie !== 0) {
            ssinh.unInhibit(inhibitCookie)
        }
        inhibitCookie = 0
        statusText.text = "Screen saver active again"
    }

    DBus {
        id: ssinh
        service: "org.freedesktop.ScreenSaver"
        path: "/org/freedesktop/ScreenSaver"
        iface: "org.freedesktop.ScreenSaver"
        connection: SessionBus
    }
    CloseButton {}
    TextEdit { id: clipBoard; x: -9999; y: -9999 }

}
