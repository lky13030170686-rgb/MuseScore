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

    readonly property bool configured: root.field ? root.field.agentConfigured : false
    readonly property bool running: root.field ? root.field.agentRunning : false
    //! Live streaming text. Held here rather than appended to the transcript: the transcript is the
    //! durable record and is rebuilt only when an event is logged, whereas this changes on every
    //! token. Mixing them would rebuild the whole list per token.
    readonly property string streaming: root.field ? root.field.agentStreamingText : ""
    readonly property string lastError: root.field ? root.field.agentLastError : ""
    readonly property string keySource: root.field ? root.field.agentApiKeySource : "none"
    //! Whether the timeline section is open. Starts OPEN: it is the answer to "what just happened to my
    //! score", which is the question a user opens this panel with, and a section that starts collapsed
    //! is one nobody finds.
    property bool timelineExpanded: true
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
        }

        StyledListView {
            id: timelineView

            Layout.fillWidth: true
            //! Expanded shows a fixed number of rows rather than filling: the timeline is a reference
            //! you glance at, and letting it take the panel would push the conversation - the thing you
            //! are actually working in - down to nothing.
            Layout.preferredHeight: root.timelineExpanded ? Math.min(6, Math.max(1, root.opCount)) * 30 : 0
            visible: root.timelineExpanded
            clip: true

            model: root.field ? root.field.recentOps : []
            spacing: 0

            delegate: AgentTimelineRow {
                required property var modelData

                width: timelineView.width
                line: modelData.line
                isUndo: modelData.isUndo === true
                isRedo: modelData.isRedo === true
            }
        }

        //! ── The conversation ──────────────────────────────────────────────────────────────
        //! `Layout.fillHeight` with a low minimum: it takes the space nobody else needs, and yields
        //! when the controls below grow.
        StyledListView {
            id: transcriptView

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60

            model: root.field ? root.field.agentTranscript : []
            spacing: 6
            clip: true

            delegate: AgentTranscriptRow {
                required property var modelData

                width: transcriptView.width
                kind: modelData.kind
                roleLabel: modelData.role
                body: modelData.text
                isError: modelData.isError === true
            }

            //! Follow the tail while the agent is working. Only while running: scrolling a finished
            //! transcript out from under a reader who is scrolling it is worse than not following.
            onCountChanged: {
                if (root.running) {
                    positionViewAtEnd()
                }
            }
        }

        //! ── Live stream, so the panel is not silent while the model is thinking ───────────
        StyledTextLabel {
            id: streamingLabel

            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            visible: root.streaming.length > 0 || root.running
            opacity: 0.8
            //! Capped: this is a liveness indicator, and an unbounded label would grow the panel's
            //! fixed content until the transcript had nothing left.
            text: root.streaming.length > 0
                  ? (root.streaming.length > 300 ? "…" + root.streaming.slice(-300) : root.streaming)
                  : qsTrc("agentharness", "thinking…")
        }

        //! ── Errors are shown, not swallowed ───────────────────────────────────────────────
        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            visible: root.lastError.length > 0
            color: ui.theme.buttonColor
            text: root.lastError
        }

        //! ── Input ─────────────────────────────────────────────────────────────────────────
        ColumnLayout {
            id: inputArea

            Layout.fillWidth: true
            spacing: 4

            TextInputField {
                id: promptField

                Layout.fillWidth: true
                //! The accessible name is what a screen reader announces and what a UI-automation
                //! script looks the field up by; without it the panel cannot be driven at all.
                accessible.name: qsTrc("agentharness", "Message to the agent")
                hint: root.configured
                      ? qsTrc("agentharness", "Ask about this score, or tell it what to change")
                      : qsTrc("agentharness", "Set an API key first")
                enabled: root.configured && !root.running

                onAccepted: {
                    root.submit()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                //! Real `FlatButton`s, not drawn ones: these are the controls a verification script
                //! has to be able to press, and a `Rectangle` + `MouseArea` is unreachable
                //! (维护手册.md §7.6 - synthetic mouse input cannot reach a Qt Quick canvas at all).
                FlatButton {
                    id: sendButton

                    Layout.fillWidth: true
                    accessible.name: qsTrc("agentharness", "Send to agent")
                    text: qsTrc("agentharness", "Send")
                    enabled: root.configured && !root.running && promptField.currentText.length > 0

                    onClicked: {
                        root.submit()
                    }
                }

                FlatButton {
                    id: keyButton

                    accessible.name: qsTrc("agentharness", "Set API key")
                    //! The source is shown because "it works on my machine" is usually an environment
                    //! variable someone forgot they set - and a key that came from the environment
                    //! cannot be cleared from here.
                    text: root.keySource === "none"
                          ? qsTrc("agentharness", "Set key")
                          : qsTrc("agentharness", "Key: %1").arg(root.keySource)
                    enabled: !root.running

                    onClicked: {
                        keyField.visible = !keyField.visible
                    }
                }
            }

            //! ── Key entry, revealed on demand ─────────────────────────────────────────────
            //! Hidden by default so the panel does not look like it is asking for a secret the user
            //! has already provided.
            TextInputField {
                id: keyField

                Layout.fillWidth: true
                visible: false
                accessible.name: qsTrc("agentharness", "API key")
                hint: qsTrc("agentharness", "Paste an API key (kept in memory for this session)")

                onAccepted: {
                    if (root.field && keyField.currentText.length > 0) {
                        root.field.setAgentApiKey(keyField.currentText)
                        keyField.currentText = ""
                        keyField.visible = false
                    }
                }
            }
        }
    }

    //! One place that decides "can this be sent", so the button's enabled state and the Enter key
    //! cannot disagree about it.
    function submit() {
        if (!root.field || !root.configured || root.running) {
            return
        }

        const text = promptField.currentText
        if (text.length === 0) {
            return
        }

        //! Cleared BEFORE sending: if the turn fails immediately the text is gone, which is the honest
        //! outcome - it was sent, and the transcript shows what happened to it.
        promptField.currentText = ""
        root.field.sendToAgent(text)
    }
}
