import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0
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
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
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
                property string lastError: ""
                color: "#888"
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
                text: "Detecting..."

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

        Button {
            text: "Refresh"
            Layout.alignment: Qt.AlignHCenter
            onClicked: root.fetchInputMethod()
        }
    }

    DBus {
        id: locale
        service: "org.freedesktop.locale1"
        path: "/org/freedesktop/locale1"
        iface: "org.freedesktop.locale1"
        connection: SystemBus

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

    DBus {
        id: sessionBus
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"

        onIntrospectionCompleted: {
            root.fetchInputMethod()
        }
    }

    DBus {
        id: ibus
        service: "org.freedesktop.IBus"
        path: "/org/freedesktop/IBus"
        iface: "org.freedesktop.IBus"
    }

    DBus {
        id: fcitx
        service: "org.fcitx.Fcitx5.Controller1"
        path: "/controller"
        iface: "org.fcitx.Fcitx5.Controller1"
    }

    function fetchInputMethod() {
        var busReply = sessionBus.listNames()
        busReply.finished.connect(function() {
            if (busReply.isError) {
                inputLabel.lastError = "Input method: error"; inputLabel.text = inputLabel.lastError
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
        var reply = ibus.currentInputContext()
        reply.finished.connect(function() {
            if (reply.isError) {
                inputLabel.lastError = "Input method: IBus (error)"; inputLabel.text = inputLabel.lastError
                return
            }
            inputLabel.text = "Input method: IBus"
        })
    }

    function queryFcitx5() {
        var reply = fcitx.currentIM()
        reply.finished.connect(function() {
            if (reply.isError) {
                inputLabel.lastError = "Input method: Fcitx5 (error)"; inputLabel.text = inputLabel.lastError
                return
            }
            inputLabel.text = "Input method: Fcitx5"
        })
    }

    Component.onCompleted: {
        // locale properties auto-bind via QML; sessionBus triggers fetchInputMethod
    }
    CloseButton {}
    TextEdit { id: clipBoard; x: -9999; y: -9999 }
    
}
