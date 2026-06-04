import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 500
    height: 400
    title: "DBus — Battery Monitor"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        RowLayout {
            spacing: 12

            Label {
                text: "Battery Status"
                font.bold: true
                font.pixelSize: 18
            }

            Rectangle {
                id: onBatteryIndicator
                width: 12; height: 12; radius: 6
                color: upower.onBattery ? "#e53935" : "#4caf50"
            }
            Label { id: onBatteryLabel; text: upower.onBattery ? "On battery" : "On AC" }

            Button {
                text: "Refresh"
                onClicked: refreshAll()
            }
        }

        ListView {
            id: deviceList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            model: ListModel { id: devicesModel }

            delegate: Rectangle {
                width: parent.width
                height: 80
                color: index % 2 === 0 ? "#f5f5f5" : "#ffffff"
                radius: 6

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            text: model.type || "Unknown"
                            font.bold: true
                            font.pixelSize: 14
                            Layout.fillWidth: true
                        }

                        Text {
                            text: model.percentage !== undefined ? Math.round(model.percentage) + "%" : "?"
                            font.pixelSize: 14
                            color: model.percentage !== undefined && model.percentage < 20 ? "#e53935" : "#333"
                        }
                    }

                    ProgressBar {
                        Layout.fillWidth: true
                        from: 0; to: 100
                        value: model.percentage || 0
                    }

                    Text {
                        text: {
                            var parts = []
                            if (model.state === 1) parts.push("Charging")
                            else if (model.state === 2) parts.push("Discharging")
                            else if (model.state === 4) parts.push("Fully charged")
                            else parts.push("Unknown state")

                            if (model.timeToEmpty > 0 && model.state === 2)
                                parts.push(" — " + formatTime(model.timeToEmpty) + " remaining")
                            if (model.timeToFull > 0 && model.state === 1)
                                parts.push(" — " + formatTime(model.timeToFull) + " until full")

                            return parts.join("")
                        }
                        font.pixelSize: 11
                        color: "#888"
                    }
                }
            }
        }

        Text {
            id: statusText
            color: "#666"
            font.italic: true
        }
    }

    // Dynamic proxy — onBattery, lidIsClosed are available as properties.
    DBusQML.DBus {
        id: upower
        service: "org.freedesktop.UPower"
        path: "/org/freedesktop/UPower"
        iface: "org.freedesktop.UPower"
        connection: DBusQML.SystemBus
    }

    function formatTime(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        return h + "h " + m + "m"
    }

    function fetchDeviceInfo(devicePath, idx) {
        // Create a temporary proxy for this device — properties auto-load
        var dev = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.UPower"; path: "' + devicePath + '"; iface: "org.freedesktop.UPower.Device"; connection: SystemBus }',
            this
        )
        dev.introspectionCompleted.connect(function() {
            devicesModel.set(idx, {
                path: devicePath,
                type: dev.model || devicePath.split("/").pop(),
                percentage: dev.percentage,
                state: dev.state,
                timeToEmpty: dev.timeToEmpty,
                timeToFull: dev.timeToFull,
                energy: dev.energy,
                powerSupply: dev.powerSupply,
            })
            dev.destroy()
        })
    }

    function refreshAll() {
        devicesModel.clear()

        var enumReply = upower.EnumerateDevices()
        enumReply.finished.connect(function() {
            if (enumReply.isError) {
                statusText.text = "UPower not available"
                return
            }
            var devices = enumReply.value
            statusText.text = devices.length + " power device(s)"

            for (var i = 0; i < devices.length; ++i) {
                devicesModel.append({
                    path: devices[i],
                    type: "Loading...",
                    percentage: 0,
                    state: 0,
                    timeToEmpty: 0,
                    timeToFull: 0,
                    energy: 0,
                    powerSupply: false,
                })
                fetchDeviceInfo(devices[i], i)
            }
        })
    }

    Component.onCompleted: refreshAll()

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
