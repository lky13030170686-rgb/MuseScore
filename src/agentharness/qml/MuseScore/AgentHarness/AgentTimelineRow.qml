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
    The information field's timeline: what has happened to this score, newest first.

    \b Why this is not the transcript. The transcript is what was SAID; this is what HAPPENED. They are
    different records of different things - a tool call that was refused changes nothing and appears in
    the transcript, while an edit the user made with the mouse appears here and nowhere in the
    conversation. The panel needs both, and conflating them would lose exactly the edits the agent did
    not make.

    \b Where the text comes from. `modelData.line` is `SemanticOp::toString()`, computed in C++. One
    place decides how an operation reads, so the panel, the log and the `field_timeline` tool cannot
    describe the same edit three different ways.

    \b Why undo/redo are colours and not icons. The colour IS the meaning here, and a row of undo
    markers reads as a run of greyed lines rather than as a column of repeated glyphs. NOTE only
    colours verified to exist on `ui.theme` are used: a typo in a theme property fails at RUNTIME,
    not at build time (`errorColor` was removed for exactly that reason).
*/
Item {
    id: root

    required property string line
    required property bool isUndo
    required property bool isRedo

    implicitHeight: 30

    StyledTextLabel {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        horizontalAlignment: Text.AlignLeft
        elide: Text.ElideRight
        color: root.isUndo ? ui.theme.buttonColor
               : root.isRedo ? ui.theme.accentColor
               : ui.theme.fontPrimaryColor
        text: root.line
    }
}
