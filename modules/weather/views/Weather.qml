import QtQuick

// The display host: owns the screen carousel, key handling and the status bar.
//
// Rotation lives here rather than in the module's nav stack because the screens
// are peers in a ring, not a hierarchy — you never "go back" from Almanac to
// Current Conditions, you just keep going round.
FocusScope {
    id: weatherRoot

    property var navParams: ({})
    signal replaceWith(string path, var params)
    signal goBack()

    // Every screen this module can show, in rotation order. `screens` below is
    // this list filtered by the Displays setting — the ring, the timer and the
    // key handling all work off the filtered list, so adding a screen is one
    // entry here plus one option in the backend's getDisplays().
    readonly property var allScreens: [
        { id: "current",  source: "screens/CurrentConditions.qml" },
        { id: "extended", source: "screens/ExtendedForecast.qml"  },
        { id: "others",   source: "screens/OtherLocations.qml"    },
        { id: "almanac",  source: "screens/Almanac.qml"           }
    ]
    property var screens: []
    property int  screenIndex: 0
    property bool paused: false
    property int  screenTimeMs: 10000

    property bool twelveHour: false

    focus: true

    Component.onCompleted: {
        var cfg = appCore.get_settings()
        var mod = (cfg.modules && cfg.modules[moduleRoot.moduleId]) || {}
        twelveHour = (mod.hours_format || "24-hour").indexOf("12") === 0
        // parseInt stops at the space, so "30 SEC" -> 30.
        screenTimeMs = (parseInt(mod.screen_time || "10") || 10) * 1000

        // Suppress the screen saver for as long as this view is up. The module
        // IS a screen saver — a rotating display you leave running — so letting
        // the saver paint over it would be actively wrong. The flag is
        // mpv-named because playback was its first user; it is the app's
        // general "something is already owning the screen" signal.
        idleTracker.mpvActive = true

        // Absent means enabled, matching MultiSelectSettings.qml, which defaults
        // an option to true when config has no entry for it.
        chosenDisplays = mod.displays || {}
        rebuildRing()

        weatherBackend.start()
        // No-op when the Music setting is off, so the view doesn't have to read
        // the setting a second time.
        weatherBackend.startMusic()

        // Allow volume keys to reach QML for weather music volume control.
        inputManager.setInterceptMediaKeys(false)
    }

    property var chosenDisplays: ({})

    // A screen is in the ring if it is enabled AND has something to show. Only
    // "others" can be empty — it depends on extra lines in weather_location.txt,
    // which are not known until the extras have resolved, so this reruns on
    // every data update rather than once at startup.
    function screenHasContent(id) {
        if (id === "others") return weatherBackend.hasOtherLocations
        return true
    }

    function rebuildRing() {
        var ring = []
        for (var i = 0; i < allScreens.length; i++) {
            var s = allScreens[i]
            var enabled = (chosenDisplays[s.id] === undefined) || chosenDisplays[s.id]
            if (enabled && screenHasContent(s.id)) ring.push(s.source)
        }
        // Never leave an empty ring — a module that shows nothing is worse than
        // one that ignores a setting.
        screens = ring.length > 0 ? ring : [ allScreens[0].source ]
        if (screenIndex >= screens.length) screenIndex = 0
    }

    Component.onDestruction: {
        if (idleTracker) {
            idleTracker.mpvActive = false
            idleTracker.resetActivity()
        }
        // Restore media key interception for mpv playback.
        inputManager.setInterceptMediaKeys(true)
    }

    function advance(step) {
        var n = weatherRoot.screens.length
        screenIndex = (screenIndex + step + n) % n
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace
                || event.key === Qt.Key_Back) {
            weatherRoot.goBack()
            event.accepted = true
        } else if (event.key === Qt.Key_Left) {
            // Stepping manually pauses the loop, matching the original's
            // behaviour: you stepped somewhere because you wanted to look at it.
            weatherRoot.paused = true
            advance(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            weatherRoot.paused = true
            advance(1)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            weatherRoot.paused = !weatherRoot.paused
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            // Independent of weatherRoot.paused: Enter pauses the carousel, and
            // music should keep playing under a screen you're lingering on.
            weatherBackend.toggleMusic()
            event.accepted = true
        } else if (event.key === Qt.Key_VolumeUp || event.key === 0x1008FF13) {
            var curUp = weatherBackend.musicVolume
            if (curUp === undefined) curUp = 50
            weatherBackend.setMusicVolume(Math.min(100, curUp + 10))
            event.accepted = true
        } else if (event.key === Qt.Key_VolumeDown || event.key === 0x1008FF11) {
            var curDown = weatherBackend.musicVolume
            if (curDown === undefined) curDown = 50
            weatherBackend.setMusicVolume(Math.max(0, curDown - 10))
            event.accepted = true
        }
    }

    Timer {
        interval: weatherRoot.screenTimeMs
        running: !weatherRoot.paused && weatherRoot.screens.length > 1
                 && weatherBackend.hasData
        repeat: true
        onTriggered: weatherRoot.advance(1)
    }

    Connections {
        target: weatherBackend
        function onLocationError(reason) {
            // replaceWith, not navigateTo: pushing this view onto the back stack
            // would make BACK from the setup screen return here and immediately
            // retry the same failing lookup, trapping the user in a loop.
            //
            // Music stops here too: playing it over "your location file is
            // missing" would be absurd.
            weatherBackend.stopMusic()
            weatherRoot.replaceWith("Setup.qml", { reason: reason })
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.surfaceColor
    }

    Text {
        anchors.centerIn: parent
        visible: !weatherBackend.hasData
        text: "LOADING..."
        color: root.primaryColor
        font.family: root.globalFont
        font.pixelSize: root.sh * 0.05 //24
    }

    // Screen area, above the status bar.
    Loader {
        id: screenLoader
        visible: weatherBackend.hasData
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: statusBar.top
        anchors.leftMargin: root.sw * 0.125 //80
        anchors.rightMargin:  root.sw * 0.125 //80
        anchors.topMargin: root.sh * 0.125 //60
        anchors.bottomMargin: root.sh * 0.02

        source: weatherRoot.screens.length > 0
                ? weatherRoot.screens[weatherRoot.screenIndex] : ""
        onLoaded: syncScreen()

        // Screens declare only the properties they use, so assign defensively
        // rather than assuming a shape.
        function syncScreen() {
            if (!item) return
            if (item.hasOwnProperty("wx"))           item.wx = weatherBackend.current
            if (item.hasOwnProperty("locationName")) item.locationName = weatherBackend.locationName
            if (item.hasOwnProperty("days"))         item.days = weatherBackend.forecast
            if (item.hasOwnProperty("almanac"))      item.almanac = weatherBackend.almanac
            if (item.hasOwnProperty("rows"))         item.rows = weatherBackend.otherLocations
            if (item.hasOwnProperty("tempUnit"))     item.tempUnit = weatherBackend.tempUnitLabel
        }
    }

    // Volume bar overlay — drawn in QML because mpv's --vo=null doesn't
    // reliably render OSD overlays. Geometry is mirrored 1:1 from
    // scripts/mpv-media-keys.lua (the themed volume bar the player views show),
    // so the Weather OSD renders exactly like the Player one: same font (VCR
    // OSD Mono = globalFont), same rows (big label above the bar row), same
    // tick count (vol_max/VOLUME_STEP = 20), same glyphs (full-height filled
    // rects, short vertically-centred dashes for unlit), same 35% gaps,
    // same colours (opaque white / white at ~31% = ASS \1a&HB0&).
    // Shows for BAR_TIMEOUT (1.5 s) when volume changes.
    Rectangle {
        id: volumeBarOverlay
        anchors.fill: parent
        color: "transparent"
        visible: false

        // Reference geometry, per-OSD-frame (issue #27)
        property real fs: Math.floor(parent.height * 0.0333333)          // lua fs
        property real lm: Math.floor(parent.width * 0.12)                // lua lm
        property real barW: Math.floor(parent.width * 0.88) - lm         // lua bar_w
        property real barH: fs * 2                                      // lua bar_h
        property real barY: Math.floor(parent.height * 0.8333333)       // lua bar_y
        property real labelY: Math.floor(parent.height * 0.7979166)     // lua row1_y
        // lua: filled = clamp(round(volume / VOLUME_STEP), 0, ticks)
        property int filled: Math.max(0, Math.min(20,
                                 Math.floor(weatherBackend.musicVolume / 5 + 0.5)))

        // Volume label — bottom-left anchored off the bar row, grows upward
        // (lua: \an1 at (lm, row1_y), fs*3, no bold/border/shadow)
        Text {
            text: (weatherBackend.musicVolume === 0) ? "MUTE" : "VOLUME"
            color: "#ffffff"                                            // lua C_WHITE
            font.family: root.globalFont
            font.pixelSize: volumeBarOverlay.fs * 3
            x: volumeBarOverlay.lm
            y: volumeBarOverlay.labelY - height
        }

        Rectangle {
            id: tickRow
            x: volumeBarOverlay.lm
            y: volumeBarOverlay.barY
            width: volumeBarOverlay.barW
            height: volumeBarOverlay.barH
            color: "transparent"

            Repeater {
                model: 20 // one tick per VOLUME_STEP (5%) across 0-100

                Rectangle {
                    // Slot maths straight from the lua: gap = 35% of slot,
                    // tick = the rest; filled = full-height rect, empty =
                    // short dash centred in the slot. Sharp corners (lua
                    // rect_cw draws none).
                    property real slotW: tickRow.width / 20
                    property real dashH: Math.max(2, Math.floor(tickRow.height * 0.15))
                    width: Math.max(1, Math.floor(slotW - Math.max(1, Math.floor(slotW * 0.35))))
                    x: index * slotW
                    height: index + 1 <= volumeBarOverlay.filled ? tickRow.height : dashH
                    y: index + 1 <= volumeBarOverlay.filled
                            ? 0
                            : Math.floor((tickRow.height - dashH) / 2)
                    color: index + 1 <= volumeBarOverlay.filled
                            ? "#ffffff"
                            : Qt.rgba(1, 1, 1, 0.31) // lua \1a&HB0& ≈ 31% opaque
                }
            }
        }

        // Fade in/out animation
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }

    Connections {
        target: weatherBackend
        // Volume-only listener: musicStateChanged would show the bar on module
        // open (music autostarts). The handler only runs when setMusicVolume
        // actually changed volume, so the bar appears exactly on volume keys.
        function onMusicVolumeChanged(volume) {
            if (weatherBackend.musicPlaying) {
                volumeBarOverlay.visible = true
                volumeBarOverlay.opacity = 1
                volumeBarTimer.start()
            }
        }
    }

    Timer {
        id: volumeBarTimer
        interval: 1500 // lua BAR_TIMEOUT (was 2000) — same fade window as the Player OSD
        onTriggered: {
            volumeBarOverlay.opacity = 0
            // Keep visible but transparent so it doesn't flash
            if (!volumeBarTimer.running) volumeBarOverlay.visible = false
        }
    }

    Connections {
        target: weatherBackend
        function onDataChanged() {
            weatherRoot.rebuildRing()
            screenLoader.syncScreen()
        }
    }

    StatusBar {
        id: statusBar
        visible: weatherBackend.hasData
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.sw * 0.125 //80
        anchors.rightMargin:  root.sw * 0.125 //80
        anchors.bottomMargin: root.sh * 0.1041667 //50

        utcOffsetSeconds: weatherBackend.utcOffsetSeconds
        twelveHour: weatherRoot.twelveHour
        // Ticker lines empty by design: current-conditions data is rendered
        // once, fully, by the active screen (CurrentConditions.qml). The module
        // used to feed the status-bar ticker this whole row set, which
        // duplicated the screen's rows (a rotating DEWPOINT under a static
        // DEWPOINT) and drew the rotating line through the footer area.
        // Status bar keeps its one fixed row: location-local date + clock.
        lines: []
    }
}
