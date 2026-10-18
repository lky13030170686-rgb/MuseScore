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
    //! The same stream in its DRAWN form (第 158 条), rendered in C++ once - see the property's note in
    //! `fieldcontroller.h`. The arriving answer goes through the same renderer as the finished one, so a
    //! half-written `**bold**` does not show its markers for a few hundred milliseconds and then snap.
    readonly property string streamingRich: root.field ? root.field.agentStreamingRich : ""
    readonly property string lastError: root.field ? root.field.agentLastError : ""
    //! The model's own reasoning stream. `FieldController` has collected it since the first day and
    //! NOTHING displayed it: a turn that spends ten seconds thinking looked like a hang while the
    //! explanation sat in a property nobody read (measured 2026-10-10). Collapsed by default - it is
    //! long, and it is not the answer - but one click away, and the label says how much there is.
    readonly property string reasoning: root.field ? root.field.agentStreamingReasoning : ""
    property bool reasoningExpanded: false

    //! ── The reasoning row's one-line summary (第 156 条) ─────────────────────────────────────────
    //! ⛔ DSH takes the LAST line while the model is still thinking and the FIRST line once it has
    //! stopped (`ReasoningRow.tsx:8-17`), and that is not a detail: while streaming, the last line is
    //! what is happening NOW, so the row doubles as the liveness signal the character count used to be;
    //! afterwards the first line is the one that says what the thinking was about.
    //! ⚠️ Empty lines are skipped - the stream arrives in deltas and a trailing newline would otherwise
    //! make the summary blink to nothing.
    readonly property string reasoningSummary: {
        const text = String(root.reasoning)
        if (text.length === 0) {
            return ""
        }

        const lines = text.split("\n").filter(function(line) {
            return line.trim().length > 0
        })
        if (lines.length === 0) {
            return ""
        }

        return root.running ? lines[lines.length - 1].trim() : lines[0].trim()
    }

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

    //! ── Which tool RESULTS are shown in full (第 155 条) ─────────────────────────────────────────
    //! Keyed by the row's own `seq`, and held HERE for exactly the reason `openedGroups` is: the
    //! transcript model is a `Q_PROPERTY` that is rebuilt on every streamed token, so a row that owned
    //! its own flag would snap shut while the user is reading it (第 132 条).
    //! ⚠️ Why a result is closed by default while a FAILED tool group is open: a group line is one row
    //! standing for many, so opening it is the only way to see what happened; a result line is already
    //! the answer, and its first line is what the row shows. Opening is for "I need the whole thing".
    property var openedResults: ({})

    function isResultOpen(seq) {
        return root.openedResults[seq] === true
    }

    function toggleResult(seq) {
        const next = Object.assign({}, root.openedResults)
        next[seq] = !root.isResultOpen(seq)
        root.openedResults = next
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

        //! ⛔ THE LIST SITS IN AN `Item` SO THE "BACK TO THE BOTTOM" BUTTON CAN FLOAT OVER IT (第 153 条).
        //! A sibling in this `ColumnLayout` cannot overlap the list, and overlapping is the whole point:
        //! DSH's slot is a zero-height sticky row (`ChatView.module.css:171-195` - `height: 0` plus
        //! `margin-top: -34px` on the button) so the control never adds to `scrollHeight`. In QML a list
        //! does not grow from an overlay, so the slot itself is unnecessary - what has to be kept is what
        //! it bought.
        //! ⚠️ The list below is NOT re-indented for the extra level, on purpose: this change should contain
        //! the lines it actually changes. QML does not read indentation.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60

        StyledListView {
            id: transcriptView

            anchors.fill: parent

            spacing: 6
            clip: true

            //! ── The live stream, IN PLACE (第 158 条) ────────────────────────────────────────────
            //! ⛔ It used to be a label BELOW the list, truncated to its last 300 characters - which made
            //! the arriving answer look like a second thing unrelated to the conversation: the transcript
            //! scrolled up here, and a detached line of text sat under it.
            //! It is the LIST'S FOOTER now: drawn after the last row, scrolling with them, reading as what
            //! it is - the answer still being written.
            //! ⚠️ Why the footer and not a row in the model: `agentTranscript` is a `Q_PROPERTY` rebuilt on
            //! every token (第 132 条), so putting the stream INSIDE it would rebuild the whole list per
            //! token. The footer is outside the model and gets the "in place" effect for free.
            //! ⚠️ It reuses `AgentTranscriptRow`, so the arriving answer already has the shape the finished
            //! one will have: same body, same 14/24, same markdown.
            footer: Item {
                width: transcriptView.width
                height: streamingRow.implicitHeight

                AgentTranscriptRow {
                    id: streamingRow

                    width: parent.width
                    kind: "assistant"
                    roleLabel: "agent"
                    body: root.streaming
                    richBody: root.streamingRich
                    webStyle: root.webStyle
                }
            }

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
                    //! Whether THIS result is shown in full (第 155 条). The view owns the answer; the row
                    //! only asks for a change.
                    resultExpanded: root.isResultOpen(rowItem.modelData.seq)

                    onResultToggleRequested: {
                        root.toggleResult(rowItem.modelData.seq)
                    }

                    //! The answer's copy action (第 160 条). The view owns the field, so it owns the
                    //! clipboard entry point; the row only asked.
                    onCopyRequested: function(text) {
                        if (root.field) {
                            root.field.copyToClipboard(text)
                        }
                    }
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
                    //! The group's inner rows are the same rows, so the same "is this result open?"
                    //! question has to reach them (第 155 条). Passed as a FUNCTION: the view owns the
                    //! state, and the group does not need to know how it is stored.
                    resultOpenChecker: root.isResultOpen

                    onToggleRequested: {
                        root.toggleGroup(rowItem.modelData.groupId, rowItem.modelData.hasError)
                    }

                    onResultToggleRequested: function(seq) {
                        root.toggleResult(seq)
                    }

                    //! Same relay for the group's inner rows (第 160 条).
                    onCopyRequested: function(text) {
                        if (root.field) {
                            root.field.copyToClipboard(text)
                        }
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

            //! ── Back to the bottom (第 153 条) ─────────────────────────────────────────────────────
            //! DSH renders it only when the reader is not at the bottom (`ChatView.tsx:826-841`), and the
            //! condition here is the LIST'S OWN answer rather than a threshold we track ourselves:
            //! `atYEnd` cannot disagree with what the user is looking at. `contentHeight > height` is what
            //! keeps it off a conversation that already fits - a control that can do nothing must not be
            //! on screen.
            //! ⚠️ 34x34, a full-round corner and DSH's floating fill (`button-floating-fill`); the radius
            //! is half the width so the shape stays a circle whatever the theme's metrics are.
            //! ⚠️ It calls `requestTailFollow()` and not `positionViewAtEnd()`: the settling window is the
            //! thing that makes the tail land correctly when rows are still growing (第 132 条).
            FlatButton {
                id: toBottomButton

                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                width: 34
                height: 34
                backgroundRadius: 17
                icon: IconCode.SMALL_ARROW_DOWN
                visible: !transcriptView.atYEnd && transcriptView.contentHeight > transcriptView.height
                accessible.name: qsTrc("agentharness", "Scroll to the newest message")
                toolTipTitle: qsTrc("agentharness", "Back to the bottom")

                onClicked: {
                    root.requestTailFollow()
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
            //! ⛔ 24px, DSH's disclosure row height (`ReasoningRow.module.css:6-9` locks the collapsed box
            //! to `24px + delta` with `contain: size layout`). `FlatButton`'s own default is taller, and
            //! a disclosure that is as tall as a paragraph stops reading as a disclosure.
            Layout.preferredHeight: 24
            //! 0 10px of chip padding, like the tool row's dot spacing (第 151 条's `margins: 6` was for a
            //! section header; this one sits among rows).
            margins: 6
            backgroundRadius: 6
            visible: root.reasoning.length > 0
            accessible.name: qsTrc("agentharness", "Show or hide the model's reasoning")
            opacity: 0.9
            //! ⚠️ The button's own `text` is kept because `FlatButton` derives `objectName` (and the
            //! navigation name) from it; what is DRAWN comes from the content item below.
            text: root.reasoningExpanded
                  ? qsTrc("agentharness", "Hide thinking (%1)").arg(root.reasoning.length)
                  : qsTrc("agentharness", "Think")

            onClicked: {
                root.reasoningExpanded = !root.reasoningExpanded
            }

            //! DSH's shape: `[chevron] Think  ·  <summary>` (`ReasoningRow.tsx:46` + `.module.css:53-91`).
            //! ⛔ The summary is where the old character count went, and it is a BETTER liveness signal:
            //! a number that climbs says "something is happening", a sentence that changes says WHAT.
            //! ⚠️ The explicit width is the same fix 第 148/151 条 needed twice: `FlatButton` centres its
            //! content, so without it the row is centred instead of reading from the left margin.
            contentItem: Component {
                RowLayout {
                    width: reasoningToggle.width - 12
                    spacing: 6

                    StyledIconLabel {
                        Layout.alignment: Qt.AlignVCenter
                        iconCode: root.reasoningExpanded ? IconCode.SMALL_ARROW_DOWN
                                                         : IconCode.SMALL_ARROW_RIGHT
                        color: ui.theme.fontSecondaryColor
                    }

                    StyledTextLabel {
                        Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignLeft
                        color: ui.theme.fontSecondaryColor
                        font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                        text: qsTrc("agentharness", "Think")
                    }

                    //! DSH's 2x2 separator dot, the same one the tool row uses.
                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.leftMargin: 2
                        Layout.rightMargin: 2
                        visible: !root.reasoningExpanded && root.reasoningSummary.length > 0
                        width: 2
                        height: 2
                        radius: 1
                        color: ui.theme.fontSecondaryColor
                        opacity: 0.6
                    }

                    StyledTextLabel {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        color: ui.theme.fontSecondaryColor
                        font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                        opacity: 0.8
                        text: root.reasoningExpanded ? "" : root.reasoningSummary
                    }
                }
            }
        }

        StyledTextLabel {
            id: reasoningLabel

            Layout.fillWidth: true
            //! Capped and clipped: reasoning is normally several times longer than the answer, and letting
            //! it take the height would push the conversation (and the input) out of the panel.
            //! ⚠️ DSH does NOT cap it (`ReasoningRow.module.css:16-24` says the body flows with the page,
            //! unlike the tool cards). That is right for a full-width page and wrong for a dock panel, and
            //! this component draws both - so the cap stays, and it is the cap's NUMBER that is ours.
            Layout.preferredHeight: root.reasoningExpanded ? Math.min(160, reasoningLabel.implicitHeight) : 0
            visible: root.reasoningExpanded
            clip: true
            opacity: 0.75
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignTop
            wrapMode: Text.WordWrap
            //! ⛔ Indented under the title by 22px, one step under the body size, at DSH's 13/20
            //! (`ReasoningRow.module.css:107-115`: `padding: 4px 0 4px calc(22px + delta)`). Without the
            //! indent the body reads as a paragraph that happens to follow a row rather than as the thing
            //! that row is about - and before this it was also at the FULL body size, so it competed with
            //! the answer.
            leftPadding: 22
            font.pixelSize: ui.theme.bodyFont.pixelSize - 1
            lineHeight: 20
            lineHeightMode: Text.FixedHeight
            text: root.reasoning
        }

        //! ── "It is working", when there is nothing to show yet ─────────────────────────────────────
        //! ⛔ The stream itself moved INTO the list (第 158 条); what is left here is the gap before the
        //! first delta arrives: a turn that spends ten seconds thinking looked like a hang (第 127 条).
        //! ⚠️ Only while running AND still empty - once deltas arrive, the footer IS the liveness signal,
        //! and two indicators for one state is one too many.
        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            visible: root.streaming.length === 0 && root.running
            opacity: 0.8
            text: qsTrc("agentharness", "thinking…")
        }

        //! ── Errors are shown, not swallowed ──────────────────────────────────────────────────
        //! ⛔ THE SHAPE IS DSH'S ERROR ROW (第 152 条): a 10px dot, then the text - the dot is the
        //! signal, the sentence is the detail. `ui-chat/src/client/chat/MessageItem.module.css:277-309`
        //! draws it as `grid-template-columns: 10px minmax(0,1fr) auto` with `gap: 8px`, the dot nudged
        //! down 5px so it sits on the first line's centre.
        //! ⚠️ WHAT WE DO NOT HAVE, and therefore do not invent: DSH's row has a TITLE (its error class,
        //! in the error colour, weight 600) and a separate message and code. Ours is one sentence from
        //! the transport ("HTTP 401 ..."), with no class to name - so there is no bold lead-in here, and
        //! the dot carries the "this is a failure" signal that the title carries there.
        //! ⛔ The sentence is `fontSecondaryColor`, not the error colour: a red paragraph is unreadable
        //! and the red is already spent on the dot. Before this, the whole line was red text with
        //! nothing to catch the eye first.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.lastError.length > 0

            Item {
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 5

                Rectangle {
                    anchors.fill: parent
                    radius: 5
                    color: ui.theme.buttonColor
                }
            }

            StyledTextLabel {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
                color: ui.theme.fontSecondaryColor
                text: root.lastError
            }
        }
    }
}
