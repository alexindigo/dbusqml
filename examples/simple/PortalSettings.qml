import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0
import DBus 1.0 as DBusQML

Window {
    id: root
    visible: true
    width: 400
    height: 300
    title: "DBus — Portal Settings"

    property bool darkMode: false
    property bool accentColor: false

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
                            var value = darkMode ? 1 : 0
                            portal.emitSignal(
                                "SettingChanged",
                                ["org.freedesktop.appearance", "color-scheme", value]
                            )
                            statusText.text = "Emitted SettingChanged(color-scheme="
                                + (darkMode ? "1" : "0") + ") on D-Bus"
                        }
                    }
                }

                RowLayout {
                    spacing: 8
                    Rectangle {
                        width: 16; height: 16; radius: 3
                        color: accentColor ? "#1e88e5" : "#ccc"
                        border.color: "#ccc"
                    }
                    Text { text: accentColor ? "Accent color enabled" : "No accent color"; font.pixelSize: 16 }
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
            text: "Refresh from portal"
            Layout.alignment: Qt.AlignHCenter
            onClicked: fetchSettings()
        }

        Text {
            id: statusText
            color: "#888"
            font.italic: true
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    function fetchSettings() {
        var reply = portal.readOne("org.freedesktop.appearance", "color-scheme")
        reply.finished.connect(function() {
            if (reply.isError) {
                errorText.text = "Portal not available"
                return
            }
            darkMode = reply.value === 1
            statusText.text = "Color scheme: " + (darkMode ? "Dark (prefer)" : "Light (prefer)")
        })

        var accentReply = portal.readOne("org.freedesktop.appearance", "accent-color")
        accentReply.finished.connect(function() {
            if (accentReply.isError) return
            accentColor = accentReply.value !== ""
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
            // Listen for real SettingChanged signals from the portal
            if (name === "SettingChanged" && args.length >= 3) {
                var ns = args[0]
                var key = args[1]
                var value = args[2]
                if (ns === "org.freedesktop.appearance" && key === "color-scheme") {
                    darkMode = (value === 1 || value === "1")
                    statusText.text = "Portal signaled: color-scheme changed to "
                        + (darkMode ? "Dark" : "Light")
                }
            }
        }
    }
    CloseButton {}
    
}
