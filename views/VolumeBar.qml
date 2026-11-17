import QtQuick

// The app's own volume bar (#34) — one for the whole shell, drawn above every
// view, shown whenever a volume key moves the system mixer.
//
// It replaces the per-module bars (Guide / WeatherChannel / Player), which each
// owned a volume number of their own and could therefore disagree with each
// other and with mpv. There is now one number, `volumeManager`, and this is the
// renderer for the screens WE own. When a child owns the display (mpv playing,
// RetroArch running) the app renderer is suspended and scripts/mpv-media-keys.lua
// draws the same bar from the same number pushed over IPC — one state, two
// renderers, identical layout (every fraction below is the lua bar's).
//
// It never takes focus: volume keys land in InputManager's event filter, so a
// bar that swallowed keys would break navigation.
Item {
    id: bar

    // Hidden until a volume key is pressed, then hidden again by the timer —
    // the same show/1.5s-fade behaviour the old module bars had.
    anchors.fill: parent
    z: 500
    opacity: 0
    visible: opacity > 0

    Behavior on opacity { NumberAnimation { duration: 120 } }

    // Guarded the same way Main.qml guards appCore: the engine outlives the
    // backend locals, so during teardown the context property reads back null.
    readonly property int level:  volumeManager ? volumeManager.level : 0
    readonly property bool muted: volumeManager ? volumeManager.muted : false
    // Must match VolumeManager::kStep — the lua renderer quantises the same way,
    // so both bars show the same number of filled ticks for the same level.
    readonly property int step:   5
    readonly property int maxLevel: 100

    readonly property real fs:    root.sh * 0.0333333
    readonly property real lm:    root.sw * 0.12
    readonly property real barW:  root.sw * 0.88 - lm
    readonly property real barH:  fs * 2
    readonly property int  ticks: Math.max(1, Math.round(maxLevel / step))
    readonly property real slotW: barW / ticks
    readonly property real gap:   Math.max(1, Math.floor(slotW * 0.35))
    readonly property real tickW: Math.max(1, Math.floor(slotW - gap))
    readonly property real dashH: Math.max(2, Math.floor(barH * 0.15))

    function show() {
        hideTimer.restart()
        opacity = 1
    }

    // The manager is the truth, so a change can come from anywhere: a key on
    // this screen, a key while a game is running, or a restart restoring the
    // saved level. All of them show the bar.
    Connections {
        target: volumeManager
        function onVolumeChanged(level, muted) { bar.show() }
    }

    Timer {
        id: hideTimer
        interval: 1500
        onTriggered: bar.opacity = 0
    }

    // Bottom-anchored so the large label grows upward off the bar row instead of
    // over the ticks. Says MUTE when muted, because a mute leaves the ticks
    // where they were and the bar would otherwise say nothing had happened.
    Text {
        x: bar.lm
        y: root.sh * 0.7979166 - height
        text: bar.muted ? "MUTE" : "VOLUME"
        color: root.primaryColor
        font.family: root.globalFont
        font.pixelSize: bar.fs * 3
    }

    Row {
        x: bar.lm
        y: root.sh * 0.8333333
        spacing: bar.slotW - bar.tickW

        Repeater {
            model: bar.ticks
            delegate: Item {
                required property int index
                width: bar.tickW
                height: bar.barH

                // A filled tick is the full height of the row; an empty one is a
                // dash centred in the same slot, so the row does not change
                // width as the volume moves.
                readonly property bool filled:
                    !bar.muted && index < Math.round(bar.level / bar.step)

                Rectangle {
                    width: parent.width
                    height: parent.filled ? bar.barH : bar.dashH
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.primaryColor
                    opacity: parent.filled ? 1.0 : 0.31
                }
            }
        }
    }
}
