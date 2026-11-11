import QtQuick
import Components

// Pick what the preset channel is: first whether it airs shows or films, then
// the category it is built from. The kind ask comes first because it decides
// what the recipe's content is looked for among — the same question the
// builder asks further down, answered here so the channel is born knowing.
// FocusScope leaf — reaches PresetSubject on select, back to Manage on escape.
FocusScope {
    id: recipeRoot
    focus: true

    property var navParams: ({})
    readonly property string moduleIcon:
        appCore ? (appCore.get_module_info(moduleId).icon || "") : ""
    property string moduleId: navParams.moduleId || ""
    // "" until the viewer has chosen shows or films; the row list is what
    // tells them which question they are on.
    property string wants: navParams.wants || ""

    signal navigateTo(string path, var params, var listState)
    signal goBack()

    readonly property var kinds: [
        { value: "tv",     label: "TV Shows" },
        { value: "movies", label: "Movies" }
    ]
    // The three recipes, in the order the plan locks them. `value` is what
    // the backend's load_catalog / create_preset_channel keys on.
    readonly property var recipes: [
        { value: "genre",      label: "By Genre" },
        { value: "network",    label: "By Network" },
        { value: "collection", label: "By Collection" }
    ]

    readonly property string kindBlurb:
        "First say what the channel airs — shows or films. The category then "
        + "fills it with everything of that kind the library has."
    readonly property string blurb:
        "A preset builds a whole channel from a category — a network, a genre, or a "
        + "collection — instead of one folder or series. Pick the category, then the "
        + "specific one."

    readonly property bool askingKind: wants === ""
    readonly property var rows: {
        var r = []
        if (askingKind) {
            for (var i = 0; i < kinds.length; i++) r.push("kind:" + kinds[i].value)
        } else {
            for (var j = 0; j < recipes.length; j++) r.push("recipe:" + recipes[j].value)
        }
        return r
    }

    function labelFor(i) {
        var row = rows[i]
        if (row === undefined) return ""
        if (row.indexOf("kind:") === 0) {
            var v = row.substring(5)
            for (var k = 0; k < kinds.length; k++)
                if (kinds[k].value === v) return kinds[k].label
            return ""
        }
        var w = row.substring(7)
        for (var j = 0; j < recipes.length; j++)
            if (recipes[j].value === w) return recipes[j].label
        return ""
    }

    OptionList {
        id: options
        anchors.fill: parent
        focus: true
        iconSource: recipeRoot.moduleIcon
        title: recipeRoot.askingKind ? "What Should It Air" : "Add Preset Channel"
        rows: recipeRoot.rows
        labelFor: function(i) { return recipeRoot.labelFor(i) }
        valueFor: function(i) { return "" }
        helpFor: function(i) { return recipeRoot.askingKind
                                   ? recipeRoot.kindBlurb : recipeRoot.blurb }
        emptyText: recipeRoot.askingKind ? recipeRoot.kindBlurb : recipeRoot.blurb
        onActivate: function(i) {
            var row = recipeRoot.rows[i]
            if (row.indexOf("kind:") === 0) {
                // Same screen, one question down: the kind rides in as a nav
                // param, so the reloaded view knows what it is already on.
                recipeRoot.navigateTo("modules/virtual_channels/views/PresetRecipe.qml", {
                    moduleId: recipeRoot.moduleId,
                    wants: row.substring(5)
                }, {})
                return
            }
            recipeRoot.navigateTo("modules/virtual_channels/views/PresetSubject.qml", {
                moduleId: recipeRoot.moduleId,
                wants: recipeRoot.wants,
                recipe: row.substring(7)
            }, {})
        }
        onBack: function() { recipeRoot.goBack() }
    }
}
