import QtQuick

Item {
    id: gamePreview
    property var game: ({})

    // Framed box with subtle background
    Rectangle {
        anchors.fill: parent
        border.color: root.accentColor
        border.width: root.sw * 0.0015625 //1
        color: Qt.rgba(0.04, 0.0, 0.58, 0.3)
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

    // Game name below the art with better styling
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
        font.bold: true
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
