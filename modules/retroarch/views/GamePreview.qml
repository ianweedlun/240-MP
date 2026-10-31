import QtQuick

Item {
    id: gamePreview
    property var game: ({})

    // Boxart — the panel highlights the art at full box size. The game name
    // lives on the row, so printing it here too was redundant and forced the
    // image to shrink to make room.
    Image {
        id: boxart
        visible: gamePreview.game.thumbnail && gamePreview.game.thumbnail !== ""
        anchors.fill: parent
        fillMode: Image.PreserveAspectFit
        source: gamePreview.game.thumbnail
        asynchronous: true
        cache: true
    }

    // Placeholder when no boxart exists
    Rectangle {
        id: noBoxartPlaceholder
        visible: !boxart.visible
        anchors.centerIn: parent
        width: parent.width * 0.7
        height: parent.height * 0.4
        color: Qt.rgba(0.04, 0.0, 0.58, 0.2)
        border.color: root.tertiaryColor
        border.width: 1
        radius: 4

        Text {
            anchors.centerIn: parent
            text: "NO\nBOXART"
            color: root.tertiaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: root.sh * 0.0333333 //16
        }
    }

    // Bottom accent line
    Rectangle {
        width: parent.width * 0.4
        height: root.sw * 0.0015625 //1
        color: root.tertiaryColor
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.015 //7
        anchors.horizontalCenter: parent.horizontalCenter
    }
}
