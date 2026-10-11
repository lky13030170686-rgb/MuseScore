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

    \b Why the columns are draggable NOW, and why that is not decoration. The first version shipped the
    proportions and no handle, on the argument that a drag changes nothing the page can DO. That was
    wrong for a page whose whole content is text: how much room the transcript gets is exactly the thing
    a user wants to change - the left column is a reference you read once, and the right column is a log
    you glance at, while the conversation is what you work in. The web client drags its boundaries and
    REMEMBERS them (第 129 条), and so does this: the widths are stored through `FieldController`, the
    same place the gateway and the model live.

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
    readonly property int sidebarDefaultWidth: 280
    readonly property int sidebarMinWidth: 240
    readonly property int sidebarMaxWidth: 380
    readonly property int rightbarDefaultWidth: 320
    readonly property int rightbarMinWidth: 280
    readonly property int rightbarMaxWidth: 420
    readonly property int centerMinWidth: 400

    //! The dragged width. ⚠️ Seeded from the stored preference ONCE: the moment the user drags, this
    //! becomes the authority (the binding is replaced by the assignment) and the new value is written
    //! back on release. That is the same one-way-then-authoritative shape the connection panel's fields
    //! use, and the reason it is not a two-way binding is that a two-way one would fight the drag.
    property int sidebarWidth: (root.field && root.field.agentSidebarWidth > 0)
                               ? root.field.agentSidebarWidth : root.sidebarDefaultWidth
    property int rightbarWidth: (root.field && root.field.agentRightbarWidth > 0)
                                ? root.field.agentRightbarWidth : root.rightbarDefaultWidth

    //! The centred content axis: the transcript column and (slightly wider) the composer card.
    readonly property real contentWidth: Math.max(520, Math.min(920, centerColumn.width * 0.72))

    readonly property bool running: root.field ? root.field.agentRunning : false

    //! The width each boundary had when the drag started. The handle reports the distance from the press,
    //! so the new width is `start + delta` - never `current + delta`, which would re-apply the whole drag
    //! on every move event and run the column away from the pointer.
    property int dragStartSidebarWidth: 0
    property int dragStartRightbarWidth: 0

    function beginDrag() {
        root.dragStartSidebarWidth = root.sidebarWidth
        root.dragStartRightbarWidth = root.rightbarWidth
    }

    //! Clamp and apply. The BOUNDS live here, next to the constants they come from - the handle and the
    //! stored preference both stay ignorant of them.
    function applySidebarDrag(delta) {
        const next = Math.max(root.sidebarMinWidth,
                              Math.min(root.sidebarMaxWidth, root.dragStartSidebarWidth + delta))
        root.sidebarWidth = next
    }

    function applyRightbarDrag(delta) {
        //! ⛔ NEGATED: the right column is on the RIGHT, so dragging its boundary left must make it WIDER.
        //! Without this the boundary moves the wrong way, which reads as a broken drag rather than as a
        //! sign error.
        const next = Math.max(root.rightbarMinWidth,
                              Math.min(root.rightbarMaxWidth, root.dragStartRightbarWidth - delta))
        root.rightbarWidth = next
    }

    //! Persist on RELEASE, not on every move: a drag emits a move per pixel, and writing the preference on
    //! each one would hammer the settings store for a value that is only meaningful when the hand stops.
    function commitWidths() {
        if (!root.field) {
            return
        }
        root.field.setAgentSidebarWidth(root.sidebarWidth)
        root.field.setAgentRightbarWidth(root.rightbarWidth)
    }

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

                //! Where the agent talks to, editable in the app. Before this, the gateway and the model
                //! could only be set by environment variables - so "can I use the agent" depended on who
                //! launched the process (第 127 条). The key stays in the composer and is never saved.
                AgentConnectionPanel {
                    Layout.fillWidth: true
                    field: root.field
                }

                //! The key's provenance still deserves a line: "it works on my machine" is usually an
                //! environment variable someone forgot they set, and a key that came from the environment
                //! cannot be cleared from here.
                StyledTextLabel {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    elide: Text.ElideRight
                    color: ui.theme.fontSecondaryColor
                    text: qsTrc("agentharness", "Key: %1")
                          .arg(root.field ? root.field.agentApiKeySource : "none")
                }
            }
        }

        //! ── The left boundary: drag it to give the conversation more (or less) room ───────────────
        //! `dragStart + delta`, never `current + delta`: the handle reports the distance from the press, so
        //! the width stays a pure function of where the pointer is.
        AgentColumnHandle {
            Layout.fillHeight: true

            onPressed: {
                root.beginDrag()
            }

            onMoved: function(delta) {
                root.applySidebarDrag(delta)
            }

            onReleased: {
                root.commitWidths()
            }
        }

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

                //! ── Header: what this session is working on ──────────────────────────────────────
                //! The score facts, plus a busy indicator while a turn runs. ⚠️ The Stop button is NOT
                //! here: it lives in the shared composer (`AgentComposer`), so the panel can stop a turn
                //! too - it had no way to before (第 127 条). One control, both surfaces.
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

        //! ── The right boundary ─────────────────────────────────────────────────────────────────────
        //! Same handle, opposite sense: this one widens the column to its RIGHT (`applyRightbarDrag`
        //! negates the delta), which is the whole reason the sign lives in the page and not in the handle.
        AgentColumnHandle {
            Layout.fillHeight: true

            onPressed: {
                root.beginDrag()
            }

            onMoved: function(delta) {
                root.applyRightbarDrag(delta)
            }

            onReleased: {
                root.commitWidths()
            }
        }

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
