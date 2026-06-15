import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    id: root
    visible: true
    width: 400
    height: 300
    title: "DBus — Portal Settings"

    property bool darkMode: false

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            text: "System Appearance"
            font.bold: true
            font.pixelSize: 20
        }

        Label {
            text: "Reads settings via org.freedesktop.portal.Settings"
            color: "#888"
            font.italic: true
            wrapMode: Text.WordWrap
        }

        GroupBox {
            title: "Color Scheme"
            Layout.fillWidth: true

            ColumnLayout {
                width: parent.width
                spacing: 12

                RowLayout {
                    spacing: 8
                    Rectangle {
                        width: 16; height: 16; radius: 3
                        color: darkMode ? "#333" : "#eee"
                        border.color: "#ccc"
                    }
                    Text { text: darkMode ? "Dark Mode" : "Light Mode"; font.pixelSize: 16; Layout.fillWidth: true }
                    Switch {
                        checked: darkMode
                        onToggled: {
                            darkMode = checked
                            statusText.text = "Dark mode: " + (darkMode ? "on" : "off")
                        }
                    }
                }

                Text {
                    id: errorText
                    color: "#e53935"
                    visible: text !== ""
                }
            }
        }

        Item { Layout.fillHeight: true }

        Button {
            text: "Read from portal"
            Layout.alignment: Qt.AlignHCenter
            onClicked: fetchSettings()
        }

        Text {
            id: statusText
            property string lastError: ""
            color: "#888"
            font.italic: true
            wrapMode: Text.WordWrap
            Layout.fillWidth: true

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

    function fetchSettings() {
        var reply = portal.readOne("org.freedesktop.appearance", "color-scheme")
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.lastError = "Portal not available (is xdg-desktop-portal running?)"; statusText.text = statusText.lastError
                return
            }
            darkMode = reply.value === 1
            statusText.text = "Color scheme: " + (darkMode ? "Dark (prefer)" : "Light (prefer)")
        })
    }

    Component.onCompleted: {
        portal.introspectionCompleted.connect(fetchSettings)
    }

    DBus {
        id: portal
        service: "org.freedesktop.portal.Desktop"
        path: "/org/freedesktop/portal/desktop"
        iface: "org.freedesktop.portal.Settings"

        onSignalReceived: function(name, args) {
            if (name === "SettingChanged" && args.length >= 3) {
                if (args[0] === "org.freedesktop.appearance" && args[1] === "color-scheme") {
                    darkMode = (args[2] === 1 || args[2] === "1")
                    statusText.text = "Portal signaled: color-scheme changed to "
                        + (darkMode ? "Dark" : "Light")
                }
            }
        }
    }
    CloseButton {}
    TextEdit { id: clipBoard; x: -9999; y: -9999 }

}
