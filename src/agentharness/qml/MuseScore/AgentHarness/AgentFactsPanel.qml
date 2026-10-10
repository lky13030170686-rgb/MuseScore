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
    The left column of the Agent page: what the agent is looking at, and what it can do.

    \b Why these three sections and not a session list. The DSH web client's sidebar lists sessions;
    this harness has exactly ONE session per run (the information field is bound to the open score and
    the session log is per process), so a list of sessions would be an invented feature. What is real,
    and what a user opening a "detailed editing window" wants, is: the score facts the agent reads, the
    size of the record it keeps, and the tool table it is allowed to call.

    \b Why the tool list is here at all. "The agent can only do what the table exposes" is the
    subsystem's central structural claim (`tools.h`), and until now it was only visible in the source.
    Showing the names is how a user can tell what the agent is able to do without reading the code.

    \b Why the values are read through a dependency. `availableTools()`, `sessionEventCount()` and
    `sessionPreview()` are `Q_INVOKABLE`, and an invokable has no notify signal: a binding that calls
    one and depends on nothing else is evaluated ONCE and then quietly goes stale. The readings below
    therefore also touch a property that does notify (`field`, `field.eventCount`,
    `field.agentTranscript`), which is what makes them re-evaluate.
*/
Item {
    id: root

    property var field

    readonly property var tools: {
        //! `field.agentTranscript.length` is here to establish the dependency, not because it is used.
        //! Without it the tool list would be whatever it was on the first evaluation (see the note above).
        const unusedDependencyTick = root.field ? root.field.agentTranscript.length : 0
        return root.field && unusedDependencyTick >= 0 ? root.field.availableTools() : []
    }

    readonly property int sessionEvents: {
        const unusedDependencyTick = root.field ? root.field.eventCount : 0
        return root.field && unusedDependencyTick >= 0 ? root.field.sessionEventCount() : 0
    }

    readonly property string sessionProjection: {
        const unusedDependencyTick = root.field ? root.field.agentTranscript.length : 0
        return root.field && unusedDependencyTick >= 0 ? root.field.sessionPreview() : ""
    }

    //! A label + value row, used by every section. Two inline components instead of one reusable file
    //! because they are only ever used here (and a file for a two-label row is a file nobody finds).
    component FactRow: RowLayout {
        Layout.fillWidth: true
        spacing: 8

        property string label: ""
        property string value: ""

        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            elide: Text.ElideRight
            color: ui.theme.fontSecondaryColor
            text: parent.label
        }

        StyledTextLabel {
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            text: parent.value
        }
    }

    component SectionTitle: StyledTextLabel {
        Layout.fillWidth: true
        Layout.topMargin: 6
        horizontalAlignment: Text.AlignLeft
        color: ui.theme.fontSecondaryColor
        text: ""
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: content

            width: parent.width
            spacing: 3

            SectionTitle {
                text: qsTrc("agentharness", "SCORE")
            }

            FactRow {
                label: qsTrc("agentharness", "Name")
                value: root.field ? (root.field.scoreName.length > 0 ? root.field.scoreName : "—") : "—"
            }

            FactRow {
                label: qsTrc("agentharness", "Measures")
                value: root.field ? String(root.field.measureCount) : "—"
            }

            FactRow {
                label: qsTrc("agentharness", "Staves")
                value: root.field ? String(root.field.staffCount) : "—"
            }

            FactRow {
                //! The revision is the number a write can be fenced against (`expectRevision`), so it is
                //! shown as the score's version rather than as an internal counter.
                label: qsTrc("agentharness", "Revision")
                value: root.field ? String(root.field.revision) : "—"
            }

            FactRow {
                label: qsTrc("agentharness", "Recorded edits")
                value: root.field ? String(root.field.eventCount) : "—"
            }

            SectionTitle {
                text: qsTrc("agentharness", "SESSION LOG")
            }

            FactRow {
                label: qsTrc("agentharness", "Events")
                value: String(root.sessionEvents)
            }

            //! The model's own view of the record: which events project to which messages. It is the
            //! answer to "what did the agent actually see", and it is derived, never stored twice.
            StyledTextLabel {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(140, implicitHeight)
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignTop
                wrapMode: Text.WordWrap
                clip: true
                color: ui.theme.fontSecondaryColor
                text: root.sessionProjection
            }

            SectionTitle {
                text: qsTrc("agentharness", "TOOLS (%1)").arg(root.tools.length)
            }

            //! ⛔ A `ListView`, NOT a `Repeater`. Inside a Layout a `Repeater` is itself the layout
            //! child, so its delegates get no layout-managed geometry: `Layout.fillWidth` on the
            //! delegate silently does nothing and every row lands at x = 0 with its implicit width.
            //! (Same family as the trap in 维护手册.md §4.2: a container that does not propagate the
            //! properties you think it propagates.)
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(10, Math.max(1, root.tools.length)) * 18
                clip: true
                interactive: contentHeight > height
                model: root.tools

                delegate: StyledTextLabel {
                    required property var modelData

                    width: ListView.view.width
                    height: 18
                    horizontalAlignment: Text.AlignLeft
                    elide: Text.ElideRight
                    color: ui.theme.fontSecondaryColor
                    text: "· " + modelData
                }
            }
        }
    }
}
