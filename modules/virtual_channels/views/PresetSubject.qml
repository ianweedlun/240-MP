import QtQuick
import Components

// Pick the specific subject value for the recipe chosen on PresetRecipe. The
// values come from the backend: genre/network from TheTVDB, collection from the
// connected media server. Selecting one creates the preset channel (its name is
// derived from the subject) and drops the viewer onto its settings, exactly
// where a manually-added channel lands.
FocusScope {
    id: subjectRoot
    focus: true

    property var navParams: ({})
    readonly property string moduleIcon:
        appCore ? (appCore.get_module_info(moduleId).icon || "") : ""
    property string moduleId: navParams.moduleId || ""
    property string recipe: navParams.recipe || "genre" // genre | network | collection

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    // The integer kind create_preset_channel expects (SubjectKind order:
    // Genre=0, Network=1, Collection=2).
    readonly property int recipeKind:
        subjectRoot.recipe === "network" ? 1
        : subjectRoot.recipe === "collection" ? 2 : 0

    property var rows: []
    property string emptyText: "Loading …"
    property bool busy: true

    Component.onCompleted: {
        var word = subjectRoot.recipe
        subjectRoot.emptyText = "Loading " + word + "s …"
        virtualChannelsBackend.load_catalog(subjectRoot.recipe)
    }

    // The backend delivers the catalog asynchronously (collections ride a
    // media-server browse); only accept the one for this recipe.
    Connections {
        target: virtualChannelsBackend
        function onCatalogReady(kind, optionsList) {
            if (kind !== subjectRoot.recipe) return
            subjectRoot.busy = false
            subjectRoot.rows = optionsList
            subjectRoot.emptyText = optionsList.length === 0
                ? "No " + subjectRoot.recipe + "s available"
                : ""
        }
        function onCatalogFailed(kind, reason) {
            if (kind !== subjectRoot.recipe) return
            subjectRoot.busy = false
            subjectRoot.emptyText = reason
        }
    }

    OptionList {
        id: options
        anchors.fill: parent
        focus: true
        iconSource: subjectRoot.moduleIcon
        title: "Choose " + String(subjectRoot.recipe).charAt(0).toUpperCase()
               + String(subjectRoot.recipe).slice(1)
        rows: subjectRoot.rows
        busy: subjectRoot.busy
        emptyText: subjectRoot.emptyText
        labelFor: function(i) { return String(subjectRoot.rows[i].label) }
        valueFor: function(i) { return "" }
        onActivate: function(i) {
            var id   = String(subjectRoot.rows[i].id)
            var name = String(subjectRoot.rows[i].label)
            var n = virtualChannelsBackend.create_preset_channel(
                        subjectRoot.recipeKind, id, name)
            if (n < 0) {
                subjectRoot.emptyText = "Could not create the channel"
            } else {
                subjectRoot.navigateTo("modules/virtual_channels/views/ChannelSources.qml", {
                    moduleId:      subjectRoot.moduleId,
                    channelNumber: n,
                    channelName:   name
                }, {})
            }
        }
        onBack: function() { subjectRoot.goBack() }
    }
}
