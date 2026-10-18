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

    \b Why it is a component. The notation page's Agent panel, the Agent page's centre column and that
    page's right column (its "Conversation" tab, 第 135 条) all show this conversation, and they must show
    the SAME thing: the transcript is derived from the session log (`agentTranscript()`), so two
    hand-written lists would be two projections of one record - free to disagree about what was said. The
    surfaces differ only in how much room they get, and that is the caller's business (it sets
    `Layout.fillHeight` and a minimum), not this file's.

    \b What it deliberately does NOT contain: the composer. Every surface that can send a message composes
    this view with `AgentComposer` beside it, so "can this be sent" is decided in exactly one place
    (第 124 条) - which is also what makes this component safe to show twice at once.

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

    //! Which folded tool groups the user has opened, keyed by the group's `groupId`.
    //! ⛔ WHY THIS LIVES HERE AND NOT IN THE ROW. The transcript is a Q_PROPERTY that is re-evaluated on
    //! every `fieldChanged` - and that fires for every streamed token - so the list model is replaced
    //! repeatedly while the agent works. A row that owned its own `expanded` flag would be rebuilt and
    //! snap shut the moment the model said anything, which is exactly when the user is reading it. The
    //! view outlives the delegates, so the state does too (第 132 条).
    //! ⚠️ Undefined is NOT false: a group that has never been touched opens by DEFAULT when it contains a
    //! failed call (`hasError`), so an error can never be hidden inside a folded row.
    property var openedGroups: ({})

    function isGroupOpen(groupId, hasError) {
        const stored = root.openedGroups[groupId]
        return stored === undefined ? hasError === true : stored
    }

    function toggleGroup(groupId, hasError) {
        const next = Object.assign({}, root.openedGroups)
        next[groupId] = !root.isGroupOpen(groupId, hasError)
        root.openedGroups = next
    }

    //! Set while the view should stay at the tail; see `requestTailFollow()`.
    property bool tailFollowPending: false

    //! Ask for the tail, and KEEP asking while the layout settles.
    //! ⛔ ONE POSITIONING PASS IS NOT ENOUGH, and this is the part that is easy to get wrong: the row
    //! that just arrived has no final height yet, so `positionViewAtEnd()` lands short of the end -
    //! measured 2026-10-11: it stopped about a third of the way into the newest answer even after a
    //! second pass on `contentHeightChanged`. A wrapped 40-line answer is laid out over several passes,
    //! so the request is kept open for a short window (`tailFollowWindow`) during which every height
    //! change re-positions. The window closes by itself, and a user who starts dragging closes it
    //! immediately - following the tail must never fight the reader.
    function requestTailFollow() {
        transcriptView.positionViewAtEnd()
        root.tailFollowPending = true
        tailFollowWindow.restart()
    }

    implicitHeight: content.implicitHeight

    //! The settling window for the tail follow (see `requestTailFollow`). A timer rather than "one more
    //! pass": the number of layout passes a wrapped paragraph needs is not ours to predict, and a
    //! non-terminating `onContentHeightChanged` follower would fight every later change (a group the user
    //! expands, for instance) forever.
    Timer {
        id: tailFollowWindow

        interval: 250
        onTriggered: {
            root.tailFollowPending = false
        }
    }

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

            //! ⛔ TWO KINDS OF ROW ARRIVE HERE NOW (第 132 条): the transcript's own rows, and the folded
            //! runs of tool rows (`toolGroup`, built by `foldToolRows` in C++). The delegate therefore
            //! holds BOTH and shows the one the data asks for.
            //! ⛔ WHY NOT A `Loader` WITH TWO `Component`s. Measured 2026-10-11: an inline `Component` gets
            //! its OWN context, so the delegate's id is not visible inside it - every row logged
            //! `ReferenceError: rowLoader is not defined` and the rows came out blank. (Feeding the data in
            //! from `onLoaded` instead does not work either: `required` properties must be set at
            //! CREATION, so the loaded row logged `Required property kind was not initialized`.) Two
            //! declarations in the delegate, one visible, has neither problem and no runtime dispatch.
            delegate: Item {
                id: rowItem

                required property var modelData

                readonly property bool isToolGroup: modelData.kind === "toolGroup"

                width: transcriptView.width
                implicitHeight: rowItem.isToolGroup ? groupRow.implicitHeight : plainRow.implicitHeight

                AgentTranscriptRow {
                    id: plainRow

                    visible: !rowItem.isToolGroup
                    width: parent.width
                    kind: rowItem.modelData.kind
                    roleLabel: rowItem.modelData.role
                    body: rowItem.modelData.text
                    //! The answer's drawn form, rendered in C++ where the transcript is built (第 150 条).
                    //! `|| ""` and not a bare assignment: the field is absent on every other kind of row,
                    //! and an `undefined` handed to a `string` property is exactly the sort of thing that
                    //! works until it does not.
                    richBody: rowItem.modelData.rich || ""
                    //! A tool call's name, as its own field (第 151 条): the row draws it as a separate
                    //! segment with its own ink, so the split is not re-derived here.
                    toolNameField: rowItem.modelData.name || ""
                    isError: rowItem.modelData.isError === true
                    webStyle: root.webStyle
                }

                //! ⚠️ The open/closed state comes from the VIEW (`openedGroups`), not from this row: the
                //! transcript model is rebuilt on every streamed token, so a row that owned its own flag
                //! would snap shut while the user is reading it.
                AgentToolGroupRow {
                    id: groupRow

                    visible: rowItem.isToolGroup
                    width: parent.width
                    group: rowItem.modelData
                    webStyle: root.webStyle
                    expanded: root.isGroupOpen(rowItem.modelData.groupId, rowItem.modelData.hasError)

                    onToggleRequested: {
                        root.toggleGroup(rowItem.modelData.groupId, rowItem.modelData.hasError)
                    }
                }
            }

            //! ⛔ THE TAIL IS FOLLOWED WHEN THE TURN ENDS TOO, not only while it runs. Measured 2026-10-11:
            //! the final assistant row is appended in the SAME event that clears `running`, so a rule of
            //! "follow while running" never fires for the one row that matters - the answer. Measured again
            //! with six turns of long answers: the view sat on turn 1 while turns 3-6 were below the fold
            //! (`z3-broken-overflow-long.png`), and with the rule below it sits on the newest turn
            //! (`z5-fixed-tail.png`).
            //! ⚠️ `moving` is respected: a reader who is dragging the list is not dragged away from what
            //! they are reading (the same reason `followTailWhileRunning` exists).
            onCountChanged: {
                if (!root.running) {
                    root.requestTailFollow()
                } else if (root.followTailWhileRunning) {
                    positionViewAtEnd()
                }
            }

            //! Every height change inside the settling window re-positions: the newest row's height is
            //! only known after it has been laid out, and the position computed before that is short.
            onContentHeightChanged: {
                if (root.tailFollowPending) {
                    positionViewAtEnd()
                }
            }

            onMovementStarted: {
                root.tailFollowPending = false
                tailFollowWindow.stop()
            }

            Connections {
                target: root

                function onRunningChanged() {
                    if (!root.running && !transcriptView.moving) {
                        root.requestTailFollow()
                    }
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
