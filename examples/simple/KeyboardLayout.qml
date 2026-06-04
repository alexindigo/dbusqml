import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0 as DBusQML

Window {
    id: root
    visible: true
    width: 400
    height: 320
    title: "DBus — Locale & Input Language"

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 16

        Label {
            text: "System Locale & Input Language"
            font.bold: true
            font.pixelSize: 18
            Layout.alignment: Qt.AlignHCenter
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 120
            height: 120
            radius: 16
            color: "#f0f0f0"

            Label {
                id: layoutLabel
                anchors.centerIn: parent
                text: locale.x11Layout ? locale.x11Layout.toUpperCase() : "--"
                font.pixelSize: 48
                font.bold: true
                color: "#333"
            }
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 4

            Label {
                id: localeLabel
                color: "#666"
                font.pixelSize: 13
                font.family: "monospace"
                text: locale.locale ? locale.locale[0].replace("LANG=", "") : ""
            }

            Label {
                id: variantLabel
                color: "#888"
                font.pixelSize: 13
                text: locale.x11Variant ? "Variant: " + locale.x11Variant : ""
            }

            Label {
                id: inputLabel
                color: "#888"
                font.pixelSize: 13
                text: "Detecting..."
            }
        }

        Button {
            text: "Refresh"
            Layout.alignment: Qt.AlignHCenter
            onClicked: root.fetchInputMethod()
        }
    }

    DBusQML.DBus {
        id: locale
        service: "org.freedesktop.locale1"
        path: "/org/freedesktop/locale1"
        iface: "org.freedesktop.locale1"
        connection: DBusQML.SystemBus

        onIntrospectionCompleted: {
            layoutLabel.text = locale.x11Layout ? locale.x11Layout.toUpperCase() : "??"
            localeLabel.text = locale.locale ? locale.locale[0].replace("LANG=", "") : ""
            variantLabel.text = locale.x11Variant ? "Variant: " + locale.x11Variant : ""
        }

        onStatusChanged: {
            if (status === 3) // Error
                variantLabel.text = "locale1 not available"
        }
    }

    DBusQML.DBus {
        id: sessionBus
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"

        onIntrospectionCompleted: {
            root.fetchInputMethod()
        }
    }

    DBusQML.DBus {
        id: ibus
        service: "org.freedesktop.IBus"
        path: "/org/freedesktop/IBus"
        iface: "org.freedesktop.IBus"
    }

    DBusQML.DBus {
        id: fcitx
        service: "org.fcitx.Fcitx5.Controller1"
        path: "/controller"
        iface: "org.fcitx.Fcitx5.Controller1"
    }

    function fetchInputMethod() {
        var busReply = sessionBus.ListNames()
        busReply.finished.connect(function() {
            if (busReply.isError) {
                inputLabel.text = "Input method: error"
                return
            }
            var names = busReply.value
            if (names.indexOf("org.freedesktop.IBus") >= 0) {
                queryIbusEngine()
            } else if (names.indexOf("org.fcitx.Fcitx5.Controller1") >= 0) {
                queryFcitx5()
            } else {
                inputLabel.text = "Input method: not detected"
            }
        })
    }

    function queryIbusEngine() {
        var reply = ibus.CurrentInputContext()
        reply.finished.connect(function() {
            if (reply.isError) {
                inputLabel.text = "Input method: IBus (error)"
                return
            }
            inputLabel.text = "Input method: IBus"
        })
    }

    function queryFcitx5() {
        var reply = fcitx.CurrentIM()
        reply.finished.connect(function() {
            if (reply.isError) {
                inputLabel.text = "Input method: Fcitx5 (error)"
                return
            }
            inputLabel.text = "Input method: Fcitx5"
        })
    }

    Component.onCompleted: {
        // locale properties auto-bind via QML; sessionBus triggers fetchInputMethod
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
