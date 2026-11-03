import QtQuick
import Components

// Settings subview: the mpv-facing knobs. Reached from Settings (the
// "Playback" row, tab-selected there like the other subviews); Return opens
// this list, ESC returns through the nav contract.
//
// The rows are defined HERE — single source of truth for the playback
// controls. Values read live from appCore.get_settings(), whose app section
// reflects the user's ~/.config/mpv/mpv.conf directly (the direct-edit
// chain: values are the file's real values, mpv built-in defaults when
// unset, "Custom (raw)" outside the menu). Left/Right persists via
// appCore.save_setting back into that same file. No override layer.
FocusScope {
    id: playbackSettingsRoot

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    property var navParams: ({})
    property var navListState: ({})

    property var appSettings: ({})

    // Flat model: the six mpv-facing controls (no sections in this view)
    property var settingsItems: []

    function buildModel() {
        var cfg = appCore.get_settings()
        appSettings = cfg.app || {}

        var items = []

        // Auto Fit — replaces the old blanket Auto Crop: the fit is decided
        // per load through MpvController IPC (see applyAutoFit), not at
        // launch. Content at or below the display's aspect fills the screen
        // (its overflow is cropped); wider content keeps its aspect (letterbox).
        // Off by default; the live CROP toggle in the OSC is unchanged.
        items.push({
            type: "list_single",
            key: "auto_fit",
            label: "Auto Fit",
            options: ["Off", "On"],
            value: appSettings["auto_fit"] || "Off",
            description: "[ON] Fit content to current aspect ratio\n [OFF] Do not reshape content",
            moduleId: ""
        })

        // Video Levels — the RGB range mpv converts YUV into. mpv's default
        // is full range; a display expecting studio (limited) levels renders
        // that as crushed blacks and blown whites, and the reverse reads as
        // washed-out gray. Takes effect on the next video. The key keeps
        // mpv's own name; the label uses what TVs and GPU drivers call it.
        items.push({
            type: "list_single",
            key: "video_output_levels",
            label: "Video Levels",
            options: ["Auto", "Limited", "Full"],
            value: appSettings["video_output_levels"] || "Auto",
            description: "Match the color range your display expects\n[LIMITED] If blacks crush  [FULL] If blacks look gray",
            moduleId: ""
        })

        // The basic mpv knobs READ AND WRITE the user's mpv.conf directly:
        // the menu shows the file's real value (mpv's built-in defaults when
        // unset; "Custom (raw)" for values outside this menu).
        items.push({
            type: "list_single",
            key: "replaygain",
            label: "Audio Normalize",
            options: ["Off", "Track", "Album"],
            value: appSettings["replaygain"] || "Custom (auto)",
            description: "Normalizes volume with ReplayGain\n[TRACK] Use per-track tags [ALBUM] Use per-album tags",
            moduleId: ""
        })
        items.push({
            type: "list_single",
            key: "audio_gain",
            label: "Sound Boost",
            options: ["0", "2", "4", "6", "8", "-4"],
            value: appSettings["audio_gain"] || "0",
            description: "Raise all audio output by decibels (dB)",
            moduleId: ""
        })
        items.push({
            type: "list_single",
            key: "deinterlace",
            label: "Deinterlace",
            options: ["Off", "Auto", "On"],
            value: appSettings["deinterlace"] || "Off",
            description: "Deinterlaced sources into progressive lines\n[AUTO] Only when the source is interlaced",
            moduleId: ""
        })
        items.push({
            type: "list_single",
            key: "audio_channels",
            label: "Audio Channels",
            options: ["Auto", "Stereo", "Mono"],
            value: appSettings["audio_channels"] || "Custom (auto)",
            description: "[STEREO] Two channels  [MONO] One channel (shared CRT speaker)",
            moduleId: ""
        })

        settingsItems = items

        // Restore the parent's saved focus, or start on the first row
        if (navListState.currentIndex !== undefined) {
            settingsList.currentIndex = Math.max(0, Math.min(settingsItems.length - 1,
                                                            navListState.currentIndex))
        } else {
            settingsList.currentIndex = 0
        }
    }

    Component.onCompleted: buildModel()

    RowTrackingListView {
        id: settingsList
        model: settingsItems

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.25 //120
        anchors.leftMargin: root.sw * 0.115625 //74
        width: root.sw * 0.76875 //492
        height: root.sh * 0.525 //252
        clip: true
        focus: true

        Keys.onUpPressed: {
            if (currentIndex > 0) currentIndex--
            else currentIndex = settingsItems.length - 1
            settingsList.positionViewAtIndex(currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            settingsList.positionViewAtIndex(currentIndex, ListView.Contain)
        }

        Keys.onLeftPressed: {
            var row = settingsItems[currentIndex]
            if (row && row.type === "list_single") {
                var opts = row.options
                var idx = opts.indexOf(row.value)
                var newIdx = (idx - 1 + opts.length) % opts.length
                var newVal = opts[newIdx]
                // Display the label; persist the parallel value when one exists.
                var savedVal = row.values ? row.values[newIdx] : newVal
                var updated = settingsItems.slice()
                updated[currentIndex] = Object.assign({}, row, { value: newVal })
                var savedIndex = currentIndex
                settingsItems = updated
                currentIndex = savedIndex
                appCore.save_setting(row.moduleId, row.key, savedVal)
            }
        }
        Keys.onRightPressed: {
            var row = settingsItems[currentIndex]
            if (row && row.type === "list_single") {
                var opts = row.options
                var idx = opts.indexOf(row.value)
                var newIdx = (idx + 1) % opts.length
                var newVal = opts[newIdx]
                var savedVal = row.values ? row.values[newIdx] : newVal
                var updated = settingsItems.slice()
                updated[currentIndex] = Object.assign({}, row, { value: newVal })
                var savedIndex = currentIndex
                settingsItems = updated
                currentIndex = savedIndex
                appCore.save_setting(row.moduleId, row.key, savedVal)
            }
        }

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                playbackSettingsRoot.goBack()
                event.accepted = true
            }
        }

        delegate: Item {
            width: settingsList.width
            height: root.sh * 0.0583333 //28

            // --- SELECTABLE ROW (this view has no sections/submenus) ---
            Rectangle {
                anchors.fill: parent
                color: settingsList.currentIndex === index ? root.accentColor : "transparent"

                // Label
                Text {
                    text: modelData.label || ""
                    color: settingsList.currentIndex === index ? root.surfaceColor : root.primaryColor
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

                // Value / arrow indicator
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: root.sw * 0.009375 //6
                    spacing: root.sw * 0.00625 //4

                    Text {
                        text: "\u25C4"
                        color: settingsList.currentIndex === index ? root.surfaceColor : root.tertiaryColor
                        font.family: root.globalFont
                        anchors.verticalCenter: parent.verticalCenter
                        topPadding: root.sh * 0.0041667 //2
                        bottomPadding: root.sh * 0.00625 //3
                        font.pixelSize: root.sh * 0.0375 //18
                    }
                    Text {
                        text: modelData.value || ""
                        color: settingsList.currentIndex === index ? root.surfaceColor : root.primaryColor
                        font.family: root.globalFont
                        font.capitalization: Font.AllUppercase
                        anchors.verticalCenter: parent.verticalCenter
                        topPadding: root.sh * 0.0041667 //2
                        leftPadding: root.sw * 0.009375 //6
                        rightPadding: root.sw * 0.009375 //6
                        bottomPadding: root.sh * 0.00625 //3
                        font.pixelSize: root.sh * 0.05 //24
                    }
                    Text {
                        text: "\u25BA"
                        color: settingsList.currentIndex === index ? root.surfaceColor : root.tertiaryColor
                        font.family: root.globalFont
                        anchors.verticalCenter: parent.verticalCenter
                        topPadding: root.sh * 0.0041667 //2
                        bottomPadding: root.sh * 0.00625 //3
                        font.pixelSize: root.sh * 0.05 //18
                    }
                }
            }
        }
    }

    // --- HELP TEXT --- (shown when a focused row has a description)
    Rectangle {
        id: rowHelpBackground
        property var currentRow: playbackSettingsRoot.settingsItems[settingsList.currentIndex]
        visible: !!(currentRow && currentRow.description)
        property color baseColor: root.primaryColor
        color: Qt.rgba(baseColor.r, baseColor.g, baseColor.b, 0.2)
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.1583333 //76
        anchors.leftMargin: root.sw * 0.125 //80
        width: root.sw * 0.75 //480
        height: root.sh * 0.0583333 //28
        clip: true
        Text {
            id: rowHelp
            text: (rowHelpBackground.currentRow && rowHelpBackground.currentRow.description) || ""
            color: root.primaryColor
            font.family: root.globalFont
            font.pixelSize: root.sh * 0.0291667 //14
            wrapMode: Text.WordWrap
            anchors.fill: parent
            anchors.margins: root.sw * 0.0125 //6
        }
    }
}
