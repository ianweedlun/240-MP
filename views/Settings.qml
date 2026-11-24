import QtQuick
import Components

FocusScope {
    id: settingsRoot

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    property var navParams: ({})
    property var navListState: ({})

    property var appSettings: ({})
    property var installedModules: []

    // Flat model: mix of section headers and rows
    property var settingsItems: []

    property bool quitOverlayVisible: false
    property int quitChoiceIndex: 0

    // CFG-01: two-step reset confirmation for a damaged settings file.
    // Step 1 (Enter on the reset row) opens the overlay — NO write happens.
    // Step 2 (Enter on Yes) performs the reset. Default choice is No.
    property bool resetOverlayVisible: false
    property int resetChoiceIndex: 1

    // Quit overlay choices. Under the autostart service (headless RPi) the quit menu has an
    // "Reboot" option and an "Exit to Terminal" option that drops to a tty1 login without
    // powering off; those options are not needed on macOS/Desktop or when run by hand, so
    // it's yes/no for that case.
    property bool autostartSession: false
    property var quitOptions: settingsRoot.autostartSession
        ? [{ label: "Power Off",        action: "quit"     },
           { label: "Reboot",           action: "reboot"   },
           { label: "Exit to Terminal", action: "terminal" },
           { label: "Cancel",           action: "cancel"   }]
        : [{ label: "Yes", action: "quit" },
           { label: "No",  action: "cancel" }]

    function buildModel() {
        var cfg = appCore.get_settings()
        appSettings = cfg.app || {}
        installedModules = appCore.get_installed_modules()
        autostartSession = appCore.isAutostartSession()

        var items = []

        // CFG-01: while the settings file is damaged, every write is refused;
        // the explicit reset is the only way to make changes again, so it goes
        // at the top of the list. The description carries the wipe semantics;
        // the overlay carries the pinned confirmation wording.
        if (appCore.configCorrupted) {
            items.push({
                type: "action",
                key: "reset_settings",
                label: "Reset Settings File",
                description: "Erases saved settings and signs out of media servers. The damaged file is kept for recovery."
            })
        }

        // APPLICATION section
        var colorOpts = ["Video 1","Late Night","Synthwave","Terminal","T-120","Amber","Kinescope","SMPTE ECR 1-1978","Dracula","Tokyo Night","Nord","Solarized Dark","Monokai"] // names must match Main.qml themes keys exactly; picker shows this order
        // Adding a new approach to add multiple custom themes at once
        var cThemes = appCore.getCustomColorSchemes()
        if (Object.keys(cThemes).length > 0) {
            for (var cTheme in cThemes) {
                if (!colorOpts.includes(cTheme) && Object.keys(cThemes[cTheme]).length === 5) colorOpts.push(cTheme)
            }
        }
        // Still support the single-theme approach
        var custom = appCore.getCustomColorScheme()
        if (!colorOpts.includes("Custom") && Object.keys(custom).length === 5) colorOpts.push("Custom")
        items.push({
            type: "list_single",
            key: "color_scheme",
            label: "Color Scheme",
            options: colorOpts,
            value: appSettings["color_scheme"] || "Video 1",
            description: "Choose your prefered color scheme\nPlease see the wiki for details on adding a custom one",
            moduleId: ""
        })

        // Start on Module — pick a module to auto-launch into on startup.
        // Only present enabled modules as options so disabled ones won't display.
        // The setting is keyed by module id and the picker shows the display name
        // If the stored id isn't an enabled module, the display falls back to None.
        var moduleOpts = ["None"] // display labels
        var moduleVals = ["None"] // stored values (module ids)
        var startupId = appSettings["startup_module"] || "None"
        var startupValue = "None"
        for (var mi = 0; mi < installedModules.length; mi++) {
            if (!installedModules[mi].enabled) continue
            moduleOpts.push(installedModules[mi].name)
            moduleVals.push(installedModules[mi].id)
            if (installedModules[mi].id === startupId) startupValue = installedModules[mi].name
        }
        items.push({
            type: "list_single",
            key: "startup_module",
            label: "Start on Module",
            options: moduleOpts,
            values: moduleVals,
            value: startupValue,
            description: "Directly launch into a specific module on startup",
            moduleId: ""
        })

        // PLAYBACK subview — all mpv-facing knobs live in their own view
        // (views/PlaybackSettings.qml), entered from the Application section's
        // list per the maintainer's refinement; rows preserved verbatim there.
        // SCREEN SAVER section — single control: OFF disables, a number sets the
        // timeout for both menu idle and playback pause (handled inside mpv).
        items.push({
            type: "list_single",
            key: "screensaver_timeout",
            label: "Screen Saver",
            options: ["OFF", "30", "60", "120"],
            value: appSettings["screensaver_timeout"] || "OFF",
            description: "Prevent CRT burn-in after seconds of inactivity or pause",
            moduleId: ""
        })

        // MODULES section — only show modules with has_settings
        var hasModuleSettings = false
        for (var i = 0; i < installedModules.length; i++) {
            if (installedModules[i].has_settings) { hasModuleSettings = true; break }
        }

        if (hasModuleSettings) {
            items.push({ type: "section", label: "Modules" })
            for (var j = 0; j < installedModules.length; j++) {
                var m = installedModules[j]
                if (m.has_settings) {
                    items.push({ type: "submenu", label: m.name, moduleId: m.id })
                }
            }
        }

        // SYSTEM section
        items.push({ type: "section", label: "Application" })
        // Playback subview first, above Controls: the maintainer asked that the
        // user find every exposed mpv knob by entering "PLAYBACK" here.
        items.push({
            type: "submenu",
            key: "playback_settings",
            label: "Playback",
            moduleId: ""
        })
        items.push({
            type: "submenu",
            key: "remap_controls",
            label: "Controls",
            moduleId: ""
        })
        items.push({
            type: "submenu",
            key: "software_update",
            label: "Update",
            moduleId: ""
        })
        items.push({ type: "quit", label: "Quit" })

        settingsItems = items

        // Restore saved position, or default to first selectable row
        if (navListState.currentIndex !== undefined) {
            settingsList.currentIndex = Math.min(navListState.currentIndex, items.length - 1)
        } else {
            for (var k = 0; k < items.length; k++) {
                if (items[k].type !== "section") {
                    settingsList.currentIndex = k
                    break
                }
            }
        }
        settingsList.positionViewAtIndex(settingsList.currentIndex, ListView.Contain)
    }

    function firstSelectableAfter(idx) {
        for (var i = idx + 1; i < settingsItems.length; i++) {
            if (settingsItems[i].type !== "section") return i
        }
        return settingsList.currentIndex
    }

    function firstSelectableBefore(idx) {
        for (var i = idx - 1; i >= 0; i--) {
            if (settingsItems[i].type !== "section") return i
        }
        return settingsList.currentIndex
    }

    Component.onCompleted: buildModel()

    // Header
    AppBar {
        iconSource: "../../assets/images/settings.svg"
        title: "Settings"
        subtitle: root.appVersion
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125 //60
        anchors.leftMargin: root.sw * 0.125 //80
    }

    // IP address
    property string ipAddress: ""
    Timer {
        interval: 5000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: settingsRoot.ipAddress = appCore.localIpAddress()
    }
    Text {
        text: settingsRoot.ipAddress
        visible: settingsRoot.ipAddress !== ""
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: root.sh * 0.125 //60
        anchors.rightMargin: root.sw * 0.125 //80
        font.pixelSize: root.sh * 0.0291667 //14
        color: root.tertiaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        topPadding: root.sh * 0.0125 //6
        rightPadding: root.sw * 0.00625 //4
    }

    // ── CFG-01 config-health banner ──
    // Degraded: settings file damaged, writes refused until a confirmed reset.
    // Recovery notice: transient, after a successful backup restore. Sits in
    // the gap between AppBar and list; the list shifts down while visible so
    // nothing overlaps (CRT-safe, root.sh-relative).
    Rectangle {
        id: configBanner
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.235
        anchors.left: parent.left
        anchors.leftMargin: root.sw * 0.115625 //74
        width: root.sw * 0.76875 //492
        height: root.sh * 0.095
        visible: appCore.configCorrupted || appCore.configNotice.length > 0
        color: appCore.configCorrupted ? "#3A2A00" : "#0A2A10"
        border.color: appCore.configCorrupted ? "#FFB000" : "#40C060"
        border.width: 1
        radius: 4
        z: 50

        Text {
            anchors.fill: parent
            anchors.margins: root.sh * 0.008
            text: appCore.configCorrupted
                  ? "Settings file is damaged. Your saved settings are protected and will not be overwritten. Reset to defaults to make changes."
                  : appCore.configNotice
            font.family: root.globalFont
            font.pixelSize: root.sh * 0.025 //12
            color: appCore.configCorrupted ? "#FFB000" : "#40C060"
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    RowTrackingListView {
        id: settingsList
        model: settingsItems

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: configBanner.visible ? root.sh * 0.345 : root.sh * 0.25 //120
        anchors.leftMargin: root.sw * 0.115625 //74
        width: root.sw * 0.76875 //492
        height: root.sh * 0.525 - (configBanner.visible ? root.sh * 0.095 : 0) //252
        clip: true
        focus: true

        Keys.onUpPressed: {
            if (currentIndex > 0) currentIndex-- 
            else {
                currentIndex = settingsItems.length-1
            }
            while (settingsItems[currentIndex].type == "section") {
                currentIndex--
            }
            settingsList.positionViewAtIndex(currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            while (settingsItems[currentIndex].type == "section") {
                currentIndex++
            }
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
                // CFG-01: only show the new value when the write actually
                // landed — a refused (degraded) save keeps the real value on
                // screen instead of lying about a change that never persisted.
                if (appCore.save_setting(row.moduleId, row.key, savedVal)) {
                    var updated = settingsItems.slice()
                    updated[currentIndex] = Object.assign({}, row, { value: newVal })
                    var savedIndex = currentIndex
                    settingsItems = updated
                    currentIndex = savedIndex
                }
            }
        }

        Keys.onRightPressed: {
            var row = settingsItems[currentIndex]
            if (row && row.type === "list_single") {
                var opts = row.options
                var idx = opts.indexOf(row.value)
                var newIdx = (idx + 1) % opts.length
                var newVal = opts[newIdx]
                // Display the label; persist the parallel value when one exists.
                var savedVal = row.values ? row.values[newIdx] : newVal
                // CFG-01: see onLeftPressed — refused saves don't change the UI.
                if (appCore.save_setting(row.moduleId, row.key, savedVal)) {
                    var updated = settingsItems.slice()
                    updated[currentIndex] = Object.assign({}, row, { value: newVal })
                    var savedIndex = currentIndex
                    settingsItems = updated
                    currentIndex = savedIndex
                }
            }
        }

        Keys.onReturnPressed: {
            var row = settingsItems[currentIndex]
            if (row && row.type === "submenu") {
                if (row.key === "software_update")
                    settingsRoot.navigateTo("views/Update.qml", {}, { currentIndex: settingsList.currentIndex })
                else if (row.key === "playback_settings")
                    settingsRoot.navigateTo("views/PlaybackSettings.qml", {}, { currentIndex: settingsList.currentIndex })
                else if (row.key === "remap_controls")
                    settingsRoot.navigateTo("views/RemapControls.qml", {}, { currentIndex: settingsList.currentIndex })
                else
                    settingsRoot.navigateTo("views/ModuleSettings.qml", { moduleId: row.moduleId }, { currentIndex: settingsList.currentIndex })
            } else if (row && row.type === "quit") {
                settingsRoot.quitChoiceIndex = 0
                settingsRoot.quitOverlayVisible = true
            } else if (row && row.type === "action" && row.key === "reset_settings") {
                // CFG-01 step 1: open the confirmation. No write happens here.
                settingsRoot.resetChoiceIndex = 1   // default No
                settingsRoot.resetOverlayVisible = true
            }
        }

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                settingsRoot.goBack()
                event.accepted = true
            }
        }

        delegate: Item {
            width: settingsList.width
            height: root.sh * 0.0583333 //28

            // --- SECTION LABEL ---
            Text {
                visible: modelData.type == "section"
                text: modelData.label || ""
                color: root.secondaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.verticalCenter: parent.verticalCenter
                topPadding: root.sh * 0.0020833 //1
                leftPadding: root.sw * 0.009375 //6
                rightPadding: root.sw * 0.009375 //6
                font.pixelSize: root.sh * 0.0291667 //14
            }

            // --- SELECTABLE ROW ---
            Rectangle {
                visible: modelData.type !== "section"
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
                        visible: modelData.type === "list_single"
                        text: "\u25C4"
                        color: settingsList.currentIndex === index ? root.surfaceColor : root.tertiaryColor
                        font.family: root.globalFont
                        anchors.verticalCenter: parent.verticalCenter
                        topPadding: root.sh * 0.0041667 //2
                        bottomPadding: root.sh * 0.00625 //3
                        font.pixelSize: root.sh * 0.0375 //18
                    }
                    Text {
                        visible: modelData.type === "list_single"
                        text: modelData.value || ""
                        color: settingsList.currentIndex === index ? root.surfaceColor : root.primaryColor
                        font.family: root.globalFont
                        font.capitalization: Font.AllUppercase
                        anchors.verticalCenter: parent.verticalCenter
                        topPadding: root.sh * 0.0041667 //2
                        leftPadding: root.sw * 0.009375 //6
                        rightPadding: root.sw * 0.009375 //6
                        bottomPadding: root.sh * 0.00625 //3
                        font.pixelSize:root.sh * 0.05 //24
                    }
                    Text {
                        visible: modelData.type === "submenu" || modelData.type === "list_single"
                        text: "\u25BA"
                        color: settingsList.currentIndex === index ? root.surfaceColor : root.tertiaryColor
                        font.family: root.globalFont
                        anchors.verticalCenter: parent.verticalCenter
                        topPadding: root.sh * 0.0041667 //2
                        bottomPadding: root.sh * 0.00625 //3
                        font.pixelSize: root.sh * 0.0375 //18
                    }
                }
            }
        }
    }

    // --- HELP TEXT --- (shown when a focused row has a description)
    Rectangle {
        id: rowHelpBackground
        property var currentRow: settingsRoot.settingsItems[settingsList.currentIndex]
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
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // --- FOOTER ---
    Text {
        id: footer
        text: root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.change + ":CHANGE " + root.hints.select + ":SELECT"
        color: root.tertiaryColor
        font.family: root.globalFont
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.1041667 //50
        anchors.leftMargin: root.sw * 0.125 //80
        font.pixelSize: root.sh * 0.0333333 //16
    }

    // --- QUIT CONFIRMATION OVERLAY ---
    Rectangle {
        anchors.fill: parent
        color: root.surfaceColor
        visible: quitOverlayVisible
        focus: quitOverlayVisible

        Keys.onUpPressed:   { quitChoiceIndex = Math.max(0, quitChoiceIndex - 1) }
        Keys.onDownPressed: { quitChoiceIndex = Math.min(quitOptions.length - 1, quitChoiceIndex + 1) }
        Keys.onReturnPressed: {
            var act = quitOptions[quitChoiceIndex].action
            if (act === "quit")          Qt.quit()
            else if (act === "reboot")   Qt.exit(12)   // matches EXIT_STATUS check in 240mp-stop
            else if (act === "terminal") Qt.exit(10)   // matches EXIT_STATUS check in 240mp-stop
            else { quitOverlayVisible = false; settingsList.forceActiveFocus() }
        }
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                quitOverlayVisible = false
                settingsList.forceActiveFocus()
                event.accepted = true
            }
        }

        Rectangle {
            color: root.surfaceColor
            anchors.centerIn: parent
            width: root.sw * 0.76875   //492
            height: root.sh * 0.2833333 //136

            Column {
                id: quitDialogColumn
                anchors.fill: parent
                spacing: root.sh * 0.05 //24

                Text {
                    text: "REALLY QUIT?"
                    color: root.secondaryColor
                    font.family: root.globalFont
                    font.pixelSize: root.sh * 0.0333333 //16
                    anchors.horizontalCenter: parent.horizontalCenter
                }

                Column {
                    Repeater {
                        model: quitOptions
                        delegate: Item {
                            width: quitDialogColumn.width
                            height: root.sh * 0.0583333 //28

                            Rectangle {
                                anchors.fill: quitOptionText
                                color: root.accentColor
                                visible: index === quitChoiceIndex
                            }

                            Text {
                                id: quitOptionText
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label
                                color: index === quitChoiceIndex ? root.surfaceColor : root.primaryColor
                                font.family: root.globalFont
                                font.capitalization: Font.AllUppercase
                                topPadding: root.sh * 0.0041667 //2
                                leftPadding: root.sw * 0.009375 //6
                                rightPadding: root.sw * 0.009375 //6
                                bottomPadding: root.sh * 0.00625 //3
                                font.pixelSize: root.sh * 0.05 //24
                            }
                        }
                    }
                }

                Text {
                    text: root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":SELECT"
                    color: root.tertiaryColor
                    font.family: root.globalFont
                    font.pixelSize: root.sh * 0.0333333 //16
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
    }

    // --- CFG-01 RESET CONFIRMATION OVERLAY ---
    // Step 2 of the damaged-file reset. Wording is pinned by the gate plan;
    // Yes performs the reset (quarantine + fresh defaults), No/Back cancels
    // without touching anything. Default selection is No.
    Rectangle {
        anchors.fill: parent
        color: root.surfaceColor
        visible: resetOverlayVisible
        focus: resetOverlayVisible

        Keys.onUpPressed:   { resetChoiceIndex = Math.max(0, resetChoiceIndex - 1) }
        Keys.onDownPressed: { resetChoiceIndex = Math.min(1, resetChoiceIndex + 1) }
        Keys.onReturnPressed: {
            if (resetChoiceIndex === 0) {
                // The ONLY path that writes over the corrupt file.
                if (appCore.resetCorruptedConfig())
                    settingsRoot.buildModel()
            }
            resetOverlayVisible = false
            settingsList.forceActiveFocus()
        }
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                resetOverlayVisible = false
                settingsList.forceActiveFocus()
                event.accepted = true
            }
        }

        Rectangle {
            color: root.surfaceColor
            anchors.centerIn: parent
            width: root.sw * 0.76875   //492
            height: root.sh * 0.40

            Column {
                id: resetDialogColumn
                anchors.fill: parent
                anchors.margins: root.sh * 0.02
                spacing: root.sh * 0.03 //14

                Text {
                    width: resetDialogColumn.width
                    text: "Reset all settings to defaults? Your damaged file will be kept for recovery."
                    color: root.secondaryColor
                    font.family: root.globalFont
                    font.pixelSize: root.sh * 0.0333333 //16
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }

                Column {
                    width: resetDialogColumn.width
                    Repeater {
                        model: [{ label: "Yes" }, { label: "No" }]
                        delegate: Item {
                            width: resetDialogColumn.width
                            height: root.sh * 0.0583333 //28

                            Rectangle {
                                anchors.fill: resetOptionText
                                color: root.accentColor
                                visible: index === resetChoiceIndex
                            }

                            Text {
                                id: resetOptionText
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label
                                color: index === resetChoiceIndex ? root.surfaceColor : root.primaryColor
                                font.family: root.globalFont
                                font.capitalization: Font.AllUppercase
                                topPadding: root.sh * 0.0041667 //2
                                leftPadding: root.sw * 0.009375 //6
                                rightPadding: root.sw * 0.009375 //6
                                bottomPadding: root.sh * 0.00625 //3
                                font.pixelSize: root.sh * 0.05 //24
                            }
                        }
                    }
                }

                Text {
                    text: root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":SELECT"
                    color: root.tertiaryColor
                    font.family: root.globalFont
                    font.pixelSize: root.sh * 0.0333333 //16
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
    }
}
