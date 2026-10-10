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

    //! Panel style (labelled paragraphs, tool rows indented) vs web style (bubbles and one-liners).
    property bool webStyle: false

    readonly property bool isUser: root.kind === "user"
    readonly property bool isTool: root.kind === "toolCall" || root.kind === "toolResult"
    readonly property bool isToolResult: root.kind === "toolResult"

    //! A tool row in web style is ONE line. The body of a tool result is a multi-line digest, and a
    //! `Text` honours `\n` no matter what `wrapMode` says - so the first version drew seven lines where
    //! the design has one. Taking the first line (and saying there is more) is what the web client's
    //! tool rows do; the full text is in the inspector beside the conversation.
    readonly property string webToolLine: {
        const body = String(root.body)
        const firstBreak = body.indexOf("\n")
        return firstBreak >= 0 ? body.substring(0, firstBreak) + " …" : body
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
            //! Tool output is capped: a `score_window` over fifty measures would otherwise push the
            //! rest of the conversation out of the panel. The full text is in the session log.
            text: root.kind === "toolResult" && root.body.length > 400
                  ? root.body.substring(0, 400) + " …"
                  : root.body
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
            readonly property real maxWidth: Math.max(120, webColumn.width * 0.72)

            width: Math.min(bubbleMetrics.width + 24, maxWidth)
            //! Derived from the text's own (unconstrained) height rather than from a fill anchor:
            //! a child that is anchored to its parent's height while the parent's height is derived
            //! from the child is the classic two-way binding, and it shows up as a QML warning plus a
            //! bubble that keeps growing by a line every relayout.
            height: bubbleText.height + 16
            radius: 12
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
                anchors.margins: 8
                wrapMode: Text.WordWrap
                font: ui.theme.bodyFont
                color: ui.theme.fontPrimaryColor
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
            text: root.body
        }

        //! A tool call is ONE compact line: the call reads `name arguments`, its result is prefixed with
        //! an arrow so a call and its answer are not confused for each other. Dimmed and elided, because
        //! a row per tool call, expanded, would bury the conversation it belongs to.
        StyledTextLabel {
            visible: root.isTool
            width: parent.width
            horizontalAlignment: Text.AlignLeft
            color: root.isError ? ui.theme.buttonColor : ui.theme.fontSecondaryColor
            text: (root.isToolResult ? "↳ " : "") + root.webToolLine
        }
    }
}
