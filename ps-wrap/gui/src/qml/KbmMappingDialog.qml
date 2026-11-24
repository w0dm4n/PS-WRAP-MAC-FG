import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

// PS-WRAP-MAC-FG: "Keyboard & mouse" — see and change the keyboard + mouse mode bindings while streaming.
// Rows = pad actions grouped like the game uses them; chips = keys / mouse buttons bound to the action.
// Click a chip to remove it, "+" to bind a new key or mouse button (the next key / click is captured).
// Data: Chiaki.window.kbmRows / kbmBind / kbmUnbind / kbmReset (qmlmainwindow_pswrapcontrol.cpp → kbm.json).
Popup {
    id: dialog

    // set by the owner: called with the item to focus when the dialog opens, and when it closes
    property var grabInput: function(item) {}
    property var releaseInput: function() {}

    property string captureAction: ""          // action waiting for a key / click ("" = not capturing)
    property string captureLabel: ""
    property string notice: ""

    readonly property var rows: Chiaki.window ? Chiaki.window.kbmRows : []

    parent: Overlay.overlay
    modal: true
    padding: 0
    width: Math.min(parent ? parent.width - 64 : 900, 1000)
    height: Math.min(parent ? parent.height - 64 : 700, 820)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    closePolicy: captureAction.length ? Popup.NoAutoClose : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)

    onOpened: {
        notice = "";
        captureAction = "";
        dialog.grabInput(closeButton);
    }
    onClosed: {
        captureAction = "";
        dialog.releaseInput();
    }

    function padName(action) {
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].action === action)
                return rows[i].pad;
        return action;
    }

    function startCapture(action, label) {
        notice = "";
        captureLabel = label;
        captureAction = action;
        captureArea.forceActiveFocus();
    }

    function finishCapture(input) {
        const action = captureAction;
        captureAction = "";
        if (!input.length) {
            notice = qsTr("That key can't be used.");
            return;
        }
        const previous = Chiaki.window.kbmActionOf(input);
        Chiaki.window.kbmBind(action, input);
        const label = Chiaki.window.kbmInputLabel(input);
        notice = previous.length && previous !== action
            ? qsTr("%1 → %2 (it was on %3)").arg(label).arg(padName(action)).arg(padName(previous))
            : qsTr("%1 → %2").arg(label).arg(padName(action));
        closeButton.forceActiveFocus();
    }

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radiusCard
        border.width: 1
        border.color: Theme.border
    }

    contentItem: Item {
      ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------------------------------------------------------- header
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.space6
            Layout.bottomMargin: Theme.space3
            spacing: Theme.space4

            ColumnLayout {
                spacing: 2
                Label {
                    text: qsTr("Keyboard & mouse")
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                Label {
                    text: qsTr("Play with WASD and the mouse. ⌘K turns the mode on or off while you play.")
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textMuted
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("Mode")
                font.pixelSize: Theme.fontLabel
                color: Theme.textMuted
            }
            Switch {
                checked: Chiaki.window && Chiaki.window.kbmEnabled
                onToggled: Chiaki.window.kbmEnabled = checked
            }
            Button {
                id: closeButton
                text: qsTr("Done")
                highlighted: true
                onClicked: dialog.close()
            }
        }

        // ---------------------------------------------------------------- mouse tuning
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space6
            Layout.rightMargin: Theme.space6
            spacing: Theme.space4

            Label {
                text: qsTr("Mouse sensitivity")
                font.pixelSize: Theme.fontLabel
                color: Theme.text
            }
            Slider {
                id: sensSlider
                Layout.fillWidth: true
                from: 0.01
                to: 0.40
                stepSize: 0.005
                value: Chiaki.window ? Chiaki.window.kbmSensitivity : 0.05
                onMoved: Chiaki.window.kbmSensitivity = value
            }
            Label {
                text: Math.round(sensSlider.value * 1000) / 10
                font.pixelSize: Theme.fontLabel
                font.weight: Font.DemiBold
                color: Theme.text
                Layout.preferredWidth: 40
            }
            CheckBox {
                text: qsTr("Invert Y")
                checked: Chiaki.window && Chiaki.window.kbmInvertY
                onToggled: Chiaki.window.kbmInvertY = checked
            }
            Button {
                text: qsTr("Reset all")
                flat: true
                onClicked: { Chiaki.window.kbmReset(); dialog.notice = qsTr("Default layout restored."); }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space6
            Layout.rightMargin: Theme.space6
            Layout.topMargin: Theme.space2
            Layout.preferredHeight: Theme.space6
            text: dialog.notice
            color: Theme.accent
            font.pixelSize: Theme.fontLabel
            elide: Text.ElideRight
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        // ---------------------------------------------------------------- bindings
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.rows
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Column {
                id: row
                required property var modelData
                required property int index
                readonly property bool firstOfGroup: index === 0 || dialog.rows[index - 1].group !== modelData.group
                width: list.width

                Label {
                    visible: row.firstOfGroup
                    width: parent.width
                    leftPadding: Theme.space6
                    topPadding: Theme.space4
                    bottomPadding: Theme.space2
                    text: row.modelData.group.toUpperCase()
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.Bold
                    font.letterSpacing: 1.2
                    color: Theme.textMuted
                }

              Rectangle {
                width: parent.width
                height: Math.max(52, chipsFlow.implicitHeight + Theme.space3 * 2)
                color: rowHover.hovered ? Theme.surfaceRaised : "transparent"

                HoverHandler { id: rowHover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space6
                    anchors.rightMargin: Theme.space6
                    spacing: Theme.space4

                    Label {
                        text: row.modelData.pad
                        Layout.preferredWidth: 130
                        font.pixelSize: Theme.fontLabel
                        font.weight: Font.DemiBold
                        color: Theme.text
                        elide: Text.ElideRight
                    }
                    Label {
                        text: row.modelData.use
                        Layout.preferredWidth: 260
                        font.pixelSize: Theme.fontLabel
                        color: Theme.textMuted
                        elide: Text.ElideRight
                    }
                    Flow {
                        id: chipsFlow
                        Layout.fillWidth: true
                        spacing: Theme.space2

                        Repeater {
                            model: row.modelData.inputs
                            delegate: AbstractButton {
                                id: chip
                                required property var modelData
                                implicitHeight: 32
                                implicitWidth: chipText.implicitWidth + 34
                                hoverEnabled: true
                                onClicked: {
                                    Chiaki.window.kbmUnbind(chip.modelData.id);
                                    dialog.notice = qsTr("Removed %1 from %2").arg(chip.modelData.label).arg(row.modelData.pad);
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Click to remove")
                                ToolTip.delay: 500
                                background: Rectangle {
                                    radius: 8
                                    color: chip.hovered ? Qt.rgba(1, 0.36, 0.36, 0.18) : Theme.bg
                                    border.width: 1
                                    border.color: chip.hovered ? Theme.danger : Theme.border
                                }
                                contentItem: Item {
                                    Label {
                                        id: chipText
                                        anchors.left: parent.left
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: chip.modelData.label
                                        font.pixelSize: Theme.fontLabel
                                        font.weight: Font.DemiBold
                                        color: Theme.text
                                    }
                                    Label {
                                        anchors.right: parent.right
                                        anchors.rightMargin: 8
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "×"
                                        font.pixelSize: Theme.fontLabel
                                        color: chip.hovered ? Theme.danger : Theme.textMuted
                                    }
                                }
                            }
                        }

                        AbstractButton {
                            id: addButton
                            implicitHeight: 32
                            implicitWidth: 40
                            hoverEnabled: true
                            onClicked: dialog.startCapture(row.modelData.action, row.modelData.pad)
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Add a key or mouse button")
                            ToolTip.delay: 500
                            background: Rectangle {
                                radius: 8
                                color: addButton.hovered ? Theme.surfaceHover : "transparent"
                                border.width: 1
                                border.color: addButton.hovered ? Theme.accent : Theme.border
                            }
                            contentItem: Label {
                                text: "+"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: Theme.fontBody
                                color: addButton.hovered ? Theme.accent : Theme.textMuted
                            }
                        }
                    }
                }
              }
            }
        }
    }

    // ---------------------------------------------------------------- capture overlay
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusCard
        visible: dialog.captureAction.length > 0
        color: Qt.rgba(0.04, 0.06, 0.08, 0.92)

        Item {
            id: captureArea
            anchors.fill: parent
            focus: dialog.captureAction.length > 0
            Keys.onPressed: (event) => {
                event.accepted = true;
                if (event.isAutoRepeat)
                    return;
                dialog.finishCapture(Chiaki.window.kbmInputForKey(event.key));
            }
            Keys.onShortcutOverride: (event) => event.accepted = true   // even ⌘-combos land here
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: (mouse) => {
                if (cancelButton.contains(mapToItem(cancelButton, mouse.x, mouse.y)))
                    return;
                dialog.finishCapture(Chiaki.window.kbmInputForMouse(mouse.button));
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: Theme.space4
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: dialog.captureLabel
                font.pixelSize: Theme.fontDisplay
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Press a key or click a mouse button")
                font.pixelSize: Theme.fontBody
                color: Theme.textMuted
            }
            Button {
                id: cancelButton
                z: 2
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.space4
                text: qsTr("Cancel")
                onClicked: { dialog.captureAction = ""; closeButton.forceActiveFocus(); }
            }
        }
    }
    }
}
