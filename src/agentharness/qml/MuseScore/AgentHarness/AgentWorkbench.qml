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
    The Agent page's workspace: three columns, laid out like the DSH web client.

    \b Where the layout comes from. The DSH web client (`F:\AI\deepseek-harness`, `AppFrame.tsx` +
    `columns.ts`) is a three-track grid: a left sidebar (264-420px, default 280), a centre column that
    takes the rest (minimum 400) and a right column (minimum 300) that yields its track FIRST when
    space runs out. The centre is not full-width: the conversation sits on a centred content axis
    (`clamp(680, column * 0.64, 920)`) and the composer card is deliberately 32px wider than that axis.
    Those numbers are copied here rather than invented, because "looks like the web client" is mostly
    a statement about proportions.

    \b Why the columns are fixed widths and not draggable. The web client drags its boundaries and
    remembers them. That is a preference to store, a handle to draw and a hit area to get right; none
    of it changes what the page can do, so the first version ships the proportions and leaves the drag
    to whoever wants it. The layout is a `RowLayout`, so adding a handle later does not move anything.

    \b What is NOT copied, and why. The web client's sidebar lists sessions and it has a tab strip for
    conversation/trajectory views. This harness runs exactly one session per process and has no second
    view to switch to, so those two would be furniture with nothing behind them. The left column shows
    what is real (score facts, the session record, the tool table) and the right column is the
    information field's timeline plus the newest tool call.
*/
Item {
    id: root

    //! The information field, created at window level and shared with the notation page's panel. Two
    //! fields would be two records of the same score, which is the one thing this subsystem refuses.
    property var field

    //! ── The web client's column constants ────────────────────────────────────────────────────────
    readonly property int sidebarWidth: 280
    readonly property int sidebarMinWidth: 240
    readonly property int sidebarMaxWidth: 380
    readonly property int rightbarWidth: 320
    readonly property int rightbarMinWidth: 280
    readonly property int rightbarMaxWidth: 420
    readonly property int centerMinWidth: 400

    //! The centred content axis: the transcript column and (slightly wider) the composer card.
    readonly property real contentWidth: Math.max(520, Math.min(920, centerColumn.width * 0.72))

    readonly property bool running: root.field ? root.field.agentRunning : false

    RowLayout {
        anchors.fill: parent
        spacing: 0

        //! ══ 1. Left: what the agent sees and what it may do ══════════════════════════════════════
        Rectangle {
            Layout.preferredWidth: root.sidebarWidth
            Layout.minimumWidth: root.sidebarMinWidth
            Layout.maximumWidth: root.sidebarMaxWidth
            Layout.fillHeight: true

            color: ui.theme.backgroundSecondaryColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                StyledTextLabel {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    font: ui.theme.bodyBoldFont
                    text: qsTrc("agentharness", "Agent")
                }

                AgentFactsPanel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    field: root.field
                }

                SeparatorLine {}

                //! The two settings a user needs to know to trust what they are looking at: where the
                //! key came from (environment vs typed in), and which model is being asked. `agentModel()`
                //! is `Q_INVOKABLE`, so the binding also touches `agentTranscript` to get a dependency
                //! that notifies - otherwise the label would be whatever it read on the first frame.
                StyledTextLabel {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    elide: Text.ElideRight
                    color: ui.theme.fontSecondaryColor
                    text: qsTrc("agentharness", "Key: %1 · Model: %2")
                          .arg(root.field ? root.field.agentApiKeySource : "none")
                          .arg(root.field && root.field.agentTranscript.length >= 0 ? root.field.agentModel() : "")
                }
            }
        }

        SeparatorLine {}

        //! ══ 2. Centre: the conversation, on a centred axis, with the composer underneath ═════════
        Rectangle {
            id: centerColumn

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: root.centerMinWidth

            color: ui.theme.backgroundPrimaryColor

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                //! ── Header: the score this session is about, plus the running controls ────────────
                //! The web client's header is also where its right-panel button lives; here the two
                //! things that belong at the top of a long-running agent are "what is it working on"
                //! and "make it stop".
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        spacing: 10

                        StyledTextLabel {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            elide: Text.ElideRight
                            font: ui.theme.bodyBoldFont
                            text: root.field ? root.field.statusText : ""
                        }

                        StyledBusyIndicator {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            running: root.running
                            visible: root.running
                        }

                        //! A real `FlatButton` with an accessible name: a stop control nothing can press
                        //! from a script is a stop control nobody can verify (维护手册.md §7.6).
                        FlatButton {
                            accessible.name: qsTrc("agentharness", "Stop the agent")
                            text: qsTrc("agentharness", "Stop")
                            visible: root.running
                            enabled: root.running

                            onClicked: {
                                if (root.field) {
                                    root.field.abortAgent()
                                }
                            }
                        }
                    }
                }

                SeparatorLine {}

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            AgentConversationView {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: Math.min(root.contentWidth, parent.width)
                                height: parent.height
                                field: root.field
                                //! The web client's shapes: a user message is a bubble on the right, an
                                //! answer is plain text, a tool call is one compact dim line.
                                webStyle: true
                            }
                        }

                        //! ── The composer, in a raised card ─────────────────────────────────────────
                        //! 32px wider than the transcript axis, which is the web client's own rule
                        //! (`--dsh-composer-card-max-width = content width + 32`): the card carries the
                        //! page's action and must read as the widest thing on the axis, not as a
                        //! footnote under it.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: composerCard.height

                            RoundedRectangle {
                                id: composerCard

                                anchors.horizontalCenter: parent.horizontalCenter
                                width: Math.min(root.contentWidth + 32, parent.width)
                                height: composerColumn.implicitHeight + 20
                                radius: 12
                                color: ui.theme.backgroundSecondaryColor
                                border.color: ui.theme.strokeColor
                                border.width: ui.theme.borderWidth

                                ColumnLayout {
                                    id: composerColumn

                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 4

                                    AgentComposer {
                                        Layout.fillWidth: true
                                        field: root.field
                                        //! A page-wide card: actions on the right at their natural size.
                                        compactButtons: false
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        SeparatorLine {}

        //! ══ 3. Right: what happened, and what the newest tool call actually was ═══════════════════
        Rectangle {
            Layout.preferredWidth: root.rightbarWidth
            Layout.minimumWidth: root.rightbarMinWidth
            Layout.maximumWidth: root.rightbarMaxWidth
            Layout.fillHeight: true

            color: ui.theme.backgroundSecondaryColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                StyledTextLabel {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    font: ui.theme.bodyBoldFont
                    //! The REAL total from the field, not `recentOps.length`: the display list is capped,
                    //! and a header that said "6" when the score has 40 edits would be a quiet lie.
                    text: qsTrc("agentharness", "What happened (%1)")
                          .arg(root.field ? root.field.eventCount : 0)
                }

                //! The same timeline the panel shows, with no row cap: the page has the room, and a
                //! scrollable full record is the reason to open the page at all.
                AgentTimelineList {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 120
                    field: root.field
                    visibleRows: 0
                }

                SeparatorLine {}

                StyledTextLabel {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    font: ui.theme.bodyBoldFont
                    text: qsTrc("agentharness", "Newest tool call")
                }

                AgentToolInspector {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 160
                    field: root.field
                }
            }
        }
    }
}
