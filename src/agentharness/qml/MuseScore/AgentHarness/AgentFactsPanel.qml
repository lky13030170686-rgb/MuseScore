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
    The left column of the Agent page: which conversation you are in, what the agent is looking at,
    and what it is allowed to do.

    \b The ORDER is the point (第 148 条). DSH's sidebar runs brand -> New Session -> navigation ->
    the session list -> the foot: the list is what the column is FOR and everything else is reference.
    Ours had it third, below five fact rows, and below a block of debug text - so "which conversation
    am I in" was the fourth thing down the column while the model's own projection of the session
    (eight lines of system prompt) sat in the middle of the first screen. That projection is a
    debugging aid; it is now FOLDED, closed by default, together with the tool list.

    \b Why the two folded sections are foldable at all rather than deleted. Both answer real
    questions - "what did the model actually get" and "what is this thing able to do at all" - and
    the second one is the visible half of the subsystem's central structural claim (`tools.h`: the
    agent can only do what the table exposes). They are just not what you open the page to look at,
    and DSH's answer to the same problem is a collapsed disclosure row, so that is the shape here.

    \b Why the values are read through a dependency. `availableTools()`, `sessionEventCount()` and
    `sessionPreview()` are `Q_INVOKABLE`, and an invokable has no notify signal: a binding that calls
    one and depends on nothing else is evaluated ONCE and then quietly goes stale. The readings below
    therefore also touch a property that does notify (`field`, `field.eventCount`,
    `field.agentTranscript`), which is what makes them re-evaluate.
