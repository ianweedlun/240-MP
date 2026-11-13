import QtQuick
import Components

FocusScope {
    id: guideRoot

    property var navParams: ({})
    property string moduleId: navParams.moduleId || ""

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    readonly property int columnCount: 3
    readonly property real halfHourMs: 30 * 60 * 1000
    readonly property real spanMs: columnCount * halfHourMs

    property real fromMs: 0
    property var rows: []
    property int windowOffset: 0
    property real nowMs: 0

    property bool autoScroll: true
    property bool autoScrollEnabled: true
    property int resumeSeconds: 30
    property bool musicStartedHere: false
    property string status: ""

    property string previewMode: "VIDEO"

    property int masterVolume: 100
    readonly property bool guideMusicOn:
        typeof weatherBackend !== "undefined" && weatherBackend && weatherBackend.musicPlaying

    onGuideMusicOnChanged: applyVolume()

    readonly property bool previewAudible:
        !guideMusicOn && previewShowsVideo
        && previewLoader.item !== null && previewLoader.item.active
        && previewLoader.item.canPlayAudio

    onPreviewAudibleChanged: applyDuck()

    function applyDuck() {
        if (typeof weatherBackend === "undefined") return
        weatherBackend.duck_music(previewAudible, 400)
    }

    // Muting silences what is playing without moving the number, so unmuting
    // puts it back where it was rather than where it happened to be turned down
    // to.
    readonly property int audibleVolume: volumeOsd.muted ? 0 : masterVolume

    function applyVolume() {
        if (previewLoader.item) previewLoader.item.volume = guideMusicOn ? 0 : audibleVolume
        if (weatherBackend)
            weatherBackend.set_music_volume(audibleVolume)
    }

    function reloadVolume() {
        var mv = appCore.get_setting(guideRoot.moduleId, "volume")
        if (mv === undefined || mv === null || String(mv) === "") return
        var v = Math.max(0, Math.min(100, parseInt(mv)))
        if (v === masterVolume) return
        masterVolume = v
        // Pushed rather than bound: the bar writes its own level when a key is
        // pressed, and a binding would be gone the first time it did.
        volumeOsd.level = v
        applyVolume()
    }
    readonly property bool previewShowsPicture: previewMode !== "OFF"
    readonly property bool previewShowsVideo:   previewMode === "VIDEO"

    property var nowPlaying: ({})
    property int  detailFor: -1

    readonly property int previewSettleMs: 250

    readonly property int previewDwellMs: 400

    Timer {
        id: previewSettle
        interval: guideRoot.previewSettleMs
        repeat: false
        onTriggered: guideRoot.startPreview()
    }

    Timer {
        id: previewDwell
        interval: guideRoot.previewDwellMs
        repeat: false
        onTriggered: {
            if (!guideRoot.showDetail) return
            var i = channelRows.currentIndex
            if (i < 0 || i >= guideRoot.rows.length) return
            var row = guideRoot.rows[i]
            if (row.isSpecial) return
            guideRoot.dwellChannel = row.number
            virtualChannelsBackend.preview_stream(row.number)
        }
    }

    property int dwellChannel: -1

    property string previewImage: ""
    property int previewSlot: -1
    property bool previewStarted: false
    property bool previewingWeather: false

    Timer {
        id: previewFollow
        interval: 1000
        repeat: true
        running: guideRoot.showDetail && guideRoot.previewShowsPicture
                 && guideRoot.previewStarted
        onTriggered: guideRoot.followSchedule()
    }

    function followSchedule() {
        if (!showDetail || !previewShowsPicture) return
        var i = channelRows.currentIndex
        if (i < 0 || i >= rows.length) return
        var row = rows[i]
        if (row.isSpecial) return

        var slot = virtualChannelsBackend.current_slot(row.number)
        if (slot === previewSlot) return

        if (slot < 0) {
            previewSlot = -1
            previewImage = ""
            if (previewLoader.item) previewLoader.item.clear()
            return
        }
        showCurrent(row.number, true)
        refreshNowPlaying()
    }

    function startPreview() {
        if (!showDetail) { stopPreview(); return }
        var i = channelRows.currentIndex
        if (i < 0 || i >= rows.length) { stopPreview(); return }
        var row = rows[i]
        if (row.viewPath === weatherView) {
            if (!previewShowsPicture) return
            previewingWeather = true
            previewStarted = true
            previewImage = ""
            if (previewLoader.item) previewLoader.item.clear()
            if (virtualChannelsBackend) virtualChannelsBackend.cancel_preview_stream()
            return
        }
        previewingWeather = false
        if (row.isSpecial) { stopPreview(); return }

        if (!previewShowsPicture) return
        previewStarted = true
        showCurrent(row.number, false)
    }

    function showCurrent(channelNumber, following) {
        var src = virtualChannelsBackend.preview_source(channelNumber,
                                                        Math.round(screen.width),
                                                        Math.round(screen.height))
        if (!src) { stopPreview(); return }

        previewImage = src.image || ""
        previewSlot  = src.slotIndex !== undefined ? src.slotIndex : -1
        if (!previewShowsVideo) return

        if (src.valid === true && previewLoader.item) {
            previewLoader.item.show(src.url, src.positionMs)
        } else if (previewLoader.item) {
            previewLoader.item.clear()
            if (following) {
                dwellChannel = channelNumber
                virtualChannelsBackend.preview_stream(channelNumber)
            } else {
                previewDwell.restart()
            }
        }
    }

    function stopPreview() {
        previewSettle.stop()
        previewDwell.stop()
        dwellChannel = -1
        previewSlot = -1
        previewStarted = false
        previewingWeather = false
        // Also runs from Component.onDestruction, by which point the context
        // property is gone.
        if (virtualChannelsBackend) virtualChannelsBackend.cancel_preview_stream()
        previewImage = ""
        if (previewLoader.item) previewLoader.item.clear()
    }

    function refreshNowPlaying() {
        if (!showDetail) { nowPlaying = ({}); detailFor = -1; return }
        if (autoScroll && detailFor >= 0) return
        var i = channelRows.currentIndex
        if (i < 0 || i >= rows.length) { nowPlaying = ({}); detailFor = -1; return }
        var row = rows[i]
        detailFor = row.number
        if (row.viewPath === weatherView) {
            nowPlaying = ({
                valid: true,
                series: "WEATHER CHANNEL",
                title: "Local Forecast",
                desc: "Live local conditions, extended forecast and almanac."
            })
        } else {
            nowPlaying = row.isSpecial ? ({ valid: false, message: row.name })
                                       : virtualChannelsBackend.now_next(row.number)
        }
    }

    function refreshDetail() {
        if (!showDetail) {
            stopPreview()
            nowPlaying = ({})
            detailFor = -1
            return
        }
        stopPreview()
        previewSettle.restart()
        var i = channelRows.currentIndex
        if (i < 0 || i >= rows.length) { nowPlaying = ({}); detailFor = -1; return }
        var row = rows[i]
        detailFor = row.number
        if (row.viewPath === weatherView) {
            nowPlaying = ({
                valid: true,
                series: "WEATHER CHANNEL",
                title: "Local Forecast",
                desc: "Live local conditions, extended forecast and almanac."
            })
        } else {
            nowPlaying = row.isSpecial ? ({ valid: false, message: row.name })
                                       : virtualChannelsBackend.now_next(row.number)
        }
    }

    function runLabel(ms) {
        var total = Math.round(Number(ms) / 1000)
        if (!isFinite(total) || total <= 0) return ""
        var m = Math.floor(total / 60)
        var sec = total % 60
        if (m >= 60) {
            var h = Math.floor(m / 60)
            return h + "H " + (m % 60) + "M"
        }
        return m > 0 ? m + "M " + (sec > 0 ? sec + "S" : "") : sec + "S"
    }

    readonly property real edgeMargin: Math.round(root.sw * 0.045)
    readonly property real topMargin:  Math.round(root.sh * 0.055)

    focus: true

    readonly property string weatherView: "WeatherChannel.qml"
    readonly property string manageView: "Manage.qml"

    function refresh() {
        nowMs = virtualChannelsBackend.now_ms()
        fromMs = Math.floor(nowMs / halfHourMs) * halfHourMs + windowOffset * halfHourMs
        var real = virtualChannelsBackend.guide_grid(fromMs, spanMs)

        // The built-in channels take their places on the dial like any other,
        // so what channel up and down will land on is what the grid shows.
        var all = []
        for (var i = 0; i < real.length; i++) all.push(real[i])
        if (toggleOn("channels.weather", false))
            all.push({ number: virtualChannelsBackend.weather_channel_number(),
                       name: "Weather", isSpecial: true,
                       viewPath: weatherView,
                       blocks: [{ startMs: fromMs, durMs: spanMs,
                                  title: "Local Forecast", series: "",
                                  ep: "", onNow: true }] })
        all.sort(function(a, b) { return a.number - b.number })
        rows = all
    }

    // The guide's own row opens nothing: the viewer is already here.
    function openSpecial(row) {
        if (row.viewPath === "") return
        navigateTo(row.viewPath, {}, { fromGuide: true })
    }

    function timeLabel(ms) { return Qt.formatDateTime(new Date(ms), "h:mm AP") }

    function toggleOn(key, dflt) {
        var v = appCore.get_setting(guideRoot.moduleId, key)
        if (v === undefined || v === null || v === "") return dflt
        if (typeof v === "string") return v.toUpperCase() === "ON"
        return !!v
    }

    Connections {
        target: virtualChannelsBackend
        function onPreviewStreamReady(ch, url, positionMs) {
            if (!guideRoot.showDetail || ch !== guideRoot.dwellChannel) return
            if (previewLoader.item) previewLoader.item.show(url, positionMs)
        }
    }

    Component.onCompleted: {
        // The consolidated single row. Old two-row configs still mean the same
        // thing until the viewer uses the new one: read the new key first,
        // fall back to the legacy pair.
        var idling = appCore.get_setting(guideRoot.moduleId, "guide.idling")
        var idlingSrc = "legacy"
        if (idling !== undefined && String(idling) !== "") {
            idlingSrc = "setting"
            var opt = String(idling)
            if (opt.indexOf("Holds") === 0) {
                autoScrollEnabled = opt.indexOf("Never") < 0
                autoScroll = false
                resumeSeconds = autoScrollEnabled
                               ? (opt.indexOf("1m") >= 0 ? 60
                                  : (opt.indexOf("5m") >= 0 ? 300 : 30))
                               : 0
            } else {
                autoScrollEnabled = true
                autoScroll = false
                resumeSeconds = 5
            }
        } else {
            autoScrollEnabled = toggleOn("guide.autoscroll", true)
            autoScroll = false
            var rs = appCore.get_setting(guideRoot.moduleId, "guide.resume_seconds")
            if (rs !== undefined && rs !== null && rs !== "") resumeSeconds = parseInt(rs) || 0
        }
        appCore.note_state("guide-idle",
                           "auto=" + (autoScrollEnabled ? "on" : "off") +
                           " resume=" + resumeSeconds + "s src=" + idlingSrc)
        var pv = appCore.get_setting(guideRoot.moduleId, "guide.preview")
        if (pv !== undefined && pv !== null && String(pv) !== "")
            previewMode = String(pv).toUpperCase()
        var mv = appCore.get_setting(guideRoot.moduleId, "volume")
        if (mv !== undefined && mv !== null && String(mv) !== "")
            masterVolume = Math.max(0, Math.min(100, parseInt(mv)))
        volumeOsd.level = masterVolume
        applyVolume()
        refresh()
        for (var k = 0; k < rows.length; k++) {
            if (!rows[k].isSpecial) {
                channelRows.currentIndex = k
                break
            }
        }
        refreshNowPlaying()

        if (idleTracker) idleTracker.mpvActive = true

        if (toggleOn("guide.music", false) && weatherBackend) {
            var wasPlaying = weatherBackend.musicPlaying
            weatherBackend.startMusic()
            musicStartedHere = !wasPlaying && weatherBackend.musicPlaying
            applyVolume()
            if (!weatherBackend.musicPlaying)
                status = "Music needs the Weather module's Music setting on"
        }
        previewSettle.restart()
        if (autoScrollEnabled && resumeSeconds > 0) resumeTimer.restart()
    }

    Component.onDestruction: {
        stopPreview()
        if (weatherBackend) weatherBackend.duck_music(false, 250)
        if (idleTracker) {
            idleTracker.mpvActive = false
            idleTracker.resetActivity()
        }
        if (musicStartedHere && weatherBackend)
            weatherBackend.stopMusic()
    }

    // mpv is never the thing making a sound on the guide -- the preview is a
    // QML player and the music is the weather module's -- so the volume keys
    // are this screen's to answer.
    VolumeOsd {
        id: volumeOsd
        moduleId: guideRoot.moduleId
        z: 100
        onAdjusted: {
            guideRoot.masterVolume = level
            guideRoot.applyVolume()
        }
    }

    Timer { interval: 30000; running: true; repeat: true; onTriggered: guideRoot.refresh() }
    Timer { interval: 5000; running: true; repeat: true; onTriggered: guideRoot.reloadVolume() }
    Timer { interval: 5000; running: true; repeat: true
            onTriggered: {
                guideRoot.nowMs = virtualChannelsBackend.now_ms()
                if (guideRoot.showDetail) guideRoot.refreshNowPlaying()
            } }

    Timer {
        id: resumeTimer
        interval: Math.max(1, guideRoot.resumeSeconds) * 1000
        running: guideRoot.autoScrollEnabled && !guideRoot.autoScroll
                 && guideRoot.resumeSeconds > 0
        repeat: false
        onTriggered: guideRoot.resumeCrawl()
    }

    function resumeCrawl() {
        windowOffset = 0
        refresh()
        autoScroll = true
        if (!showDetail) refreshDetail()
    }

    function takeControl() {
        var wasCrawling = autoScroll
        autoScroll = false
        resumeTimer.restart()
        if (wasCrawling) refreshDetail()
    }

    // Digits typed on the remote move the highlight to that channel as they
    // come; Enter, or a moment's pause, leaves it there for Enter to watch. A
    // number that turns out to be no channel puts things back as they were
    // before the first digit: the row that was highlighted, or the crawl.
    property bool entryOpen: false
    property bool entryWasCrawling: false
    property int  entryFrom: -1
    ChannelEntry {
        id: channelEntry
        dial: guideRoot.rows.map(function(r) { return r.number })
        onTyped: function(digits) {
            if (digits === "") { restoreHighlight(); return }   // Back while typing
            if (!guideRoot.entryOpen) {
                guideRoot.entryOpen        = true
                guideRoot.entryWasCrawling = guideRoot.autoScroll
                guideRoot.entryFrom        = channelRows.currentIndex
            }
            highlightChannel(parseInt(digits, 10))
        }
        onChosen: function(number) { highlightChannel(number); guideRoot.entryOpen = false }
        onDropped: restoreHighlight()
    }

    function restoreHighlight() {
        if (!guideRoot.entryOpen) return
        guideRoot.entryOpen = false
        if (guideRoot.entryWasCrawling) {
            resumeCrawl()
        } else {
            channelRows.currentIndex = guideRoot.entryFrom
            refreshDetail()
        }
    }

    function highlightChannel(number) {
        for (var i = 0; i < rows.length; i++) {
            if (rows[i].number !== number) continue
            takeControl()
            channelRows.currentIndex = i
            refreshDetail()
            return
        }
    }

    function tuneFromGuide(direction) {
        stopPreview()
        if (rows.length === 0) return
        var i = direction > 0 ? 0 : rows.length - 1
        var row = rows[i]
        // The guide's own row is nowhere to go; from either end, the first
        // channel beyond it is.
        if (row.isSpecial && row.viewPath === "" && rows.length > 1) row = rows[i + direction]
        if (row.isSpecial) { openSpecial(row); return }
        navigateTo("Player.qml", {
            moduleId:      guideRoot.moduleId,
            channelNumber: row.number,
            channelName:   row.name
        }, { fromGuide: true })
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_ChannelUp || event.key === Qt.Key_PageUp) {
            tuneFromGuide(1); event.accepted = true; return
        }
        if (event.key === Qt.Key_ChannelDown || event.key === Qt.Key_PageDown) {
            tuneFromGuide(-1); event.accepted = true; return
        }
        // Enter on a typed number highlights it and then watches, below. A
        // number no channel has is dropped, and the press with it.
        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && channelEntry.digits !== "") {
            if (!channelEntry.commit()) { event.accepted = true; return }
        } else if (channelEntry.press(event.key, event.isAutoRepeat)) { event.accepted = true; return }
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            goBack()
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            guideRoot.takeControl()
            var n = guideRoot.rows.length
            if (n > 0)
                channelRows.currentIndex = (channelRows.currentIndex - 1 + n) % n
            guideRoot.refreshDetail()
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            guideRoot.takeControl()
            var count = guideRoot.rows.length
            if (count > 0)
                channelRows.currentIndex = (channelRows.currentIndex + 1) % count
            guideRoot.refreshDetail()
            event.accepted = true
        } else if (event.key === Qt.Key_Left) {
            guideRoot.takeControl()
            guideRoot.windowOffset = Math.max(0, guideRoot.windowOffset - 1)
            guideRoot.refresh()
            event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            guideRoot.takeControl()
            guideRoot.windowOffset += 1
            guideRoot.refresh()
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            var i = channelRows.currentIndex
            // During the crawl the ListView model is rows.length * copies, so the
            // highlight index can sit past the row count. A stray value like that
            // can outlive the crawl and land on the real row with the same
            // residue; wrap it before the guard below would otherwise reject it.
            if (guideRoot.rows.length > 0) i %= guideRoot.rows.length
            if (!guideRoot.autoScroll && i >= 0 && i < guideRoot.rows.length) {
                var row = guideRoot.rows[i]
                guideRoot.stopPreview()
                if (row.isSpecial) {
                    guideRoot.openSpecial(row)
                } else {
                    navigateTo("Player.qml", {
                        moduleId:      guideRoot.moduleId,
                        channelNumber: row.number,
                        channelName:   row.name
                    }, { fromGuide: true })
                }
            }
            event.accepted = true
        }
    }

    readonly property real gridWidth:   Math.round(width - edgeMargin * 2)
    readonly property real channelColW: Math.round(gridWidth * 0.30)
    readonly property real timeAreaW:   gridWidth - channelColW
    readonly property real pxPerMs:     timeAreaW / spanMs
    readonly property real rowH:        Math.round(root.sh * 0.10)
    readonly property real headerH:     Math.round(root.sh * 0.075)

    readonly property bool showDetail:  rows.length > 0 && (previewShowsPicture || !autoScroll)
    onShowDetailChanged: if (!showDetail) stopPreview()
    readonly property real detailH:     showDetail ? Math.round(height * 0.42) : 0

    Rectangle {
        anchors.fill: parent
        color: root.surfaceColor
    }

    Item {
        id: detail
        visible: guideRoot.showDetail
        x: guideRoot.edgeMargin
        y: guideRoot.topMargin
        width: guideRoot.gridWidth
        height: Math.max(0, guideRoot.detailH - root.sh * 0.02)

        Item {
            id: screen
            visible: guideRoot.previewShowsPicture
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            height: parent.height
            width: visible ? Math.min(parent.width * 0.5, height * 4 / 3) : 0
            clip: true

            Rectangle {
                anchors.fill: parent
                color: guideRoot.previewingWeather ? root.surfaceColor : "black"
            }

            Image {
                id: previewStill
                anchors.fill: parent
                source: guideRoot.previewImage
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                visible: status === Image.Ready
            }

            Loader {
                id: previewLoader
                anchors.fill: parent
                asynchronous: true
                onLoaded: {
                    item.finished.connect(guideRoot.followSchedule)
                    item.volume = guideRoot.guideMusicOn ? 0 : guideRoot.audibleVolume
                }
                active: guideRoot.previewShowsVideo
                source: "ChannelPreview.qml"
                onStatusChanged: {
                    if (status === Loader.Error)
                        console.log("[guide] no video preview available; showing the still instead")
                }
            }

            Item {
                id: wxPreviewHost
                anchors.fill: parent
                visible: guideRoot.previewingWeather && wxPreviewLoader.status === Loader.Ready
                clip: true

                Item {
                    width: root.sw
                    height: root.sh
                    transformOrigin: Item.TopLeft
                    scale: Math.min(wxPreviewHost.width / Math.max(1, root.sw),
                                    wxPreviewHost.height / Math.max(1, root.sh))

                    Loader {
                        id: wxPreviewLoader
                        anchors.fill: parent
                        focus: false
                        readonly property bool wantWeather:
                            guideRoot.showDetail && guideRoot.previewShowsPicture
                            && guideRoot.previewingWeather
                        onWantWeatherChanged: {
                            if (wantWeather) {
                                setSource(Qt.resolvedUrl("../../weather/views/Weather.qml"), {
                                    "navParams": { previewMode: true, moduleId: "com.240mp.weather" }
                                })
                            } else {
                                setSource("")
                            }
                        }
                    }
                }
            }
        }

        Item {
            id: about
            anchors.left: screen.right
            anchors.leftMargin: guideRoot.previewShowsPicture ? root.sw * 0.025 : 0
            anchors.right: parent.right
            height: parent.height

            Text {
                id: channelLine
                text: {
                    var target = guideRoot.detailFor
                    for (var j = 0; j < guideRoot.rows.length; j++) {
                        if (guideRoot.rows[j].number === target)
                            return "CH " + guideRoot.rows[j].number + "  " + guideRoot.rows[j].name
                    }
                    var i = channelRows.currentIndex
                    if (guideRoot.rows.length > 0 && i >= 0) {
                        var r = guideRoot.rows[i % guideRoot.rows.length]
                        if (r) return "CH " + r.number + "  " + r.name
                    }
                    return ""
                }
                visible: text !== ""
                color: root.accentColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.top: parent.top
                anchors.left: parent.left
                width: parent.width
                elide: Text.ElideRight
                font.pixelSize: root.sh * 0.026
            }

            Text {
                id: seriesLine
                text: {
                    var n = guideRoot.nowPlaying
                    if (!n || n.valid !== true) return n && n.message ? n.message : "Off air"
                    return n.series ? n.series : (n.title || "")
                }
                color: root.primaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.top: channelLine.visible ? channelLine.bottom : parent.top
                anchors.topMargin: channelLine.visible ? root.sh * 0.004 : 0
                anchors.left: parent.left
                width: parent.width
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                font.pixelSize: root.sh * 0.036
            }

            Text {
                id: epLine
                text: {
                    var n = guideRoot.nowPlaying
                    if (!n || n.valid !== true) return ""
                    if (!n.series) return n.ep ? n.ep : ""
                    var bits = []
                    if (n.ep) bits.push(n.ep)
                    if (n.title) bits.push(n.title)
                    return bits.join("  ")
                }
                visible: text !== ""
                color: root.secondaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.top: seriesLine.bottom
                anchors.topMargin: root.sh * 0.006
                anchors.left: parent.left
                width: parent.width
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                font.pixelSize: root.sh * 0.030
            }

            Text {
                id: runLine
                text: {
                    var n = guideRoot.nowPlaying
                    if (!n || n.valid !== true) return ""
                    var bits = [guideRoot.runLabel(n.durMs)]
                    if (n.nextTitle) bits.push("NEXT: " + n.nextTitle)
                    return bits.join("   ·   ")
                }
                visible: text !== ""
                color: root.tertiaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.top: epLine.visible ? epLine.bottom : seriesLine.bottom
                anchors.topMargin: root.sh * 0.010
                anchors.left: parent.left
                width: parent.width
                elide: Text.ElideRight
                font.pixelSize: root.sh * 0.024
            }

            Item {
                id: progress
                visible: guideRoot.nowPlaying && guideRoot.nowPlaying.valid === true
                         && Number(guideRoot.nowPlaying.durMs) > 0
                anchors.top: runLine.visible ? runLine.bottom : epLine.bottom
                anchors.topMargin: root.sh * 0.012
                anchors.left: parent.left
                width: parent.width
                height: root.sh * 0.050

                Text {
                    id: elapsed
                    text: {
                        var n = guideRoot.nowPlaying
                        if (!n || n.valid !== true) return ""
                        return guideRoot.runLabel(n.offsetMs) + " IN"
                    }
                    color: root.tertiaryColor
                    font.family: root.globalFont
                    font.capitalization: Font.AllUppercase
                    anchors.top: parent.top
                    anchors.left: parent.left
                    font.pixelSize: root.sh * 0.022
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    width: parent.width
                    height: root.sh * 0.010
                    property color baseColor: root.primaryColor
                    color: Qt.rgba(baseColor.r, baseColor.g, baseColor.b, 0.2)

                    Rectangle {
                        height: parent.height
                        color: root.accentColor
                        width: {
                            var n = guideRoot.nowPlaying
                            if (!n || n.valid !== true) return 0
                            var d = Number(n.durMs)
                            if (!isFinite(d) || d <= 0) return 0
                            return parent.width * Math.max(0, Math.min(1, Number(n.offsetMs) / d))
                        }
                    }
                }
            }

            Text {
                text: {
                    var n = guideRoot.nowPlaying
                    return (n && n.valid === true && n.desc) ? n.desc : ""
                }
                color: root.secondaryColor
                font.family: root.globalFont
                anchors.top: progress.visible ? progress.bottom : runLine.bottom
                anchors.topMargin: root.sh * 0.012
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: parent.width
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
                font.pixelSize: root.sh * 0.024
            }
        }
    }

    Item {
        id: header
        x: guideRoot.edgeMargin
        y: guideRoot.topMargin + guideRoot.detailH
        width: guideRoot.gridWidth
        height: guideRoot.headerH

        Rectangle { anchors.fill: parent; color: root.accentColor }

        Text {
            id: guideDate
            text: Qt.formatDateTime(new Date(guideRoot.fromMs), "ddd d MMM")
            color: root.surfaceColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            anchors.verticalCenter: parent.verticalCenter
            x: root.sw * 0.0078125
            font.pixelSize: root.sh * 0.030
        }

        // Straight after the date. The date follows the window as it is
        // scrolled forward; the clock stays on now.
        Text {
            text: guideRoot.nowMs > 0 ? guideRoot.timeLabel(guideRoot.nowMs) : ""
            color: root.surfaceColor
            font.family: root.globalFont
            font.capitalization: Font.AllUppercase
            anchors.verticalCenter: parent.verticalCenter
            x: guideDate.x + guideDate.width + root.sw * 0.0156
            font.pixelSize: root.sh * 0.030
        }

        Repeater {
            model: guideRoot.columnCount
            delegate: Text {
                required property int index
                text: guideRoot.timeLabel(guideRoot.fromMs + index * guideRoot.halfHourMs)
                color: root.surfaceColor
                font.family: root.globalFont
                anchors.verticalCenter: parent.verticalCenter
                x: guideRoot.channelColW + index * (guideRoot.timeAreaW / guideRoot.columnCount)
                   + root.sw * 0.0046875
                font.pixelSize: root.sh * 0.030
            }
        }
    }

    RowTrackingListView {
        id: channelRows
        x: guideRoot.edgeMargin
        y: header.y + header.height
        width: guideRoot.gridWidth
        height: Math.round(guideRoot.height - y
                           - (guideRoot.autoScroll ? guideRoot.topMargin * 0.5 : root.sh * 0.06))
        clip: true
        interactive: false
        currentIndex: 0
        highlightMoveDuration: 0
        highlightFollowsCurrentItem: !guideRoot.autoScroll

        readonly property int setHeight: Math.max(1, guideRoot.rows.length * guideRoot.rowH)
        readonly property int copies: guideRoot.rows.length > 0
                                      ? Math.max(2, Math.ceil(height / setHeight) + 1)
                                      : 0
        model: guideRoot.autoScroll ? guideRoot.rows.length * copies
                                    : guideRoot.rows.length

        delegate: Item {
            width: channelRows.width
            height: guideRoot.rowH

            required property int index
            readonly property var rowData: guideRoot.rows.length > 0
                                           ? guideRoot.rows[index % guideRoot.rows.length]
                                           : null
            readonly property bool selected: !guideRoot.autoScroll
                                             && index === channelRows.currentIndex

            Rectangle {
                x: 1
                y: 0
                width: guideRoot.channelColW - 1
                height: parent.height - 1
                color: parent.selected ? root.accentColor : "transparent"
            }

            Rectangle {
                visible: !parent.selected
                x: 1
                y: 0
                width: guideRoot.channelColW - 1
                height: parent.height - 1
                property color baseColor: root.primaryColor
                color: Qt.rgba(baseColor.r, baseColor.g, baseColor.b, 0.13)
            }

            // Crisp 1px hairlines on top (z: 2) so selected row keeps identical borders.
            Rectangle { z: 2; x: 0; y: 0; width: 1; height: parent.height; color: root.tertiaryColor }
            Rectangle { z: 2; x: guideRoot.channelColW - 1; y: 0; width: 1; height: parent.height; color: root.tertiaryColor }
            Rectangle { z: 2; x: parent.width - 1; y: 0; width: 1; height: parent.height; color: root.tertiaryColor }
            Rectangle { z: 2; x: 0; y: parent.height - 1; width: parent.width; height: 1; color: root.tertiaryColor }

            Text {
                id: chNum
                text: parent.rowData ? parent.rowData.number : ""
                color: parent.selected ? root.surfaceColor : root.primaryColor
                font.family: root.globalFont
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: root.sw * 0.0078125
                width: root.sw * 0.05
                horizontalAlignment: Text.AlignRight
                font.pixelSize: root.sh * 0.036
            }
            Text {
                text: parent.rowData ? parent.rowData.name : ""
                color: parent.selected ? root.surfaceColor : root.primaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: chNum.right
                anchors.leftMargin: root.sw * 0.009375
                width: guideRoot.channelColW - chNum.width - root.sw * 0.022
                elide: Text.ElideRight
                font.pixelSize: root.sh * 0.033
            }

            Repeater {
                model: parent.rowData ? parent.rowData.blocks : []
                delegate: Item {
                    id: blockDelegate
                    required property var modelData

                    readonly property real rawX: (modelData.startMs - guideRoot.fromMs) * guideRoot.pxPerMs
                    readonly property int  snappedX: Math.round(Math.max(0, rawX))
                    readonly property real rawRight: (modelData.startMs + modelData.durMs - guideRoot.fromMs) * guideRoot.pxPerMs
                    readonly property int  snappedRight: Math.round(Math.min(guideRoot.timeAreaW, rawRight))

                    x: guideRoot.channelColW + snappedX
                    y: 0
                    width: Math.max(0, snappedRight - snappedX)
                    height: parent.height - 1
                    visible: width > 2

                    Rectangle {
                        anchors.fill: parent
                        color: blockDelegate.modelData.onNow ? root.secondaryColor : root.surfaceColor
                    }
                    Rectangle {
                        x: parent.width - 1; y: 0; width: 1; height: parent.height
                        color: root.tertiaryColor
                    }

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: root.sw * 0.0046875
                        anchors.rightMargin: root.sw * 0.003125
                        verticalAlignment: Text.AlignVCenter
                        text: {
                            var base = blockDelegate.modelData.series
                                       ? blockDelegate.modelData.series + ": " + blockDelegate.modelData.title
                                       : blockDelegate.modelData.title
                            return blockDelegate.modelData.count > 1 ? base + "  +" + (blockDelegate.modelData.count - 1)
                                                       : base
                        }
                        color: blockDelegate.modelData.onNow ? root.surfaceColor : root.primaryColor
                        font.family: root.globalFont
                        font.capitalization: Font.AllUppercase
                        elide: Text.ElideRight
                        font.pixelSize: root.sh * 0.030
                    }
                }
            }

            Text {
                visible: !parent.rowData || !parent.rowData.blocks
                         || parent.rowData.blocks.length === 0
                x: guideRoot.channelColW + root.sw * 0.0046875
                anchors.verticalCenter: parent.verticalCenter
                text: "No listings"
                color: root.tertiaryColor
                font.family: root.globalFont
                font.capitalization: Font.AllUppercase
                font.pixelSize: root.sh * 0.030
            }
        }

        NumberAnimation on contentY {
            running: guideRoot.autoScroll && guideRoot.rows.length > 0
            loops: Animation.Infinite
            from: 0
            to: guideRoot.rows.length * guideRoot.rowH
            duration: Math.max(1, guideRoot.rows.length * guideRoot.rowH) * 45
        }
    }

    Rectangle {
        width: 2
        height: guideRoot.autoScroll
                ? channelRows.height
                : Math.min(channelRows.height, guideRoot.rows.length * guideRoot.rowH)
        color: root.primaryColor
        opacity: 0.6
        y: channelRows.y
        x: guideRoot.edgeMargin + guideRoot.channelColW
           + Math.round((guideRoot.nowMs - guideRoot.fromMs) * guideRoot.pxPerMs)
        visible: guideRoot.rows.length > 0
                 && guideRoot.nowMs >= guideRoot.fromMs
                 && guideRoot.nowMs < guideRoot.fromMs + guideRoot.spanMs
    }

    Text {
        visible: guideRoot.rows.length === 0
        anchors.centerIn: parent
        text: "No channels"
        color: root.tertiaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        font.pixelSize: root.sh * 0.0333333
    }

    Text {
        visible: guideRoot.status !== ""
        text: guideRoot.status
        color: root.tertiaryColor
        font.family: root.globalFont
        font.capitalization: Font.AllUppercase
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: root.sh * 0.012
        anchors.leftMargin: guideRoot.edgeMargin
        font.pixelSize: root.sh * 0.026
    }

    Text {
        visible: !guideRoot.autoScroll
        text: root.hints.back + ":BACK " + root.hints.navigate + ":CHANNEL "
              + root.hints.change + ":TIME " + root.hints.select + ":WATCH"
        color: root.tertiaryColor
        font.family: root.globalFont
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.bottomMargin: root.sh * 0.012
        anchors.rightMargin: guideRoot.edgeMargin
        font.pixelSize: root.sh * 0.026
    }
}
