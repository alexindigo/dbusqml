import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 500
    height: 400
    title: "DBus — Systemd Manager"

    property string unitName: ""

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label { text: "Systemd Unit Manager"; font.bold: true; font.pixelSize: 18 }

        RowLayout {
            spacing: 8

            TextField {
                id: unitInput
                Layout.fillWidth: true
                placeholderText: "Unit name (e.g. cups.service)"
                text: "cups.service"
            }

            Button {
                text: "Check"
                onClicked: checkUnit(unitInput.text)
            }
        }

        GroupBox {
            title: "Unit Info"
            Layout.fillWidth: true
            visible: unitName !== ""

            ColumnLayout {
                width: parent.width
                spacing: 4

                Text { id: unitPathText; font.family: "monospace" }
                Text { id: unitActiveState; font.family: "monospace" }
                Text { id: unitSubState; font.family: "monospace" }
                Text { id: unitLoadState; font.family: "monospace" }
            }
        }

        RowLayout {
            spacing: 8
            enabled: unitName !== ""

            Button {
                text: "Start"
                onClicked: {
                    statusText.text = "Starting " + unitInput.text + "..."
                    systemd.StartUnit(unitInput.text, "replace")
                }
            }
            Button {
                text: "Stop"
                onClicked: {
                    statusText.text = "Stopping " + unitInput.text + "..."
                    systemd.StopUnit(unitInput.text, "replace")
                }
            }
            Button {
                text: "Restart"
                onClicked: {
                    statusText.text = "Restarting " + unitInput.text + "..."
                    systemd.RestartUnit(unitInput.text, "replace")
                }
            }
        }

        Text {
            id: statusText
            color: "#666"
            font.italic: true
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    // Dynamic proxy — after introspection, StartUnit(), StopUnit(),
    // and RestartUnit() are callable directly.
    DBusQML.DBus {
        id: systemd
        service: "org.freedesktop.systemd1"
        path: "/org/freedesktop/systemd1"
        iface: "org.freedesktop.systemd1.Manager"
        connection: DBusQML.SystemBus
    }

    DBusQML.DBus {
        id: systemdUnit
        service: "org.freedesktop.systemd1"
        path: "/org/freedesktop/systemd1"
        iface: "org.freedesktop.systemd1.Unit"
        connection: DBusQML.SystemBus
    }

    function checkUnit(name) {
        statusText.text = "Looking up " + name + "..."
        var reply = systemd.GetUnit(name)
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.text = "Unit not found: " + reply.error.message
                unitName = ""
                return
            }
            unitName = name
            var unitPath = reply.value
            unitPathText.text = "Path: " + unitPath
            // Point the unit proxy to the returned path — triggers introspection
            systemdUnit.path = unitPath
        })
    }

    // Read properties after introspection via valueChanged
    Connections {
        target: systemdUnit
        function onValueChanged(key, value) {
            if (key === "activeState")
                unitActiveState.text = "ActiveState: " + value
            else if (key === "subState")
                unitSubState.text = "SubState: " + value
            else if (key === "loadState")
                unitLoadState.text = "LoadState: " + value
        }
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