*/
Item {
    id: root

    property var field

    //! Whether the two reference sections are open. Both start CLOSED - see the note above: they
    //! answer questions you arrive with, not questions the page asks you.
    property bool modelViewExpanded: false
    property bool toolsExpanded: false

    //! The two disclosure labels, defined ONCE and read from both the button and its content item.
    //! ⛔ Why they live on the file root: an inline `Component` gets its own context, and 第 132 条
    //! measured what that costs - a delegate's ids are invisible inside one, and the rows came out
    //! blank with a `ReferenceError` per row. What IS known to work is reaching the FILE ROOT
    //! (`AgentToolGroupRow.qml` does exactly that and is verified on screen). Reaching a SIBLING id
    //! from inside the component is the case nobody has measured, so this file does not rely on it -
    //! and defining the text once still keeps the button and its label from disagreeing.
    readonly property string modelViewLabel: qsTrc("agentharness", "Messages the model received")
    readonly property string toolsLabel: qsTrc("agentharness", "Tools (%1)").arg(root.tools.length)

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

    //! A section heading. One step down from the body size (DSH's sidebar labels run at its
    //! secondary tier, `13/20`) and in the secondary ink: it is a divider between blocks of facts,
    //! not a title competing with them.
    component SectionTitle: StyledTextLabel {
        Layout.fillWidth: true
        Layout.topMargin: 6
        horizontalAlignment: Text.AlignLeft
        color: ui.theme.fontSecondaryColor
        font.pixelSize: ui.theme.bodyFont.pixelSize - 1
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

            //! ══ 1. Which conversation you are in ═══════════════════════════════════════════════
            //! First, because it decides what every row below describes (第 141 条 built the list;
            //! 第 148 条 moved it to where DSH puts it).
            AgentSessionList {
                Layout.fillWidth: true
                field: root.field
            }

            SeparatorLine {
                Layout.topMargin: 4
            }

            //! ══ 2. What the agent is looking at ════════════════════════════════════════════════
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
                //! ⛔ `revision` IS the fence - the same number `score_revision` returns and `expectRevision`
                //! compares (第 140 条). It used to be a private event counter wearing this name, so the
                //! number shown here was one a write would have refused.
                label: qsTrc("agentharness", "Revision (write fence)")
                value: root.field ? String(root.field.revision) : "—"
            }

            FactRow {
                //! The other number, and it is NOT a revision: how many raw events this field has recorded.
                //! It keeps climbing when the revision goes back down (undo), which is why the two rows exist
                //! side by side instead of one line printing the same value twice.
                label: qsTrc("agentharness", "Recorded edits")
                value: root.field ? String(root.field.eventCount) : "—"
            }

            //! ══ 3. What the model actually received (folded) ═══════════════════════════════════
            //! ⛔ This block used to be ALWAYS OPEN and it was the biggest thing on the first screen:
            //! the projection prints the system prompt and the user messages back, which is eight
            //! lines of text that a user reads once and then scrolls past forever. Folding it is how
            //! DSH treats the same class of content (a collapsed disclosure row).
            FlatButton {
                id: modelViewToggle

                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.preferredHeight: 24
                margins: 6
                backgroundRadius: 6
                transparent: true
                accessible.name: root.modelViewExpanded
                                 ? qsTrc("agentharness", "Hide what the model received")
                                 : qsTrc("agentharness", "Show what the model received")
                toolTipTitle: qsTrc("agentharness", "The session record, as the model sees it")
                text: root.modelViewLabel

                onClicked: {
                    root.modelViewExpanded = !root.modelViewExpanded
                }

                //! A custom content item because `FlatButton` centres its label and this line is a
                //! section header: it reads left-aligned, in the same column as the rows it stands for.
                //! ⚠️ It reaches out to the FILE ROOT (`root.modelViewExpanded`), never to a delegate's
                //! id: an inline `Component` has its own context and a delegate's ids are not visible
                //! inside it (第 132 条 measured exactly that). This is the same shape as
                //! `AgentToolGroupRow.qml`, which is the version that works.
                contentItem: Component {
                    RowLayout {
                        //! ⛔ WITHOUT THIS LINE THE LABEL IS CENTRED. `FlatButton` positions its content
                        //! with `anchors.horizontalCenter`, so the item is only as wide as its own content
                        //! and a `Text.AlignLeft` inside it has nothing to align against - the first
                        //! version drew both disclosure labels in the middle of the column, which is not
                        //! what a list header looks like.
                        //! ⚠️ `modelViewToggle.width` is a SIBLING id reached from inside an inline
                        //! `Component`. 第 132 条's failure was with a DELEGATE's ids, which are genuinely
                        //! invisible there; this one is measured working (the labels render with the right
                        //! text, and `ui-probe` clicks them by their accessible names).
                        //! ⚠️ `- 12` keeps the 6px `margins` on both sides: the item is centred inside the
                        //! button, so half the difference lands on each side.
                        width: modelViewToggle.width - 12
                        spacing: 6

                        StyledIconLabel {
                            Layout.alignment: Qt.AlignVCenter
                            iconCode: root.modelViewExpanded ? IconCode.SMALL_ARROW_DOWN
                                                             : IconCode.SMALL_ARROW_RIGHT
                            color: ui.theme.fontSecondaryColor
                        }

                        StyledTextLabel {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            horizontalAlignment: Text.AlignLeft
                            elide: Text.ElideRight
                            color: ui.theme.fontSecondaryColor
                            text: modelViewToggle.text
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.modelViewExpanded
                spacing: 3

                //! ⛔ WHAT IS TRUE ABOUT THEIR LIFETIME, said where the list is. 第 141 条 wrote "nothing
                //! is written to disk" here; 第 142 条 made that false, and a sentence like that has to be
                //! corrected in the place a user reads it - one file per conversation, and deleting the
                //! file is how a user deletes the conversation (there is no manifest to disagree with that).
                StyledTextLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: ui.theme.fontSecondaryColor
                    font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                    text: qsTrc("agentharness", "Kept between runs - one file per session, in the application data folder.")
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
            }

            //! ══ 4. What it is allowed to do (folded) ═══════════════════════════════════════════
            FlatButton {
                id: toolsToggle

                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.preferredHeight: 24
                margins: 6
                backgroundRadius: 6
                transparent: true
                accessible.name: root.toolsExpanded
                                 ? qsTrc("agentharness", "Hide the list of tools")
                                 : qsTrc("agentharness", "Show the list of tools")
                toolTipTitle: qsTrc("agentharness", "The tools this agent is allowed to call")
                text: qsTrc("agentharness", "Tools (%1)").arg(root.tools.length)

                onClicked: {
                    root.toolsExpanded = !root.toolsExpanded
                }

                contentItem: Component {
                    RowLayout {
                        //! Same reason as the disclosure above: without an explicit width the label is
                        //! centred by `FlatButton`, not aligned by `Text.AlignLeft`.
                        width: toolsToggle.width - 12
                        spacing: 6

                        StyledIconLabel {
                            Layout.alignment: Qt.AlignVCenter
                            iconCode: root.toolsExpanded ? IconCode.SMALL_ARROW_DOWN : IconCode.SMALL_ARROW_RIGHT
                            color: ui.theme.fontSecondaryColor
                        }

                        StyledTextLabel {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            horizontalAlignment: Text.AlignLeft
                            elide: Text.ElideRight
                            color: ui.theme.fontSecondaryColor
                            text: toolsToggle.text
                        }
                    }
                }
            }

            //! ⛔ A `ListView`, NOT a `Repeater`. Inside a Layout a `Repeater` is itself the layout
            //! child, so its delegates get no layout-managed geometry: `Layout.fillWidth` on the
            //! delegate silently does nothing and every row lands at x = 0 with its implicit width.
            //! (Same family as the trap in 维护手册.md §4.2: a container that does not propagate the
            //! properties you think it propagates.)
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: root.toolsExpanded
                                        ? Math.min(10, Math.max(1, root.tools.length)) * 18
                                        : 0
                visible: root.toolsExpanded
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
                    font.pixelSize: ui.theme.bodyFont.pixelSize - 1
                    text: "· " + modelData
                }
            }
        }
    }
}
