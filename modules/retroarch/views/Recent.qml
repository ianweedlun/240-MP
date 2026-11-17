import QtQuick
import Components

// Recently played, newest first — the answer to "I was playing that an hour ago
// and now I have to find it again in a 1746-row list".
//
// Reached from the RECENT row at the top of the playlist screen. Each entry is a
// complete launch payload (see RecentGamesStore), so Return starts the game
// directly without touching a playlist file.
//
// Layout contract: same as Games.qml — thumbnail preview column (gated by the
// thumbnail_display setting) plus the A-Z letter-strip quick-scroll. The strip
// follows the rows' DISPLAY order (newest first here), not an alpha re-sort:
// jumping to a bucket has to land on the first row actually showing that letter.
FocusScope {
    id: recentRoot

    property var navParams: ({})
    property var navListState: navParams.navListState || ({})
    property bool listLoaded: false

    readonly property string thumbDisplay: {
        var v = appCore.get_setting(moduleRoot.moduleId, "thumbnail_display")
        // null/undefined means "never set" — the manifest default (RIGHT).
        // OFF itself must stay OFF, or "hide" looks identical to "right side".
        return (v === "LEFT" || v === "RIGHT" || v === "OFF") ? v : "RIGHT"
    }
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
        // No alpha sort: the recent list is newest-first, so the strip must
        // follow first appearance order to jump to the right row.
        return result
    }

    function syncLetterToGame() {
        if (letterIndex.length === 0 || !recentList.model) return
        var cur = recentList.model[recentList.currentIndex]
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

    // --- Zone geometry (same contract as Games.qml) --------------------------
    readonly property real letterX: root.sw - root.sw * 0.125 - letterColW
    readonly property real rightBoundary: showLetterNav ? (letterX - letterMargin)
                                                        : (root.sw - root.sw * 0.115625)
    readonly property real panelW: root.sw * 0.22
    readonly property real panelX: previewOnLeft ? root.sw * 0.125
                                                 : rightBoundary - panelW
    readonly property real listX: (showPreview && previewOnLeft)
        ? panelX + panelW + root.sw * 0.015625
        : root.sw * 0.115625
    readonly property real listRight: (showPreview && !previewOnLeft)
        ? panelX - root.sw * 0.015625
        : rightBoundary

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    focus: true
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            goBack()
            event.accepted = true
        }
    }

    AppBar {
        iconSource: moduleRoot.moduleIcon
        title: moduleRoot.moduleName
        subtitle: "RECENTLY PLAYED"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125
        anchors.leftMargin: root.sw * 0.125
    }

    // Count, right-aligned like the games list's "N GAMES · A-Z" header.
    Text {
        id: recentCountHeader
        visible: recentRoot.listLoaded && recentList.count > 0
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.195
        anchors.right: parent.right
        anchors.rightMargin: root.sw * 0.125
        text: recentList.count + " GAMES"
        color: root.secondaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        font.pixelSize: root.sh * 0.027
    }

    // Empty state — reachable only if the list was emptied between opening the
    // row and arriving here (a fresh install, or a wiped data dir).
    Column {
        anchors.centerIn: parent
        spacing: root.sh * 0.0333333
        visible: recentList.count === 0 && recentRoot.listLoaded
        Text {
            text: "Nothing played yet"
            color: root.secondaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.05
        }
        Text {
            text: "Launch a game and it lands here"
            color: root.tertiaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.0333333
        }
    }

    RowTrackingListView {
        id: recentList
        objectName: "recentList" // lab smoke-test handle
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25
        // Fixed zone geometry, not competing anchors + width bindings.
        x: recentRoot.listX
        width: recentRoot.listRight - recentRoot.listX
        height: root.sh * 0.525
        keyNavigationEnabled: true
        clip: true
        focus: true
        opacity: recentRoot.letterNavActive ? 0.3 : 1.0

        Keys.onReturnPressed: {
            var game = model[currentIndex]
            if (!game) return
            // The stored entry already carries romPath, corePath and systemId.
            retroarchBackend.launchGame(game, game.systemId || "")
        }

        Keys.onRightPressed: {
            if (recentRoot.showLetterNav && recentRoot.letterIndex.length > 0) {
                recentRoot.syncLetterToGame()
                recentRoot.letterNavActive = true
                letterList.forceActiveFocus()
            }
        }

        delegate: Item {
            width: recentList.width
            height: root.sh * 0.0583333

            Item {
                id: textClip
                width: Math.min(rowText.implicitWidth, recentList.width)
                height: parent.height
                clip: true

                Rectangle {
                    color: root.accentColor
                    anchors.fill: rowText
                    visible: recentList.currentIndex === index && !recentRoot.letterNavActive
                }

                Text {
                    id: rowText
                    text: modelData.name
                    color: (recentList.currentIndex === index && !recentRoot.letterNavActive)
                           ? root.surfaceColor : root.primaryColor
                    font.family: root.globalFont
                    font.capitalization: Font.AllUppercase
                    anchors.verticalCenter: parent.verticalCenter
                    x: 0
                    topPadding: root.sh * 0.0041667
                    leftPadding: root.sw * 0.009375
                    rightPadding: root.sw * 0.009375
                    bottomPadding: root.sh * 0.00625
                    font.pixelSize: root.sh * 0.05
                }

                SequentialAnimation {
                    running: (recentList.currentIndex === index) &&
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

            Text {
                text: modelData.shortName || ""
                // Right-aligned system label; never takes the inverted colour,
                // since the highlight bar only spans the name text.
                color: root.tertiaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                rightPadding: root.sw * 0.009375
                font.pixelSize: root.sh * 0.0333333
            }
        }

        Keys.onUpPressed: {
            if (count === 0) return
            if (currentIndex > 0) currentIndex--
            else currentIndex = count - 1
            recentList.positionViewAtIndex(recentList.currentIndex, ListView.Contain)
            recentRoot.syncLetterToGame()
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            recentList.positionViewAtIndex(recentList.currentIndex, ListView.Contain)
            recentRoot.syncLetterToGame()
        }
        onRowSelected: function(index) {
            if (previewLoader.item)
                previewLoader.item.game = recentRoot.ensureThumbnail(model[index])
        }
    }

    // Preview pane — same boxart component as the games list. The recent store
    // deliberately drops cached thumbnails; resolve from the stored playlist
    // basename on demand so moved folders can't leave a stale absolute path.
    Loader {
        id: previewLoader
        objectName: "previewLoader" // lab smoke-test handle
        visible: recentRoot.showPreview && recentList.count > 0
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25
        x: recentRoot.panelX
        width: recentRoot.panelW
        height: root.sh * 0.525
        source: "GamePreview.qml"
    }

    // Letter navigation panel (Plex parity), same contract as Games.qml.
    ListView {
        id: letterList
        objectName: "letterList" // lab smoke-test handle
        model: recentRoot.letterIndex
        visible: recentRoot.showLetterNav
        opacity: recentRoot.letterNavActive ? 1.0 : 0.3
        x: recentRoot.letterX
        anchors.top: recentList.top
        width: recentRoot.letterColW
        height: recentList.height
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
            recentList.currentIndex = recentRoot.letterIndex[currentIndex].firstIndex
            recentList.positionViewAtIndex(recentList.currentIndex, ListView.Beginning)
        }

        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) {
                currentIndex++
            } else {
                currentIndex = 0
                letterList.positionViewAtIndex(letterList.currentIndex, ListView.Beginning)
            }
            recentList.currentIndex = recentRoot.letterIndex[currentIndex].firstIndex
            recentList.positionViewAtIndex(recentList.currentIndex, ListView.Beginning)
        }

        Keys.onReturnPressed: {
            recentRoot.letterNavActive = false
            recentList.forceActiveFocus()
        }

        Keys.onLeftPressed: {
            recentRoot.letterNavActive = false
            recentList.forceActiveFocus()
        }

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                recentRoot.letterNavActive = false
                recentList.forceActiveFocus()
                event.accepted = true
            }
        }

        delegate: Item {
            width: letterList.width
            height: root.sh * 0.04375

            Rectangle {
                color: root.accentColor
                anchors.fill: parent
                visible: letterList.currentIndex === index && recentRoot.letterNavActive
            }

            Text {
                text: modelData.letter
                color: (letterList.currentIndex === index && recentRoot.letterNavActive)
                       ? root.surfaceColor : root.primaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                font.pixelSize: root.sh * 0.0354167
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                rightPadding: root.sw * 0.009375
                topPadding: root.sh * 0.0041667
                bottomPadding: root.sh * 0.00625
            }
        }
    }

    Text {
        id: footer
        text: recentRoot.showLetterNav
              ? root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.browse + ":BROWSE " + root.hints.select + ":PLAY"
              : root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":PLAY"
        color: root.tertiaryColor
        font.family: root.globalFont
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.1041667
        anchors.leftMargin: root.sw * 0.125
        font.pixelSize: root.sh * 0.0333333
    }

    function ensureThumbnail(game) {
        if (!game) return game
        if (game.thumbnail === undefined || game.thumbnail === "") {
            game.thumbnail = retroarchBackend.resolveThumbnail(game.name, game.playlistBasename || "")
        }
        return game
    }

    Component.onCompleted: {
        var games = retroarchBackend.recentGames()
        recentList.model = games
        recentRoot.listLoaded = true
        recentRoot.letterIndex = recentRoot.buildLetterIndex(games)
        if (games.length > 0) {
            var restore = (navListState.currentIndex !== undefined) ? navListState.currentIndex : 0
            recentList.currentIndex = Math.min(restore, games.length - 1)
            recentList.positionViewAtIndex(recentList.currentIndex, ListView.Contain)
            recentRoot.syncLetterToGame()
            previewLoader.item.game = recentRoot.ensureThumbnail(games[recentList.currentIndex])
        }
        recentList.forceActiveFocus()
    }
}
