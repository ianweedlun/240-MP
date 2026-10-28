import QtQuick

Item {
    id: playlistPreview
    property var playlist: ({})

    Rectangle {
        anchors.fill: parent
        border.color: root.tertiaryColor
        border.width: root.sw * 0.0015625 //1
        color: "transparent"
    }

    Text {
        id: plName
        text: playlistPreview.playlist.name || ""
        color: root.primaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        width: parent.width * 0.9
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.05 //24
        font.pixelSize: root.sh * 0.04 //20
    }

    Rectangle {
        width: parent.width * 0.5
        height: root.sw * 0.0015625 //1
        color: root.tertiaryColor
        anchors.top: plName.bottom
        anchors.topMargin: root.sh * 0.02 //10
        anchors.horizontalCenter: parent.horizontalCenter
    }

    Text {
        text: (playlistPreview.playlist.count || 0) + " GAMES"
        color: root.secondaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        horizontalAlignment: Text.AlignHCenter
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        font.pixelSize: root.sh * 0.0333333 //16
    }
}
