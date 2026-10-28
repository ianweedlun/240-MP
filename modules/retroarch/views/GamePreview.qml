import QtQuick

Item {
    id: gamePreview
    property var game: ({})

    Rectangle {
        anchors.fill: parent
        border.color: root.tertiaryColor
        border.width: root.sw * 0.0015625 //1
        color: "transparent"
    }

    // Boxart, if RetroArch has one for this game
    Image {
        id: boxart
        visible: gamePreview.game.thumbnail && gamePreview.game.thumbnail !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.03 //14
        width: parent.width * 0.85
        height: parent.height * 0.55
        fillMode: Image.PreserveAspectFit
        source: gamePreview.game.thumbnail
    }

    // Placeholder when no boxart exists
    Text {
        visible: !boxart.visible
        text: "NO\nBOXART"
        color: root.tertiaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        horizontalAlignment: Text.AlignHCenter
        anchors.centerIn: parent
        font.pixelSize: root.sh * 0.0333333 //16
    }

    // Game name below the art
    Text {
        id: gameName
        text: gamePreview.game.name || ""
        color: root.primaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        width: parent.width * 0.9
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.04 //18
        font.pixelSize: root.sh * 0.0333333 //16
    }
}
