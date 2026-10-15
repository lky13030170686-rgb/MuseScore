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
import MuseScore.AgentHarness

/*!
    The Agent panel: a side dock on the notation page.

    \b What it is. Two halves, and they are the two halves of the whole subsystem:

      1. **The conversation** - talk to the agent, watch it call tools, see what came back. This is the
         "write" side's face: every tool call here goes through the tool table and lands on the score.
      2. **The information field** - what has happened to this score, newest first, as the undo
         stack's own names. This is the "read" side's face.

    They are in one panel on purpose. The point of the agent is that its edits land in the score the
    user is looking at, and the point of the timeline is to see those edits *as* edits - together they
    answer "what is it doing, and what did it just do".

    \b Why the three pieces are separate files. The Agent page (`AgentPage.qml`) shows the same
    conversation, timeline and composer with more room. A second hand-written list or input row would
    be a second place to get "can this be sent" right, and the symptom would be a Send button that
    looks enabled and quietly does nothing - so both surfaces are built from `AgentConversationView`,
    `AgentTimelineList` and `AgentComposer`, and this file is only the layout that arranges them.

    \b Why the transcript comes from the log. `agentTranscript()` is derived from the session log, the
    same record the model's history is derived from. A panel that kept its own message list would be a
    second truth, and the first symptom of drift would be the user reading a transcript that does not
    match what the agent actually saw.

    \b Accessibility is a build requirement, not a nicety. Controls are real `FlatButton`s with
    `accessible.name`, and the input is a real `TextInputField`. A hand-drawn `Rectangle` +
    `MouseArea` cannot be reached by UI Automation, which would make this panel impossible to verify
    from a script (维护手册.md §7.6 - synthetic mouse input cannot reach a Qt Quick canvas at all).
*/
Item {
    id: root

    property alias navigationSection: navPanel.section
    property alias contentNavigationPanelOrderStart: navPanel.order

    //! The information field, created at window level (see WindowContent.qml) and passed down.
    //! Deliberately NOT created here: `DockPanel` only instantiates its content while the panel is
    //! visible, so a field owned by this panel would begin recording the moment the user opened
    //! the panel - i.e. it would be missing exactly the history they opened it to ask about.
    property var field

    //! Whether the timeline section is open. Starts OPEN: it is the answer to "what just happened to my
    //! score", which is the question a user opens this panel with, and a section that starts collapsed
    //! is one nobody finds.
    property bool timelineExpanded: true
    //! Whether the session list is open. Starts CLOSED - see the note where it is drawn (第 141 条).
    property bool sessionsExpanded: false
    //! How many conversations this process has, for the toggle's label. Read from the field's own list
    //! rather than counted here: the panel does not own sessions and must not look like it does.
    readonly property int sessionCount: root.field ? root.field.agentSessions.length : 0
    //! How many operations the field has recorded, for the toggle's label. Read through the field rather
    //! than `recentOps.length`, because `recentOps` is capped for display while this is the real total.
    readonly property int opCount: root.field ? root.field.eventCount : 0

    NavigationPanel {
        id: navPanel
        name: "AgentHarnessSection"
        direction: NavigationPanel.Vertical
        enabled: root.enabled && root.visible
    }

    //! ⛔ A `ColumnLayout`, not a `Column` with hand-computed heights.
    //! The first version gave the transcript `parent.height - y - inputArea.height`, which looked
    //! right and was not: the streaming label and the error line sit between them, so the input area
    //! was pushed past the bottom of the panel and its buttons were clipped - the panel showed a
    //! conversation and no way to continue it. A layout that allocates the flexible child LAST cannot
    //! get that wrong, and does not need updating when a row is added.
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        //! ── Header: what the field is looking at right now ────────────────────────────────
        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            text: root.field ? root.field.statusText : ""
        }

        //! ── The information field's timeline ──────────────────────────────────────────────
        //! ⛔ THIS WAS LOST when the panel was rebuilt around the conversation, and its absence is not
        //! cosmetic: the transcript is what was SAID, this is what HAPPENED. An edit the user made with
        //! the mouse appears here and nowhere in the conversation, and a refused tool call appears in
        //! the conversation and nowhere here. Without this the panel cannot answer "what just happened
        //! to my score", which is half of what the panel is for.
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            //! A real `FlatButton` with an accessible name, for the same reason as the others: a
            //! hand-drawn toggle cannot be reached by UI Automation, so the panel could not be verified
            //! from a script (维护手册.md §7.6).
            FlatButton {
                id: timelineToggle

                Layout.fillWidth: true
                accessible.name: qsTrc("agentharness", "Show or hide the score timeline")
                text: root.timelineExpanded
                      ? qsTrc("agentharness", "Hide what happened")
                      : qsTrc("agentharness", "What happened (%1)").arg(root.opCount)

                onClicked: {
                    root.timelineExpanded = !root.timelineExpanded
                }
            }

            //! ── Settings, the collapsed-rail shape (第 130 条) ───────────────────────────────
            //! DSH's settings trigger is a full row in the wide sidebar and a 36x36 icon button when the
            //! rail is collapsed. A dock panel is the collapsed case: there is no room for a label, and a
            //! gear at the end of this row is where the eye already is. ⚠️ It opens the SAME dialog the
            //! Agent page opens - the settings are one record, and a panel-only settings screen would be
            //! the second place to keep "what the gateway is" right.
            FlatButton {
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                backgroundRadius: 28
                transparent: true
                icon: IconCode.SETTINGS_COG
                accessible.name: qsTrc("agentharness", "Agent settings")
                toolTipTitle: qsTrc("agentharness", "Settings")

                onClicked: {
                    panelSettingsDialog.open()
                }
            }
        }

        AgentTimelineList {
            Layout.fillWidth: true
            field: root.field
            expanded: root.timelineExpanded
            //! A fixed number of rows rather than filling: in a side panel the timeline is a reference
            //! you glance at, and letting it take the space would push the conversation - the thing you
            //! are actually working in - down to nothing. The page passes 0 and lets it fill.
            visibleRows: 6
        }

        //! ── Which conversation this is (第 141 条) ────────────────────────────────────────
        //! ⛔ COLLAPSED by default, which is the opposite of the timeline's choice, and for the same kind of
        //! reason: the timeline answers the question a user opens this panel with ("what just happened to my
        //! score"), while switching conversations is something you reach for occasionally. Two expanded
        //! sections would leave the conversation - the thing being worked in - with the leftover space.
        FlatButton {
            id: sessionsToggle

            Layout.fillWidth: true
            accessible.name: qsTrc("agentharness", "Show or hide the session list")
            text: root.sessionsExpanded
                  ? qsTrc("agentharness", "Hide sessions")
                  : qsTrc("agentharness", "Sessions (%1)").arg(root.sessionCount)

            onClicked: {
                root.sessionsExpanded = !root.sessionsExpanded
            }
        }

        AgentSessionList {
            Layout.fillWidth: true
            field: root.field
            //! Three rows in a dock panel: the list is a switcher, not a library, and the panel's own
            //! conversation area must stay the tallest thing in it.
            visibleRows: 3
            visible: root.sessionsExpanded
        }

        //! ── The conversation ──────────────────────────────────────────────────────────────
        //! `Layout.fillHeight` with a low minimum: it takes the space nobody else needs, and yields
        //! when the controls below grow.
        AgentConversationView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            field: root.field
        }

        //! ── Input ─────────────────────────────────────────────────────────────────────────
        AgentComposer {
            Layout.fillWidth: true
            field: root.field
        }
    }

    //! The same settings surface the Agent page opens (第 130 条). ⚠️ `layoutResettable` stays false: the
    //! column widths belong to the Agent page, and a dialog that claimed to reset something it cannot
    //! reach would be this project's least favourite kind of lie.
    AgentSettingsDialog {
        id: panelSettingsDialog

        field: root.field
    }
}
