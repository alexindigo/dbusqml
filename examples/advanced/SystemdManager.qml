import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0
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
                    systemd.startUnit(unitInput.text, "replace")
                }
            }
            Button {
                text: "Stop"
                onClicked: {
                    statusText.text = "Stopping " + unitInput.text + "..."
                    systemd.stopUnit(unitInput.text, "replace")
                }
            }
            Button {
                text: "Restart"
                onClicked: {
                    statusText.text = "Restarting " + unitInput.text + "..."
                    systemd.restartUnit(unitInput.text, "replace")
                }
            }
        }

        Text {
            id: statusText
            property string lastError: ""
            color: "#666"
            font.italic: true
            Layout.fillWidth: true
            wrapMode: Text.WordWrap

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

    // Dynamic proxy — after introspection, StartUnit(), StopUnit(),
    // and RestartUnit() are callable directly.
    DBus {
        id: systemd
        service: "org.freedesktop.systemd1"
        path: "/org/freedesktop/systemd1"
        iface: "org.freedesktop.systemd1.Manager"
        connection: SystemBus
    }

    DBus {
        id: systemdUnit
        service: "org.freedesktop.systemd1"
        path: "/org/freedesktop/systemd1"
        iface: "org.freedesktop.systemd1.Unit"
        connection: SystemBus
    }

    function checkUnit(name) {
        statusText.text = "Looking up " + name + "..."
        var reply = systemd.getUnit(name)
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.lastError = "Unit not found: " + reply.error.message; statusText.text = statusText.lastError
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
    CloseButton {}
    TextEdit { id: clipBoard; x: -9999; y: -9999 }
    
}
