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

    //! The width the user dragged to, or -1 while the stored preference (then the default) decides.
    //! ⛔ A SEPARATE "dragged" value rather than assigning over the effective one: assigning to a property
    //! breaks its binding, and that binding is what lets "reset the preferences" put the columns back.
    //! (第 129 条 assigned straight to `sidebarWidth`; the reset added in 第 130 条 is why this changed.)
    property int draggedSidebarWidth: -1
    property int draggedRightbarWidth: -1

    readonly property int sidebarWidth: root.draggedSidebarWidth >= 0
                                        ? root.draggedSidebarWidth
                                        : (root.field && root.field.agentSidebarWidth > 0
                                           ? root.field.agentSidebarWidth : root.sidebarDefaultWidth)
    readonly property int rightbarWidth: root.draggedRightbarWidth >= 0
                                         ? root.draggedRightbarWidth
                                         : (root.field && root.field.agentRightbarWidth > 0
                                            ? root.field.agentRightbarWidth : root.rightbarDefaultWidth)

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
        root.draggedSidebarWidth = Math.max(root.sidebarMinWidth,
                                            Math.min(root.sidebarMaxWidth, root.dragStartSidebarWidth + delta))
    }

    function applyRightbarDrag(delta) {
        //! ⛔ NEGATED: the right column is on the RIGHT, so dragging its boundary left must make it WIDER.
        //! Without this the boundary moves the wrong way, which reads as a broken drag rather than as a
        //! sign error.
        root.draggedRightbarWidth = Math.max(root.rightbarMinWidth,
                                             Math.min(root.rightbarMaxWidth, root.dragStartRightbarWidth - delta))
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

    //! Drop the local drag values, so the effective widths fall back to the stored preference (which the
    //! settings dialog has just set to "never dragged") and then to the defaults. ⛔ This has to happen
    //! HERE and not in the dialog: the widths are this page's live layout state, and a stored preference
    //! that changed while the page kept its old numbers would be exactly the "it says it reset and
    //! nothing moved" failure this project keeps refusing.
    function resetLayout() {
        root.draggedSidebarWidth = -1
        root.draggedRightbarWidth = -1
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

                //! ── The settings trigger, at the FOOT of the column (第 130 条) ────────────────────
                //! DSH puts it exactly here: a sidebar-foot row - a full-width button with the gear and
                //! the label, and a connection indicator at its right end. ⚠️ The gateway and model
                //! FIELDS moved into the settings panel (they are settings), but their panel did not: the
                //! dialog embeds this very component, so there is still one place that knows how to apply
                //! and read them back. Showing both would be two live editors of one value.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    FlatButton {
                        id: settingsTrigger

                        Layout.fillWidth: true
                        accessible.name: qsTrc("agentharness", "Agent settings")
                        text: qsTrc("agentharness", "Settings")
                        icon: IconCode.SETTINGS_COG
                        transparent: true

                        onClicked: {
                            settingsDialog.open()
                        }
                    }

                    //! ── The connection indicator (DSH's `ConnectionIndicator`) ─────────────────────
                    //! DSH shows it next to the trigger and only when something is worth saying: while a
                    //! turn runs, or when the last one failed. ⚠️ Ours is the same idea with the states
                    //! this program actually has - and it is a real button, so "open the connection
                    //! settings" is reachable instead of being a coloured dot.
                    FlatButton {
                        id: connectionIndicator

                        readonly property bool hasProblem:
                            root.field ? root.field.agentLastError.length > 0 : false

                        visible: root.running || connectionIndicator.hasProblem
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        backgroundRadius: 28
                        transparent: true
                        icon: connectionIndicator.hasProblem ? IconCode.INFO : IconCode.CLOCK
                        //! ⚠️ `ui.theme` has no error colour (themeapi.h)，so a problem is shown with
                        //! the same token the transcript's error rows use (`buttonColor` on the accent
                        //! side) plus the INFO glyph - inventing a colour name would compile and then
                        //! fail at runtime (第 124 条的教训).
                        iconColor: connectionIndicator.hasProblem ? ui.theme.buttonColor
                                                                  : ui.theme.accentColor
                        accessible.name: root.running
                                         ? qsTrc("agentharness", "The agent is working")
                                         : qsTrc("agentharness", "The last turn failed - open the settings")
                        toolTipTitle: root.running
                                      ? qsTrc("agentharness", "Working…")
                                      : (root.field ? root.field.agentLastError : "")

                        onClicked: {
                            settingsDialog.activeSection = "connection"
                            settingsDialog.open()
                        }
                    }
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

    //! ══ The settings surface ══════════════════════════════════════════════════════════════════════
    //! One instance per surface, both reading the same field: the dialog is a VIEW of the settings, not a
    //! second copy of them (which is the rule this subsystem applies to the transcript and the timeline as
    //! well). ⚠️ `layoutResettable` is true only here, because the widths are this page's live state - the
    //! panel's dialog can change the stored preference but cannot move columns it does not own.
    AgentSettingsDialog {
        id: settingsDialog

        field: root.field
        layoutResettable: true

        onLayoutResetRequested: {
            root.resetLayout()
        }
    }
}
