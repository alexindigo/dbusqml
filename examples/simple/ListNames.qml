import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import DBus 1.0

Window {
    id: root
    visible: true
    width: 640
    height: 480
    title: "DBus — List D-Bus Names"

    property var allNames: []

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16

        Text {
            text: "Known D-Bus services:"
            font.pixelSize: 16
            font.bold: true
        }

        TextField {
            id: filterField
            Layout.fillWidth: true
            placeholderText: "Filter..."
            Layout.topMargin: 8
            onTextChanged: root.applyFilter()
        }

        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 8
            clip: true
            model: ListModel { id: namesModel }
            ScrollBar.vertical: ScrollBar {}

            delegate: Text {
                text: "• " + model.name
                font.pixelSize: 12
                font.family: "monospace"
            }
        }
    }

    function applyFilter() {
        namesModel.clear()
        var filter = filterField.text
        for (var i = 0; i < allNames.length; i++) {
            if (filter === "" || allNames[i].indexOf(filter) >= 0)
                namesModel.append({"name": allNames[i]})
        }
    }

    // Dynamic proxy — ListNames() is callable directly after introspection.
    DBus {
        id: bus
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"

        onIntrospectionCompleted: {
            var reply = bus.ListNames()
            reply.finished.connect(function() {
                if (reply.isError) {
                    console.error("Error:", reply.error.message)
                    return
                }
                root.allNames = reply.value
                root.applyFilter()
            })
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
