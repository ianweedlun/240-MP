import QtQuick

FocusScope {
    id: moduleRoot

    signal goBack()

    property var navParams: ({})

    property string moduleId: "com.240mp.youtube"

    property var _moduleInfo: appCore ? appCore.get_module_info(moduleId) : ({})
    property string moduleName: _moduleInfo.name || ""
    property string moduleIcon: _moduleInfo.icon || ""

    property var navStack: []
    property var currentParams: ({})

    function navigateTo(viewPath, params, fromState) {
        var resolved = Qt.resolvedUrl(viewPath)
        navStack.push({ source: internalLoader.source, params: currentParams, listState: fromState || {} })
        currentParams = params || {}
        internalLoader.setSource(resolved, { "navParams": params || {} })
    }

    // Repoint the list position BACK restores after playlist autoplay advances
    // in place, so exiting the player lands on the last-played item.
    function updateBackListState(state) {
        if (navStack.length === 0) return
        navStack[navStack.length - 1].listState = state
    }

    function navigateBack() {
        if (navStack.length === 0) {
            moduleRoot.goBack()
            return
        }
        var prev = navStack.pop()
        if (!prev.source || prev.source.toString() === "") {
            moduleRoot.goBack()
            return
        }
        var restored = Object.assign({}, prev.params)
        restored.navListState = prev.listState || {}
        currentParams = restored
        internalLoader.setSource(prev.source, { "navParams": restored })
    }

    Loader {
        id: internalLoader
        anchors.fill: parent
        focus: true
        onLoaded: { if (item) item.forceActiveFocus() }

        Connections {
            target: internalLoader.item
            ignoreUnknownSignals: true
            function onNavigateTo(path, params, listState) { moduleRoot.navigateTo(path, params, listState) }
            function onGoBack() { moduleRoot.navigateBack() }
            function onUpdateBackListState(state) { moduleRoot.updateBackListState(state) }
        }
    }

    // Plan D: device-flow pairing card — visible only while a pairing is
    // pending (or right after it lands/declines, one glance to confirm).
    property string pairCode: ""
    property string pairUrl: ""
    property string pairNote: ""
    property bool pairActive: false

    Connections {
        target: youtubeBackend
        function onDevicePairRequested(userCode, url) {
            moduleRoot.pairCode = userCode
            moduleRoot.pairUrl = url
            moduleRoot.pairActive = true
            moduleRoot.pairNote = ""
        }
        function onDeviceSignInFinished(ok, reason) {
            moduleRoot.pairActive = false
            moduleRoot.pairNote = ok ? "SIGNED IN" : reason
            // Let the confirmation be readable for a moment, then clear so no
            // stale card ever persists on the module screen.
            Qt.setTimeout("pair-note-clear", ok ? 6000 : 10000,
                          function() { moduleRoot.pairNote = "" })
        }
    }

    Rectangle {
        id: pairCard
        visible: moduleRoot.pairActive || moduleRoot.pairNote !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.05
        width: root.sw * 0.55; height: root.sh * 0.22
        color: root.surfaceColor
        radius: root.sh * 0.01
        Column {
            spacing: root.sh * 0.018
            Text {
                text: "PAIR YOUTUBE"
                color: root.accentColor
                font.family: root.globalFont
                font.pixelSize: root.sh * 0.035
            }
            Text {
                text: moduleRoot.pairUrl
                color: root.secondaryColor
                font.family: root.globalFont
                font.pixelSize: root.sh * 0.03
            }
            Text {
                text: moduleRoot.pairCode !== "" ? "TYPE CODE: " + moduleRoot.pairCode
                                                 : moduleRoot.pairNote
                color: root.primaryColor
                font.family: root.globalFont
                font.pixelSize: root.sh * 0.05
            }
        }
    }

    Component.onCompleted: {
        // Issue #28 plan A warm-up: before any view opens, kick the backend
        // pass that pre-resolves the newest unfinished resume target with
        // yt-dlp in the background. If the user walks over to resume that
        // video, the signed stream URL is already cached and mpv skips
        // ytdl_hook's multi-second re-resolve at play time.
        var resolution = appCore.get_setting(moduleId, "playback_resolution") || "480p"
        youtubeBackend.warmResumePlayback(resolution)
        // Plan D: opening the module is the pair entry point (TV-app convention).
        // A flow still running from an earlier open is re-read, not restarted;
        // the Connections block above fills the card when the code lands.
        if (youtubeBackend.pairActive() === true) {
            moduleRoot.pairCode = youtubeBackend.pairUserCode()
            moduleRoot.pairUrl = youtubeBackend.pairVerificationUrl()
            moduleRoot.pairActive = true
        } else if (youtubeBackend.signedIn() !== true) {
            youtubeBackend.startDeviceSignIn()
        }
        navigateTo("Items.qml", {})
    }
}
