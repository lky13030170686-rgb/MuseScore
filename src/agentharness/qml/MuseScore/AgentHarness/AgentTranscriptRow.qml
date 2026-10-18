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

    //! Panel style (labelled paragraphs, tool rows indented) vs web style (bubbles and one-liners).
    property bool webStyle: false

    readonly property bool isUser: root.kind === "user"
    readonly property bool isTool: root.kind === "toolCall" || root.kind === "toolResult"
    readonly property bool isToolResult: root.kind === "toolResult"
    //! Exactly the answer rows. `attempt` (a failed round's reason) is a message too, but it is not the
    //! model's prose and nobody writes lists in it, so it stays verbatim.
    readonly property bool isAssistant: root.kind === "assistant"

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
            visible: !root.isUser && !root.isTool
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
        RowLayout {
            visible: root.isTool
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
    }
}
