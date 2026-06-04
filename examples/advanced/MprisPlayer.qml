import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0

Window {
    visible: true
    width: 560
    height: 400
    title: "DBus — MPRIS Media Player Remote"

    property string activePlayer: ""
    property bool playerActive: activePlayer !== ""

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

        Label { text: "Select a media player"; font.bold: true; font.pixelSize: 16 }

        ComboBox {
            id: playerSelector
            Layout.fillWidth: true
            model: ListModel { id: playersModel }
            textRole: "display"
            onActivated: activePlayer = playersModel.get(index).service
        }

        Button {
            text: "Refresh player list"
            onClicked: discoverPlayers()
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            visible: playerActive
            border.color: "#ccc"
            radius: 8

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                Label {
                    id: playbackStatus
                    text: "• " + (playerProxy.playbackStatus || "Unknown")
                    font.bold: true
                    font.pixelSize: 14
                    color: playerProxy.playbackStatus === "Playing" ? "#4caf50" : "#999"
                }

                Label {
                    id: trackInfo
                    text: {
                        var meta = playerProxy.metadata
                        if (!meta) return "No track metadata"
                        var artist = meta["xesam:artist"] ? meta["xesam:artist"][0] : "Unknown"
                        var title  = meta["xesam:title"]  || "Unknown Title"
                        return artist + " — " + title
                    }
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8

                Button {
                    text: "⏮"
                    onClicked: playerProxy.Previous()
                }
                Button {
                    text: playerProxy.playbackStatus === "Playing" ? "⏸" : "▶"
                    onClicked: playerProxy.PlayPause()
                }
                Button {
                    text: "⏭"
                    onClicked: playerProxy.Next()
                }
                }
            }
        }

        Text {
            id: statusLine
            text: playerActive ? "" : "Select a player from the list to control it"
            color: "#888"
            font.italic: true
        }
    }

    DBus {
        id: playerProxy
        service: ""
        path: ""
        iface: ""

        onSignalReceived: function(name, args) {
            if (name === "PropertiesChanged") {
                var changed = args[1]
                for (var key in changed) {
                    playerProxy[key] = changed[key]
                }
            } else if (name === "Seeked") {
                statusLine.text = "Track seeked to position " + args[0]
            }
        }

        Component.onCompleted: {
            valueChanged.connect(function(key) {
                if (key === "PlaybackStatus")
                    statusLine.text = "Playback: " + playerProxy[key]
            })
        }
    }

    function discoverPlayers() {
        var reply = bus.ListNames()
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
        })
    }

    Component.onCompleted: discoverPlayers()

    // Dynamic proxy — ListNames() is callable directly after introspection.
    DBus {
        id: bus
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
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
