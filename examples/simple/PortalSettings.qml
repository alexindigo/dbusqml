import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 400
    height: 280
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
        }

        GroupBox {
            title: "Color Scheme"
            Layout.fillWidth: true

            ColumnLayout {
                width: parent.width
                spacing: 8

                RowLayout {
                    spacing: 8
                    Rectangle { width: 16; height: 16; radius: 3; color: darkMode ? "#333" : "#eee"; border.color: "#ccc" }
                    Text { text: darkMode ? "Dark Mode" : "Light Mode"; font.pixelSize: 16 }
                }

                RowLayout {
                    spacing: 8
                    Rectangle {
                        width: 16; height: 16; radius: 3
                        color: accentColor ? "#1e88e5" : "#ccc"
                        border.color: "#ccc"
                    }
                    Text {
                        text: accentColor ? "Accent color enabled" : "No accent color"
                        font.pixelSize: 16
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
            text: "Refresh"
            Layout.alignment: Qt.AlignHCenter
            onClicked: fetchSettings()
        }

        Text {
            id: statusText
            color: "#888"
            font.italic: true
        }
    }

    function fetchSettings() {
        var reply = portal.ReadOne(
            "org.freedesktop.appearance",
            "color-scheme"
        )
        reply.finished.connect(function() {
            if (reply.isError) {
                errorText.text = "Portal not available"
                return
            }
            darkMode = reply.value === 1
            statusText.text = "Color scheme: " + (darkMode ? "Dark (prefer)" : "Light (prefer)")
        })

        var accentReply = portal.ReadOne(
            "org.freedesktop.appearance",
            "accent-color"
        )
        accentReply.finished.connect(function() {
            if (accentReply.isError) return
            accentColor = accentReply.value !== ""
        })
    }

    Component.onCompleted: fetchSettings()

    // Dynamic proxy — ReadOne() is callable directly after introspection.
    DBusQML.DBus {
        id: portal
        service: "org.freedesktop.portal.Desktop"
        path: "/org/freedesktop/portal/desktop"
        iface: "org.freedesktop.portal.Settings"
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
