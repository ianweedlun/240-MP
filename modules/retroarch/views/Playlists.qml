import QtQuick
import Components

FocusScope {
    id: playlistsRoot

    property var navParams: ({})
    property var navListState: navParams.navListState || ({})
    readonly property string systemId: navParams.systemId || ""
    readonly property string systemName: navParams.systemName || ""
    readonly property bool showAllPlaylists: systemId === ""

    // See Games.qml: the LOADING guard needs a real truth flag —
    // `Component.onCompletedCalled` was never a Qt property.
    property bool listLoaded: false
    property string playlistSortLabel: "SYSTEM"
    property bool sortHeaderFocused: false

    // Same persisted setting the Settings menu writes. Playlists have no letter
    // strip, so the header is reached with Up from the first row.
    function togglePlaylistSort() {
        var next = retroarchBackend.playlistSort() === "COMPANY" ? "SYSTEM" : "COMPANY"
        appCore.save_setting(moduleRoot.moduleId, "playlist_sort", next)
    }

    signal navigateTo(string path, var params, var listState)
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
        subtitle: showAllPlaylists ? "ALL PLAYLISTS" : systemName
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125 //60
        anchors.leftMargin: root.sw * 0.125 //80
    }

    // Sort header: only the direction word is the control, matching Games and
    // Recent. Up from the first playlist row focuses it; Enter toggles the
    // persisted playlist_sort setting.
    Row {
        id: playlistSortHeader
        objectName: "playlistSortHeader" // lab smoke-test handle
        visible: playlistsRoot.listLoaded && playlistList.count > 0
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.195
        anchors.right: parent.right
        anchors.rightMargin: root.sw * 0.125
        spacing: 0

        Text {
            id: playlistCountLabel
            objectName: "playlistSortCount" // lab smoke-test handle
            text: playlistList.count + " PLAYLISTS  ·  "
            color: root.secondaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            font.pixelSize: root.sh * 0.027
        }

        Item {
            id: playlistSortButton
            width: playlistSortDirection.implicitWidth + root.sw * 0.02
            height: playlistSortDirection.implicitHeight + root.sh * 0.012

            Rectangle {
                id: sortHeaderHighlight
                objectName: "sortHeaderHighlight" // lab smoke-test handle
                anchors.fill: parent
                visible: playlistsRoot.sortHeaderFocused
                color: root.accentColor
            }

            Text {
                id: playlistSortDirection
                objectName: "playlistSortDirection" // lab smoke-test handle
                anchors.centerIn: parent
                text: playlistsRoot.playlistSortLabel
                color: playlistsRoot.sortHeaderFocused ? root.surfaceColor : root.secondaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                font.pixelSize: root.sh * 0.027

                Keys.enabled: true
                Keys.onReturnPressed: function(event) {
                    playlistsRoot.togglePlaylistSort()
                    event.accepted = true
                }
                Keys.onUpPressed: function(event) { event.accepted = true } // top of the tab order
                Keys.onDownPressed: function(event) {
                    playlistsRoot.sortHeaderFocused = false
                    playlistList.forceActiveFocus() // returns to the row you were on
                    event.accepted = true
                }
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                        playlistsRoot.sortHeaderFocused = false
                        playlistList.forceActiveFocus()
                        event.accepted = true
                    }
                }
            }
        }
    }

    // Loading indicator
    Rectangle {
        id: loadingIndicator
        anchors.centerIn: parent
        width: root.sw * 0.3
        height: root.sh * 0.15
        color: "transparent"
        visible: playlistList.count === 0 && !playlistsRoot.listLoaded
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
        visible: playlistList.count === 0 && !loadingIndicator.visible
        Text {
            text: showAllPlaylists ? "No playlists found" : "No playlists for " + systemName
            color: root.secondaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.05 //24
        }
        Text {
            text: showAllPlaylists ? "Add games in RetroArch to create playlists" : "Add games to this core in RetroArch"
            color: root.tertiaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.0333333 //16
        }
    }

    RowTrackingListView {
        id: playlistList
        objectName: "playlistList" // lab smoke-test handle

        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25 //120
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: root.sw * 0.115625 //74
        anchors.rightMargin: root.sw * 0.115625 //74
        width: root.sw * 0.76875
        height: root.sh * 0.525 //252
        keyNavigationEnabled: true
        clip: true
        focus: true

        Keys.onReturnPressed: {
            var pl = model[currentIndex]
            if (!pl) return
            // The RECENT row is a door, not a playlist: it opens the recently
            // played list, which launches straight from stored entries.
            if (pl.recentRow) {
                navigateTo("Recent.qml", {},
                    { currentIndex: playlistList.currentIndex, key: "__recent" })
                return
            }
            navigateTo("Games.qml",
                { playlistPath: pl.path, playlistName: pl.name, playlistBasename: pl.name, shortName: pl.shortName || pl.name, systemId: systemId },
                { currentIndex: playlistList.currentIndex, key: pl.path })
        }

        delegate: Item {
            width: playlistList.width
            height: root.sh * 0.0583333 //28

            Item {
                id: textClip
                width: Math.min(rowText.implicitWidth, playlistList.width)
                height: parent.height
                clip: true

                Rectangle {
                    color: root.accentColor
                    anchors.fill: rowText
                    visible: playlistList.currentIndex === index
                }

                Text {
                    id: rowText
                    // Flat "all playlists" list: label each row by its short
                    // system name (the whole point of skipping the systems view).
                    // Inside a single system, the full playlist name is shown.
                    text: showAllPlaylists ? modelData.shortName : modelData.name
                    color: playlistList.currentIndex === index ? root.surfaceColor : root.primaryColor
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
                    running: (playlistList.currentIndex === index) &&
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
                text: modelData.count + " GAMES"
                // The highlight bar spans only the name text (textClip), never
                // this right-aligned column — so the count must never take the
                // inverted surfaceColor, or it goes dark-on-dark when selected.
                color: root.tertiaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                rightPadding: root.sw * 0.009375 //6
                font.pixelSize: root.sh * 0.0333333 //16
            }
        }

        Keys.onUpPressed: function(event) {
            if (count === 0) return
            if (currentIndex > 0) {
                currentIndex--
            } else {
                // Past the top row: focus the playlist sort header instead of
                // wrapping to the bottom.
                playlistsRoot.sortHeaderFocused = true
                playlistSortDirection.forceActiveFocus()
                event.accepted = true
                return
            }
            playlistList.positionViewAtIndex(playlistList.currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            playlistList.positionViewAtIndex(playlistList.currentIndex, ListView.Contain)
        }
    }

    Component.onCompleted: {
        refreshPlaylistModel()
        playlistList.forceActiveFocus()
    }

    // Rows the list actually shows: an optional RECENT door, then the playlists.
    // Kept as its own function so the index restore below can talk about row
    // *identity* — a bare index silently points one row off the moment the recent
    // row appears (first launch) or disappears (data wipe).
    function buildRows(pls) {
        var rows = []
        if (showAllPlaylists) {
            var recent = retroarchBackend.recentGames()
            if (recent.length > 0) {
                rows.push({ recentRow: true, name: "RECENTLY PLAYED",
                            shortName: "RECENTLY PLAYED", count: recent.length, path: "" })
            }
        }
        for (var i = 0; i < pls.length; i++) rows.push(pls[i])
        return rows
    }

    function refreshPlaylistModel() {
        var pls = showAllPlaylists
            ? retroarchBackend.refreshAllPlaylists()
            : retroarchBackend.refreshPlaylists(systemId)
        var rows = buildRows(pls)
        playlistList.model = rows
        playlistsRoot.playlistSortLabel = retroarchBackend.playlistSort()
        playlistsRoot.listLoaded = true
        if (rows.length === 0) return

        var target = 0
        var key = navListState.key
        if (key !== undefined) {
            for (var i = 0; i < rows.length; i++) {
                var rowKey = rows[i].recentRow ? "__recent" : rows[i].path
                if (rowKey === key) { target = i; break }
            }
        } else if (navListState.currentIndex !== undefined) {
            target = navListState.currentIndex
        }
        playlistList.currentIndex = Math.min(Math.max(target, 0), rows.length - 1)
        playlistList.positionViewAtIndex(playlistList.currentIndex, ListView.Contain)
    }

    Connections {
        target: appCore
        function onModuleSettingChanged(mid, key, value) {
            if (mid === moduleRoot.moduleId && key === "playlist_sort") {
                refreshPlaylistModel()
            }
        }
    }

    // Footer
    Text {
        id: footer
        text: root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":SELECT"
        color: root.tertiaryColor
        font.family: root.globalFont
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.1041667 //50
        anchors.leftMargin: root.sw * 0.125 //80
        font.pixelSize: root.sh * 0.0333333 //16
    }
}
