import QtQuick
import Components

// The LOCATION setting picker (#37): enter coordinates from the d-pad, no
// keyboard. The dial composes "lat, lon" — OK applies it through the
// backend's "write as the primary line of weather_location.txt, then re-resolve"
// path, so the live forecast re-resolves to the new coordinates in place and
// then closes back to Settings.
//
// Coordinates only (no letters): place names stay on the conf-file path —
// geocoding wants disambiguation ("CATONSVILLE, MD") the dial can spell too,
// but dialing names is slow, and coordinates never touch the geocoder.
FocusScope {
    id: locationPicker
    signal goBack()

    property var navParams: ({})
    property string moduleId: navParams.moduleId || ""
    property string settingKey: navParams.settingKey || ""

    // 4x4 grid: digits, separators, and the three actions.
    readonly property var glyphs: ["0","1","2","3",
                                  "4","5","6","7",
                                  "8","9",".",",",
                                  "-","OK","WIPE","BACK"]
    property int cell: 0
    property string buffer: ""
    property string notice: ""

    function currentLine() { return weatherBackend.primaryLocationLine() }
    function looksLikeCoords(s) { return weatherBackend.looksLikeCoordinates(s) }

    function actOnCell() {
        const glyph = locationPicker.glyphs[locationPicker.cell]
        if (glyph === "OK")        commit()
        else if (glyph === "WIPE") { locationPicker.buffer = ""; locationPicker.notice = "" }
        else if (glyph === "BACK") goBack()
        else if (locationPicker.buffer.length < 24)
            locationPicker.buffer += glyph
    }

    function commit() {
        if (locationPicker.buffer.length === 0) {
            locationPicker.notice = "NOTHING ENTERED"
            return
        }
        if (!locationPicker.looksLikeCoords(locationPicker.buffer)) {
            locationPicker.notice = "NOT COORDINATES YET"
            return
        }
        appCore.save_setting(locationPicker.moduleId, locationPicker.settingKey,
                             locationPicker.buffer)
        locationPicker.buffer = ""
        goBack()
    }

    // Escape/Backspace leave; Enter acts on the focused cell. Key codes,
    // not characters — onKeyPressed misses control keys. Arrows fall to
    // the handlers below.
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace
                                       || event.key === Qt.Key_Back) {
            goBack()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            actOnCell()
            event.accepted = true
        }
    }
    Keys.onKeyDown:  locationPicker.cell = (locationPicker.cell + 4) % 16
    Keys.onKeyUp:    locationPicker.cell = (locationPicker.cell + 12) % 16
    Keys.onKeyLeft:  locationPicker.cell = Math.max(0, locationPicker.cell - 1)
    Keys.onKeyRight: locationPicker.cell = Math.min(15, locationPicker.cell + 1)

    AppBar {
        iconSource: "../assets/images/logo.svg"
        title: "LOCATION"
        subtitle: locationPicker.currentLine() === "" ? "NO LOCATION SET"
                                                      : locationPicker.currentLine()
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125
        anchors.leftMargin: root.sw * 0.125
    }

    // The dial itself. A core GridView: cells fill the item from its top
    // left, so the cursor rectangle can be positioned from the same
    // metrics and can never drift off the cells.
    GridView {
        id: dial
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.30
        anchors.left: parent.left
        anchors.leftMargin: root.sw * 0.125
        anchors.right: parent.right
        anchors.rightMargin: root.sw * 0.125
        columns: 4
        rows: 4
        model: locationPicker.glyphs.length
        cellWidth: width / columns
        cellHeight: height / rows
        delegate: Text {
            anchors.centerIn: parent
            text: locationPicker.glyphs[index]
            font.pixelSize: root.sh * 0.030
            color: "#ffffff"
        }
    }

    Rectangle {
        x: dial.x + (locationPicker.cell % 4) * dial.cellWidth
        y: dial.y + Math.floor(locationPicker.cell / 4) * dial.cellHeight
        width: dial.cellWidth
        height: dial.cellHeight
        color: "transparent"
        borderColor: "#ffffff"
        border.width: Math.max(1, root.sw * 0.006)
    }

    // The composed spec, live. Nothing takes effect until OK.
    Text {
        text: locationPicker.buffer === "" ? "ENTER COORDINATES" : locationPicker.buffer
        color: "#ffffff"
        font.pixelSize: root.sh * 0.038
        anchors.left: parent.left
        anchors.leftMargin: root.sw * 0.125
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.10
    }

    Text {
        id: noticeText
        text: locationPicker.notice
        color: locationPicker.notice === "" ? "transparent" : "#ffffff"
        font.pixelSize: root.sh * 0.034
        anchors.left: parent.left
        anchors.leftMargin: root.sw * 0.125
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.02
    }

    Timer {
        id: noticeTimer
        running: locationPicker.notice !== ""
        interval: 2000
        onTimeout: locationPicker.notice = ""
    }
}
