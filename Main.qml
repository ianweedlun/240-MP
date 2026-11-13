import QtQuick
import QtQuick.Window

Window {
    id: root
    flags: Qt.FramelessWindowHint | Qt.Window
    x:      Qt.platform.os === "osx" ? macScreenX      : Screen.virtualX
    y:      Qt.platform.os === "osx" ? macScreenY      : Screen.virtualY
    width:  Qt.platform.os === "osx" ? macScreenWidth  : Screen.width
    height: Qt.platform.os === "osx" ? macScreenHeight : Screen.height
    // macOS uses manual geometry (the target display chosen by the app-level
    // "display_index" setting, resolved in main.cpp) + a native fullscreen
    // call to keep the mpv-over-window layering intact. Everywhere else,
    // request true fullscreen so a desktop compositor's panel/dock (KDE on the
    // Steam Deck, labwc on the Pi) is covered rather than left stacked on top;
    // the Screen.* bindings track the window's screen, so they follow the
    // display_index move main.cpp performs after load. Headless EGLFS is
    // already fullscreen, so this is a no-op there.
    visibility: Qt.platform.os === "osx" ? Window.AutomaticVisibility
                                         : Window.FullScreen
    visible: true
    color: root.surfaceColor

    // --- Color Schemes ---
    readonly property var themes: ({
        "Video 1": {
            "primary": "#FFFFFF",
            "secondary": "#C2BFE4",
            "tertiary": "#8D8ACD",  // was #8480C9: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#0A0094",
            "accent": "#AECFFF"
        },
        "Late Night": {
            "primary": "#FFFFFF",
            "secondary": "#A1A1A1",
            "tertiary": "#767676",  // was #444444: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#000000",
            "accent": "#FFD900"
        },
        "Synthwave": {
            "primary": "#FFFFFF",
            "secondary": "#D48BFF",
            "tertiary": "#9A5FCF",  // was #7836B5: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#12012B",
            "accent": "#00E5FF"
        },
        "Terminal": {
            "primary": "#4AF626",
            "secondary": "#32A81B",
            "tertiary": "#288815",  // was #1A590E: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#000000",
            "accent": "#4AF626"
        },
        "T-120": {
            "primary": "#000000",
            "secondary": "#6F6F6F",  // was #818181: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "tertiary": "#956616",  // was #df9c27: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#FAF5E8",
            "accent": "#D72712"
            // was #EE442F: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
        },
        "Amber": {
            "primary": "#FFB000",
            "secondary": "#B37B00",
            "tertiary": "#B37B00",
            "surface": "#000000",
            "accent": "#FFEE11"
        },
        "Kinescope": {
            "primary": "#FFFFFF",
            "secondary": "#9E9E9E",
            "tertiary": "#7E7E7E",  // was #424242: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#121212",
            "accent": "#FFFFFF"
        },
        "SMPTE ECR 1-1978": {  // 75% max 0xFF == 0xBF, 40% max 0xFF == 0x66, 7.5% max 0xFF == 0x13; 75/7.5 targets per https://en.wikipedia.org/wiki/SMPTE_color_bars#Analog_NTSC - mixed with 40% in "off channels" to both wash out and improve contrast
            "primary": "#BFBFBF",
            "secondary": "#66BF66",
            "tertiary": "#7777C6",  // was #6666BF: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#131313",
            "accent": "#BF6666"
        },
        // All palettes below: hex sets taken from published specs only (no eyeballed guesses).
        // Role mapping is documented in each entry's comment.
        "Dracula": {  // official draculatheme.com palette (https://draculatheme.com/contribute): text F8F8FF / comment 62727A / current-line 44475A / background 282A36 / purple BD93F9
            "primary": "#F8F8FF",   // text
            "secondary": "#86959D",  // was #62727A: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit) // comment (dimmed text)
            "tertiary": "#8E92A9",  // was #44475A: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)  // current line (panel rows)
            "surface": "#282A36",   // background
            "accent": "#BD93F9"     // purple (selection highlight, signal dot)
        },
        "Tokyo Night": {  // official tokyo-night-vscode-theme README palette (https://github.com/tokyo-night/tokyo-night-vscode-theme): terminal-white C0CAF5 / markdown-text 9AA5CE / comments 565F89 / editor-background 1A1B26 / terminal-red F7768E
            "primary": "#C0CAF5",
            "secondary": "#9AA5CE",
            "tertiary": "#7A83AB",  // was #565F89: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#1A1B26",
            "accent": "#F7768E"
        },
        "Nord": {  // published Gogh-based "Nord" terminal scheme (colors.properties dumps, e.g. github.com/Ghosalach/Gogh): foreground D8DEE9 / bright-black 4C566A / color0 3B4252 / background 2E3440 / ice-blue 88C0D0
            "primary": "#D8DEE9",
            "secondary": "#939EB2",  // was #4C566A: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "tertiary": "#949EB2",  // was #3B4252: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#2E3440",
            "accent": "#88C0D0"
        },
        "Solarized Dark": {  // official Solarized spec via published "Solarized Dark" terminal scheme: base2-cream EEE8D5 (emphasized text) / foreground 839496 / base02 073642 (panels) / base03 002B36 (background) / blue 268BD2
            "primary": "#EEE8D5",
            "secondary": "#839496",
            "tertiary": "#149BBD",  // was #073642: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#002B36",
            "accent": "#3295DA"
            // was #268BD2: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
        },
        "Monokai": {  // published Monokai scheme (base46 catalog, credits https=//monokai.pro/): text F8F8F2 / dim-gray A59F85 / panel one_bg 363731 / background 272822 / pink F92672
            "primary": "#F8F8F2",
            "secondary": "#A59F85",
            "tertiary": "#8E9183",  // was #363731: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
            "surface": "#272822",
            "accent": "#FA4D8C"
            // was #F92672: hue preserved, lifted to AA floor 4.5 vs surface (theme_audit)
        }
    })
    property var allThemes: themes  // may gain a "Custom" entry on startup
    property string currentTheme: "Video 1"
    property string primaryColor:   (allThemes[currentTheme] || allThemes["Video 1"]).primary
    property string secondaryColor: (allThemes[currentTheme] || allThemes["Video 1"]).secondary
    property string tertiaryColor:  (allThemes[currentTheme] || allThemes["Video 1"]).tertiary
    property string surfaceColor:   (allThemes[currentTheme] || allThemes["Video 1"]).surface
    property string accentColor:    (allThemes[currentTheme] || allThemes["Video 1"]).accent

    readonly property real sw: width
    readonly property real sh: height

    Connections {
        target: appCore
        function onAppSettingChanged(key, value) {
            if (key === "color_scheme") {
                root.currentTheme = value
            } else if (key === "screensaver_timeout") {
                var sec = parseInt(value)
                if (sec > 0) {
                    idleTracker.threshold = sec
                    idleTracker.enabled = true
                } else {  // "OFF"
                    idleTracker.enabled = false
                    if (screenSaverActive) screenSaverActive = false
                }
            }
        }
    }

    Component.onCompleted: {
        var cfg = appCore.get_settings()

        var cThemes = appCore.getCustomColorSchemes()
        if (Object.keys(cThemes).length > 0) {
            var t = Object.assign({}, themes, root.allThemes)
            for (var cTheme in cThemes) {
                if (Object.keys(cThemes[cTheme]).length === 5) {
                    t[cTheme] = cThemes[cTheme]
                }
            }
            root.allThemes = t
        }

        var custom = appCore.getCustomColorScheme()
        if (Object.keys(custom).length === 5) {
            var t = Object.assign({}, themes, root.allThemes)
            t["Custom"] = custom
            root.allThemes = t
        }

        var savedTheme = (cfg.app && cfg.app.color_scheme) || "Video 1"
        if (savedTheme === "Custom" && !root.allThemes["Custom"]) {
            appCore.save_setting("", "color_scheme", "Video 1")
            savedTheme = "Video 1"
        }
        root.currentTheme = savedTheme

        // Screensaver: the tracker starts disabled; this is the single place the
        // saved setting is applied (live changes land in onAppSettingChanged above,
        // mirroring color_scheme). parseInt("OFF") is NaN, so OFF stays disabled.
        var ssSec = parseInt(cfg.app && cfg.app.screensaver_timeout)
        if (ssSec > 0) {
            idleTracker.threshold = ssSec
            idleTracker.enabled = true
        }

        // Break declarative bindings on macOS so the C++ NSWindow override
        // in forceWindowFullScreenOnScreen() isn't immediately re-fought by QML.
        if (Qt.platform.os === "osx") {
            root.x = macScreenX
            root.y = macScreenY
            root.width = macScreenWidth
            root.height = macScreenHeight
        }
    }
    
    FontLoader {
        id: font; source: "assets/fonts/VCR_OSD_MONO_1.001.ttf"
    }
    // Unifont fills in glyphs VCR OSD Mono doesn't have (CJK, Hangul, etc.),
    // it's a bitmap-style font so it actually matches the retro CRT look
    // instead of a mismatched serif/sans fallback. This Qt build's font value
    // type has no font.families (checked plugins.qmltypes: family only), so
    // loading it here just registers it with Qt's fontconfig-backed font
    // database; Qt's own missing-glyph fallback then picks it up for every
    // existing font.family: root.globalFont binding with no further changes.
    FontLoader {
        id: unifontLoader; source: "assets/fonts/unifont.otf"
    }
    property string globalFont: font.name;

    // --- INPUT / APP INFO MIRRORS ---
    // Views must bind these via `root.*`, never the appCore/inputManager
    // context properties directly: when the module Loader swaps views, the
    // dying view's context properties resolve to null and any binding on them
    // throws a TypeError during teardown. id-resolved `root.*` stays valid
    // (root lives as long as the app), so these mirrors are teardown-safe.
    // The null guards absorb the same nulling here at app shutdown, when the
    // engine invalidates the root context itself.
    readonly property var hints: inputManager ? inputManager.hints : ({})
    readonly property string appVersion: appCore ? appCore.appVersion : ""

    // --- SCREEN SAVER STATE ---
    property bool screenSaverActive: false

    // Playback counts as user activity even when no key started it (an NFC
    // card tap launches mpv directly). If the saver was showing at launch,
    // nothing key-driven ever reaches its dismiss handler — focus has moved
    // into the module's player view — so clear it on playback transitions.
    function dismissScreenSaver() {
        if (!screenSaverActive) return
        screenSaverActive = false
        moduleLoader.forceActiveFocus()
        logState("screensaver", "dismissed")
    }

    // --- DEBUG STATE LOG ---
    // Machine-readable screen-state trail (/tmp/240mp_state.log on desktops/Pi).
    // A remote test loop tails this instead of OCR'ing screenshots: view loads,
    // nav transitions with stack depth, screensaver changes.
    function logState(what, detail) {
        if (appCore)
            appCore.note_state(what, detail || "")
    }

    // --- APP-LEVEL NAV STACK ---
    property var appNavStack: []
    property var appCurrentParams: ({})
    property bool _startupNavigated: false

    // --- MPV PLAYBACK TRACKING ---
    // Block the screen saver while mpv is playing so it never flashes during or
    // immediately after playback. The core guard is in IdleTracker (mpvActive
    // property), which also resets the idle timer on transitions.
    Connections {
        target: mpvController
        function onPositionChanged(ms) {
            if (ms > 0 && !idleTracker.mpvActive) {
                idleTracker.mpvActive = true
                idleTracker.resetActivity()
                root.dismissScreenSaver()
            }
        }
        function onPlaybackEnded(finalPositionMs, finalDurationMs, reason) {
            idleTracker.mpvActive = false
            idleTracker.resetActivity()
            root.dismissScreenSaver()
        }
    }

    // A running user script suppresses the screen saver too — a takeover script
    // owns the display, and even a console one is legitimately silent for as long
    // as it takes. Its own flag rather than reusing mpvActive, so ending one
    // session can't unblock the saver while the other is still going.
    Connections {
        target: scriptsBackend
        function onScriptRunningChanged() {
            idleTracker.scriptActive = scriptsBackend.scriptBusy
            idleTracker.resetActivity()
            if (!scriptsBackend.scriptBusy)
                root.dismissScreenSaver()
        }
    }

    // --- MODULE LOADER ---
    Loader {
        id: moduleLoader;
        anchors.fill: parent;
        focus: true;
        source: "views/ModuleList.qml";

        Keys.onPressed: (event) => {
            if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_Q) {
                Qt.quit()
            }
        }

        onLoaded: {
            item.forceActiveFocus()
            // Tag AppCore with the live view so default-named screenshots
            // carry the open view in their filename (debug aid).
            appCore.note_view(moduleLoader.source)
            logState("load", moduleLoader.source + " stack=" + root.appNavStack.length)
            if (!root._startupNavigated) {
                root._startupNavigated = true
                var entryPoint = appCore.startupModuleEntryPoint()
                if (entryPoint) {
                    root.appNavStack.push({
                        source: moduleLoader.source,
                        params: root.appCurrentParams,
                        listState: {}
                    })
                    moduleLoader.setSource(entryPoint, { "navParams": { fromAppStartup: true } })
                }
            }
        }

        Connections {
            target: moduleLoader.item
            ignoreUnknownSignals: true

            function onNavigateTo(path, params, listState) {
                // A bare "View.qml" token is module-relative (the same convention
                // Root.qml's bridge uses with Qt.resolvedUrl) — but only when the
                // hop goes through that bridge. Views opened app-level (Settings'
                // "Manage Channels" row loads Manage.qml directly) hand their
                // navigateTo here, where a bare token resolves against the share
                // root — file-not-found, grey screen (live Pi: editing a channel
                // greyed out at "file:///opt/240mp/share/240mp/ChannelSources.qml").
                // Resolve bare tokens against the live view's URL; tokens that
                // already carry a directory ("views/", "modules/") stay as given.
                // (Plain string math, not Qt.resolvedUrl: the two-arg call binds
                // QtQuick's element/context overload and marshals the QUrl origin
                // as QObject* — TypeError on every bare-token hop.)
                var resolved = path
                if (/^[A-Za-z0-9_]+\.qml$/.test(path)) {
                    var src = appCore ? appCore.current_view_source() : ""
                    if (src.length > 0)
                        resolved = src.slice(0, src.lastIndexOf('/')) + '/' + path
                }
                root.appNavStack.push({ source: moduleLoader.source, params: root.appCurrentParams, listState: listState || {} })
                root.appCurrentParams = params || {}
                logState("navigate", "to=" + resolved + " stack=" + root.appNavStack.length + " listState=" + JSON.stringify(listState || {}))
                moduleLoader.setSource(resolved, { "navParams": params || {} })
            }

            function onGoBack() {
                if (root.appNavStack.length === 0) {
                    // Module is at its top level — exit module entirely, return to ModuleList.
                    // Use the full app-level path: a bare "ModuleList.qml" resolves
                    // relative to the last-loaded source's directory (the module's own
                    // views/, which has no such file) — never guess here.
                    logState("back", "module-exit -> views/ModuleList.qml")
                    moduleLoader.setSource("views/ModuleList.qml", { "navParams": { fromAppStartup: false } })
                    root.appCurrentParams = {}
                    return
                }
                var returnTo = (root.appCurrentParams && root.appCurrentParams.returnTo) ? String(root.appCurrentParams.returnTo) : ""
                var prev = root.appNavStack.pop()
                if (returnTo !== "") {
                    while (root.appNavStack.length > 0 && String(prev.source).indexOf("/" + returnTo) < 0) {
                        prev = root.appNavStack.pop()
                    }
                }
                root.appCurrentParams = prev.params
                logState("back", "restore=" + prev.source + " stack=" + root.appNavStack.length)
                moduleLoader.setSource(prev.source, { "navParams": prev.params, "navListState": prev.listState || {} })
            }
        }
    }

    // --- SCREEN SAVER (Idle Tracker integration) ---
    Connections {
        target: idleTracker
        function onActiveChanged() {
            // Only show on active → true; never hide here — the overlay's
            // key handler owns dismissal, preventing the C++ event filter's
            // synchronous reset from stealing the key from QML.
            if (idleTracker.active && idleTracker.enabled) {
                if (!screenSaverActive) {
                    logState("screensaver", "active")
                    var usableW = screenSaverOverlay.width - bounceLogo.width
                    var usableH = screenSaverOverlay.height - bounceLogo.height
                    bounceLogo.x = Math.random() * (usableW > 0 ? usableW : 1)
                    bounceLogo.y = Math.random() * (usableH > 0 ? usableH : 1)
                    bounceLogo.vx = (Math.random() > 0.5 ? 1 : -1) * (1 + Math.random() * 1.5)
                    bounceLogo.vy = (Math.random() > 0.5 ? 1 : -1) * (1 + Math.random() * 1.5)
                    screenSaverActive = true
                    screenSaverOverlay.forceActiveFocus()
                }
            }
        }
    }

    Item {
        id: screenSaverOverlay
        anchors.fill: parent
        visible: screenSaverActive
        z: 9999
        focus: visible

        // Solid black background — no transparency so it serves as a true
        // CRT burn-in prevention black frame between the logo bounces.
        Rectangle {
            anchors.fill: parent
            color: "#000000"
        }

        // Bouncing logo — classic DVD player screen saver
        Image {
            id: bounceLogo
            source: "assets/images/logo.svg"
            sourceSize.width: root.sw * 0.05
            sourceSize.height: root.sw * 0.05
            fillMode: Image.PreserveAspectFit
            antialiasing: true

            property real vx: 0
            property real vy: 0

            // Physics tick at ~60 fps while the overlay is visible
            Timer {
                interval: 16
                repeat: true
                running: screenSaverActive
                onTriggered: {
                    bounceLogo.x += bounceLogo.vx
                    bounceLogo.y += bounceLogo.vy

                    if (bounceLogo.x + bounceLogo.width > screenSaverOverlay.width) {
                        bounceLogo.x = screenSaverOverlay.width - bounceLogo.width
                        bounceLogo.vx = -Math.abs(bounceLogo.vx)
                    } else if (bounceLogo.x < 0) {
                        bounceLogo.x = 0
                        bounceLogo.vx = Math.abs(bounceLogo.vx)
                    }

                    if (bounceLogo.y + bounceLogo.height > screenSaverOverlay.height) {
                        bounceLogo.y = screenSaverOverlay.height - bounceLogo.height
                        bounceLogo.vy = -Math.abs(bounceLogo.vy)
                    } else if (bounceLogo.y < 0) {
                        bounceLogo.y = 0
                        bounceLogo.vy = Math.abs(bounceLogo.vy)
                    }
                }
            }
        }

        // Capture any keypress to dismiss — consumes the event so the
        // underlying view never sees it, preventing accidental navigation.
        // Ctrl+Q still quits (moduleLoader's handler is a sibling, so it
        // can't see keys focused here — handle the chord directly).
        Keys.onPressed: (event) => {
            event.accepted = true
            if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_Q) {
                Qt.quit()
                return
            }
            screenSaverActive = false
            moduleLoader.forceActiveFocus()
            logState("screensaver", "dismissed-by-key")
        }
    }
}
