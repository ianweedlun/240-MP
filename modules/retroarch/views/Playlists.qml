import QtQuick
import Components

FocusScope {
    id: playlistsRoot

    property var navParams: ({})
    property var navListState: navParams.navListState || ({})
    readonly property string systemId: navParams.systemId || ""
    readonly property string systemName: navParams.systemName || ""
    readonly property bool showPreview: {
        var v = appCore.get_setting(moduleRoot.moduleId, "show_preview")
        return v !== false && v !== "OFF"
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
        subtitle: systemName
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125 //60
        anchors.leftMargin: root.sw * 0.125 //80
    }

    // Empty state
    Column {
        anchors.centerIn: parent
        spacing: root.sh * 0.0333333 //16
        visible: playlistList.count === 0
        Text {
            text: "No playlists for " + systemName
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
        id: playlistList
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.25 //120
        anchors.leftMargin: root.sw * 0.115625 //74
        width: playlistsRoot.showPreview ? root.sw * 0.5 : root.sw * 0.76875
        height: root.sh * 0.525 //252
        keyNavigationEnabled: true
        clip: true
        focus: true

        Keys.onReturnPressed: {
            var pl = model[currentIndex]
            if (!pl) return
            navigateTo("Games.qml",
                { playlistPath: pl.path, playlistName: pl.name, systemId: systemId },
                { currentIndex: playlistList.currentIndex })
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
                    text: modelData.name
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
                color: playlistList.currentIndex === index ? root.surfaceColor : root.tertiaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                rightPadding: root.sw * 0.009375 //6
                font.pixelSize: root.sh * 0.0333333 //16
            }
        }

        Keys.onUpPressed: {
            if (count === 0) return
            if (currentIndex > 0) currentIndex--
            else currentIndex = count - 1
            playlistList.positionViewAtIndex(playlistList.currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            playlistList.positionViewAtIndex(playlistList.currentIndex, ListView.Contain)
        }
        onCurrentIndexChanged: if (previewLoader.item)
            previewLoader.item.playlist = model[playlistList.currentIndex]
    }

    // Preview pane — the selected playlist's identity, when enabled
    Loader {
        id: previewLoader
        visible: playlistsRoot.showPreview && playlistList.count > 0
        anchors.right: parent.right
        anchors.rightMargin: root.sw * 0.125 //80
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.25 //120
        width: root.sw * 0.22
        height: root.sh * 0.525 //252
        source: "PlaylistPreview.qml"
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

    Component.onCompleted: {
        var pls = retroarchBackend.refreshPlaylists(systemId)
        playlistList.model = pls
        if (pls.length > 0) {
            var restore = (navListState.currentIndex !== undefined) ? navListState.currentIndex : 0
            playlistList.currentIndex = Math.min(restore, pls.length - 1)
            playlistList.positionViewAtIndex(playlistList.currentIndex, ListView.Contain)
            if (previewLoader.item)
                previewLoader.item.playlist = pls[playlistList.currentIndex]
        }
        playlistList.forceActiveFocus()
    }
}
