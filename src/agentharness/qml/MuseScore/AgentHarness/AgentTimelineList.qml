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
    The information field's timeline: what has happened to this score, newest first.

    \b Why it is a component. Same reason as AgentConversationView.qml: the notation page's panel and
    the Agent page show the same record, and one of them having its own list is how the two start
    disagreeing. They differ only in room - the panel shows a handful of rows because it is a glance
    surface, the page lets the list take what it is given.

    \b Two sizing modes, and the caller picks. `visibleRows > 0` sizes the list to that many rows (the
    panel: "a reference you glance at", so it must not push the conversation down to nothing).
    `visibleRows == 0` fills whatever the caller allocates and scrolls (the page: the timeline is one
    of its three columns).
*/
Item {
    id: root

    //! The information field (created at window level - see WindowContent.qml).
    property var field

    //! Collapsed shows nothing at all - the panel's "What happened (N)" button owns that decision.
    property bool expanded: true
    //! Rows to show when sizing to content; 0 = fill the caller's space instead.
    property int visibleRows: 6

    readonly property int rowHeight: 30
    //! The REAL total, not `recentOps.length`: the model list is capped for display, and a label that
    //! said "6" when the score has 40 edits would be a quiet lie.
    readonly property int opCount: root.field ? root.field.eventCount : 0

    visible: root.expanded
    implicitHeight: root.expanded && root.visibleRows > 0
                    ? Math.min(root.visibleRows, Math.max(1, root.opCount)) * root.rowHeight
                    : 0

    StyledListView {
        id: timelineView

        anchors.fill: parent
        spacing: 0
        clip: true

        model: root.field ? root.field.recentOps : []

        delegate: AgentTimelineRow {
            required property var modelData

            width: timelineView.width
            line: modelData.line
            isUndo: modelData.isUndo === true
            isRedo: modelData.isRedo === true
        }
    }
}
