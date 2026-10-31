import QtQuick 6.10

// Core list primitive: a ListView that tracks its own highlighted row.
// Views use THIS instead of bare ListView; views/modules never declare or
// emit selection signals themselves (project extendability philosophy:
// telemetry lives in core primitives, views stay plain).
//
// The component self-logs the row change to the state log (C++ note_state is
// a no-op when built with TELEMETRY=OFF). It also emits rowSelected(index)
// so an instance may listen for its own purpose (e.g. Games.qml drives its
// preview pane from it) — instances read the row themselves via model[index].
// Instances must NOT override onCurrentIndexChanged: that would replace this
// component's telemetry handler.
ListView {
    id: rowTrackedList

    signal rowSelected(index: int)

    function displayName(index) {
        if (!model)
            return ""
        var row = model[index]
        if (!row || typeof row !== "object")
            return ""
        return row.name || row.label || row.text || row.title || ""
    }

    onCurrentIndexChanged: {
        if (currentIndex < 0)
            return
        rowSelected(currentIndex)
        var name = displayName(currentIndex)
        if (name === "")
            name = "(row " + currentIndex + ")"
        appCore.note_state("select", name + " idx=" + currentIndex)
    }
}
