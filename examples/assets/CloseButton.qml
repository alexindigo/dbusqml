import QtQuick

// Reusable close button for all examples.
// Place at the end of any Window to add a dismiss button.
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
