import QtQuick
import Components

// Pick the preset recipe: the kind of category a preset channel is built from.
// The choice here decides what the next screen enumerates. FocusScope leaf —
// reaches PresetSubject on select, back to Manage on escape.
FocusScope {
    id: recipeRoot
    focus: true

    property var navParams: ({})
    readonly property string moduleIcon:
        appCore ? (appCore.get_module_info(moduleId).icon || "") : ""
    property string moduleId: navParams.moduleId || ""

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    // The three recipes, in the order the plan locks them. `value` is the
    // string the backend's load_catalog / create_preset_channel keys on.
    readonly property var recipes: [
        { value: "genre",      label: "By Genre" },
        { value: "network",    label: "By Network" },
        { value: "collection", label: "By Collection" }
    ]

    readonly property string blurb:
        "A preset builds a whole channel from a category — a network, a genre, or a "
        + "collection — instead of one folder or series. Pick the category, then the "
        + "specific one."

    OptionList {
        id: options
        anchors.fill: parent
        focus: true
        iconSource: recipeRoot.moduleIcon
        title: "Add Preset Channel"
        rows: recipeRoot.recipes
        labelFor: function(i) { return recipeRoot.recipes[i].label }
        valueFor: function(i) { return "" }
        helpFor: function(i) { return recipeRoot.blurb }
        emptyText: recipeRoot.blurb
        onActivate: function(i) {
            recipeRoot.navigateTo("modules/virtual_channels/views/PresetSubject.qml", {
                moduleId: recipeRoot.moduleId,
                recipe: recipeRoot.recipes[i].value
            }, {})
        }
        onBack: function() { recipeRoot.goBack() }
    }
}
