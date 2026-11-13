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

    // Truth for the LOADING guard: set once refreshGames has populated the model.
    // NOTE: `Component.onCompletedCalled` is not a Qt property — the Component
    // scope exposes only completed/destruction signal handlers (verified against
    // QQmlComponentAttached in plugins.qmltypes). The phantom read undefined =
    // falsy forever, so an empty playlist kept the spinner and its
    // "No games in this playlist" state was unreachable.
    property bool listLoaded: false
    readonly property string thumbDisplay: {
        var v = appCore.get_setting(moduleRoot.moduleId, "thumbnail_display")
        // null/undefined means "never set" — the manifest default (RIGHT),
        // not "off". OFF itself must stay OFF: it was previously caught by the
        // default arm and rendered as RIGHT, which made "hide" look identical
        // to "right side".
        return (v === "LEFT" || v === "RIGHT" || v === "OFF") ? v : "RIGHT"
    }
    // Thumbnail setting only affects game list view. Playlist view always hides preview.
    readonly property bool showPreview: thumbDisplay !== "OFF"
    readonly property bool previewOnLeft: thumbDisplay === "LEFT"

    // --- Alphabet quick scroll state & helpers -------------------------------
    property bool letterNavActive: false
    property var letterIndex: []
    readonly property bool showLetterNav: letterIndex.length > 1
    readonly property real letterColW: root.sw * 0.0328125 // 21px on 640w
    readonly property real letterMargin: root.sw * 0.02 // ~13px margin

    function bucket(text) {
        var ch = text.charAt(0).toUpperCase()
        return (ch >= 'A' && ch <= 'Z') ? ch : '#'
    }

    function sortKey(item) {
        var t = ((item && item.name) || "").trim()
        var articles = ["the ", "a ", "an "]
        var lower = t.toLowerCase()
        for (var i = 0; i < articles.length; i++) {
            if (lower.indexOf(articles[i]) === 0) {
                t = t.substring(articles[i].length)
                break
            }
        }
        return bucket(t)
    }

    function buildLetterIndex(gameArr) {
        if (!gameArr || gameArr.length === 0) return []
        var seen = {}
        var result = []
        for (var i = 0; i < gameArr.length; i++) {
            var letter = sortKey(gameArr[i])
            if (!seen[letter]) {
                seen[letter] = true
                result.push({ letter: letter, firstIndex: i })
            }
        }
        result.sort(function(a, b) {
            if (a.letter === '#') return -1
            if (b.letter === '#') return 1
            return a.letter < b.letter ? -1 : 1
        })
        return result
    }

    function syncLetterToGame() {
        if (letterIndex.length === 0 || !gameList.model) return
        var cur = gameList.model[gameList.currentIndex]
        if (!cur) return
        var curLetter = sortKey(cur)
        for (var i = 0; i < letterIndex.length; i++) {
            if (letterIndex[i].letter === curLetter) {
                letterList.currentIndex = i
                break
            }
        }
        letterList.positionViewAtIndex(letterList.currentIndex, ListView.Contain)
    }

    // --- Zone geometry -------------------------------------------------------
    // Letter index is always pinned to the far right when active/visible.
    readonly property real letterX: root.sw - root.sw * 0.125 - letterColW
    readonly property real rightBoundary: showLetterNav ? (letterX - letterMargin) : (root.sw - root.sw * 0.115625)

    readonly property real panelW: root.sw * 0.22
    readonly property real panelX: previewOnLeft ? root.sw * 0.125
                                                 : rightBoundary - panelW
    readonly property real listX: (showPreview && previewOnLeft)
        ? panelX + panelW + root.sw * 0.015625
        : root.sw * 0.115625
    readonly property real listRight: (showPreview && !previewOnLeft)
        ? panelX - root.sw * 0.015625
        : rightBoundary

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
        visible: gameList.count === 0 && !gamesRoot.listLoaded
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

    RowTrackingListView {
        id: gameList
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25 //120
        // Horizontal placement comes from the zone properties (fixed x/width),
        // not from anchors that the width binding then fights.
        x: gamesRoot.listX
        width: gamesRoot.listRight - gamesRoot.listX
        height: root.sh * 0.525 //252
        keyNavigationEnabled: true
        clip: true
        focus: true
        opacity: gamesRoot.letterNavActive ? 0.3 : 1.0

        Keys.onReturnPressed: {
            var game = model[currentIndex]
            if (!game) return
            retroarchBackend.launchGame(game, navParams.systemId)
        }

        Keys.onRightPressed: {
            if (gamesRoot.showLetterNav && gamesRoot.letterIndex.length > 0) {
                gamesRoot.syncLetterToGame()
                gamesRoot.letterNavActive = true
                letterList.forceActiveFocus()
            }
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
                    visible: gameList.currentIndex === index && !gamesRoot.letterNavActive
                }

                Text {
                    id: rowText
                    text: modelData.name
                    color: (gameList.currentIndex === index && !gamesRoot.letterNavActive)
                           ? root.surfaceColor : root.primaryColor
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
            gamesRoot.syncLetterToGame()
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Contain)
            gamesRoot.syncLetterToGame()
        }
        onRowSelected: function(index) {
            if (previewLoader.item)
                previewLoader.item.game = ensureThumbnail(model[index])
            gamesRoot.syncLetterToGame()
        }
    }

    // Preview pane — the selected game's boxart, when enabled. Fixed column
    // from the zone geometry; it never overlaps the list in any mode.
    Loader {
        id: previewLoader
        visible: gamesRoot.showPreview && gameList.count > 0
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25 //120
        x: gamesRoot.panelX
        width: gamesRoot.panelW
        height: root.sh * 0.525 //252
        source: "GamePreview.qml"
    }

    // Letter navigation panel (Plex parity)
    ListView {
        id: letterList
        model: gamesRoot.letterIndex
        visible: gamesRoot.showLetterNav
        opacity: gamesRoot.letterNavActive ? 1.0 : 0.3
        x: gamesRoot.letterX
        anchors.top: gameList.top
        width: gamesRoot.letterColW
        height: gameList.height
        clip: true
        focus: false

        Keys.onUpPressed: {
            if (count === 0) return
            if (currentIndex > 0) {
                currentIndex--
            } else {
                currentIndex = count - 1
                letterList.positionViewAtIndex(letterList.currentIndex, ListView.Beginning)
            }
            gameList.currentIndex = gamesRoot.letterIndex[currentIndex].firstIndex
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Beginning)
        }

        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) {
                currentIndex++
            } else {
                currentIndex = 0
                letterList.positionViewAtIndex(letterList.currentIndex, ListView.Beginning)
            }
            gameList.currentIndex = gamesRoot.letterIndex[currentIndex].firstIndex
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Beginning)
        }

        Keys.onReturnPressed: {
            gamesRoot.letterNavActive = false
            gameList.forceActiveFocus()
        }

        Keys.onLeftPressed: {
            gamesRoot.letterNavActive = false
            gameList.forceActiveFocus()
        }

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                gamesRoot.letterNavActive = false
                gameList.forceActiveFocus()
                event.accepted = true
            }
        }

        delegate: Item {
            width: letterList.width
            height: root.sh * 0.04375 //21

            Rectangle {
                color: root.accentColor
                anchors.fill: parent
                visible: letterList.currentIndex === index && gamesRoot.letterNavActive
            }

            Text {
                text: modelData.letter
                color: (letterList.currentIndex === index && gamesRoot.letterNavActive)
                       ? root.surfaceColor : root.primaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                font.pixelSize: root.sh * 0.0354167 //17
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                rightPadding: root.sw * 0.009375 //6
                topPadding: root.sh * 0.0041667 //2
                bottomPadding: root.sh * 0.00625 //3
            }
        }
    }

    // Footer
    Text {
        id: footer
        text: gamesRoot.showLetterNav
              ? root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.browse + ":BROWSE " + root.hints.select + ":PLAY"
              : root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":PLAY"
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
        gamesRoot.letterIndex = gamesRoot.buildLetterIndex(games)
        gamesRoot.listLoaded = true
        if (games.length > 0) {
            var restore = (navListState.currentIndex !== undefined) ? navListState.currentIndex : 0
            gameList.currentIndex = Math.min(restore, games.length - 1)
            gameList.positionViewAtIndex(gameList.currentIndex, ListView.Contain)
            gamesRoot.syncLetterToGame()
            if (previewLoader.item)
                previewLoader.item.game = ensureThumbnail(games[gameList.currentIndex])
        }
        gameList.forceActiveFocus()
    }
}
