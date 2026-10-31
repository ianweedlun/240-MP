import QtQuick
import Components

FocusScope {
    id: systemsRoot

    property var navParams: ({})
    property var navListState: navParams.navListState || ({})

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
        visible: systemList.count === 0 && !Component.onCompletedCalled
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
        visible: systemList.count === 0 && !loadingIndicator.visible
        Text {
            text: "No systems found"
            color: root.secondaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.05 //24
        }
        Text {
            text: "Point RETROARCH at your playlist folder in Settings"
            color: root.tertiaryColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            horizontalAlignment: Text.AlignHCenter
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: root.sh * 0.0333333 //16
        }
    }

    // System list — narrower when the preview pane is up
    ListView {
        id: systemList
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
            var sys = model[currentIndex]
            if (!sys) return
            navigateTo("Playlists.qml",
                { systemId: sys.id, systemName: sys.name },
                { currentIndex: systemList.currentIndex })
        }

        delegate: Item {
            width: systemList.width
            height: root.sh * 0.0583333 //28

            Item {
                id: textClip
                width: Math.min(rowText.implicitWidth, systemList.width)
                height: parent.height
                clip: true

                Rectangle {
                    color: root.accentColor
                    anchors.fill: rowText
                    visible: systemList.currentIndex === index
                }

                Text {
                    id: rowText
                    text: modelData.name
                    color: systemList.currentIndex === index ? root.surfaceColor : root.primaryColor
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
                    running: (systemList.currentIndex === index) &&
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
                color: systemList.currentIndex === index ? root.surfaceColor : root.tertiaryColor
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
            systemList.positionViewAtIndex(systemList.currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            systemList.positionViewAtIndex(systemList.currentIndex, ListView.Contain)
        }
        onCurrentIndexChanged: if (previewLoader.item)
            previewLoader.item.system = model[systemList.currentIndex]
    }

    Component.onCompleted: {
        var systems = retroarchBackend.refreshSystems()
        systemList.model = systems
        if (systems.length > 0) {
            var restore = (navListState.currentIndex !== undefined) ? navListState.currentIndex : 0
            systemList.currentIndex = Math.min(restore, systems.length - 1)
            systemList.positionViewAtIndex(systemList.currentIndex, ListView.Contain)
        }
        systemList.forceActiveFocus()
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
