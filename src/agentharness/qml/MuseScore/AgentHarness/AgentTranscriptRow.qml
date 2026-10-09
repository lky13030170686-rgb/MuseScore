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

    \b Why this is a separate file. The panel has three kinds of row (user / agent / tool) and the
    difference between them is entirely visual - a colour, an indent, whether the text is elided. Put
    inline in the panel's delegate it became a wall of nested ternaries; here each kind is one branch
    and the panel reads as layout.

    \b Why no controls. Rows are read-only text. Anything interactive inside a scrolling transcript is
    a control the user can lose focus in, and the panel is meant to stay usable while the agent works.
*/
Item {
    id: root

    required property string kind
    required property string roleLabel
    required property string body
    required property bool isError

    implicitHeight: rowLayout.implicitHeight + 8

    Column {
        id: rowLayout

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.kind === "toolCall" || root.kind === "toolResult" ? 12 : 0
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
                   : root.kind === "toolCall" || root.kind === "toolResult" ? ui.theme.fontSecondaryColor
                   : ui.theme.fontPrimaryColor
            //! Tool output is capped: a `score_window` over fifty measures would otherwise push the
            //! rest of the conversation out of the panel. The full text is in the session log.
            text: root.kind === "toolResult" && root.body.length > 400
                  ? root.body.substring(0, 400) + " …"
                  : root.body
        }
    }
}
