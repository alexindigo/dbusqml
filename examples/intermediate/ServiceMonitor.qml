import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    visible: true
    width: 600
    height: 500
    title: "DBus — Service Monitor"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            text: "D-Bus Service Monitor"
            font.bold: true
            font.pixelSize: 18
        }

        RowLayout {
            spacing: 8

            TextField {
                id: serviceInput
                Layout.fillWidth: true
                placeholderText: "Service name (e.g. org.freedesktop.DBus)"
                text: "org.freedesktop.DBus"
            }

            Button {
                text: "Check"
                onClicked: checkService(serviceInput.text)
            }
        }

        Label { text: "Registered services:"; font.bold: true }

        ListView {
            id: serviceList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            model: ListModel { id: servicesModel }

            delegate: Rectangle {
                width: parent.width
                height: 28
                color: index % 2 === 0 ? "#f5f5f5" : "#ffffff"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: model.name
                        font.family: "monospace"
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        color: model.active ? "#4caf50" : "#ccc"
                        ToolTip.visible: ma.containsMouse
                        ToolTip.text: model.active ? "Active" : "Inactive"

                        MouseArea {
                            id: ma
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                    }
                }
            }

            section.property: "firstLetter"
            section.criteria: ViewSection.FullString
            section.delegate: Text {
                text: section
                font.bold: true
                font.pixelSize: 14
                color: "#333"
            }
        }

        Text {
            id: statusText
            color: "#666"
            font.italic: true
        }
    }

    DBusQML.DBus {
        id: dbusProxy
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"

        onSignalReceived: function(name, args) {
            if (name === "NameOwnerChanged") {
                var serviceName = args[0]
                var oldOwner = args[1]
                var newOwner = args[2]

                for (var i = 0; i < servicesModel.count; ++i) {
                    if (servicesModel.get(i).name === serviceName) {
                        servicesModel.set(i, { name: serviceName, active: newOwner !== "", firstLetter: serviceName.charAt(0) })
                        return
                    }
                }
                if (newOwner !== "") {
                    servicesModel.append({
                        name: serviceName,
                        active: true,
                        firstLetter: serviceName.charAt(0)
                    })
                }
            }
        }
    }

    function checkService(name) {
        var reply = dbusProxy.NameHasOwner(name)
        reply.finished.connect(function() {
            if (reply.isError) {
                statusText.text = "Error checking " + name + ": " + reply.error.message
            } else {
                statusText.text = name + " is " + (reply.value ? "running" : "not running")
            }
        })
    }

    function refreshServices() {
        servicesModel.clear()

        var reply = dbusProxy.ListNames()
        reply.finished.connect(function() {
            if (reply.isError) return
            var names = reply.value
            statusText.text = names.length + " services on the bus"

            for (var i = 0; i < names.length; ++i) {
                servicesModel.append({
                    name: names[i],
                    active: true,
                    firstLetter: names[i].charAt(0)
                })
            }
        })
    }

    Component.onCompleted: refreshServices()
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
