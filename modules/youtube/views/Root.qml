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

    Component.onCompleted: {
        // Issue #28 plan A warm-up: before any view opens, kick the backend
        // pass that pre-resolves the newest unfinished resume target with
        // yt-dlp in the background. If the user walks over to resume that
        // video, the signed stream URL is already cached and mpv skips
        // ytdl_hook's multi-second re-resolve at play time.
        var resolution = appCore.get_setting(moduleId, "playback_resolution") || "480p"
        youtubeBackend.warmResumePlayback(resolution)
        // Issue #28 plan D, relocated per Ian: the module open no longer
        // starts or shows a pairing — opening YouTube while unpaired shows
        // plain file-based lists with no prompt. Device pairing lives in
        // the Settings sign-in subview (views/SignIn.qml) and that subview
        // owns the whole flow, including sign-out.
        navigateTo("Items.qml", {})
    }
}
