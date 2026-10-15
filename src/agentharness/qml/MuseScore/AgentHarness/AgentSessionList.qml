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
}
