import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    id: root
    visible: true
    width: 500
    height: 350
    title: "DBus — Portal Demo (Server + Client)"

    property int colorScheme: 0

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label { text: "Portal Settings Demo"; font.bold: true; font.pixelSize: 18 }
        Label { text: "DBusAdaptor (server) + DBusProxy (client) in one window"; color: "#888"; font.italic: true; wrapMode: Text.WordWrap }

        GroupBox { title: "Server (DBusAdaptor)"; Layout.fillWidth: true
            ColumnLayout { width: parent.width; spacing: 8
                Label { text: "Exposes " + adaptor.service + " as " + adaptor.iface; font.pixelSize: 12; color: "#666" }
                RowLayout { spacing: 8
                    Text { text: "colorScheme: " + adaptor.colorScheme; font.family: "monospace" }
                    Button { text: "Emit SettingChanged"
                        onClicked: adaptor.emitSignal("SettingChanged",
                            ["org.freedesktop.appearance", "color-scheme", adaptor.colorScheme])
                    }
                }
            }
        }

        GroupBox { title: "Client (DBusProxy)"; Layout.fillWidth: true
            ColumnLayout { width: parent.width; spacing: 8
                Label { text: "Reads from " + portal.service + " using " + portal.iface; font.pixelSize: 12; color: "#666" }
                RowLayout { spacing: 8
                    Rectangle { width: 16; height: 16; radius: 3; color: root.colorScheme === 1 ? "#333" : "#eee"; border.color: "#ccc" }
                    Text { text: root.colorScheme === 1 ? "Dark Mode" : "Light Mode"; font.pixelSize: 16; Layout.fillWidth: true }
                    Switch { checked: root.colorScheme === 1
                        onToggled: {
                            root.colorScheme = checked ? 1 : 0
                            adaptor.colorScheme = root.colorScheme
                            adaptor.emitSignal("SettingChanged",
                                ["org.freedesktop.appearance", "color-scheme", root.colorScheme])
                        }
                    }
                }
                RowLayout {
                    Button { text: "Read from portal"
                        onClicked: {
                            var reply = portal.readSetting("org.freedesktop.appearance", "color-scheme")
                            reply.finished.connect(function() {
                                if (reply.isError) { statusText.lastError = "Error: " + reply.error.message; statusText.text = statusText.lastError; return }
                                root.colorScheme = reply.value
                                adaptor.colorScheme = reply.value
                                statusText.text = "Read color-scheme = " + reply.value
                            })
                        }
                    }
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
        }
    }

    DBusAdaptor {
        id: adaptor
        service: "org.freedesktop.impl.portal.atmosphera"
        path: "/org/freedesktop/portal/desktop"
        iface: "org.freedesktop.impl.portal.Settings"
        property int colorScheme: 0
        function readSetting(ns, key) {
            if (ns === "org.freedesktop.appearance" && key === "color-scheme")
                return colorScheme
            return ""
        }
    }

    DBus {
        id: portal
        service: "org.freedesktop.impl.portal.atmosphera"
        path: "/org/freedesktop/portal/desktop"
        iface: "org.freedesktop.impl.portal.Settings"
        onSignalReceived: function(name, args) {
            if (name === "SettingChanged" && args.length >= 3) {
                if (args[0] === "org.freedesktop.appearance" && args[1] === "color-scheme") {
                    root.colorScheme = args[2]
                    adaptor.colorScheme = args[2]
                }
            }
        }
    }

    Text {
        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8
        text: "✕"; font.pixelSize: 16
        color: mouseArea.containsMouse ? "black" : "#999"
        MouseArea { id: mouseArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Qt.quit() }
    }
    TextEdit { id: clipBoard; x: -9999; y: -9999 }
}
