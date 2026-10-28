import QtQuick
import Components

FocusScope {
    id: gamesRoot

    property var navParams: ({})
    property var navListState: navParams.navListState || ({})
    readonly property string playlistPath: navParams.playlistPath || ""
    readonly property string playlistName: navParams.playlistName || ""
    readonly property bool showPreview: {
        var v = appCore.get_setting(moduleRoot.moduleId, "show_preview")
        return v !== false && v !== "OFF"
    }

    signal goBack()

    focus: true
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            goBack()
            event.accepted = true
        }
    }

    // ---
    // UI
    // ---

    AppBar {
        iconSource: moduleRoot.moduleIcon
        title: moduleRoot.moduleName
        subtitle: playlistName
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125 //60
        anchors.leftMargin: root.sw * 0.125 //80
    }

    // Empty state
    Column {
        anchors.centerIn: parent
        spacing: root.sh * 0.0333333 //16
        visible: gameList.count === 0
        Text {
            text: "No games in this playlist"
            color: root.secondaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.05 //24
        }
        Text {
            text: "Add games to this core in RetroArch"
            color: root.tertiaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.0333333 //16
        }
    }

    ListView {
        id: gameList
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.25 //120
        anchors.leftMargin: root.sw * 0.115625 //74
        width: gamesRoot.showPreview ? root.sw * 0.5 : root.sw * 0.76875
        height: root.sh * 0.525 //252
        keyNavigationEnabled: true
        clip: true
        focus: true

        Keys.onReturnPressed: {
            var game = model[currentIndex]
            if (!game) return
            retroarchBackend.launchGame(game, navParams.systemId)
        }

        delegate: Item {
            width: gameList.width
            height: root.sh * 0.0583333 //28

            Item {
                id: textClip
                width: Math.min(rowText.implicitWidth, gameList.width)
                height: parent.height
                clip: true

                Rectangle {
                    color: root.accentColor
                    anchors.fill: rowText
                    visible: gameList.currentIndex === index
                }

                Text {
                    id: rowText
                    text: modelData.name
                    color: gameList.currentIndex === index ? root.surfaceColor : root.primaryColor
                    font.family: root.globalFont
                    font.capitalization: Font.AllUppercase
                    anchors.verticalCenter: parent.verticalCenter
                    x: 0
                    topPadding: root.sh * 0.0041667 //2
                    leftPadding: root.sw * 0.009375 //6
                    rightPadding: root.sw * 0.009375 //6
                    bottomPadding: root.sh * 0.00625 //3
                    font.pixelSize: root.sh * 0.05 //24
                }

                SequentialAnimation {
                    running: (gameList.currentIndex === index) &&
                             (rowText.implicitWidth > textClip.width)
                    loops: Animation.Infinite
                    onRunningChanged: if (!running) rowText.x = 0
                    PauseAnimation { duration: 1500 }
                    NumberAnimation {
                        target: rowText; property: "x"
                        to: textClip.width - rowText.implicitWidth
                        duration: Math.abs(to) * 20
                    }
                    PauseAnimation { duration: 2000 }
                    PropertyAction { target: rowText; property: "x"; value: 0 }
                }
            }
        }

        Keys.onUpPressed: {
            if (count === 0) return
            if (currentIndex > 0) currentIndex--
            else currentIndex = count - 1
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Contain)
        }
        onCurrentIndexChanged: if (previewLoader.item)
            previewLoader.item.game = model[gameList.currentIndex]
    }

    // Preview pane — the selected game's boxart, when enabled
    Loader {
        id: previewLoader
        visible: gamesRoot.showPreview && gameList.count > 0
        anchors.right: parent.right
        anchors.rightMargin: root.sw * 0.125 //80
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25 //120
        width: root.sw * 0.22
        height: root.sh * 0.525 //252
        source: "GamePreview.qml"
    }

    // Footer
    Text {
        id: footer
        text: root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":PLAY"
        color: root.tertiaryColor
        font.family: root.globalFont
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.1041667 //50
        anchors.leftMargin: root.sw * 0.125 //80
        font.pixelSize: root.sh * 0.0333333 //16
    }

    Component.onCompleted: {
        var games = retroarchBackend.refreshGames(playlistPath)
        gameList.model = games
        if (games.length > 0) {
            var restore = (navListState.currentIndex !== undefined) ? navListState.currentIndex : 0
            gameList.currentIndex = Math.min(restore, games.length - 1)
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Contain)
            if (previewLoader.item)
                previewLoader.item.game = games[gameList.currentIndex]
        }
        gameList.forceActiveFocus()
    }
}
