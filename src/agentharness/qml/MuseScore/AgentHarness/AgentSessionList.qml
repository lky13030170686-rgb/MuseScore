/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
import QtQuick
//! ⛔ FOR `Popup` (第 144 条). Forgetting it is not a small mistake: the error is
//! `Popup is not a type`, and because this component is created inside the WINDOW's content, that one line
//! takes the whole notation page down - measured 2026-10-17, the app came up on the home page with no score
//! open, which reads as "the score won't open" rather than "one component failed to load".
import QtQuick.Controls
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

/*!
    The conversations of this process: one row each, and the button that starts another.

    \b Why this component did not exist until 第 141 条. Both `AgentFactsPanel.qml` and
    `AgentWorkbench.qml` used to say, in a comment, that the DSH client's session list was deliberately
    not copied because the harness ran exactly ONE session per process - "a list would be furniture with
    nothing behind it". That was sound reasoning on a premise that could be checked, and the premise is
    now false: `FieldController::newAgentSession()` makes a second conversation, so the list has
    something to list. The shape of the round is 第 135 条's: a claim about what does not exist is a
    claim, and once it stops being true the thing it blocked becomes a feature.

    \b What a row says, and why only that. The label is the FIRST user message, derived from the log
    (`sessionTitleFrom`) - not a name the user has to invent. The number on the right is how many events
    that conversation holds, which is the only other thing that tells two rows apart. Nothing here is
    stored: the live row is read from the live log on every rebuild, so a title cannot describe a
    conversation that has since grown.

    \b Why the current row is still a button. It is filled, like the tab strip's active chip, and clicking it
    re-selects the conversation you are already in - a harmless no-op, which is what every selection list
    does and what the tab strip already does one file over. ⛔ The first version DISABLED it, and the
    screenshot settled the argument: a disabled capsule renders dimmed, so "you are here" came out looking
    like "this session is broken". The fill says where you are; `enabled` is left for the one thing that
    really does make the list unusable - a turn in flight, which owns the live log (see `busy`).

    \b Why the rows are real `FlatButton`s. Same reason as the tab strip and the settings rail: a
    hand-drawn row never reaches the accessibility tree, so `ui-probe -Action click -Name …` cannot reach
    it and a screen reader cannot name it. ⚠️ The dock PANEL's contents are not in that tree either
    (第 90 条) - the Agent page's copy is the one a script can drive, and the panel's is verified by
    coordinate clicks and screenshots.
*/
Item {
    id: root

    property var field

    //! How many rows to draw; 0 means all of them. The Agent page has room for the whole list, the dock
    //! panel does not - and a panel that scrolls past its own composer is worse than a short list.
    property int visibleRows: 0

    readonly property var sessions: root.field ? root.field.agentSessions : []
    //! ⛔ A turn in flight holds the live log: the session must not be swapped under it. The controller
    //! refuses too (`newAgentSession` / `switchAgentSession`), and that refusal is the backstop, not the
    //! plan - a control that is disabled says why before anything is clicked.
    readonly property bool busy: root.field ? root.field.agentRunning : false
    readonly property int shownCount: root.visibleRows > 0
                                      ? Math.min(root.visibleRows, root.sessions.length)
                                      : root.sessions.length

    //! ── Renaming and deleting (第 144 条) ─────────────────────────────────────────────────────────
    //! Which row is being renamed / confirmed (-1 = neither), and what is in the rename box.
    //!
    //! ⛔ THE RENAME BOX IS A POPUP, AND THAT IS A MEASURED DECISION (2026-10-17). The first version put a
    //! `TextInputField` inline in this column and focused it with `ensureActiveFocus()`. The focus arrived
    //! and was gone 700 ms later - a probe (`console.warn` + `Window.activeFocusItem`) named the thief:
    //! `QQuickLoader(…, "Agent_central_content")`, the page shell's central loader, which re-takes focus
    //! after the click that opened the box. An inline field in a PAGE cannot hold focus against the page's
    //! own navigation; a `Popup` owns its focus scope, which is exactly why the settings dialog's fields
    //! work (第 130/131 条 verified typed input into them). So the box lives in a popup, and it is small
    //! enough for the dock panel - the size constraint 第 130 条 measured.
    function beginRename(index, name) {
        root.renamingIndex = index
        root.confirmingIndex = -1
        root.renameText = name
        renameDialog.open()
    }

    function commitRename() {
        if (root.field && root.renamingIndex >= 0) {
            root.field.renameAgentSession(root.renamingIndex, root.renameText)
        }
        root.renamingIndex = -1
        renameDialog.close()
    }

    function cancelRowAction() {
        root.renamingIndex = -1
        root.confirmingIndex = -1
        renameDialog.close()
    }

    property int renamingIndex: -1
    property int confirmingIndex: -1
    property string renameText: ""

    implicitHeight: layout.implicitHeight

    ColumnLayout {
        id: layout

        anchors.fill: parent
        spacing: 2

        Repeater {
            model: root.sessions.slice(0, root.shownCount)

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                FlatButton {
                    id: sessionRow

                    Layout.fillWidth: true
                    Layout.preferredHeight: 26
                    //! The tab strip's proportions (第 135 条), because this is the same kind of thing:
                    //! a capsule whose fill says whether it is the one you are looking at.
                    margins: 10
                    backgroundRadius: 12
                    transparent: !modelData.current
                    //! ⛔ NOT `!modelData.current`: see the note at the top of this file - a disabled capsule
                    //! reads as a broken one. The current row stays clickable and the controller treats
                    //! "switch to the session I am in" as the no-op it is.
                    enabled: !root.busy

                    //! ⚠️ `accessible.name` is not derived from `text` (第 129 条): it is what the
                    //! automation tree reports, and it is the only way a script can tell two rows apart.
                    accessible.name: qsTrc("agentharness", "Switch to session: %1").arg(modelData.title)
                    toolTipTitle: modelData.current
                                  ? qsTrc("agentharness", "You are in this session")
                                  : qsTrc("agentharness", "Switch to this session")
                    text: modelData.title

                    onClicked: {
                        root.field.switchAgentSession(index)
                    }
                }

                //! How much is in it. The one fact that separates two conversations whose first messages
                //! look alike, and the reason the row is not just a title.
                StyledTextLabel {
                    horizontalAlignment: Text.AlignRight
                    color: ui.theme.fontSecondaryColor
                    text: String(modelData.events)
                }

                //! ── Rename (第 144 条) ────────────────────────────────────────────────────────────
                //! Real `FlatButton`s with `accessible.name`, like every other control here: a hand-drawn
                //! glyph cannot be reached by UI Automation, which is what makes this list scriptable.
                FlatButton {
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    backgroundRadius: 12
                    transparent: true
                    icon: IconCode.EDIT
                    enabled: !root.busy
                    accessible.name: qsTrc("agentharness", "Rename session: %1").arg(modelData.title)
                    toolTipTitle: qsTrc("agentharness", "Rename this session")
                    onClicked: {
                        root.beginRename(index, modelData.name)
                    }
                }

                //! ── Delete (第 144 条) ────────────────────────────────────────────────────────────
                //! ⛔ IT ASKS FIRST, inline: the file is removed and a conversation is not recoverable, so a
                //! single misclick must not be enough. Inline rather than a dialog because of the popup trap
                //! above - and because "are you sure" does not need a window.
                FlatButton {
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    backgroundRadius: 12
                    transparent: true
                    icon: IconCode.DELETE_TANK
                    enabled: !root.busy
                    accessible.name: qsTrc("agentharness", "Delete session: %1").arg(modelData.title)
                    toolTipTitle: qsTrc("agentharness", "Delete this session")
                    onClicked: {
                        root.confirmingIndex = index
                        root.renamingIndex = -1
                    }
                }
            }
        }

        //! ── "Are you sure" for a delete, in place (第 144 条) ──────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: root.confirmingIndex >= 0

            StyledTextLabel {
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: ui.theme.fontSecondaryColor
                text: qsTrc("agentharness", "Delete this session?")
            }

            FlatButton {
                Layout.preferredWidth: 72
                Layout.preferredHeight: 24
                backgroundRadius: 12
                text: qsTrc("agentharness", "Delete")
                accessible.name: qsTrc("agentharness", "Confirm deleting this session")
                onClicked: {
                    const which = root.confirmingIndex
                    root.cancelRowAction()
                    root.field.deleteAgentSession(which)
                }
            }

            FlatButton {
                Layout.preferredWidth: 72
                Layout.preferredHeight: 24
                backgroundRadius: 12
                transparent: true
                text: qsTrc("agentharness", "Cancel")
                accessible.name: qsTrc("agentharness", "Cancel deleting")
                onClicked: {
                    root.cancelRowAction()
                }
            }
        }

        FlatButton {
            id: newSessionButton

            Layout.fillWidth: true
            Layout.preferredHeight: 26
            margins: 10
            backgroundRadius: 12
            transparent: true
            icon: IconCode.PLUS
            enabled: !root.busy

            accessible.name: qsTrc("agentharness", "New session")
            toolTipTitle: root.busy
                          ? qsTrc("agentharness", "A turn is running - stop it first")
                          : qsTrc("agentharness", "Start a new conversation")
            text: qsTrc("agentharness", "New session")

            onClicked: {
                root.field.newAgentSession()
            }
        }
    }

    //! ── The rename box (第 144 条) ──────────────────────────────────────────────────────────────────
    //! ⛔ A POPUP, and the comment on `beginRename` says why it is not an inline field: an inline field in a
    //! page cannot hold focus against the page's own navigation (measured - the focus arrived and was gone
    //! 700 ms later, taken by the shell's central loader). A popup owns its focus scope, which is exactly
    //! why the settings dialog's text fields accept typing (第 130/131 条).
    //!
    //! ⚠️ SMALL ON PURPOSE (第 130 条's constraint): a window-sized popup does not survive a text field
    //! taking focus, because `TextInputField` clears the window flag and the native window is rebuilt. This
    //! one is 280x116, and `CloseOnPressOutside` gives it the same "click away to dismiss" gesture as the
    //! settings dialog.
    Popup {
        id: renameDialog

        parent: root
        width: 280
        height: 116
        anchors.centerIn: parent
        padding: 12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: ui.theme.backgroundPrimaryColor
            border.color: ui.theme.strokeColor
            radius: 8
        }

        contentItem: ColumnLayout {
            spacing: 8

            StyledTextLabel {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                font: ui.theme.bodyBoldFont
                text: qsTrc("agentharness", "Rename session")
            }

            //! ⛔ A real `TextInputField` (a hand-drawn box never reaches the automation tree - 第 126/129 条),
            //! and its write-back uses the SIGNAL'S ARGUMENT: the component does not put what you type into
            //! `currentText`, so reading that property inside the handler gives the value from before the
            //! keystroke (第 126 条 - measured again here, and the reason the first version saved an empty
            //! name).
            TextInputField {
                id: renameField

                Layout.fillWidth: true
                accessible.name: qsTrc("agentharness", "New session name")
                onTextChanged: function(newTextValue) {
                    root.renameText = newTextValue
                }
                onAccepted: {
                    root.commitRename()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                FlatButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    backgroundRadius: 12
                    text: qsTrc("agentharness", "Save name")
                    accessible.name: qsTrc("agentharness", "Save the new session name")
                    onClicked: {
                        root.commitRename()
                    }
                }

                FlatButton {
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 24
                    backgroundRadius: 12
                    transparent: true
                    text: qsTrc("agentharness", "Cancel")
                    accessible.name: qsTrc("agentharness", "Cancel renaming")
                    onClicked: {
                        root.cancelRowAction()
                    }
                }
            }
        }

        //! ⛔ BOTH PROPERTIES AND THE INNER FIELD, and this is 第 126 条 twice: once the user has typed in a
        //! `TextInputField` its inner editor no longer follows `currentText`, so assigning only the property
        //! would leave the PREVIOUS name on screen while the value behind it was the new one.
        //! ⚠️ THE UNELIDED NAME (`modelData.name`, not `title`): prefilling with the row's shortened label
        //! would truncate the real name the moment somebody pressed Save.
        onOpened: {
            renameField.currentText = root.renameText
            renameField.inputField.text = root.renameText
            renameField.ensureActiveFocus()
            //! ⛔ SELECTED, so typing REPLACES the old name. Measured 2026-10-17: without this the caret sits
            //! wherever the pointer happens to be, and typing inserts into the middle of the old name - the
            //! first run produced "what is in this scoreRenamed by hand?", which is a rename nobody asked for.
            //! (It is also what a rename box does everywhere else: open, type, done.)
            renameField.selectAll()
        }

        //! Closing by Esc or by clicking away is a CANCEL: leaving `renamingIndex` set would make the next
        //! pencil click edit a row that is no longer the one on screen.
        onClosed: {
            root.renamingIndex = -1
        }
    }
}
