import QtQuick

Item {
    id: preview

    property bool active: false
    property bool ready: false

    signal finished()

    property int volume: 100
    readonly property bool canPlayAudio: true

    property string currentUrl: ""

    onVolumeChanged: {
        if (active && typeof virtualChannelsBackend !== "undefined" && virtualChannelsBackend) {
            virtualChannelsBackend.set_preview_volume(Math.max(0, Math.min(1, volume / 100.0)))
        }
    }

    function show(url, positionMs) {
        if (url === undefined || url === null || String(url) === "") { clear(); return }
        if (typeof virtualChannelsBackend === "undefined" || !virtualChannelsBackend
                || !virtualChannelsBackend.preview_video_available()) {
            clear()
            return
        }
        var u = String(url)
        if (active && currentUrl === u) return
        currentUrl = u
        ready = false
        virtualChannelsBackend.start_preview_video(
            u, Number(positionMs) || 0, Math.max(0, Math.min(1, volume / 100.0)))
        active = true
    }

    function clear() {
        ready = false
        active = false
        currentUrl = ""
        if (typeof virtualChannelsBackend !== "undefined" && virtualChannelsBackend) {
            virtualChannelsBackend.stop_preview_video()
        }
    }

    Image {
        id: frameView
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        asynchronous: false
        cache: false
        source: (preview.active
                 && typeof virtualChannelsBackend !== "undefined"
                 && virtualChannelsBackend)
                ? virtualChannelsBackend.previewFrame
                : ""
        visible: preview.active && status === Image.Ready
        onStatusChanged: {
            if (status === Image.Ready && preview.active) preview.ready = true
        }
    }

    Component.onDestruction: clear()
}
