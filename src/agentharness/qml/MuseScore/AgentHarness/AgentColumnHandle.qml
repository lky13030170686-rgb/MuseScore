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

/*!
    The draggable boundary between two columns of the Agent page.

    \b Why it is its own file. The page has two boundaries and they behave identically except for which
    way is "wider" - and that difference belongs to the PAGE (it knows which column is on which side),
    not to the handle. So the handle reports "the pointer moved this far since the press" and nothing
    else. One implementation, two users, no second drag to get wrong (维护手册.md §4.5).

    \b Why it reports the delta from the PRESS and not from the last move. A move-based delta has to be
    accumulated by the caller, and any dropped or coalesced move event then leaves the column shorter
    than the pointer - it drifts away from the cursor during a fast drag. Reporting
    `current - pressStart` makes the width a pure function of where the pointer is, so a lost event
    costs nothing.

    \b Why it is 7px wide around a 1px line. The line is what you see; the 7px is what you can hit. A 1px
    target is not draggable in practice, and this is the same reasoning as the notation view's selection
    handles. ⚠️ It also has to be found by a SCRIPT: the panel and the page's controls are not in the UI
    Automation tree (维护手册.md §7.6), so a verification drag has to aim at real pixels - and 7px of
    slop is what makes that possible at all (第 129 条: an 8px coordinate mistake was enough to miss it).
*/
Item {
    id: root

    implicitWidth: 7

    //! Emitted while the pointer is down, with the distance moved since the press.
    signal moved(real delta)
    //! The hand went down. The page uses it to remember the width the drag started from.
    signal pressed()
    signal released()

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        width: 1
        color: ui.theme.strokeColor
    }

    //! The affordance: the boundary lights up when the pointer is on it, and stays lit while dragging.
    //! Without it a user has to discover by accident that the columns can be resized at all.
    Rectangle {
        anchors.fill: parent
        color: ui.theme.accentColor
        opacity: (dragArea.containsMouse || dragArea.pressed) ? 0.45 : 0.0

        Behavior on opacity {
            NumberAnimation { duration: 80 }
        }
    }

    MouseArea {
        id: dragArea

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.SplitHCursor

        //! Scene coordinates, because the handle itself MOVES as the drag proceeds: measuring the pointer
        //! in the handle's own frame would make every pixel of column movement cancel itself out and the
        //! drag would not move anything.
        property real pressSceneX: 0

        onPressed: function(mouse) {
            pressSceneX = mapToItem(null, mouse.x, mouse.y).x
            root.pressed()
        }

        onPositionChanged: function(mouse) {
            if (!pressed) {
                return
            }
            root.moved(mapToItem(null, mouse.x, mouse.y).x - pressSceneX)
        }

        onReleased: {
            root.released()
        }
    }
}
