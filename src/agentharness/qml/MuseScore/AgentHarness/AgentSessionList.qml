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

    //! ── The search box (第 163 条) ───────────────────────────────────────────────────────────────
    //! DSH's sidebar search: a 28px round button at rest that expands into a 30px field with a 10px corner
    //! (`ui-workspace/src/client/rows/WorkspaceBrowser.module.css:131-163`), the input at 13px over a
    //! tertiary placeholder (`:195-218`).
    property bool searchOpen: false
    property string filter: ""

    //! What the list draws: the filter applied first, then the row cap.
    //! ⚠️ It matches the NAME (the un-elided one) and case-insensitively - the title is what a user types.
    //! The event count is deliberately not searchable: "17" would match every conversation that happens to
    //! hold 17 events, which is not something anyone searches for.
    //! ⛔ Each row keeps a HANDLE to its own identity (`id`) and to its position in the unfiltered list
    //! (`index`, added in `agentSessions()`), because a delegate's `index` is an index into what is SHOWN -
    //! with a filter on, that number means a different conversation, and the failure is silent.
    readonly property var filteredSessions: {
        const q = root.filter.trim().toLowerCase()
        if (q.length === 0) {
            return root.sessions
        }

        const out = []
        for (let i = 0; i < root.sessions.length; ++i) {
            const s = root.sessions[i]
            if (String(s.name).toLowerCase().indexOf(q) >= 0) {
                out.push(s)
            }
        }
        return out
    }

    //! Nothing matched, and the user is looking at a filter - a distinct state from "there are no
    //! conversations", and the two must not say the same sentence.
    readonly property bool filteredEmpty: root.filter.trim().length > 0 && root.filteredSessions.length === 0

    readonly property int shownCount: root.visibleRows > 0
                                      ? Math.min(root.visibleRows, root.filteredSessions.length)
                                      : root.filteredSessions.length

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

        //! ── The search control (第 163 条) ────────────────────────────────────────────────────────
        //! DSH keeps it at the right end of the group header: a 28px round button while idle, a 30px field
        //! once opened. ⚠️ The button only appears when there is more than one conversation to search -
        //! a search box over a single row is furniture (第 135/141 条's test, applied again).
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            spacing: 4
            visible: root.sessions.length > 1

            Item {
                Layout.fillWidth: true
            }

            FlatButton {
                id: searchToggle

                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                backgroundRadius: 14
                transparent: true
                icon: IconCode.SEARCH
                visible: !root.searchOpen
                accessible.name: qsTrc("agentharness", "Search the conversations")
                toolTipTitle: qsTrc("agentharness", "Search the conversations")

                onClicked: {
                    root.searchOpen = true
                    searchField.ensureActiveFocus()
                }

                //! ⛔ A NOTE FOR WHOEVER VERIFIES THIS (第 164 条): the control that CLOSES the search sits
                //! exactly where this button was, and appears in the same frame that this one hides. A
                //! helper that clicks a control twice (`ui-probe` does - "clicked … 2 time(s)") therefore
                //! opens the search and immediately closes it, and the reading is "clicking does nothing".
                //! Use a single `click:x,y` step, not `uia:`.
            }

            //! ⛔ The field is wrapped in an `Item`, and that is the trap 第 130 条 measured: a
            //! `TextInputField`'s `implicitWidth` is `parent.width`, so putting one straight into a
            //! `RowLayout` makes the layout recurse ("Detected recursive rearrange") and give up.
            Item {
                id: searchSlot

                Layout.fillWidth: true
                Layout.preferredHeight: 30
                visible: root.searchOpen
                //! ⛔ `visible` and not `opacity` HERE, unlike the "Copied" label (第 162 条): this item
                //! must take NO room while the search is closed, and a faded item still claims its 30px.
                //! ⚠️ Measured consequence (第 164 条): the frame it becomes visible it still has `w=0 h=0`
                //! - the layout has not run yet - so it appears one frame late. Harmless, and it is why a
                //! probe that prints geometry the instant `visible` flips will report a zero-size field
                //! that is in fact about to be 110x30.

                TextInputField {
                    id: searchField

                    anchors.fill: parent
                    accessible.name: qsTrc("agentharness", "Filter the conversations")

                    onTextChanged: function(text) {
                        root.filter = text
                    }
                }
            }

            FlatButton {
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                backgroundRadius: 14
                transparent: true
                icon: IconCode.CLOSE_X_ROUNDED
                visible: root.searchOpen
                accessible.name: qsTrc("agentharness", "Close the search")
                toolTipTitle: qsTrc("agentharness", "Close the search")

                onClicked: {
                    root.searchOpen = false
                    root.filter = ""
                    searchField.clear()
                }
            }
        }

        //! What the filter matched nothing on. ⛔ A DIFFERENT SENTENCE FROM "there are no conversations":
        //! one is a state the user made, the other is a fact about the process, and saying the wrong one
        //! sends them looking for the wrong fix.
        StyledTextLabel {
            Layout.fillWidth: true
            visible: root.filteredEmpty
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            color: ui.theme.fontSecondaryColor
            font.pixelSize: ui.theme.bodyFont.pixelSize - 1
            text: qsTrc("agentharness", "No conversation matches “%1”.").arg(root.filter)
        }

        Repeater {
            model: root.filteredSessions.slice(0, root.shownCount)

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                //! ── One row: the fill, the title, and the button that makes it clickable ─────────────
                //! ⛔ THE TITLE IS NOT `FlatButton.text`, and that is a measured decision (both halves
                //! were seen on screen, 2026-10-18). `FlatButton` CENTRES whatever it is given
                //! (`contentLoader.anchors.horizontalCenter`), so the title came out centred like a
                //! button label; and the obvious fix - a custom `contentItem` - is the one shape 第 132 条
                //! proved does NOT work inside a DELEGATE: an inline `Component` gets its own context and
                //! the delegate's ids are invisible in it (every row logged a `ReferenceError` and came
                //! out blank). So: title as a label in the row, and a transparent `FlatButton` on top as
                //! the hit target. One control, still real, still named in the accessibility tree.
                Item {
                    Layout.fillWidth: true
                    //! DSH's session row is 32px (`ui-workspace/src/client/rows/Rows.module.css:103-110`),
                    //! not the 36px of its PANEL navigation rows: those are two different things and the
                    //! first draft of 界面规格.md copied the wrong one. Both are in 界面规格.md §四.
                    Layout.preferredHeight: 32

                    //! The fill. ⛔ The first version used `ui.theme.backgroundTertiaryColor` because it
                    //! sounded like "one step up from the background" - on screen it is BLACK, so the row
                    //! read as a hole in the column (worse than what it replaced). The version before that
                    //! used `FlatButton`'s default `buttonColor`: a solid grey capsule with a hairline
                    //! border, i.e. what an input field looks like. DSH's row fill is `interactive-bg-hover`
                    //! - 8% white in dark, 6% ink in light (`ui-theme/.../design-platform.css:295` / `:200`),
                    //! which is the FOREGROUND at a very low alpha. Painting it that way is what makes it
                    //! right in both themes instead of only in the one I happened to be looking at.
                    //! Hover and selection being the SAME fill is DSH's own choice (Rows.module.css:13-20):
                    //! the fill says "where you just clicked", it is not a highlight.
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: ui.theme.fontPrimaryColor
                        opacity: (modelData.current || sessionRowButton.mouseArea.containsMouse) ? 0.08 : 0
                    }

                    StyledTextLabel {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        //! `label-primary` when it is the row you are in, `label-secondary` otherwise -
                        //! DSH's row ink (Rows.module.css:1-11 puts `label-primary` on the shell; the
                        //! inactive rows read as secondary next to it).
                        color: modelData.current ? ui.theme.fontPrimaryColor : ui.theme.fontSecondaryColor
                        text: modelData.title
                    }

                    FlatButton {
                        id: sessionRowButton

                        anchors.fill: parent
                        //! No background at all - not even the hairline border the default one draws.
                        //! (`backgroundItem` is a `Component`; `null` makes the Loader load nothing.)
                        backgroundItem: null
                        text: ""
                        //! ⛔ NOT `!modelData.current`: see the note at the top of this file - a disabled
                        //! capsule reads as a broken one. The current row stays clickable and the controller
                        //! treats "switch to the session I am in" as the no-op it is.
                        enabled: !root.busy
                        //! ⚠️ `accessible.name` is not derived from `text` (第 129 条): it is what the
                        //! automation tree reports, and it is the only way a script can tell two rows apart.
                        accessible.name: qsTrc("agentharness", "Switch to session: %1").arg(modelData.title)
                        toolTipTitle: modelData.current
                                      ? qsTrc("agentharness", "You are in this session")
                                      : qsTrc("agentharness", "Switch to this session")

                        onClicked: {
                            //! ⛔ `modelData.index`, NOT the delegate's `index` (第 163 条): with a filter on,
                            //! the delegate's index counts what is SHOWN, so clicking the first match of a
                            //! search would switch to the first conversation - silently, because the row
                            //! does change to something.
                            root.field.switchAgentSession(modelData.index)
                        }
                    }
                }

                //! How much is in it. The one fact that separates two conversations whose first messages
                //! look alike, and the reason the row is not just a title.
                //! ⚠️ DSH puts a RELATIVE TIME here, at `12px` / `label-tertiary`
                //! (`ui-workspace/src/client/rows/Rows.module.css:204-209`) - it is secondary
                //! information, not a second title. We keep the event count (第 141 条's reason for
                //! having it at all), but not its size: at the body size it competed with the title.
                StyledTextLabel {
                    horizontalAlignment: Text.AlignRight
                    color: ui.theme.fontSecondaryColor
                    font.pixelSize: ui.theme.bodyFont.pixelSize - 2
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
                        root.beginRename(modelData.index, modelData.name)
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
                        //! Same reason as the row above (第 163 条): this index is handed to
                        //! `deleteAgentSession()` later, and under a filter the delegate's index points at
                        //! a different conversation - which would delete the wrong one.
                        root.confirmingIndex = modelData.index
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
            //! DSH: 38px tall, `padding: 8px 16px`, `border-radius: 12`, a hairline border, an
            //! elevated fill, and a CENTRED label (`ui-sidebar/src/client/SidebarRoot.module.css:250-269`).
            //! It is the sidebar's one prominent action, so it is deliberately the tallest thing in
            //! this column - our 26px transparent capsule read as one more row in the list.
            //! ⚠️ I wrote "left-aligned" in the first draft of 界面规格.md; the CSS says
            //! `justify-content: center`, so `FlatButton`'s default content item is already right
            //! and this needs no custom `contentItem` (which would have brought 第 132 条's
            //! inline-`Component` context problem with it).
            Layout.preferredHeight: 38
            margins: 16
            backgroundRadius: 12
            //! Not transparent any more: DSH fills it with `button-elevated-fill`, which is exactly
            //! `FlatButton`'s default fill (`ui.theme.buttonColor`). The hairline border comes from
            //! the default background item, which is why no border is set here.
            transparent: false
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
