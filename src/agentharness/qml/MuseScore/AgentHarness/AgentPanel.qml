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
import MuseScore.AgentHarness

/*!
    The Agent panel: a side dock on the notation page.

    \b Why a side dock and not a page. The whole point of the agent is that its edits land in the
    score the user is looking at. A separate main page would hide the score while the agent works,
    so the user would have to take the result on faith.

    \b What it shows today (M0/M1). The information field, live: one row per recorded operation,
    with the undo-stack's own name for it. This is not decoration - it is the visible half of the
    M1 acceptance criterion ("change a few notes by hand and the field's operations line up with
    what you did"). The chat half arrives with the agent loop in M2; the panel is laid out to take
    it without restructuring.
*/
Item {
    id: root

    property alias navigationSection: navPanel.section
    property alias contentNavigationPanelOrderStart: navPanel.order

    //! The information field, created at window level (see WindowContent.qml) and passed down.
    //! Deliberately NOT created here: `DockPanel` only instantiates its content while the panel is
    //! visible, so a field owned by this panel would begin recording the moment the user opened
    //! the panel - i.e. it would be missing exactly the history they opened it to ask about.
    property var field

    NavigationPanel {
        id: navPanel
        name: "AgentHarnessSection"
        direction: NavigationPanel.Vertical
        enabled: root.enabled && root.visible
    }

    Column {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        //! ── Header: what the field is looking at right now ────────────────────────────
        StyledTextLabel {
            width: parent.width
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            text: root.field ? root.field.statusText : ""
        }

        //! ── The record itself, newest first ───────────────────────────────────────────
        StyledListView {
            id: eventsView

            width: parent.width
            height: parent.height - y - hintLabel.height - parent.spacing

            model: root.field ? root.field.recentEvents : []
            spacing: 2
            clip: true

            delegate: ListItemBlank {
                id: eventDelegate

                required property var modelData
                required property int index

                width: eventsView.width
                height: 34

                //! Undo and redo are marked by colour, not by a filled row: the text colour is
                //! what carries the meaning, and it keeps the list readable when several rows
                //! in a row are undos. NOTE: only colours verified to exist on `ui.theme` are
                //! used here - a typo in a theme property fails at runtime, not at build time.
                StyledTextLabel {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    horizontalAlignment: Text.AlignLeft
                    elide: Text.ElideRight
                    color: eventDelegate.modelData.isUndo ? ui.theme.buttonColor
                           : eventDelegate.modelData.isRedo ? ui.theme.accentColor
                           : ui.theme.fontPrimaryColor
                    text: {
                        const e = eventDelegate.modelData
                        const tag = e.isUndo ? "[undo] " : e.isRedo ? "[redo] " : ""
                        const where = e.tickFrom >= 0
                            ? " @tick " + e.tickFrom + (e.tickTo > e.tickFrom ? ".." + e.tickTo : "")
                            : ""
                        return "#" + e.seq + "  " + tag + e.action + where
                    }
                }
            }
        }

        StyledTextLabel {
            id: hintLabel
            width: parent.width
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: root.field && root.field.hasScore
                  ? qsTrc("agentharness", "Recording every change to this score. Set MUSE_AGENT_FIELD_TRACE=1 to also write each one to the log.")
                  : qsTrc("agentharness", "Open a score to start recording.")
        }
    }
}
