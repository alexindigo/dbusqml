import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../assets"
import DBus 1.0

Window {
    id: root
    visible: true
    width: 480
    height: 440
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
            Layout.fillHeight: true
            color: "white"
            border.color: "#ccc"
            radius: 8

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                // Artwork
                Image {
                    id: artwork
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 160
                    Layout.preferredHeight: 160
                    fillMode: Image.PreserveAspectFit
                    source: {
                        var meta = playerProxy.metadata
                        if (!meta) return ""
                        var artUrl = meta["mpris:artUrl"]
                        return artUrl ? artUrl : ""
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: "transparent"
                        border.color: "#e0e0e0"
                        visible: artwork.source === ""
                    }
                }

                // Title
                Label {
                    text: {
                        var meta = playerProxy.metadata
                        if (!meta) return activePlayer ? "Connecting..." : "No player selected"
                        return meta["xesam:title"] || "Unknown Title"
                    }
                    color: "black"
                    font.pixelSize: 15
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }

                // Artist — Album
                Label {
                    text: {
                        var meta = playerProxy.metadata
                        if (!meta) return ""
                        var artist = meta["xesam:artist"] ? meta["xesam:artist"][0] : ""
                        var album  = meta["xesam:album"] || ""
                        if (artist && album) return artist + " — " + album
                        return artist || album
                    }
                    font.pixelSize: 12
                    color: "#888"
                    horizontalAlignment: Text.AlignHCenter
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                // Playback state
                Label {
                    text: playerProxy.playbackStatus
                        ? (playerProxy.playbackStatus === "Playing" ? "▶ Playing" : "⏸ Paused")
                        : ""
                    font.pixelSize: 12
                    color: playerProxy.playbackStatus === "Playing" ? "#4caf50" : "#888"
                    Layout.alignment: Qt.AlignHCenter
                }

                // Controls
                RowLayout {
                    spacing: 16
                    Layout.alignment: Qt.AlignHCenter

                    Button {
                        text: "⏮"
                        font.pixelSize: 18
                        flat: true
                        onClicked: playerProxy.previous()
                    }
                    Button {
                        text: playerProxy.playbackStatus === "Playing" ? "⏸" : "▶"
                        font.pixelSize: 24
                        flat: true
                        onClicked: playerProxy.playPause()
                    }
                    Button {
                        text: "⏭"
                        font.pixelSize: 18
                        flat: true
                        onClicked: playerProxy.next()
                    }
                }

                Item { Layout.fillHeight: true }
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
