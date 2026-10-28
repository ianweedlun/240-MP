import QtQuick

// Right-hand preview pane for a system row. RetroArch stores no system-level
// artwork, so this shows the core's identity and game count — the "channel
// bug" equivalent for the first tier.
Item {
    id: systemPreview
    property var system: ({})

    // Framed box
    Rectangle {
        anchors.fill: parent
        border.color: root.tertiaryColor
        border.width: root.sw * 0.0015625 //1
        color: "transparent"
    }

    // Core name, centred
    Text {
        id: coreName
        text: systemPreview.system.name || ""
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

    // Decorative divider
    Rectangle {
        width: parent.width * 0.5
        height: root.sw * 0.0015625 //1
        color: root.tertiaryColor
        anchors.top: coreName.bottom
        anchors.topMargin: root.sh * 0.02 //10
        anchors.horizontalCenter: parent.horizontalCenter
    }

    // Count
    Text {
        text: (systemPreview.system.count || 0) + " GAMES"
        color: root.secondaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        horizontalAlignment: Text.AlignHCenter
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        font.pixelSize: root.sh * 0.0333333 //16
    }
}
