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
    property real noticeUntil: 0

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
            locationPicker.notice = "NOTHING ENTERED"; locationPicker.noticeUntil = Date.now() + 2500
            return
        }
        if (!locationPicker.looksLikeCoords(locationPicker.buffer)) {
            locationPicker.notice = "NOT COORDINATES YET"; locationPicker.noticeUntil = Date.now() + 2500
            return
        }
        appCore.save_setting(locationPicker.moduleId, locationPicker.settingKey,
                             locationPicker.buffer)
        locationPicker.buffer = ""
        goBack()
    }

    Keys.onPressed: function(event) {
        // Arrows come through the same event as all keys: a key-code test
        // rather than per-handler signal (this Qt build's Keys namespace has
        // none of the directional handlers).
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            locationPicker.goBack()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Return) {
            locationPicker.actOnCell()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Down) { locationPicker.move((locationPicker.cell + 4) % 16); event.accepted = true; return }
        if (event.key === Qt.Key_Up) { locationPicker.move((locationPicker.cell + 12) % 16); event.accepted = true; return }
        if (event.key === Qt.Key_Left) { locationPicker.move(Math.max(0, locationPicker.cell - 1)); event.accepted = true; return }
        if (event.key === Qt.Key_Right) { locationPicker.move(Math.min(15, locationPicker.cell + 1)); event.accepted = true; return }
    }

    // The dial itself: a Repeater grid — same primitive the NfcCardWriter's
    // card picker uses (GridView is not in this Qt build). Delegates lay out
    // row-major with wrapping; a quarter-width delegate fixes the grid at 4x4.
    Repeater {
        id: dial
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.30
        anchors.left: parent.left
        anchors.leftMargin: root.sw * 0.125
        anchors.right: parent.right
        anchors.rightMargin: root.sw * 0.125
        model: 16
        delegate: Item {
            width: parent.width / 4
            height: root.sh * 0.09

            Rectangle {
                anchors.fill: parent
                color: index === locationPicker.cell ? root.accentColor : "transparent"
            }
            Text {
                anchors.centerIn: parent
                text: locationPicker.glyphs[index]
                color: index === locationPicker.cell ? root.surfaceColor : "#ffffff"
                font.family: root.globalFont
                font.pixelSize: root.sh * 0.030
            }
        }
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
        visible: Date.now() < locationPicker.noticeUntil
        color: "#ffffff"
        font.pixelSize: root.sh * 0.034
        anchors.left: parent.left
        anchors.leftMargin: root.sw * 0.125
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.02
    }

}
