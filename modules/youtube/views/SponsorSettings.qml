import QtQuick
import Components

// SponsorBlock per-category controls, opened from the module Settings
// "SponsorBlock" submenu row (issue #28: one submenu row so the per-category
// labels here can be short). Values auto-save on LEFT/RIGHT cycle — same
// model as the app-level Settings rows, no SELECT needed.
//
// Runtime defaults (SPONSOR + SELF PROMO skip silently until the user says
// otherwise) live in both places deliberately: displayed here and applied in
// Player.qml, so the screen never lies about what playback is doing.
FocusScope {
    id: sponsorRoot

    signal goBack()

    focus: true

    // Loader contract: Main.qml injects navParams via setSource's sourceProperties
    // — the file-scoped declaration is mandatory (same as SignIn/every sibling view).
    property var navParams: ({})

    property string moduleId: navParams.moduleId || "com.240mp.youtube"

    // File-scope module-info snapshot (SignIn.qml pattern): context props like
    // appCore are not visible inside imported-component bindings (AppBar{}),
    // so resolve once here and hand the value in.
    property var _selfInfo: appCore ? appCore.get_module_info(moduleId) : ({})

    property var cats: [
        { key: "sponsor_sponsor",      label: "SPONSOR",      def: "Skip" },
        { key: "sponsor_selfpromo",    label: "SELF PROMO",   def: "Skip" },
        { key: "sponsor_intro",        label: "INTRO",        def: "Off"  },
        { key: "sponsor_outro",        label: "OUTRO",        def: "Off"  },
        { key: "sponsor_intermission", label: "INTERMISSION", def: "Off"  },
        { key: "sponsor_preview",      label: "PREVIEW",      def: "Off"  },
        { key: "sponsor_filler",       label: "FILLER",       def: "Off"  },
        { key: "sponsor_interaction",  label: "INTERACTION",  def: "Off"  }
    ]
    // Current value per row, index-aligned with cats (re-assigned as a whole
    // array on every cycle so the delegate's text binding re-evaluates).
    property var rowVals: []

    Component.onCompleted: {
        var vals = []
        for (var i = 0; i < cats.length; ++i)
            vals.push(appCore.get_setting(moduleId, cats[i].key) || cats[i].def)
        rowVals = vals
    }

    function cycle(row, dir) {
        if (row < 0 || row >= cats.length)
            return
        var opts = ["Off", "Mark", "Skip"]
        var cur = rowVals[row] || cats[row].def
        var p = opts.indexOf(cur)
        if (p < 0)
            p = 0
        p = (p + dir + opts.length) % opts.length
        var next = opts[p]
        var arr = rowVals.slice()
        arr[row] = next
        rowVals = arr
        appCore.save_setting(moduleId, cats[row].key, next)
    }

    AppBar {
        iconSource: _selfInfo.icon
        title: "YOUTUBE"
        subtitle: "SPONSORBLOCK"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125
        anchors.leftMargin: root.sw * 0.125
    }

    ListView {
        id: catList
        model: sponsorRoot.cats
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.30
        anchors.leftMargin: root.sw * 0.125
        width: root.sw * 0.75
        height: root.sh * 0.48
        clip: true
        focus: true

        Keys.onUpPressed: {
            if (count === 0) return
            if (currentIndex > 0) currentIndex--
            else currentIndex = count - 1
            catList.positionViewAtIndex(catList.currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            catList.positionViewAtIndex(catList.currentIndex, ListView.Contain)
        }
        Keys.onLeftPressed: cycle(catList.currentIndex, -1)
        Keys.onRightPressed: cycle(catList.currentIndex, 1)

        delegate: Item {
            width: catList.width
            height: root.sh * 0.0583333

            Rectangle {
                anchors.fill: parent
                color: catList.currentIndex === index ? root.accentColor : "transparent"

                Text {
                    text: modelData.label + ": " + (sponsorRoot.rowVals[index] || modelData.def)
                    color: catList.currentIndex === index ? root.surfaceColor : root.primaryColor
                    font.family: root.globalFont
                    font.capitalization: Font.AllUppercase
                    anchors.verticalCenter: parent.verticalCenter
                    x: 0
                    topPadding: root.sh * 0.0041667
                    leftPadding: root.sw * 0.009375
                    rightPadding: root.sw * 0.009375
                    bottomPadding: root.sh * 0.00625
                    font.pixelSize: root.sh * 0.05
                }
            }
        }
    }

    Text {
        id: footer
        text: root.hints.back + ":BACK " + (root.hints.left || "LEFT") + "/" + (root.hints.right || "RIGHT") + ":SET"
        color: root.tertiaryColor
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.1041667
        anchors.leftMargin: root.sw * 0.125
        anchors.left: parent.left
        font.family: root.globalFont
        font.pixelSize: root.sh * 0.0333333
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            goBack()
            event.accepted = true
        }
    }
}
