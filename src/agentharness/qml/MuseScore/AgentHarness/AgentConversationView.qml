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
