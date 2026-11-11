import QtQuick
import Components

// Pick the specific subject value for the recipe chosen on PresetRecipe. The
// values come from the backend: genre/network from TheTVDB, collection from the
// connected media server. Selecting one creates the preset channel (its name is
// derived from the subject), and the channel seeds itself with the content that
// matches — so the viewer lands on the builder with everything already picked,
// and the work in front of them is removing what they don't want. A recipe the
// library cannot fill creates nothing: the viewer stays here with a line saying
// so, instead of an empty channel parked on the dial.
FocusScope {
    id: subjectRoot
    focus: true

    property var navParams: ({})
    readonly property string moduleIcon:
        appCore ? (appCore.get_module_info(moduleId).icon || "") : ""
    property string moduleId: navParams.moduleId || ""
    property string recipe: navParams.recipe || "genre" // genre | network | collection
    // Whether the channel airs shows or films, asked one screen earlier so the
    // recipe's content is looked for among the right kind from the start.
    property string wants:  navParams.wants  || "tv"     // tv | movies

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    // The integer kind create_preset_channel expects (SubjectKind order:
    // Genre=0, Network=1, Collection=2). A movie "network" is a STUDIO — same
    // kind integer, mode-decided interpretation — but its catalog comes from a
    // different source, so the catalog ask below keys on "studio".
    readonly property int recipeKind:
        subjectRoot.recipe === "network" ? 1
        : subjectRoot.recipe === "collection" ? 2 : 0
    readonly property bool studioMovie:
        subjectRoot.recipe === "network" && subjectRoot.wants === "movies"
    readonly property string catalogKind: studioMovie ? "studio" : recipe
    readonly property string subjectWord:
        studioMovie ? "Studio"
        : recipe.charAt(0).toUpperCase() + recipe.slice(1)

    property var rows: []
    property string emptyText: "Loading …"
    property bool busy: true
    // The channel exists but its content is still being looked for — the
    // enumeration is server work, so the answer comes on its own signal.
    property bool seeding: false
    property int seedChannel: -1
    property string seedName: ""
    property string status: ""

    Component.onCompleted: {
        subjectRoot.emptyText = "Loading " + subjectWord.toLowerCase() + "s …"
        virtualChannelsBackend.load_catalog(subjectRoot.catalogKind)
    }

    // The backend delivers the catalog asynchronously (collections ride a
    // media-server browse); only accept the one for this recipe.
    Connections {
        target: virtualChannelsBackend
        function onCatalogReady(kind, optionsList) {
            if (kind !== subjectRoot.catalogKind) return
            subjectRoot.busy = false
            console.log("[presetflow] catalog ready:", subjectRoot.catalogKind,
                        "rows:", optionsList.length)
            subjectRoot.rows = optionsList
            subjectRoot.emptyText = optionsList.length === 0
                ? "No " + subjectRoot.subjectWord + "s available"
                : ""
        }
        function onCatalogFailed(kind, reason) {
            if (kind !== subjectRoot.catalogKind) return
            subjectRoot.busy = false
            subjectRoot.emptyText = reason
        }
        function onPresetSeeded(ch, ok, message) {
            if (ch !== subjectRoot.seedChannel) return
            subjectRoot.seeding = false
            if (ok) {
                subjectRoot.navigateTo("ChannelSources.qml", {
                    moduleId:      subjectRoot.moduleId,
                    channelNumber: subjectRoot.seedChannel,
                    channelName:   subjectRoot.seedName
                }, {})
            } else {
                // Nothing filled the channel, so nothing goes on the dial.
                console.log("[presetflow] seed refused for channel", ch, "-", message)
                virtualChannelsBackend.delete_channel(subjectRoot.seedChannel)
                subjectRoot.seedChannel = -1
                subjectRoot.status = message
                subjectRoot.busy = false
                subjectRoot.emptyText = ""
            }
        }
    }

    OptionList {
        id: options
        anchors.fill: parent
        focus: true
        iconSource: subjectRoot.moduleIcon
        title: "Choose " + subjectRoot.subjectWord
        rows: subjectRoot.rows
        busy: subjectRoot.busy || subjectRoot.seeding
        status: subjectRoot.status
        emptyText: subjectRoot.emptyText
        labelFor: function(i) { return String(subjectRoot.rows[i].label) }
        valueFor: function(i) { return "" }
        onActivate: function(i) {
            if (subjectRoot.seeding) return
            var id   = String(subjectRoot.rows[i].id)
            var name = String(subjectRoot.rows[i].label)
            var n = virtualChannelsBackend.create_preset_channel(
                        subjectRoot.recipeKind, id, name, subjectRoot.wants)
            if (n < 0) {
                subjectRoot.emptyText = "Could not create the channel"
            } else {
                console.log("[presetflow] subject activated:", subjectRoot.catalogKind,
                            id, name, "-> channel", n)
                subjectRoot.seedChannel = n
                subjectRoot.seedName = name
                subjectRoot.seeding = true
                subjectRoot.status = "Filling " + name + "…"
                virtualChannelsBackend.seed_preset_channel(n)
            }
        }
        onBack: function() { subjectRoot.goBack() }
    }
}
