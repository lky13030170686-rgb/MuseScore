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
//! ⛔ FOR `RowLayout`, the tool row's four-part shape (第 151 条). Forgetting it is not a small mistake
//! and it is not local: the error is `RowLayout is not a type`, and because these rows are created
//! inside the WINDOW's content, that one line takes the whole notation page down - measured, the app
//! came up on the home page with the window titled `MuseScoreStudio5Development` instead of the score,
//! which reads as "the score won't open". 第 144 条 recorded exactly this with `Popup`; this is the
//! second time, so the lesson is written where the next import will be added.
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

/*!
    One row of the agent conversation.

    \b Why this is a separate file. The conversation has three kinds of row (user / agent / tool) and the
    difference between them is entirely visual - a colour, an indent, whether the text is elided. Put
    inline in a delegate it became a wall of nested ternaries; here each kind is one branch and the
    caller reads as layout.

    \b Why two styles, and why they are in ONE file. The notation page's Agent panel is a narrow dock
    and reads best as labelled paragraphs; the Agent page is a full workbench laid out like the DSH web
    client, where a user message is a right-aligned bubble, an answer is plain text and a tool call is
    one compact dim line. Those are two renderings of the SAME row model (`agentTranscript()`), so they
    live here together: splitting them into two files is how the two surfaces would start disagreeing
    about what a tool row even is.

    \b Why no controls. Rows are read-only text. Anything interactive inside a scrolling transcript is
    a control the user can lose focus in, and both surfaces are meant to stay usable while the agent
    works.
*/
Item {
    id: root

    required property string kind
    required property string roleLabel
    required property string body
    required property bool isError

    //! The answer's DRAWN form (第 150 条): what `markdownToRichText()` produced, computed in C++ where
    //! the transcript itself is built. Empty for every other kind of row, and this file never parses
    //! anything - one renderer, one place, so the panel and the two page surfaces cannot disagree about
    //! what the model said.
    property string richBody: ""

    //! A tool CALL's name, as its own field (第 151 条) - the row draws it as a separate segment with its
    //! own ink and size, so the split is done where it already exists (C++) instead of being re-derived
    //! here from `"<name> <arguments>"`.
    property string toolNameField: ""

    //! Whether this row's full text is on screen (tool results only - 第 155 条).
    //! ⚠️ The VALUE comes from the view and the CHANGE is only asked for: the transcript model is rebuilt
    //! on every streamed token, so a row owning this flag would snap shut while it is being read (第 132 条).
    property bool resultExpanded: false

    //! This result row was clicked - the view decides what that means.
    signal resultToggleRequested()

    //! This answer should be copied (第 160 条). ⛔ The ROW cannot do it: it has no field, and it is
    //! rebuilt on every streamed token (第 132 条) - so it asks, and the view (which owns the field and
    //! therefore the clipboard entry point) does it. Same shape as `resultToggleRequested`.
    signal copyRequested(string text)

    //! Panel style (labelled paragraphs, tool rows indented) vs web style (bubbles and one-liners).
    property bool webStyle: false

    readonly property bool isUser: root.kind === "user"
    readonly property bool isTool: root.kind === "toolCall" || root.kind === "toolResult"
    readonly property bool isToolResult: root.kind === "toolResult"
    //! Exactly the answer rows. `attempt` (a failed round's reason) is a message too, but it is not the
    //! model's prose and nobody writes lists in it, so it stays verbatim.
    readonly property bool isAssistant: root.kind === "assistant"
    //! The model's reasoning, which gets its own row (第 157 条). ⛔ It has to be excluded from the
    //! answer branch explicitly: that branch's condition is `!isUser && !isTool`, which a reasoning row
    //! SATISFIES - measured: the reasoning was drawn twice, once as a full-width paragraph above the
    //! `Think` line and once inside it.
    readonly property bool isReasoning: root.kind === "reasoning"

    //! A tool row in web style is ONE line. The body of a tool result is a multi-line digest, and a
    //! `Text` honours `\n` no matter what `wrapMode` says - so the first version drew seven lines where
    //! the design has one. Taking the first line (and saying there is more) is what the web client's
    //! tool rows do; the full text is in the inspector beside the conversation.
    readonly property string webToolLine: {
        const body = String(root.body)
        const firstBreak = body.indexOf("\n")
        return firstBreak >= 0 ? body.substring(0, firstBreak) + " …" : body
    }

    //! ── The tool row's two segments (第 151 条) ──────────────────────────────────────────────────
    //! DSH's tool row is `[leading] gap6 [name 13/24] gap8 [2x2 dot] gap8 [summary 13/24 tertiary]`
    //! (`ui-tool/src/client/tool/components/ToolRow.module.css:1-2` + `:68-90`), and the two texts are
    //! at DIFFERENT tiers: the name is the row's subject, the arguments are what it did.
    readonly property string toolName: {
        if (root.toolNameField.length > 0) {
            return root.toolNameField
        }
        //! The fallback exists for one caller only: a `toolGroup`'s inner rows are rebuilt from maps that
        //! may predate the field. Splitting on the first space is what `agentTranscript()` does.
        const s = String(root.body)
        const cut = s.indexOf(" ")
        return cut > 0 ? s.substring(0, cut) : s
    }

    //! What is left of the call after its name - the arguments, verbatim. ⛔ NOT pretty-printed: DSH runs
    //! its arguments through `JSON.parse` + `JSON.stringify(…, 2)` before drawing them, and ours are
    //! short enough to read as they arrive (`{"first":1,"last":2}`). Re-formatting here would be a
    //! second opinion about what the model asked for, for no visible gain.
    readonly property string toolSummary: {
        if (root.isToolResult) {
            return root.webToolLine
        }
        const s = String(root.body)
        const cut = s.indexOf(" ")
        return cut > 0 ? s.substring(cut + 1) : ""
    }

    implicitHeight: root.webStyle ? webColumn.implicitHeight + 10 : panelColumn.implicitHeight + 8

    //! ══ The panel's rendering ══════════════════════════════════════════════════════════════════
    Column {
        id: panelColumn

        visible: !root.webStyle
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.isTool ? 12 : 0
        anchors.rightMargin: 4
        spacing: 2

        StyledTextLabel {
            width: parent.width
            horizontalAlignment: Text.AlignLeft
            //! The role is a separate small line rather than a prefix, so a long answer does not have
            //! its first line pushed off to the right by "agent: ".
            opacity: 0.65
            text: root.roleLabel
        }

        StyledTextLabel {
            width: parent.width
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            //! Only colours verified to exist on `ui.theme`: a typo in a theme property fails at
            //! RUNTIME, not at build time.
            color: root.isError ? ui.theme.buttonColor
                   : root.isTool ? ui.theme.fontSecondaryColor
                   : ui.theme.fontPrimaryColor
            //! ⛔ THE ANSWER GOES THROUGH THE RENDERER, everything else is verbatim (第 150 条).
            //! The explicit `Text.PlainText` is not decoration: the default is `Text.AutoText`, which
            //! SNIFFS for markup - so a user message containing `<b>` came back bolded, i.e. the
            //! transcript showed something the user did not type.
            textFormat: root.isAssistant ? Text.RichText : Text.PlainText
            //! Tool output is capped: a `score_window` over fifty measures would otherwise push the
            //! rest of the conversation out of the panel. The full text is in the session log.
            text: root.isAssistant
                  ? root.richBody
                  : (root.kind === "toolResult" && root.body.length > 400
                     ? root.body.substring(0, 400) + " …"
                     : root.body)
            //! Same 14/24 as the page's copy (第 154 条) - the panel is the same conversation in less
            //! room, not a different rendering of it.
            lineHeight: root.isAssistant ? 24 : 1.0
            lineHeightMode: root.isAssistant ? Text.FixedHeight : Text.ProportionalHeight
        }
    }

    //! ══ The Agent page's rendering (the DSH web client's shapes) ═══════════════════════════════
    Column {
        id: webColumn

        visible: root.webStyle
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 2

        //! A user message is a bubble, right-aligned, with NO role label: its position says who said
        //! it, which is exactly how the web client reads.
        RoundedRectangle {
            id: bubble

            visible: root.isUser
            anchors.right: parent.right
            anchors.rightMargin: 4

            //! Hug the text up to a maximum, the way a chat bubble does. Measured with `TextMetrics`
            //! rather than `implicitWidth`: for a wrapped `Text` the meaning of `implicitWidth` depends
            //! on whether a width was already set, and a bubble that has to be told its own content
            //! width is a circular binding waiting to happen.
            //! ⛔ DSH's cap is `min(contentWidth * 0.702, 82%)` (`MessageItem.module.css:11-22`): the
            //! 0.702 is figma's 525px held as a SHARE of the axis, so it widens with the column instead
            //! of being a magic number. Ours was `column * 0.72` against the wrong base - the whole
            //! column rather than the content axis (第 151 条).
            readonly property real maxWidth: Math.max(120, Math.min(webColumn.width * 0.702,
                                                                     webColumn.width * 0.82))

            //! ⛔ `22px` corner and `10px 16px` of padding (第 151 条). DSH's bubble is a 42px-tall single
            //! line at the default size - 22 of line height plus 10 a side - and the 22px radius is what
            //! makes it read as a chat bubble rather than as a card. We had 12px and 8px.
            width: Math.min(bubbleMetrics.width + 32, maxWidth)
            //! Derived from the text's own (unconstrained) height rather than from a fill anchor:
            //! a child that is anchored to its parent's height while the parent's height is derived
            //! from the child is the classic two-way binding, and it shows up as a QML warning plus a
            //! bubble that keeps growing by a line every relayout.
            height: bubbleText.height + 20
            radius: 22
            color: ui.theme.backgroundSecondaryColor

            TextMetrics {
                id: bubbleMetrics

                font: ui.theme.bodyFont
                text: root.body
            }

            Text {
                id: bubbleText

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.topMargin: 10
                wrapMode: Text.WordWrap
                font: ui.theme.bodyFont
                color: ui.theme.fontPrimaryColor
                //! ⛔ The user's own words are drawn EXACTLY as typed. `Text`'s default is `Text.AutoText`,
                //! which sniffs for markup - so a question containing `<b>` rendered as a bold word and the
                //! bubble showed something the user had not written. Only the agent's answers are markdown.
                textFormat: Text.PlainText
                text: root.body
            }
        }

        //! An answer is plain text - no bubble, no border. Same as the web client: the agent's words
        //! are the page, not a card on it.
        StyledTextLabel {
            visible: !root.isUser && !root.isTool && !root.isReasoning
            width: parent.width
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            color: root.isError ? ui.theme.buttonColor : ui.theme.fontPrimaryColor
            textFormat: root.isAssistant ? Text.RichText : Text.PlainText
            text: root.isAssistant ? root.richBody : root.body
            //! ⛔ THE ANSWER'S LINE HEIGHT IS 24px, NOT THE FONT'S OWN ~15px (第 154 条).
            //! DSH's body is `14px / 24px` (`ui-chat/.../AssistantMarkdown.module.css:9-15`), and that
            //! ratio is most of what makes a long answer readable - measured before this: a 40-line
            //! answer fitted in a 675px transcript, i.e. ~15px a line, and it read as a wall.
            //! ⚠️ `FixedHeight` and not `ProportionalHeight`: Qt's idea of "1.0" is the FONT's own
            //! spacing (~16px here), so a multiplier would have to be guessed per font, while DSH's
            //! ladder is stated in pixels. `lineHeight` reaches rich text through `QTextBlockFormat`,
            //! which is why it works on `Text.RichText` at all.
            //! ⚠️ Only the assistant's rows: a tool line and a user bubble have their own sizes, and a
            //! fixed 24 on them would stretch the ones that are meant to be compact.
            lineHeight: root.isAssistant ? 24 : 1.0
            lineHeightMode: root.isAssistant ? Text.FixedHeight : Text.ProportionalHeight
        }

        //! ── A tool row, in DSH's four parts (第 151 条) ───────────────────────────────────────────
        //! `[10px leading] gap6 [name 13/24] gap8 [2x2 dot] gap8 [summary 13/24 tertiary]`
        //! (`ui-tool/src/client/tool/components/ToolRow.module.css:1-2`, `:68-90`). The two texts sit at
        //! DIFFERENT tiers on purpose: the name is the row's subject, the arguments are what it did.
        //! ⛔ TWO DELIBERATE DEPARTURES, both because our rows are not DSH's:
        //!   ① DSH's leading slot is the TOOL'S ICON (search / read / bash / ...). Ours would have to be
        //!      one of 38 glyphs picked by name - and "pick an icon by its name" is exactly the mistake
        //!      维护手册.md §4.8 records (a glyph is not what its name says, and that costs a rendered
        //!      sheet to find out). The slot carries the row's own state instead: a dot for a call,
        //!      `↳` for its result.
        //!   ② DSH draws a call and its result as ONE row, which is what lets its dot mean
        //!      running/ok/failed. Ours are TWO rows - the fold GROUPS them, it does not merge them
        //!      (`transcriptfold.h`) - and neither row carries the other's state. A dot that claimed to
        //!      know would be inventing it; the failure colour on the result line is what says "this one
        //!      did not work".
        //! ── The answer's actions (第 160 条) ────────────────────────────────────────────────────────
        //! DSH's message actions: a 28px square button with a full-round corner, the tertiary ink at rest
        //! and the secondary ink on hover, in a row with an 8px gap that sits under the body
        //! (`MessageIconActions.module.css:5-10`, `:57-81`; `AssistantMarkdown.module.css:66-70` sets the
        //! gap above it). ⚠️ The answer only: a user's own message needs no copy button (they typed it),
        //! and a tool result already has its own expansion (第 155 条).
        RowLayout {
            id: messageActions

            visible: root.isAssistant && root.body.length > 0
            width: parent.width
            spacing: 8
            FlatButton {
                id: copyButton

                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.topMargin: 8
                //! Half the width, so the shape is a circle whatever the theme's metrics are.
                backgroundRadius: 14
                transparent: true
                icon: IconCode.COPY
                accessible.name: qsTrc("agentharness", "Copy this answer")
                toolTipTitle: qsTrc("agentharness", "Copy the answer as text")

                onClicked: {
                    root.copyRequested(root.body)
                    copiedTimer.restart()
                }
            }

            //! ⛔ A CLICK NEEDS AN ANSWER ON SCREEN. DSH swaps the icon for a check mark; we have no such
            //! glyph in `IconCode`, and a button that does not change looks like a button that did
            //! nothing - so the feedback is a word beside it, for a moment.
            StyledTextLabel {
                id: copiedLabel

                Layout.alignment: Qt.AlignVCenter
                Layout.topMargin: 8
                //! ⛔ `opacity` AND NOT `visible`, and the reason is the ROW'S GEOMETRY, not whether it
                //! draws: a hidden item is out of the `RowLayout`, so showing it re-distributes the row
                //! (and the spacer beside it jumps). Fading keeps the geometry still - the label is always
                //! there, only its ink changes. ⚠️ Its text stays for the same reason: empty text would
                //! still claim `implicitWidth`, and then the width would jump instead.
                //! ⛔⛔ AND A CORRECTION WORTH KEEPING (第 162 条): 第 161 条 wrote here that `visible`
                //! "went true for 2.3s and nothing was drawn". **That was wrong.** The geometry probe
                //! (`[copied-geom] label x=36 y=15 w=37 h=14 op=1`) showed a perfectly laid-out label, and
                //! the real reason no screenshot ever caught it was the VERIFICATION: the click helper
                //! sleeps 400ms internally, so `sleep:1200` in the step landed at 1.6s - exactly the
                //! timer's interval. Four "it does not show" readings were four screenshots taken on the
                //! boundary. ⇒ See `copiedTimer` for what changed as a result.
                opacity: copiedTimer.running ? 1 : 0
                color: ui.theme.fontSecondaryColor
                font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                text: qsTrc("agentharness", "Copied")
            }

            //! Pushes the pair to the row's start - the same fix the `Think` line needed (第 157 条).
            Item {
                Layout.fillWidth: true
            }
        }

        //! Not an `Item`, so it adds nothing to the column's height.
        //! ⛔ 2.6 SECONDS, AND THE NUMBER IS A VERIFICATION FINDING (第 162 条). It was 1600ms - DSH's
        //! check-mark swap is about that long - and "Copied" was never caught on screen: not because it
        //! was not drawn (the geometry probe proved it was), but because every screenshot landed at
        //! 1.1-1.9s: the click helper sleeps 400ms of its own, so a `sleep:1200` step fires at 1.6s.
        //! ⚠️ A WORD NEEDS LONGER THAN AN ICON: DSH swaps a glyph the eye already knows, while this is a
        //! word the user has to read. 2.6s is long enough to read and short enough not to linger.
        //! ⛔ And the lesson is about the MEASUREMENT, not the timer: a window that a check keeps missing
        //! by a hair looks exactly like a feature that does not work.
        Timer {
            id: copiedTimer
            interval: 2600
        }

        //! ── The model's reasoning, as a transcript row (第 157 条) ─────────────────────────────────
        //! ⛔ WHY IT EXISTS: the reasoning was in the session log from the first day and **no reader used
        //! it** - the only surface that ever showed it was `agentStreamingReasoning`, which `AgentLoop`
        //! clears when the next response starts. So after a turn finished, what the model thought was
        //! gone from the UI (第 156 条 measured that: the row was styled and not on screen).
        //! ⚠️ Drawn OPEN rather than as DSH's disclosure: DSH's row is collapsible because a page full of
        //! thinking is noise, but a HISTORICAL row is read on purpose - the user scrolled back to it - and
        //! a second fold state to keep per row is the thing 第 132 条 warned about. The cap is what keeps
        //! it from owning the screen, the same 160px the live row uses.
        Column {
            visible: root.kind === "reasoning"
            width: parent.width
            spacing: 2

            RowLayout {
                width: parent.width
                spacing: 6

                StyledIconLabel {
                    Layout.alignment: Qt.AlignVCenter
                    iconCode: IconCode.SMALL_ARROW_RIGHT
                    color: ui.theme.fontSecondaryColor
                }

                StyledTextLabel {
                    Layout.alignment: Qt.AlignVCenter
                    horizontalAlignment: Text.AlignLeft
                    color: ui.theme.fontSecondaryColor
                    font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                    text: qsTrc("agentharness", "Think")
                }

                //! ⛔ Pushes the pair to the row's start. Without it the layout spreads the spare width
                //! between the two items and `Think` lands in the middle of the row while the chevron
                //! stays at x=0 (measured) - which reads as two unrelated things on one line.
                Item {
                    Layout.fillWidth: true
                }
            }

            StyledTextLabel {
                width: parent.width
                //! 13/20, indented under the `Think` line by 22px - DSH's reasoning body
                //! (`ReasoningRow.module.css:107-115`), and the same numbers the live row uses (第 156 条).
                //! ⚠️ `height`, not `Layout.preferredHeight`: this Column is not a Layout container, so a
                //! `Layout.*` attachment here would bind nothing (and Qt would warn). The cap is the same
                //! 160px the live row uses.
                height: Math.min(160, implicitHeight)
                leftPadding: 22
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignTop
                wrapMode: Text.WordWrap
                clip: true
                opacity: 0.75
                font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                lineHeight: 20
                lineHeightMode: Text.FixedHeight
                text: root.body
            }
        }

        //! ── A tool row, and the whole result on request (第 155 条) ───────────────────────────────
        //! The row itself is unchanged; what is new is that a RESULT can be opened to show its full
        //! text in the code font. ⛔ Why this is the answer to "the agent changed something and I cannot
        //! see what it got back": the row is one line by design (a multi-line digest per tool call
        //! buries the conversation), and the full text was only ever visible for the NEWEST call, in the
        //! right column. Every older call's result was unreachable.
        //! ⚠️ The block below is NOT re-indented for the extra levels, on purpose - this change should
        //! contain the lines it actually changes (same note as AgentConversationView's list).
        Column {
            visible: root.isTool
            width: parent.width
            spacing: 4

            Item {
                width: parent.width
                height: toolRow.implicitHeight

        RowLayout {
            id: toolRow
            width: parent.width
            spacing: 6

            Item {
                Layout.preferredWidth: 10
                Layout.preferredHeight: 20
                Layout.alignment: Qt.AlignVCenter

                StyledTextLabel {
                    anchors.centerIn: parent
                    visible: root.isToolResult
                    color: root.isError ? ui.theme.buttonColor : ui.theme.fontSecondaryColor
                    text: "↳"
                }

                Rectangle {
                    anchors.centerIn: parent
                    visible: !root.isToolResult
                    width: 6
                    height: 6
                    radius: 3
                    color: ui.theme.accentColor
                    opacity: 0.7
                }
            }

            StyledTextLabel {
                Layout.alignment: Qt.AlignVCenter
                visible: !root.isToolResult
                color: ui.theme.fontSecondaryColor
                font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                text: root.toolName
            }

            //! DSH's separator: a 2x2 dot with 8px on each side (`ToolRow.module.css:72-79`).
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 2
                Layout.rightMargin: 2
                visible: !root.isToolResult && root.toolSummary.length > 0
                width: 2
                height: 2
                radius: 1
                color: ui.theme.fontSecondaryColor
                opacity: 0.6
            }

            StyledTextLabel {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                //! ⛔ `StyledTextLabel`'s own default is `Text.AlignHCenter`, and a `fillWidth` label that
                //! is centred puts the arguments in the MIDDLE of the row, detached from the name they
                //! belong to (measured 2026-10-18: `score_overview ·` on the left, `{}` floating in the
                //! centre). Every left-hand column here says AlignLeft explicitly for the same reason.
                horizontalAlignment: Text.AlignLeft
                color: root.isError ? ui.theme.buttonColor : ui.theme.fontSecondaryColor
                font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                elide: Text.ElideRight
                opacity: root.isToolResult ? 1.0 : 0.8
                text: root.toolSummary
            }
        }

                //! The hit target. ⛔ A real `FlatButton` with `backgroundItem: null`, NOT a `MouseArea`
                //! over the row: a hand-drawn control never reaches the accessibility tree, so the
                //! transcript could not be driven or verified from a script (维护手册.md §7.6), and a
                //! `MouseArea` would also swallow the drag that scrolls the list. DSH's tool row is a
                //! `<button>` for the same reason.
                //! ⚠️ Only a RESULT is expandable: a call's "full text" is its arguments, which the row
                //! already shows in full, so opening it would do nothing.
                FlatButton {
                    anchors.fill: parent
                    visible: root.isToolResult
                    backgroundItem: null
                    text: ""
                    accessible.name: root.resultExpanded
                                     ? qsTrc("agentharness", "Hide the full tool result")
                                     : qsTrc("agentharness", "Show the full tool result")
                    toolTipTitle: qsTrc("agentharness", "The whole result, not just its first line")
                    onClicked: {
                        root.resultToggleRequested()
                    }
                }
            }

            //! The whole result. DSH's expanded body is a card on the code-block surface with a 12px
            //! corner, the code font and a cap of its own (`ToolRow.module.css:212-232`: the IN and OUT
            //! sections scroll INDEPENDENTLY, `max-height: 150px`). We have one section - the result -
            //! and it scrolls inside itself for the same reason: a 50-measure `score_window` must not
            //! push the conversation off the screen.
            //! ⚠️ DSH's code font stack, minus the bare `monospace` tail: on Windows that tail falls back
            //! to SimSun for CJK, and a code block with Songti in it reads as a different document.
            Rectangle {
                id: resultCard

                visible: root.isToolResult && root.resultExpanded
                width: parent.width
                //! 24 = 12 of padding a side (`ToolRow.module.css:224-232` uses `padding: 12px 16px`).
                height: Math.min(150, resultText.implicitHeight + 24)
                radius: 12
                //! `backgroundSecondaryColor` and not `backgroundTertiaryColor`: in this theme the
                //! tertiary is BLACK (第 148 条 measured it), so the card would read as a hole cut in
                //! the transcript. DSH's code surface is one step ABOVE the page, not below it.
                color: ui.theme.backgroundSecondaryColor
                border.color: ui.theme.strokeColor
                border.width: ui.theme.borderWidth

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 12
                    contentWidth: width
                    contentHeight: resultText.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Text {
                        id: resultText

                        width: parent.width
                        wrapMode: Text.WrapAnywhere
                        //! ⛔ `font.family`, NOT `font.families`. The QML `font` value type has no
                        //! `families` - it exists on `QFont` in C++, which is where DSH's stack comes
                        //! from - and assigning it fails at LOAD time with
                        //! `Cannot assign to non-existent property "families"`. Because these rows are
                        //! created inside the window's content, that one line takes the whole notation
                        //! page down and the app comes up on the home page (第 144 / 151 条's shape, and
                        //! this is the third time, so the note is at the line).
                        //! ⚠️ Qt walks its own fallback chain for anything this family lacks, which is
                        //! what covers the CJK case DSH's extra names were there for.
                        font.family: "Consolas"
                        font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                        color: ui.theme.fontSecondaryColor
                        textFormat: Text.PlainText
                        //! ⛔ A tool result is a digest with meaningful line breaks; `wrapMode` alone does
                        //! NOT preserve them (第 124 条 measured that a `Text` breaks on `\n` whatever the
                        //! wrap mode says - which is exactly what a result wants).
                        text: root.body
                    }
                }
            }
        }
    }
}
