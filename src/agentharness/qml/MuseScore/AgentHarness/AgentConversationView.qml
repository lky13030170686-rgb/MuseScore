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
    The agent conversation: the transcript, the live stream, and the last error.

    \b Why it is a component. The notation page's Agent panel and the Agent page both show this
    conversation, and they must show the SAME thing: the transcript is derived from the session log
    (`agentTranscript()`), so two hand-written lists would be two projections of one record - free to
    disagree about what was said. The two surfaces differ only in how much room they get, and that is
    the caller's business (it sets `Layout.fillHeight` and a minimum), not this file's.

    \b Why the stream and the error live in here too. They sit between the list and the input, in that
    order, on both surfaces: the stream is what the agent is saying right now, the error is why it
    stopped. Keeping all three in one component means a caller cannot forget one and ship a
    conversation that goes silent while the model is thinking.

    \b Why the transcript comes from the log. See AgentPanel.qml: one record, two projections.
*/
Item {
    id: root

    //! The information field (created at window level - see WindowContent.qml).
    property var field

    //! Follow the tail while the agent works. Only while running: scrolling a finished transcript out
    //! from under a reader who is scrolling it is worse than not following.
    property bool followTailWhileRunning: true

    //! Panel style (labelled paragraphs) or web style (bubbles and one-liners) - see AgentTranscriptRow.
    property bool webStyle: false

    readonly property bool running: root.field ? root.field.agentRunning : false
    //! Live streaming text. Held out of the transcript model: the transcript is the durable record and
    //! is rebuilt only when an event is logged, whereas this changes on every token. Mixing them would
    //! rebuild the whole list per token.
    readonly property string streaming: root.field ? root.field.agentStreamingText : ""
    readonly property string lastError: root.field ? root.field.agentLastError : ""
    //! The model's own reasoning stream. `FieldController` has collected it since the first day and
    //! NOTHING displayed it: a turn that spends ten seconds thinking looked like a hang while the
    //! explanation sat in a property nobody read (measured 2026-10-10). Collapsed by default - it is
    //! long, and it is not the answer - but one click away, and the label says how much there is.
    readonly property string reasoning: root.field ? root.field.agentStreamingReasoning : ""
    property bool reasoningExpanded: false

    implicitHeight: content.implicitHeight

    ColumnLayout {
        id: content

        anchors.fill: parent
        spacing: 6

        StyledListView {
            id: transcriptView

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60

            spacing: 6
            clip: true

            model: root.field ? root.field.agentTranscript : []

            delegate: AgentTranscriptRow {
                required property var modelData

                width: transcriptView.width
                kind: modelData.kind
                roleLabel: modelData.role
                body: modelData.text
                isError: modelData.isError === true
                webStyle: root.webStyle
            }

            onCountChanged: {
                if (root.followTailWhileRunning && root.running) {
                    positionViewAtEnd()
                }
            }
        }

        //! ── The model's reasoning, one click away ─────────────────────────────────────────────
        //! It sits ABOVE the answer because that is the order it happens in. A real `FlatButton` with an
        //! accessible name, like every other control here: a hand-drawn toggle cannot be reached by UI
        //! Automation, so the panel could not be verified from a script (维护手册.md §7.6).
        FlatButton {
            id: reasoningToggle

            Layout.fillWidth: true
            visible: root.reasoning.length > 0
            accessible.name: qsTrc("agentharness", "Show or hide the model's reasoning")
            opacity: 0.9
            //! The character count is the liveness signal while a turn runs: the reasoning grows first and
            //! the answer comes after, so a counter that keeps moving is what tells the user "it is
            //! working, not stuck".
            text: root.reasoningExpanded
                  ? qsTrc("agentharness", "Hide thinking (%1)").arg(root.reasoning.length)
                  : qsTrc("agentharness", "Thinking… (%1)").arg(root.reasoning.length)

            onClicked: {
                root.reasoningExpanded = !root.reasoningExpanded
            }
        }

        StyledTextLabel {
            id: reasoningLabel

            Layout.fillWidth: true
            //! Capped and clipped: reasoning is normally several times longer than the answer, and letting
            //! it take the height would push the conversation (and the input) out of the panel.
            Layout.preferredHeight: root.reasoningExpanded ? Math.min(160, reasoningLabel.implicitHeight) : 0
            visible: root.reasoningExpanded
            clip: true
            opacity: 0.75
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignTop
            wrapMode: Text.WordWrap
            text: root.reasoning
        }

        //! ── Live stream, so the surface is not silent while the model is thinking ─────────────
        StyledTextLabel {
            id: streamingLabel

            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            visible: root.streaming.length > 0 || root.running
            opacity: 0.8
            //! Capped: this is a liveness indicator, and an unbounded label would grow the fixed
            //! content until the transcript had nothing left.
            text: root.streaming.length > 0
                  ? (root.streaming.length > 300 ? "…" + root.streaming.slice(-300) : root.streaming)
                  : qsTrc("agentharness", "thinking…")
        }

        //! ── Errors are shown, not swallowed ──────────────────────────────────────────────────
        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            visible: root.lastError.length > 0
            color: ui.theme.buttonColor
            text: root.lastError
        }
    }
}
