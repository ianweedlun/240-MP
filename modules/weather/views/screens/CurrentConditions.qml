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
    function rowTexts() {
        var w = screen.wx
        var out = [
            "HUMIDITY: " + (w.humidity || ""),
            "DEWPOINT: " + (w.dewPoint || ""),
            "PRESSURE: " + (w.pressure || ""),
            "WIND: " + (w.wind || ""),
            "VISIBILITY: " + (w.visibility || "")
        ]
        if (w.airQuality !== undefined) out.push("AIR QUALITY: " + (w.airQuality || ""))
        if (w.uvIndex !== undefined) out.push("UV INDEX: " + (w.uvIndex || ""))
        return out
    }

    // Wrap-aware vertical reservation. Qt 6 has no Item.clipContent (the
    // first fix attempt threw "Cannot assign to non-existent property" in the
    // journal and blanked the view), so rows are CONTAINED BY BUDGET: a row
    // that would wrap reserves two slots, following rows shift down, and the
    // row set never draws through the status bar. The glyph-advance estimate
    // is deliberately pessimistic (1.0em) — over-reserving leaves a gap,
    // under-reserving is what we must avoid.
    function reservedLines(int i) {
        var cap = Math.max(8, Math.floor(rowArea.width / screen.lineSize))
        return rowTexts()[i].length > cap ? 2 : 1
    }
    function reservedOffset(int idx) {
        var y = 0
        for (var i = 0; i < idx; ++i)
            y += reservedLines(i) * (screen.lineSize + screen.lineGap)
        return y
    }
    function rowFit() {
        var used = 0, fit = 0
        var n = rowTexts().length
        for (var i = 0; i < n; ++i) {
            used += reservedLines(i) * (screen.lineSize + screen.lineGap)
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
            model: Math.min(screen.rowTexts().length, screen.rowFit())
            Text {
                x: 0
                y: screen.reservedOffset(index)
                width: rowArea.width
                wrapMode: Text.WordWrap
                text: screen.rowTexts()[index]
                color: root.primaryColor
                font.family: root.globalFont
                font.pixelSize: screen.lineSize
            }
        }
    }
}
