import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    id: root
    visible: true
    width: 560
    height: 380
    title: "DBus — MPRIS Media Player Remote"

    property string activePlayer: ""

    onActivePlayerChanged: {
        if (activePlayer) {
            playerProxy.service = activePlayer
            playerProxy.path = "/org/mpris/MediaPlayer2"
            playerProxy.iface = "org.mpris.MediaPlayer2.Player"
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label { text: "Media Player"; font.bold: true; font.pixelSize: 18 }

        ComboBox {
            id: playerSelector
            Layout.fillWidth: true
            model: ListModel { id: playersModel }
            textRole: "display"
            onActivated: function(index) { activePlayer = playersModel.get(index).service }
        }

        Button {
            text: "Refresh"
            onClicked: root.discoverPlayers()
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            border.color: "#ccc"
            radius: 8

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 8

                Label {
                    text: {
                        var meta = playerProxy.metadata
                        if (!meta) return activePlayer ? "Connecting..." : "No player selected"
                        var artist = meta["xesam:artist"] ? meta["xesam:artist"][0] : "Unknown"
                        var title  = meta["xesam:title"] || "Unknown Title"
                        return artist + " — " + title
                    }
                    font.pixelSize: 14
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    Layout.alignment: Qt.AlignHCenter
                }

                Label {
                    text: playerProxy.playbackStatus
                        ? (playerProxy.playbackStatus === "Playing" ? "▶ Playing" : "⏸ Paused")
                        : ""
                    font.pixelSize: 13
                    color: "#888"
                    Layout.alignment: Qt.AlignHCenter
                }

                RowLayout {
                    spacing: 16
                    Layout.alignment: Qt.AlignHCenter

                    Button {
                        text: "⏮"
                        font.pixelSize: 18
                        onClicked: playerProxy.call("Previous")
                    }
                    Button {
                        text: playerProxy.playbackStatus === "Playing" ? "⏸" : "▶"
                        font.pixelSize: 22
                        onClicked: playerProxy.call("PlayPause")
                    }
                    Button {
                        text: "⏭"
                        font.pixelSize: 18
                        onClicked: playerProxy.call("Next")
                    }
                }
            }
        }

        Text {
            id: statusLine
            color: "#888"
            font.italic: true
        }
    }

    DBus {
        id: playerProxy
        service: ""
        path: ""
        iface: ""

        onStatusChanged: {
            if (status === 3)
                statusLine.text = "Failed to connect to " + (activePlayer || "player")
        }
    }

    DBus {
        id: bus
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
    }

    Connections {
        target: bus
        function onIntrospectionCompleted() { discoverPlayers() }
    }

    function discoverPlayers() {
        var reply = bus.listNames()
        reply.finished.connect(function() {
            if (reply.isError) return
            playersModel.clear()
            var names = reply.value
            for (var i = 0; i < names.length; ++i) {
                if (names[i].indexOf("org.mpris.MediaPlayer2.") === 0) {
                    var displayName = names[i].replace("org.mpris.MediaPlayer2.", "")
                    playersModel.append({ display: displayName, service: names[i] })
                }
            }
            if (playersModel.count > 0) {
                playerSelector.currentIndex = 0
                activePlayer = playersModel.get(0).service
            }
            if (playersModel.count === 0)
                statusLine.text = "No media players detected"
        })
    }

    CloseButton {}

}
