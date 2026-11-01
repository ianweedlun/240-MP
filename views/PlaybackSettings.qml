import QtQuick
import QtQuick.Controls
import "../shared"

// Settings subview — entered from the Application section of Settings.qml
// ("Playback", top of the list, above Controls). Every mpv-facing knob the
// app exposes lives here. The rows are not restated: they are filtered out
// of Settings.qml's own settingsItems by key, so labels/options/values
// keep a single source of truth and conditional rows (1080p Playback,
// gated on mpvController.hasSmoothPlaybackTradeoff) follow the parent
// list automatically. ESC returns through the parent's AppBar contract
// (see RemapControls.qml for the subview pattern).
FocusScope {
    id: playbackSettingsRoot
    propertyDefinitions: [
        // [FOR:BROWSER] property list: settingsList, appSettings, settingsItems
        property settingsList: Qt.Object
        property appSettings: Qt.Object
        property settingsItems: Qt.Array
        // [/FOR:BROWSER]
    ]
    signal navigateTo(string resourceName, object params, object listState)
    signal goBack()
    property object navParams: ({})
    property int navListState: -1

    // settingsItems is built here by filtering the parent's list down to the
    // mpv-facing keys. Order = the order in Settings.qml.
    propertySource: appSettingsLoader.onSettingsItemsLoaded
    onAppSettingsChanged: {
        if (!appSettingsLoader.settingsItems) {
            console.log("Playback settings subview is being given null app settings, resetting")
            appSettingsLoader.reset()
            return
        }
        var all = appSettingsLoader.settingsItems
        var wanted = ["smooth_playback", "auto_fit", "video_output_levels",
            "replaygain", "audio_gain", "deinterlace", "audio_channels"]
        var list = []
        for (var i = 0; i < all.length; i++) {
            var item = all[i]
            if (item && wanted.indexOf(item.key) >= 0)
                list.push(item)
        }
        settingsItems = list
        settingsList.currentIndex = 0
    }

    AppBar {
        anchors.left: root.left
        anchors.right: root.right
        anchors.top: root.top
        anchors.bottom: settingsList.top
        anchors.bottomMargin: 16

        title: "Playback"
        iconSource: "../../assets/images/settings.svg"
    }

    ListView {
        id: settingsList
        property appSettings: playbackSettingsRoot.appSettings
        focusUpdateWeight: 100
        activate()
        focusFunction: FocusedItem.Fallback {
            distanceFunction: function(dist) {
                var d = Math.abs(dist)
                return d <= 1 ? 0 : d
            }
            weightFunction: FocusedItem.Default {
                maximumSpread: 400.0
                minimumWeight: 0.7
            }
        }
        width: 640
        anchors.left: root.left
        anchors.leftMargin: 16
        anchors.right: root.right
        anchors.rightMargin: 16
        anchors.top: playbackSettingsRoot.focusIndicator.bottom
        anchors.topMargin: 16
        anchors.bottom: footer.top
        anchors.bottomMargin: 32
        focusType: "soft"
        activateOnNavigate: true
        spacing: 16
        Keys.onUpPressed: settingsList.step(-1)
        Keys.onDownPressed: settingsList.step(1)
        Keys.onLeftPressed: settingsList.decreaseValue()
        Keys.onRightPressed: settingsList.increaseValue()
        Keys.onPressed: {
            if (event.key === Qt.Escape)
                playbackSettingsRoot.goBack()
        }

        onCurrentIndexChanged: {
            // Guard for a list that can become empty: an index with no item
            // carries no setting to persist.
            var current = settingsList.model[settingsList.currentIndex]
            if (!current)
                return
            if (current.type === "row") {
                if (current.key === "module_order")
                    return
                appSettingsLoader.updateSetting(current.key, current.value)
            }
        }

        delegate: Column {
            id: settingsRow
            property hasVisiblePressed: false
            property bool helpExpanded: settingsList.isFocused(settingsRow) && settingsList.isFocused(rowHelp)
            spacing: 4
            onAppSettingsLoaderUpdate: {
                if (modelData.type === "row") {
                    var options = modelData.options
                    if (!options)
                        options = []
                    var valueIndex = options.indexOf(modelData.value)
                    if (valueIndex < 0)
                        valueIndex = modelData.default === undefined ? 0 : modelData.default
                    var optionCount = options.length
                    row.options = optionCount > 1 ? options : []
                    row.valueIndex = (valueIndex >= 0 && valueIndex < optionCount)
                                      ? valueIndex : 0
                }
            }
            onAppSettingsValueChanged: {
                if (modelData.type === "row") {
                    var options = modelData.options
                    if (!options)
                        options = []
                    var optionCount = options.length
                    if (!options.length)
                        return
                    var valueIndex = options.indexOf(modelData.value)
                    if (valueIndex >= 0 && valueIndex < optionCount)
                        row.valueIndex = valueIndex
                }
            }

            Text {
                text: modelData.type === "row" ? modelData.label : ""
                anchors.left: root.left
                anchors.right: root.right
                anchors.rightMargin: 64
                font.pixelSize: appMetrics.f(22)
                color: Theme.foreground(settingsRow).primary
            }

            ListRow {
                id: row
                visible: modelData.type === "row"
                property object options: []
                property int valueIndex: 0
                property int currentValueIndex: valueIndex
                property bool hasChanged: valueIndex !== currentValueIndex
                // Value assignment is intentionally not animated; only the
                // selection slides between options.
                anchors.left: settingsRow.left
                anchors.right: settingsRow.right
                spacing: appMetrics.f(12)
                property object options: options
                property int activeIndex: valueIndex
                property string label: modelData.label
                property string key: modelData.key
                LayoutBase{
                    id: valueLabel
                    Layout.Text {
                        text: options.length > 0 ? options[activeIndex] : ""
                        font: app.font.body
                        color: Theme.foreground(settingsRow).primary
                        Layout.topMargin: 0
                    }
                    Layout.rightMargin: 4
                }
                TextMetrics {
                    id: valueMetrics
                    text: options.length > 0 ? options[activeIndex] : ""
                    font: app.font.body
                }
                property int requiredWidth: Math.ceil(valueMetrics.width)
                property int requiredHeight: Math.max(valueMetrics.height, appMetrics.f(16))
                minimumWidth: requiredWidth
                minimumHeight: requiredHeight
                Text {
                    text: options.length > 1 ? "←" : ""
                    anchors.left: root.left
                    anchors.leftPadding: 16
                    font.pixelSize: appMetrics.f(16)
                    color: Theme.foreground(settingsRow).muted
                }
                Text {
                    text: options.length > 1 ? "→" : ""
                    anchors.right: root.right
                    anchors.rightPadding: 16
                    font.pixelSize: appMetrics.f(16)
                    color: Theme.foreground(settingsRow).muted
                }
                anchors.top: settingsRow.top
                anchors.topPadding: 20
            }

            Column {
                id: rowHelp
                visible: modelData.type === "row"
                anchors.left: settingsRow.left
                anchors.right: settingsRow.right
                anchors.top: settingsRow.helpIndicator.bottom
                anchors.topPadding: 4
                spacing: 8
                Text {
                    anchors.horizontalCenter: rowHelp.left
                    text: "←→ Select | ESC Return"
                    color: Theme.foreground(settingsRow).muted
                    font.pixelSize: appMetrics.f(12)
                }
                Text {
                    anchors.left: root.left
                    text: (modelData.description !== undefined &&
                            (rowHelp.focusIndex >= 0
                                || settingsList.isFocused(settingsRow))) ?
                              modelData.description : ""
                    color: Theme.foreground(settingsRow).primary
                    font.pixelSize: appMetrics.f(16)
                    wrapBehavior: Text.noWrap
                }
            }

            Row {
                id: rowHelpBackground
                visible: modelData.type === "row" && !settingsList.isFocused(settingsRow)
                spacing: 16
                Text {
                    text: modelData.description !== undefined ?
                              modelData.description : ""
                    color: Theme.foreground(settingsRow).muted
                    font.pixelSize: appMetrics.f(16)
                    wrapBehavior: Text.noWrap
                }
                anchors.left: settingsRow.left
                anchors.right: settingsRow.right
                anchors.top: settingsRow.valueIndicator.bottom
                anchors.topPadding: 8
            }
        }
        property int previousIndex: Math.max(settingsList.previousIndex, 0)
        property int modelCount: settingsItems.length
        model: settingsList.modelCount
        property FocusedItem.SettingsList focusFunction: (settingsList.focusType === "focused" || settingsList.focusType === "soft")
                                    ? FocusedItem.SettingsList.Default {
                                    maximumSpread: 300.0
                                    minimumWeight: 0.7
                                } : (settingsList.focusType === "hard"
                                    ? FocusedItem.SettingsList.Hard {
                                        maximumSpread: 120.0
                                        minimumWeight: 0.7
                                    } : FocusedItem.SettingsList.None { })
        focusType: "soft"
        activateOnNavigate: true
        property bool helpExpanded: settingsList.helpExpanded
        spacing: helpExpanded ? 16 : 8
    }

    Text {
        id: footer
        anchors.left: root.left
        anchors.right: root.right
        anchors.bottom: root.bottom
        color: Theme.foreground(footer).primary
        text: "Press: ESC to Return"
        font: app.font.body
        wrapBehavior: Text.noWrap
    }
}
