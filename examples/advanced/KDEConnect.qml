import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 640
    height: 500
    title: "DBus — KDE Connect"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label { text: "KDE Connect Devices"; font.bold: true; font.pixelSize: 18 }

        Button {
            text: "Refresh devices"
            onClicked: fetchDevices()
        }

        ListView {
            id: deviceList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            model: ListModel { id: devicesModel }

            delegate: Rectangle {
                width: deviceList.width
                height: deviceExpanded ? 200 : 60
                color: index % 2 === 0 ? "#f5f5f5" : "#ffffff"
                clip: true

                property bool deviceExpanded: false

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: model.name || "Unknown device"
                            font.bold: true
                            font.pixelSize: 14
                            Layout.fillWidth: true
                        }

                        Rectangle {
                            width: 10; height: 10; radius: 5
                            color: model.reachable ? "#4caf50" : "#ccc"
                        }

                        Button {
                            text: deviceExpanded ? "▲" : "▼"
                            flat: true
                            onClicked: deviceExpanded = !deviceExpanded
                        }
                    }

                    RowLayout {
                        visible: deviceExpanded
                        spacing: 8

                        ColumnLayout {
                            spacing: 4
                            Text { text: "Battery: " + (model.batteryCharge !== undefined ? model.batteryCharge + "%" : "?") }
                            Text { text: "Charging: " + (model.batteryCharging ? "Yes" : "No") }
                            Text { text: "Signal: " + (model.cellularStrength !== undefined ? model.cellularStrength + "%" : "?") }
                        }

                        ColumnLayout {
                            spacing: 4

                            Button { text: "Ring"; enabled: model.reachable; onClicked: ringDevice(model.deviceId) }
                            Button { text: "Share URL"; enabled: model.reachable; onClicked: shareUrl(model.deviceId) }
                            Button { text: model.paired ? "Unpair" : "Pair"
                                onClicked: {
                                    if (model.paired) unpairDevice(model.deviceId)
                                    else pairDevice(model.deviceId)
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            id: statusText
            property string lastError: ""
            color: "#666"
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

    // KDE Connect daemon proxy — devices() is callable directly.
    DBus {
        id: daemon
        service: "org.kde.kdeconnect"
        path: "/modules/kdeconnect"
        iface: "org.kde.kdeconnect.daemon"
    }

    function createDeviceProxy(deviceId, subsystem) {
        var path = "/modules/kdeconnect/devices/" + deviceId
        var iface = "org.kde.kdeconnect.device"
        if (subsystem) { path += "/" + subsystem; iface = "org.kde.kdeconnect.device." + subsystem }
        return Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.kde.kdeconnect"; path: "' + path + '"; iface: "' + iface + '" }',
            this
        )
    }

    function fetchDeviceInfo(deviceId, idx) {
        var devicePath = "/modules/kdeconnect/devices/" + deviceId

        var dev = createDeviceProxy(deviceId, "")
        var bat = createDeviceProxy(deviceId, "battery")
        var cell = createDeviceProxy(deviceId, "connectivity_report")

        var pending = 3
        var updateModel = function() {
            devicesModel.set(idx, {
                deviceId: deviceId,
                name: dev.name || deviceId,
                reachable: dev.isReachable,
                paired: dev.isPaired,
                batteryCharge: bat.charge !== undefined ? bat.charge : 0,
                batteryCharging: bat.isCharging || false,
                cellularStrength: cell.cellularNetworkStrength !== undefined ? cell.cellularNetworkStrength : 0,
            })
            dev.destroy(); bat.destroy(); cell.destroy()
        }

        dev.introspectionCompleted.connect(function() {
            if (--pending === 0) updateModel()
        })
        bat.introspectionCompleted.connect(function() {
            if (--pending === 0) updateModel()
        })
        cell.introspectionCompleted.connect(function() {
            if (--pending === 0) updateModel()
        })
    }

    function ringDevice(deviceId) {
        var obj = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.kde.kdeconnect"; path: "/modules/kdeconnect/devices/' + deviceId + '/findmyphone"; iface: "org.kde.kdeconnect.device.findmyphone" }',
            this
        )
        obj.ring()
        obj.destroy()
        statusText.text = "Rung " + deviceId
    }

    function pairDevice(deviceId) {
        var obj = createDeviceProxy(deviceId, "")
        obj.requestPairing()
        obj.destroy()
        statusText.text = "Pairing requested for " + deviceId
    }

    function unpairDevice(deviceId) {
        var obj = createDeviceProxy(deviceId, "")
        obj.unpair()
        obj.destroy()
        statusText.text = "Unpaired " + deviceId
    }

    function shareUrl(deviceId) {
        var obj = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.kde.kdeconnect"; path: "/modules/kdeconnect/devices/' + deviceId + '/share"; iface: "org.kde.kdeconnect.device.share" }',
            this
        )
        obj.shareUrl("https://dbusqml.github.io")
        obj.destroy()
        statusText.text = "Shared URL to " + deviceId
    }

    function fetchDevices() {
        devicesModel.clear()
        statusText.text = "Fetching devices..."

        var reply = daemon.getProperty("devices")
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.lastError = "KDE Connect daemon not available"; statusText.text = statusText.lastError
                return
            }
            var ids = reply.value
            statusText.text = ids.length + " device(s) found"

            for (var i = 0; i < ids.length; ++i) {
                devicesModel.append({
                    deviceId: ids[i], name: "Loading...", reachable: false, paired: false,
                    batteryCharge: 0, batteryCharging: false, cellularStrength: 0,
                })
                fetchDeviceInfo(ids[i], i)
            }
        })
    }

    Component.onCompleted: {
        daemon.introspectionCompleted.connect(fetchDevices)
    }

    Text {
        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8
        text: "✕"; font.pixelSize: 16
        color: mouseArea.containsMouse ? "black" : "#999"
        MouseArea { id: mouseArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Qt.quit() }
    }
    TextEdit { id: clipBoard; x: -9999; y: -9999 }
}
