import QtQuick
import Components

// Recently played, newest first — the answer to "I was playing that an hour ago
// and now I have to find it again in a 1746-row list".
//
// Reached from the RECENT row at the top of the playlist screen. Each entry is a
// complete launch payload (see RecentGamesStore), so Return starts the game
// directly without touching a playlist file.
FocusScope {
    id: recentRoot

    property var navParams: ({})
    property var navListState: navParams.navListState || ({})
    property bool listLoaded: false

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
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: root.sw * 0.115625
        anchors.rightMargin: root.sw * 0.115625
        width: root.sw * 0.76875
        height: root.sh * 0.525
        keyNavigationEnabled: true
        clip: true
        focus: true

        Keys.onReturnPressed: {
            var game = model[currentIndex]
            if (!game) return
            // The stored entry already carries romPath, corePath and systemId.
            retroarchBackend.launchGame(game, game.systemId || "")
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
                    visible: recentList.currentIndex === index
                }

                Text {
                    id: rowText
                    text: modelData.name
                    color: recentList.currentIndex === index ? root.surfaceColor : root.primaryColor
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
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            recentList.positionViewAtIndex(recentList.currentIndex, ListView.Contain)
        }
    }

    Component.onCompleted: {
        var games = retroarchBackend.recentGames()
        recentList.model = games
        recentRoot.listLoaded = true
        if (games.length > 0) {
            var restore = (navListState.currentIndex !== undefined) ? navListState.currentIndex : 0
            recentList.currentIndex = Math.min(restore, games.length - 1)
            recentList.positionViewAtIndex(recentList.currentIndex, ListView.Contain)
        }
        recentList.forceActiveFocus()
    }

    Text {
        id: footer
        text: root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":PLAY"
        color: root.tertiaryColor
        font.family: root.globalFont
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.1041667
        anchors.leftMargin: root.sw * 0.125
        font.pixelSize: root.sh * 0.0333333
    }
}
