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
    A column's tab strip: which of that column's views is on screen.

    \b Where the shape comes from. The DSH web client's right column is a dock surface whose top edge is
    a tab strip (`packages/client/ui-dockkit/src/components/dockkit.module.css`): a 28px row with 4px
    between chips, 10px of inset at the start, and each chip a 28px capsule (`border-radius: 12`) whose
    label is 13px in the secondary ink - bare when idle, filled when active, hover filled in between.
    Those numbers are copied rather than invented, because "looks like the web client" is mostly a
    statement about proportions (第 124 条).

    \b Two things are deliberately NOT copied, both because they carry meaning this column does not have.
    The kit draws a 10px hairline slot between chips and blanks the two beside the active one: those
    separate chips that can be dragged, split and closed. Here the chips are two fixed views, so the
    capsule fill alone says which one is on screen and the hairlines would be furniture. The kit's chips
    also carry close and add controls, which need a tab that can go away - nothing here can.

    \b Why every chip is a real `FlatButton`. Same reason as the settings dialog's nav rail (第 130 条):
    a hand-drawn chip never reaches the accessibility tree, so UI Automation cannot click it and a screen
    reader cannot name it. ⚠️ That is not a guess - measured 2026-10-13 on the real page: the chips come
    back as `Button 'Trajectory' rect=1612,81 132x28` and `Button 'Conversation' rect=1748,81 132x28`,
    and `ui-probe -Action click -Name Conversation -ControlType Button` switches the view. ⛔ It also
    nearly produced a false finding the other way: `ui-probe -Action pages` lists RADIO BUTTONS only
    (the page tabs), so reading it as "what is on this page" says the page has six controls and no
    buttons - it has 41 elements, 27 of them buttons (`ui-probe -Action controls`).
*/
Item {
    id: root

    //! The chips, as `[{ id, label }]`. Order is the order they are drawn in.
    property var tabs: []
    //! The `id` of the chip that is on screen. An id no chip has draws every chip idle - which is the
    //! honest picture of "the column is showing something none of these chips is".
    property string activeId: ""

    //! A chip was clicked. The caller decides what that means; this component owns no view state.
    signal activated(string id)

    //! DSH's chip height, and therefore the strip's.
    readonly property int chipHeight: 28
    //! DSH's chip floor: a two-word label must not make the strip read as one wide button.
    readonly property int chipMinWidth: 80

    implicitHeight: root.chipHeight

    RowLayout {
        anchors.fill: parent
        spacing: 4

        Repeater {
            model: root.tabs

            FlatButton {
                id: chip

                Layout.preferredHeight: root.chipHeight
                Layout.minimumWidth: root.chipMinWidth

                //! DSH's chip padding is `0 10px`, i.e. the label plus 20px; `FlatButton`'s own default is
                //! 16px a side, which would draw a two-word chip about 24px wider than the web client's.
                //! ⚠️ `margins` feeds the WIDTH only (`implicitHeight` is `max(content,
                //! defaultButtonSize)`), so the 28px above still decides the height.
                margins: 10

                //! DSH fills the active chip and leaves the rest bare; hover is `FlatButton`'s own state,
                //! so it works on both without this file painting anything.
                backgroundRadius: 12
                transparent: root.activeId !== modelData.id
                //! ⚠️ The label is the ONLY name this control has, so it is set explicitly: `accessible`
                //! is what the automation tree reports, and it is not derived from `text` (第 129 条).
                accessible.name: modelData.label
                text: modelData.label

                onClicked: {
                    root.activated(modelData.id)
                }
            }
        }

        //! Pushes the chips to the strip's start, so a wide column does not stretch them.
        Item {
            Layout.fillWidth: true
        }
    }
}
