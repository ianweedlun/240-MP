import QtQuick

FocusScope {
    id: playerRoot

    property var navParams: ({})

    signal goBack()
    // Emitted when autoplay advances in place, so Root can move the list
    // cursor BACK returns to onto the now-playing playlist item.
    signal updateBackListState(var state)

    property var    item:     navParams.item || ({})
    property string videoUrl: item.url || ""
    property string videoId:  item.videoId || ""
    property var    playlistItems: navParams.playlistItems || []
    property int    playlistIndex: navParams.playlistIndex === undefined ? -1 : navParams.playlistIndex

    property string directPlayUrl:  ""   // fresh cached googlevideo video URL (plan A), else empty
    property string directAudioUrl: ""   // its bestaudio pair, when the format split one
    property bool   overlayVisible:   false
    property bool   playbackStarted:  false
    property int    savedPositionMs:  0
    property int    choiceIndex:      0
    property string errorMessage:     ""
    property var    ytdlArgs:         []
    property int    lastStartMs:      0   // what the last attempt started from, for retry

    // Track last non-null values during playback for robust save on exit
    property int    lastKnownPositionMs: 0
    property int    lastKnownDurationMs: 0

    focus: true

    function doPlay(startMs) {
        overlayVisible = false
        lastStartMs = startMs
        // directPlayUrl/directAudioUrl and ytdlArgs are set together by
        // preparePlayback; when a fresh signed pair is on hand mpv takes it
        // with --ytdl=no and skips ytdl_hook's re-resolve entirely. The audio
        // half rides --audio-file= (ytdl_hook's own mechanism for split
        // formats) so first play is exactly as fast as resume.
        // SponsorBlock: both halves get the SAME edl:// cut ranges so the audio
        // stays in sync across every jump; "mark" categories add titled
        // boundaries (OSC chapters) without cutting. Empty edl strings = plain
        // URLs (nothing active for this video).
        var playUrl = directPlayUrl
        var args = ytdlArgs.slice()
        var oscRanges = ""
        if (playUrl !== "") {
            const res = appCore.get_setting(moduleRoot.moduleId, "playback_resolution") || "480p"
            const cats = ["sponsor", "selfpromo", "intro", "outro", "intermission", "preview", "filler", "interaction"]
            var acts = ({})
            for (var ci = 0; ci < cats.length; ++ci) {
                // Red-parity defaults while the key is untouched: sponsor and
                // self-promo skip silently, everything else stays off.
                var dflt = (cats[ci] === "sponsor" || cats[ci] === "selfpromo") ? "skip" : "off"
                acts[cats[ci]] = (appCore.get_setting(moduleRoot.moduleId, "sponsor_" + cats[ci]) || dflt).toLowerCase()
            }
            const edl = youtubeBackend.buildPlaybackEdl(playerRoot.videoId, res, acts)
            if (edl.videoEdl !== undefined && edl.videoEdl !== "")
                playUrl = edl.videoEdl
            if (directAudioUrl !== "")
                args.push("--audio-file=" + (edl.audioEdl !== undefined && edl.audioEdl !== "" ? edl.audioEdl : directAudioUrl))
            if (edl.ranges !== undefined && edl.ranges !== "")
                oscRanges = edl.ranges
            // OSC shows this media title instead of the (very long) edl:// URL.
            var t = (item && item.title) ? item.title : "YouTube"
            if (t.length > 36)
                t = t.substring(0, 33) + "..."
            args.push("--title=" + t)
        } else {
            playUrl = videoUrl
        }
        // OSC segment marks get their data out-of-band: raw source ranges the
        // script converts to the cut virtual timeline itself (empty clears).
        mpvController.setOscRanges(oscRanges)
        mpvController.loadAndPlay(playUrl, startMs / 1000.0, 0, -2, [], [], false, -1, 0.0, "", false, "", false, [], 0.0, false, args)
    }

    // Cached-URL lookup + next-window warm-up for the CURRENT item (issue #28
    // plan A): take a fresh cached stream URL pair if one exists, ask the
    // backend to resolve this video regardless (no-op when a fresh window is
    // already stored), so quitting-and-resuming hits the direct path next time.
    // A video-only cache hit with no audio half is rejected — a silent picture
    // is a worse failure than a slow start. Returns true when a fresh pair is
    // on hand RIGHT NOW (launch can proceed immediately); false means the
    // backend's background resolve is the only route and playAt() will wait
    // for playbackUrlReady rather than pay ytdl_hook's re-resolve.
    function preparePlayback() {
        var resolution = appCore.get_setting(moduleRoot.moduleId, "playback_resolution") || "480p"
        // Segment windows: cached hit re-emits sponsorReady synchronously; a
        // cold window is one ~300 ms GET, usually beating the yt-dlp resolve.
        waitingSponsor = true
        youtubeBackend.requestSponsorSegments(videoId)
        const direct = youtubeBackend.cachedPlaybackUrl(videoId, resolution)
        const directAudio = youtubeBackend.cachedPlaybackAudioUrl(videoId, resolution)
        // single-stream cache entries (live broadcasts, combined formats) are
        // fully muxed: no audio half exists and none is needed.
        const single = youtubeBackend.cachedPlaybackSingle(videoId, resolution)
        if (direct !== "" && (directAudio !== "" || single)) {
            directPlayUrl = direct
            directAudioUrl = directAudio
            ytdlArgs = ["--ytdl=no"]
            appCore.note_state("yt-play", "vid=" + videoId + " path=warm res=" + resolution)
            youtubeBackend.resolvePlayback(videoId, resolution)
            return true
        }
        directPlayUrl = ""
        directAudioUrl = ""
        ytdlArgs = []
        appCore.note_state("yt-play", "vid=" + videoId + " path=cold res=" + resolution)
        youtubeBackend.resolvePlayback(videoId, resolution)
        return false
    }

    Timer {
        id: sponsorWaitTimer
        // Bounded sponsor hold: a dead SponsorBlock domain means the backend
        // silently gives up (transient err) and no sponsorReady ever fires.
        // Playback must never depend on that request — expire the hold and
        // launch without segments (late segments still apply via property filter).
        interval: 6000
        repeat: false
        onTriggered: {
            if (!playerRoot.waitingSponsor)
                return
            playerRoot.waitingSponsor = false
            appCore.note_state("yt-sponsor", "vid=" + playerRoot.videoId + " wait-expired")
            if (playerRoot.launchAfterSponsor) {
                playerRoot.launchAfterSponsor = false
                play(pendingPlayStartMs)
            }
        }
    }

    // Starting mpv runs synchronously and, on the Pi, immediately switches VT
    // (suspending Qt's render thread) before the LOADING frame can paint. Defer
    // the launch one tick so the loading indicator is rendered first.
    Timer {
        id: startTimer
        interval: 50
        repeat: false
        property int pendingStartMs: 0
        onTriggered: doPlay(pendingStartMs)
    }

    function play(startMs) {
        startTimer.pendingStartMs = startMs
        startTimer.restart()
    }

    // Entry point for every playback start. When the signed URL pair is already
    // cached the launch is immediate; when it isn't, we wait (bounded) for the
    // background resolve fired by preparePlayback instead of handing mpv the
    // watch URL and paying ytdl_hook's multi-second re-resolve. The wait cap
    // keeps a dead network or a yt-dlp hang from trapping the view on LOADING —
    // past it we take the ytdl path as the always-works fallback.
    property bool   waitingForResolve: false
    property int    pendingPlayStartMs: 0
    property bool   waitingSponsor: false
    property bool   launchAfterSponsor: false

    Timer {
        id: resolveWaitTimer
        interval: 15000   // yt-dlp warm resolve is ~4-6s on the Pi; 15s is generous
        repeat: false
        onTriggered: {
            if (!playerRoot.waitingForResolve)
                return
            playerRoot.waitingForResolve = false
            resolveWaitTimer.stop()
            appCore.note_state("yt-fallback", "vid=" + playerRoot.videoId + " waited=15s reason=timeout")
            playerRoot.waitingSponsor = false
            var resolution = appCore.get_setting(moduleRoot.moduleId, "playback_resolution") || "480p"
            playerRoot.ytdlArgs = ["--ytdl=yes", "--ytdl-format=" + youtubeBackend.ytdlFormatForResolution(resolution)]
            playerRoot.play(playerRoot.pendingPlayStartMs)
        }
    }

    Connections {
        target: youtubeBackend
        function onPlaybackUrlReady(readyVideoId) {
            if (!playerRoot.waitingForResolve || readyVideoId !== playerRoot.videoId)
                return
            playerRoot.waitingForResolve = false
            resolveWaitTimer.stop()
            // The cache now holds the fresh pair — re-arm the direct path.
            var resolution = appCore.get_setting(moduleRoot.moduleId, "playback_resolution") || "480p"
            const direct = youtubeBackend.cachedPlaybackUrl(playerRoot.videoId, resolution)
            const directAudio = youtubeBackend.cachedPlaybackAudioUrl(playerRoot.videoId, resolution)
            const single = youtubeBackend.cachedPlaybackSingle(playerRoot.videoId, resolution)
            if (direct !== "" && (directAudio !== "" || single)) {
                appCore.note_state("yt-resolve", "vid=" + playerRoot.videoId + " ok pair-ready")
                playerRoot.directPlayUrl = direct
                playerRoot.directAudioUrl = directAudio
                playerRoot.ytdlArgs = ["--ytdl=no"]
                if (playerRoot.waitingSponsor) {
                    // hold until sponsorReady arrives so the launch can carry
                    // the EDL (transient fetch failure is covered by the wait
                    // timer, which clears waitingSponsor before falling back).
                    playerRoot.launchAfterSponsor = true
                    sponsorWaitTimer.restart()
                    return
                }
                playerRoot.play(playerRoot.pendingPlayStartMs)
            }
            // If the pair somehow isn't there, let the wait timer's ytdl
            // fallback handle it rather than racing a second resolve.
        }
    }

    Connections {
        target: youtubeBackend
        function onSponsorReady(readySponsorId) {
            if (readySponsorId !== playerRoot.videoId)
                return
            playerRoot.waitingSponsor = false
            appCore.note_state("yt-sponsor", "vid=" + playerRoot.videoId + " ready")
            if (playerRoot.launchAfterSponsor) {
                playerRoot.launchAfterSponsor = false
                playerRoot.play(playerRoot.pendingPlayStartMs)
            }
        }
    }

    function playAt(startMs) {
        if (directPlayUrl !== "") {
            play(startMs)
            return
        }
        waitingForResolve = true
        pendingPlayStartMs = startMs
        resolveWaitTimer.restart()
    }

    function playNextPlaylistItem() {
        if (playlistIndex < 0 || playlistIndex + 1 >= playlistItems.length)
            return false
        item = playlistItems[++playlistIndex]
        playbackStarted = false
        lastKnownPositionMs = 0
        lastKnownDurationMs = 0
        savedPositionMs = 0
        updateBackListState({ currentIndex: playlistIndex })
        preparePlayback()
        playAt(0)
        return true
    }

    Keys.onPressed: function(event) {
        if (errorMessage !== "") {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                goBack()
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                errorMessage = ""
                // Retry: the cache may have been filled by the resolve that
                // ran while the error was up — re-prep so we take the direct
                // path when it's available.
                preparePlayback()
                playAt(lastStartMs)
                event.accepted = true
            }
        } else if (overlayVisible) {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
                goBack()
                event.accepted = true
            } else if (event.key === Qt.Key_Up) {
                choiceIndex = 0
                event.accepted = true
            } else if (event.key === Qt.Key_Down) {
                choiceIndex = 1
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                overlayVisible = false
                // Resume prompt sat open while the background resolve ran, so
                // the pair is very likely cached by now — but if the user hit
                // select instantly, playAt waits for the resolve to land.
                playAt(choiceIndex === 0 ? savedPositionMs : 0)
                event.accepted = true
            }
        } else {
            if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
                if (!playerRoot.playbackStarted)
                    goBack()
                else
                    mpvController.sendKey("ESC")
                event.accepted = true
            } else if (event.key === Qt.Key_Backspace) {
                if (!playerRoot.playbackStarted)
                    goBack()
                else
                    mpvController.sendKey("BS")
                event.accepted = true
            } else if (event.key === Qt.Key_Up) {
                mpvController.sendKey("UP")
                event.accepted = true
            } else if (event.key === Qt.Key_Down) {
                mpvController.sendKey("DOWN")
                event.accepted = true
            } else if (event.key === Qt.Key_Left) {
                mpvController.sendKey("LEFT")
                event.accepted = true
            } else if (event.key === Qt.Key_Right) {
                mpvController.sendKey("RIGHT")
                event.accepted = true
            } else if (event.key === Qt.Key_Space) {
                mpvController.sendKey("SPACE")
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                mpvController.sendKey("ENTER")
                event.accepted = true
            }
        }
    }

    Connections {
        target: mpvController

        function onPositionChanged(ms) {
            if (ms > 0) {
                playerRoot.playbackStarted = true
                playerRoot.lastKnownPositionMs = ms
            }
        }
        function onDurationChanged(ms) {
            if (ms > 0) playerRoot.lastKnownDurationMs = ms
        }

        function onPlaybackEnded(finalPositionMs, finalDurationMs, reason) {
            // yt-dlp missing/outdated or an unplayable stream surfaces as mpv exit
            // code 2 before any position event — show the error instead of leaving.
            if (reason === "failed" && !playbackStarted) {
                playerRoot.errorMessage = "PLAYBACK FAILED\n\nPLEASE CHECK THAT YT-DLP IS INSTALLED AND UP TO DATE"
                return
            }
            var pos = lastKnownPositionMs || finalPositionMs
            var dur = lastKnownDurationMs || finalDurationMs
            // Completed videos stay in history with pos 0: they list under
            // History but never trigger the resume prompt.
            if (dur > 0 && pos >= dur * 0.95)
                youtubeBackend.savePosition(videoId, 0, item.title || "", item.channelName || "")
            else if (pos > 5000)
                youtubeBackend.savePosition(videoId, pos, item.title || "", item.channelName || "")
            var nextItemEnabled = appCore.get_setting(moduleRoot.moduleId, "autoplay_playlist_items")
            if (reason === "eof" && (nextItemEnabled === true || nextItemEnabled === "ON")
                    && playerRoot.playNextPlaylistItem())
                return
            goBack()
        }
    }

    Component.onCompleted: {
        if (videoUrl === "") {
            goBack()
            return
        }
        preparePlayback()

        var resumeSetting = appCore.get_setting(moduleRoot.moduleId, "resume_playback") || "Ask"
        var saved = youtubeBackend.getSavedPosition(videoId)
        var savedPos = saved.pos || 0

        if (resumeSetting === "Always") {
            playAt(savedPos)
        } else if (savedPos > 0) {
            savedPositionMs = savedPos
            overlayVisible = true
        } else {
            playAt(0)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "black"

        // Shown while mpv launches and buffers the stream (before its window
        // takes over). Hidden once the first position update arrives, or while
        // the resume prompt is up.
        Text {
            text: "LOADING..."
            color: "white"
            font.family: root.globalFont
            anchors.centerIn: parent
            font.pixelSize: root.sh * 0.05 //24
            visible: !overlayVisible && !playbackStarted && errorMessage === ""
        }

        Column {
            anchors.centerIn: parent
            spacing: root.sh * 0.05 //24
            visible: errorMessage !== ""

            Text {
                text: errorMessage
                color: "white"
                font.family: root.globalFont
                width: root.sw * 0.5625 //360
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                anchors.horizontalCenter: parent.horizontalCenter
                font.pixelSize: root.sh * 0.0375 //18
            }
            Text {
                text: root.hints.back + ":BACK " + root.hints.select + ":RETRY"
                color: "#919191"
                font.family: root.globalFont
                anchors.horizontalCenter: parent.horizontalCenter
                font.pixelSize: root.sh * 0.0333333 //16
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.surfaceColor
        visible: overlayVisible

        Rectangle {
            id: dialogRect
            color: root.surfaceColor
            anchors.centerIn: parent
            width: root.sw * 0.76875
            height: root.sh * 0.2833333

            Column {
                id: dialogColumn
                anchors.fill: parent
                spacing: root.sh * 0.05

                Text {
                    text: "RESUME PLAYBACK?"
                    color: root.secondaryColor
                    font.family: root.globalFont
                    font.pixelSize: root.sh * 0.0333333
                    anchors.horizontalCenter: parent.horizontalCenter
                }

                Column {
                    Repeater {
                        model: [
                            "Resume from " + formatTime(savedPositionMs),
                            "Start from the beginning"
                        ]
                        delegate: Item {
                            width: dialogColumn.width
                            height: root.sh * 0.0583333

                            Rectangle {
                                anchors.fill: delegateText
                                color: root.accentColor
                                visible: index === choiceIndex
                            }

                            Text {
                                id: delegateText
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData
                                color: index === choiceIndex ? root.surfaceColor : root.primaryColor
                                font.family: root.globalFont
                                font.capitalization: Font.AllUppercase
                                topPadding: root.sh * 0.0041667
                                leftPadding: root.sw * 0.009375
                                rightPadding: root.sw * 0.009375
                                bottomPadding: root.sh * 0.00625
                                font.pixelSize: root.sh * 0.0416667
                            }
                        }
                    }
                }

                Text {
                    text: root.hints.back + ":BACK " + root.hints.navigate + ":NAVIGATE " + root.hints.select + ":SELECT"
                    color: root.tertiaryColor
                    font.family: root.globalFont
                    font.pixelSize: root.sh * 0.0333333
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
    }

    function formatTime(ms) {
        var s   = Math.floor(ms / 1000)
        var h   = Math.floor(s / 3600)
        var m   = Math.floor((s % 3600) / 60)
        var sec = s % 60
        if (h > 0)
            return h + ":" + pad(m) + ":" + pad(sec)
        return m + ":" + pad(sec)
    }

    function pad(n) { return n < 10 ? "0" + n : "" + n }
}
