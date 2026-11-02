import QtQuick
import QtQuick.Effects

// One WeatherStar screen: a left-aligned column of fixed text lines.
//
// Every value arrives from the backend already formatted for the active Units
// setting, so this file is purely layout. Fields are limited to what Open-Meteo
// actually reports — no derived or estimated values.
Item {
    id: screen

    // NOT named `data` — that is Item's default property in QML and
    // declaring it would shadow the children list.
    property var    wx: ({})
    property string locationName: ""

    readonly property real lineSize: root.sh * 0.05 //24
    readonly property real lineGap:  root.sh * 0.0138021 //6.625

    // Ordered row set: always-present rows first, then honest-absent ones
    // (the air-quality/UV APIs don't cover every region). The status-bar
    // ticker no longer carries this set (it duplicated the screen); this
    // view is the single renderer for current-conditions rows.
    const kSummaryTag = "\u0001"   // internal tag: must-show row (AQ/UV/…)
    function rowTexts() {
        var w = screen.wx
        var out = [
            "HUMIDITY: " + (w.humidity || ""),
            "DEWPOINT: " + (w.dewPoint || ""),
            "PRESSURE: " + (w.pressure || ""),
            "WIND: " + (w.wind || ""),
            "VISIBILITY: " + (w.visibility || "")
        ]
        if (w.airQuality !== undefined) out.push(kSummaryTag + "AIR QUALITY: " + (w.airQuality || ""))
        if (w.uvIndex !== undefined) out.push(kSummaryTag + "UV INDEX: " + (w.uvIndex || ""))
        return out
    }

    // Wrap-aware vertical reservation. Qt 6 has no Item.clipContent (the
    // first fix attempt threw "Cannot assign to non-existent property" in the
    // journal and blanked the view), so rows are CONTAINED BY BUDGET: a row
    // that would wrap reserves two slots, following rows shift down, and the
    // row set never draws through the status bar. The glyph-advance estimate
    // is deliberately pessimistic (1.0em) — over-reserving leaves a gap,
    // under-reserving is what we must avoid.
    function reservedLines(i) {
        var cap = Math.max(8, Math.floor(rowArea.width / screen.lineSize))
        return rowTexts()[i].length > cap ? 2 : 1
    }
    function reservedOffset(idx) {
        var y = 0
        for (var i = 0; i < idx; ++i)
            y += reservedLines(i) * (screen.lineSize + screen.lineGap)
        return y
    }
    function rowOrder() {
        // Render order = priority order: must-show rows occupy the top of the
        // column, nice-to-have rows follow. The old prefix order cut AQ/UV at
        // 480 px; this order guarantees they land on the Pi CRT.
        var n = rowTexts().length
        var prio = rowPriority()
        var out = []
        for (var p = 0; p <= 1; ++p) {
            for (var i = 0; i < n; ++i)
                if (prio[i] === p) out.push(i)
        }
        return out
    }
    function rowPriority() {
        // Keep the current-conditions block and the summary rows (AQ/UV/
        // dew point/visibility) on screen at Pi height; almanac extras and
        // station details are nice-to-have and yield when space is tight.
        var out = []
        for (var i = 0; i < rowTexts().length; ++i) {
            out.push(rowTexts()[i].indexOf(kSummaryTag) === 0 ? 0 : 1)
        }
        return out
    }
    function rowFit() {
        // Height budget over the priority-ordered rows (see rowOrder): must-
        // show rows claim first, nice-to-have rows spend what remains.
        var order = rowOrder()
        var used = 0, fit = 0
        var lineH = screen.lineSize + screen.lineGap
        for (var k = 0; k < order.length; ++k) {
            used += reservedLines(order[k]) * lineH
            if (used > rowArea.height) break
            fit++
        }
        return fit
    }

    Text {
        id: title
        text: "CURRENTLY IN " + screen.locationName
        color: root.primaryColor
        font.family: root.globalFont
        font.pixelSize: screen.lineSize
        // Stops short of the icon rather than the screen edge, so a long
        // location name elides instead of running underneath it.
        width: parent.width - icon.width - (root.sw * 0.02)
        elide: Text.ElideRight
    }

    // Anchored to the right edge rather than following the title, so its
    // position doesn't move with the length of the location name.
    //
    // Tinted at runtime rather than shipped coloured — the app has a custom
    // colour scheme feature, and a baked-in colour would be wrong the moment
    // anyone changes theme. Same hidden-Image + MultiEffect pattern as
    // nfc_reader/views/Items.qml.
    Item {
        id: icon
        anchors.right: parent.right
        anchors.verticalCenter: title.verticalCenter
        width:  iconImage.width
        height: root.sh * 0.11
        // An unmapped code gives an empty name; draw nothing rather than a
        // broken-image box.
        visible: iconImage.source != ""

        Image {
            id: iconImage
            visible: false
            height: parent.height
            sourceSize.height: height
            fillMode: Image.PreserveAspectFit
            source: screen.wx.iconName
                ? "../../assets/images/wx/" + screen.wx.iconName + ".svg"
                : ""
        }
        MultiEffect {
            anchors.fill: iconImage
            source: iconImage
            colorization: 1.0
            colorizationColor: root.accentColor
        }
    }

    Text {
        id: conditions
        anchors.top: title.bottom
        anchors.topMargin: root.sh * 0.0854167 //41
        text: (screen.wx.condition || "") + " " + (screen.wx.temperature || "")
        color: root.accentColor
        font.family: root.globalFont
        font.pixelSize: root.sh * 0.0875 //42
    }

    // Data-line area: bottom-bounded to the screen; rows past the vertical
    // budget are hidden instead of overrunning into the status bar (the z-
    // fighting on Pi screenshots came from an unbounded row set).
    Item {
        id: rowArea
        anchors.top: conditions.bottom
        anchors.topMargin: root.sh * 0.0395833 //19
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        Repeater {
            id: rowRepeater
            property var renderOrder: screen.rowOrder()
            model: Math.min(screen.rowTexts().length, screen.rowFit())
            Text {
                x: 0
                text: { var t = screen.rowTexts()[renderOrder[index]]; return t.indexOf(screen.kSummaryTag) === 0 ? t.substring(1) : t }
                y: screen.reservedOffset(renderOrder[index])
                width: rowArea.width
                wrapMode: Text.WordWrap
                onText: appCore.note_state("renderdbg", "i=" + index + " prio=" + String(screen.rowPriority()[renderOrder[index]]) + " row=" + text.substring(0, 24))
                color: root.primaryColor
                font.family: root.globalFont
                font.pixelSize: screen.lineSize
            }
        }
    }
}
