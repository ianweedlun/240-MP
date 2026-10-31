import QtQuick
import Components

FocusScope {
    id: gamesRoot

    property var navParams: ({})
    property var navListState: navParams.navListState || ({})
    readonly property string playlistPath: navParams.playlistPath || ""
    readonly property string playlistName: navParams.playlistName || ""
    readonly property string playlistBasename: navParams.playlistBasename || ""
    readonly property string shortName: navParams.shortName || ""
    readonly property string thumbDisplay: {
        var v = appCore.get_setting(moduleRoot.moduleId, "thumbnail_display")
        // null/undefined means "never set" — the manifest default (RIGHT),
        // not "off". Without this, a config whose migration hasn't written
        // yet hides the panel on every view recreation.
        return (v === "LEFT" || v === "RIGHT") ? v : "RIGHT"
    }
    // Thumbnail setting only affects game list view. Playlist view always hides preview.
    readonly property bool showPreview: thumbDisplay !== "OFF"
    readonly property bool previewOnLeft: thumbDisplay === "LEFT"

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
        subtitle: shortName || playlistName
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125 //60
        anchors.leftMargin: root.sw * 0.125 //80
    }

    // Loading indicator
    Rectangle {
        id: loadingIndicator
        anchors.centerIn: parent
        width: root.sw * 0.3
        height: root.sh * 0.15
        color: "transparent"
        visible: gameList.count === 0 && !Component.onCompletedCalled
        border.color: root.accentColor
        border.width: 2
        radius: 8

        Text {
            anchors.centerIn: parent
            text: "LOADING..."
            color: root.accentColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: root.sh * 0.04 //20
        }

        SequentialAnimation {
            running: loadingIndicator.visible
            loops: Animation.Infinite
            PauseAnimation { duration: 500 }
            NumberAnimation { target: loadingIndicator; property: "opacity"; to: 0.3; duration: 500 }
            PauseAnimation { duration: 500 }
            NumberAnimation { target: loadingIndicator; property: "opacity"; to: 1; duration: 500 }
        }
    }

    // Empty state
    Column {
        anchors.centerIn: parent
        spacing: root.sh * 0.0333333 //16
        visible: gameList.count === 0 && !loadingIndicator.visible
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
        anchors.topMargin: root.sh * 0.25 //120
        anchors.left: previewOnLeft ? previewLoader.right : parent.left
        anchors.right: previewOnLeft ? parent.right : previewLoader.left
        anchors.leftMargin: previewOnLeft ? root.sw * 0.015625 : root.sw * 0.115625 //10 : 74
        anchors.rightMargin: previewOnLeft ? root.sw * 0.115625 : root.sw * 0.015625 //74 : 10
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
            previewLoader.item.game = ensureThumbnail(model[gameList.currentIndex])
    }

    // Preview pane — the selected game's boxart, when enabled
    Loader {
        id: previewLoader
        visible: gamesRoot.showPreview && gameList.count > 0
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25 //120
        width: root.sw * 0.22
        height: root.sh * 0.525 //252
        source: "GamePreview.qml"
        anchors.left: previewOnLeft ? parent.left : undefined
        anchors.leftMargin: previewOnLeft ? root.sw * 0.125 : undefined //80
        anchors.right: previewOnLeft ? undefined : parent.right
        anchors.rightMargin: previewOnLeft ? undefined : root.sw * 0.125 //80
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

    function ensureThumbnail(game) {
        if (!game) return game
        if (game.thumbnail === undefined || game.thumbnail === "") {
            game.thumbnail = retroarchBackend.resolveThumbnail(game.name, playlistBasename)
        }
        return game
    }

    Component.onCompleted: {
        var games = retroarchBackend.refreshGames(playlistPath)
        gameList.model = games
        if (games.length > 0) {
            var restore = (navListState.currentIndex !== undefined) ? navListState.currentIndex : 0
            gameList.currentIndex = Math.min(restore, games.length - 1)
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Contain)
            if (previewLoader.item)
                previewLoader.item.game = ensureThumbnail(games[gameList.currentIndex])
        }
        gameList.forceActiveFocus()
    }
}
