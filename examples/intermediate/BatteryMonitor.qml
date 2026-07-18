import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    id: root
    visible: true
    width: 600
    height: 500
    title: "DBus — Battery Monitor"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            text: "Battery Status"
            font.bold: true
            font.pixelSize: 18
        }

        GroupBox {
            title: "System Power"
            Layout.fillWidth: true

            GridLayout {
                columns: 2
                width: parent.width
                rowSpacing: 4
                columnSpacing: 12

                Text { text: "Power Source:"; font.bold: true }
                Text { text: upower.onBattery ? "Battery" : "AC Adapter"; color: upower.onBattery ? "#e53935" : "#4caf50" }

                Text { text: "Daemon:"; font.bold: true }
                Text { text: "UPower " + (upower.daemonVersion || "?"); color: "#666" }

                Text { text: "Lid:"; font.bold: true }
                Text { text: upower.lidIsPresent !== undefined ? (upower.lidIsClosed ? "Closed" : "Open") : "—"; color: "#666" }

                Text { text: "Devices:"; font.bold: true }
                Text { id: deviceCountText; text: "—"; color: "#666" }
            }
        }

        RowLayout {
            spacing: 8

        Text {
            id: statusText
            property string lastError: ""
            color: "#888"
            font.italic: true
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
            spacing: 4

            model: ListModel { id: devicesModel }

            delegate: GroupBox {
                width: deviceList.width
                title: {
                    var types = ["AC Adapter", "Battery", "UPS", "Monitor", "Mouse", "Keyboard", "PDA", "Phone"]
                    return (types[model.type - 1] || "Device") + (model.percentage !== undefined ? "  " + model.percentage.toFixed(0) + "%" : "")
                }

                ColumnLayout {
                    width: deviceList.width
                    spacing: 4

                    ProgressBar {
                        Layout.fillWidth: true
                        from: 0; to: 100
                        value: model.percentage || 0
                    }

                    Text {
                        text: {
                            var s = ({
                                1: "Charging", 2: "Discharging",
                                3: "Empty", 4: "Fully Charged",
                                5: "Pending Charge", 6: "Pending Discharge"
                            })[model.state] || "Unknown"
                            if (model.timeToEmpty > 0 && model.state === 2)
                                s += "  --  " + formatTime(model.timeToEmpty) + " remaining"
                            if (model.timeToFull > 0 && (model.state === 1 || model.state === 5))
                                s += "  --  " + formatTime(model.timeToFull) + " until full"
                            return s
                        }
                        font.pixelSize: 12
                        color: "#666"
                    }

                    GridLayout {
                        columns: 2
                        width: deviceList.width
                        rowSpacing: 2
                        columnSpacing: 12

                        Repeater {
                            model: {
                                var rows = []
                                if ((model.energy || 0) > 0)
                                    rows.push({ label: "Energy", value: model.energy.toFixed(1) + " / " + (model.energyFull || 0).toFixed(1) + " Wh" })
                                if ((model.energyRate || 0) > 0)
                                    rows.push({ label: "Rate", value: model.energyRate.toFixed(2) + " W" })
                                if ((model.voltage || 0) > 0)
                                    rows.push({ label: "Voltage", value: model.voltage.toFixed(2) + " V" })
                                if ((model.temperature || 0) > 0)
                                    rows.push({ label: "Temperature", value: model.temperature.toFixed(1) + " C" })
                                if ((model.capacity || 0) > 0)
                                    rows.push({ label: "Health", value: model.capacity.toFixed(0) + "%" })
                                if ((model.chargeCycles || 0) > 0)
                                    rows.push({ label: "Cycles", value: model.chargeCycles })
                                if (model.vendor)
                                    rows.push({ label: "Vendor", value: model.vendor })
                                if (model.model)
                                    rows.push({ label: "Model", value: model.model })
                                if (model.serial)
                                    rows.push({ label: "Serial", value: model.serial })
                                if ((model.technology || 0) > 0) {
                                    var techs = ["", "", "NiMH", "Li-ion", "Li-Po", "LiFePO4", "Lead Acid"]
                                    rows.push({ label: "Chemistry", value: techs[model.technology] || "Unknown" })
                                }
                                return rows
                            }

                            Text {
                                text: modelData.label + ":"
                                font.bold: true
                                font.pixelSize: 11
                                color: "#888"
                            }
                            Text {
                                text: modelData.value
                                font.pixelSize: 11
                                font.family: "monospace"
                                color: "#333"
                            }
                        }
                    }
                }
            }
        }
    }

    function formatTime(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        return h + "h " + m + "m"
    }

    function fetchDeviceInfo(devicePath, idx) {
        var dev = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.UPower"; path: "' + devicePath + '"; iface: "org.freedesktop.UPower.Device"; connection: SystemBus }',
            this
        )
        dev.introspectionCompleted.connect(function() {
            devicesModel.set(idx, {
                type: dev.type,
                percentage: dev.percentage,
                state: dev.state,
                timeToEmpty: dev.timeToEmpty,
                timeToFull: dev.timeToFull,
                energy: dev.energy,
                energyFull: dev.energyFull,
                energyRate: dev.energyRate,
                voltage: dev.voltage,
                temperature: dev.temperature,
                capacity: dev.capacity,
                chargeCycles: dev.chargeCycles,
                vendor: dev.vendor,
                model: dev.model,
                serial: dev.serial,
                technology: dev.technology,
                isPresent: dev.isPresent,
                isRechargeable: dev.isRechargeable,
                powerSupply: dev.powerSupply,
                online: dev.online,
            })
            dev.destroy()
        })
    }

    function refreshAll() {
        devicesModel.clear()
        statusText.text = "Fetching devices..."

        var enumReply = upower.enumerateDevices()
        enumReply.finished.connect(function() {
            if (enumReply.isError) {
                statusText.lastError = "UPower not available"; statusText.text = statusText.lastError
                return
            }
            var devices = enumReply.value
            statusText.text = ""
            deviceCountText.text = devices.length

            for (var i = 0; i < devices.length; ++i) {
                devicesModel.append({})
                fetchDeviceInfo(devices[i], i)
            }
        })
    }

    // UPower properties auto-bind as QML properties after introspection.
    DBus {
        id: upower
        service: "org.freedesktop.UPower"
        path: "/org/freedesktop/UPower"
        iface: "org.freedesktop.UPower"
        connection: SystemBus
    }

    Component.onCompleted: {
        upower.introspectionCompleted.connect(refreshAll)
    }
    CloseButton {}
    TextEdit { id: clipBoard; x: -9999; y: -9999 }

}