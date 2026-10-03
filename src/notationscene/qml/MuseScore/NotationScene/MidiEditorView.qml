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

//! NOTE: The piano roll of the "MIDI" page (musescore://midi).
//!
//! It draws the notes of the very same score the notation page shows, as blocks on a
//! pitch/time grid, and writes edits back through MidiEditorModel.
//!
//! Two rules are borrowed from the audio lane drag (see 音频轨/notes):
//!   * a drag never touches the score - it only moves a local preview;
//!   * the edit is submitted once, on mouse release.

pragma ComponentBehavior: Bound

import QtQuick

import Muse.Ui

Item {
    id: root

    property var model: null

    // ── view parameters ──────────────────────────────────────────────────────
    property real rowHeight: 12
    property real pixelsPerTick: 0.09
    property real scrollX: 0
    property real scrollY: 0
    property bool velocityLaneVisible: true

    readonly property real keyboardWidth: 70
    readonly property real rulerHeight: 24
    readonly property real toolBarHeight: 36
    readonly property real velocityLaneHeight: velocityLaneVisible ? 96 : 0

    // ── model ────────────────────────────────────────────────────────────────
    readonly property bool hasScore: model !== null && model.hasScore
    readonly property int lowestPitch: hasScore ? model.lowestPitch : 48
    readonly property int highestPitch: hasScore ? model.highestPitch : 84
    readonly property int totalTicks: hasScore ? Math.max(1920, model.totalTicks) : 1920

    //! NOTE: cached on purpose - `model.notes` hands out a copy of the whole list on every read.
    readonly property var notes: hasScore ? model.notes : []
    readonly property var measures: hasScore ? model.measures : []
    readonly property real playbackTick: model !== null ? model.playbackTick : 0
    readonly property bool isPlaying: model !== null && model.isPlaying

    // ── geometry ─────────────────────────────────────────────────────────────
    readonly property real viewportWidth: Math.max(1, rollArea.width)
    readonly property real viewportHeight: Math.max(1, rollArea.height - rulerHeight)
    readonly property real contentWidth: totalTicks * pixelsPerTick
    readonly property real contentHeight: (highestPitch - lowestPitch + 1) * rowHeight
    readonly property real maxScrollX: Math.max(0, contentWidth - viewportWidth)
    readonly property real maxScrollY: Math.max(0, contentHeight - viewportHeight)

    // ── theme ────────────────────────────────────────────────────────────────
    readonly property color backgroundColor: ui.theme.backgroundPrimaryColor
    readonly property color panelColor: ui.theme.backgroundSecondaryColor
    readonly property color gridColor: ui.theme.strokeColor
    readonly property color barLineColor: ui.theme.fontSecondaryColor
    readonly property color textColor: ui.theme.fontPrimaryColor
    readonly property color dimTextColor: ui.theme.fontSecondaryColor
    readonly property color noteColor: ui.theme.accentColor
    readonly property color cursorColor: ui.theme.fontPrimaryColor

    //! NOTE: one colour per staff so that several instruments stay distinguishable,
    //!       the same idea as Dorico's track colours.
    readonly property var staffPalette: [
        "#4a9eff", "#ff9f4a", "#4ade80", "#f472b6",
        "#a78bfa", "#facc15", "#22d3ee", "#fb7185"
    ]

    // ── interaction state (preview only, never written to the score mid-drag) ──
    property int dragNoteIndex: -1
    property int dragStartPitch: 0
    property int dragPreviewPitch: -1
    property real dragStartY: 0

    //! What the current drag edits. Dorico draws the played extent over the notated one; grabbing the
    //! right edge of a block edits the played length, Shift+dragging edits the played start, and a
    //! plain drag still edits the pitch.
    readonly property int dragModePitch: 0
    readonly property int dragModePlayStart: 1
    readonly property int dragModePlayLength: 2
    property int dragMode: 0

    property real dragStartX: 0
    property int dragStartPlayStart: 0
    property int dragStartPlayDuration: 0
    property int dragPreviewPlayStart: 0
    property int dragPreviewPlayDuration: 0

    //! Drag snapping for the played layer, in ticks (1/32 of a whole note at 480 ticks per quarter).
    readonly property int playSnapTicks: 60

    property int hoveredNoteIndex: -1

    property int velocityDragTick: -1
    property bool velocityDragging: false
    //! Which staff the current velocity drag edits. The lane is split into one horizontal band per
    //! staff, so a drag must never touch another staff's notes that happen to sit on the same tick.
    property int velocityDragStaff: -1

    //! One band per staff. Without this, every staff's notes pile onto the same bar and a single
    //! drag edits all of them at once - which is exactly what a multi-instrument score must not do.
    readonly property int staffCount: (model !== null && model.hasScore) ? Math.max(1, model.staffCount) : 1

    // ── helpers ──────────────────────────────────────────────────────────────
    function clamp(v, lo, hi) {
        return Math.max(lo, Math.min(hi, v))
    }

    function isBlackKey(pitch) {
        var pc = ((pitch % 12) + 12) % 12
        return pc === 1 || pc === 3 || pc === 6 || pc === 8 || pc === 10
    }

    function pitchName(pitch) {
        var names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        var pc = ((pitch % 12) + 12) % 12
        return names[pc] + (Math.floor(pitch / 12) - 1)
    }

    function staffColor(staffIndex) {
        return staffPalette[((staffIndex % staffPalette.length) + staffPalette.length) % staffPalette.length]
    }

    function xForTick(tick) {
        return tick * pixelsPerTick - scrollX
    }

    function tickForX(x) {
        return (x + scrollX) / pixelsPerTick
    }

    //! NOTE: yForPitch/pitchForY work in "roll content" coordinates, i.e. relative to the top of
    //!       the grid (inside the mouse area), NOT to the top of the view. The keyboard canvas adds
    //!       rulerHeight because the ruler occupies the top strip of the same column.
    function yForPitch(pitch) {
        return (highestPitch - pitch) * rowHeight - scrollY
    }

    function pitchForY(y) {
        return highestPitch - Math.floor((y + scrollY) / rowHeight)
    }

    function noteWidth(note) {
        //! NOTE: a minimum width so that very short notes stay clickable.
        return Math.max(3, note.durationTicks * pixelsPerTick - 1)
    }

    function noteHeight() {
        return Math.max(3, rowHeight - 1)
    }

    function repaintAll() {
        keyboardCanvas.requestPaint()
        gridCanvas.requestPaint()
        rulerCanvas.requestPaint()
        velocityCanvas.requestPaint()
    }

    function clampScroll() {
        //! NOTE: assigning through Math.max keeps the value inside the range without
        //!       fighting the binding system.
        var sx = clamp(scrollX, 0, maxScrollX)
        var sy = clamp(scrollY, 0, maxScrollY)
        if (sx !== scrollX) {
            scrollX = sx
        }
        if (sy !== scrollY) {
            scrollY = sy
        }
    }

    function zoomBy(factor, pivotX) {
        var tickAtPivot = tickForX(pivotX)
        var next = clamp(pixelsPerTick * factor, 0.01, 4.0)
        pixelsPerTick = next
        scrollX = tickAtPivot * next - pivotX
        clampScroll()
        repaintAll()
    }

    function fitToWidth() {
        if (!hasScore) {
            return
        }
        pixelsPerTick = clamp(viewportWidth / totalTicks, 0.01, 4.0)
        scrollX = 0
        repaintAll()
    }

    function noteIndexAt(x, y) {
        //! NOTE: iterate backwards because later notes are painted on top.
        for (var i = notes.length - 1; i >= 0; --i) {
            var note = notes[i]
            var pitch = (i === dragNoteIndex && dragPreviewPitch >= 0) ? dragPreviewPitch : note.pitch

            //! NOTE: the note is grabbable over the union of its two extents. The played bar can sit
            //!       entirely outside the notated block (a note pushed late, or played much longer),
            //!       and it would then be impossible to grab back.
            if (!hitHorizontally(note, x)) {
                continue
            }
            var ny = yForPitch(pitch)
            if (y >= ny && y <= ny + noteHeight()) {
                return i
            }
        }
        return -1
    }

    function hitHorizontally(note, x) {
        var nx = xForTick(note.tick)
        if (x >= nx && x <= nx + noteWidth(note)) {
            return true
        }

        if (!note.hasPlayOverride) {
            return false
        }

        var px = xForTick(note.playTick)
        return x >= px && x <= px + playedWidth(note)
    }

    function playedWidth(note) {
        return Math.max(2, note.playDurationTicks * pixelsPerTick - 1)
    }

    function velocityAt(x, staffIndex) {
        //! NOTE: the lane works per time position WITHIN one staff: a chord of that staff is edited
        //!       as a whole (which is what makes dragging usable), but another staff's notes that
        //!       happen to share the tick are left alone.
        var best = -1
        var bestDist = 6
        for (var i = 0; i < notes.length; ++i) {
            if (notes[i].staffIndex !== staffIndex) {
                continue
            }

            var dist = Math.abs(xForTick(notes[i].tick) - x)
            if (dist <= bestDist) {
                bestDist = dist
                best = notes[i].tick
            }
        }
        return best
    }

    function velocityLaneHeight() {
        return velocityCanvas.height / staffCount
    }

    function velocityLaneAt(y) {
        return clamp(Math.floor(y / velocityLaneHeight()), 0, staffCount - 1)
    }

    function velocityForY(y) {
        //! Measured inside the note's own band, so dragging to the top of any band means 127.
        var laneH = velocityLaneHeight()
        var inLane = (y - velocityLaneAt(y) * laneH) / laneH
        return clamp(Math.round((1.0 - inLane) * 127), 1, 127)
    }

    onScrollXChanged: repaintAll()
    onScrollYChanged: repaintAll()
    onRowHeightChanged: repaintAll()
    onPixelsPerTickChanged: repaintAll()
    onNotesChanged: repaintAll()
    onPlaybackTickChanged: repaintAll()
    onHeightChanged: repaintAll()
    onWidthChanged: repaintAll()

    // ── toolbar ──────────────────────────────────────────────────────────────
    Rectangle {
        id: toolBar

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.toolBarHeight

        color: root.panelColor

        Text {
            id: titleLabel

            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter

            text: root.hasScore
                  ? qsTrc("notationscene", "MIDI editor") + " — " + root.model.scoreName
                  : qsTrc("notationscene", "MIDI editor")

            color: root.textColor
            font: ui.theme.bodyFont
        }

        Text {
            anchors.left: titleLabel.right
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter

            visible: root.hasScore

            text: qsTrc("notationscene", "Drag the right edge of a note = played length · Shift+drag = played start · velocity lane: one band per staff, drag = own velocity, right-click = follow dynamics")

            color: root.dimTextColor
            font: ui.theme.bodyFont
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Repeater {
                model: [
                    { "label": "−", "tip": qsTrc("notationscene", "Zoom out") },
                    { "label": "+", "tip": qsTrc("notationscene", "Zoom in") },
                    { "label": qsTrc("notationscene", "Fit"), "tip": qsTrc("notationscene", "Fit to width") }
                ]

                Rectangle {
                    id: button

                    required property var modelData
                    required property int index

                    width: Math.max(28, buttonLabel.implicitWidth + 16)
                    height: 22
                    radius: 3
                    color: buttonMouse.containsMouse ? ui.theme.buttonColor : "transparent"
                    border.width: 1
                    border.color: root.gridColor

                    Text {
                        id: buttonLabel

                        anchors.centerIn: parent
                        text: button.modelData.label
                        color: root.textColor
                        font: ui.theme.bodyFont
                    }

                    MouseArea {
                        id: buttonMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (button.index === 0) {
                                root.zoomBy(1 / 1.25, root.viewportWidth / 2)
                            } else if (button.index === 1) {
                                root.zoomBy(1.25, root.viewportWidth / 2)
                            } else {
                                root.fitToWidth()
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: Math.max(28, velocityToggleLabel.implicitWidth + 16)
                height: 22
                radius: 3
                color: root.velocityLaneVisible ? ui.theme.buttonColor : "transparent"
                border.width: 1
                border.color: root.gridColor

                Text {
                    id: velocityToggleLabel

                    anchors.centerIn: parent
                    text: qsTrc("notationscene", "Velocity")
                    color: root.textColor
                    font: ui.theme.bodyFont
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.velocityLaneVisible = !root.velocityLaneVisible
                }
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: root.gridColor
        }
    }

    // ── roll ─────────────────────────────────────────────────────────────────
    Item {
        id: rollArea

        anchors.top: toolBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: velocityLane.top

        // piano keyboard (scrolls vertically only)
        Canvas {
            id: keyboardCanvas

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            width: root.keyboardWidth

            onPaint: {
                var ctx = getContext("2d")
                var w = width
                var h = height

                ctx.clearRect(0, 0, w, h)
                ctx.fillStyle = root.panelColor
                ctx.fillRect(0, 0, w, h)

                // ruler corner
                ctx.fillStyle = root.backgroundColor
                ctx.fillRect(0, 0, w, root.rulerHeight)

                ctx.save()
                ctx.beginPath()
                ctx.rect(0, root.rulerHeight, w, h - root.rulerHeight)
                ctx.clip()

                var whiteW = w * 0.62
                var blackW = w * 0.40

                for (var pitch = root.lowestPitch; pitch <= root.highestPitch; ++pitch) {
                    var y = root.yForPitch(pitch) + root.rulerHeight
                    if (y > h || y + root.rowHeight < root.rulerHeight) {
                        continue
                    }

                    var black = root.isBlackKey(pitch)
                    if (!black) {
                        ctx.fillStyle = "#f2f2f2"
                        ctx.fillRect(0, y, whiteW, Math.max(1, root.rowHeight - 1))
                        ctx.strokeStyle = "#9a9a9a"
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(0, y + root.rowHeight - 0.5)
                        ctx.lineTo(whiteW, y + root.rowHeight - 0.5)
                        ctx.stroke()

                        if (pitch % 12 === 0 && root.rowHeight >= 8) {
                            ctx.fillStyle = "#4a4a4a"
                            ctx.font = "9px sans-serif"
                            ctx.textAlign = "right"
                            ctx.textBaseline = "middle"
                            ctx.fillText(root.pitchName(pitch), whiteW - 3, y + root.rowHeight / 2)
                        }
                    }
                }

                for (var p2 = root.lowestPitch; p2 <= root.highestPitch; ++p2) {
                    if (!root.isBlackKey(p2)) {
                        continue
                    }
                    var y2 = root.yForPitch(p2) + root.rulerHeight
                    if (y2 > h || y2 + root.rowHeight < root.rulerHeight) {
                        continue
                    }
                    ctx.fillStyle = "#2b2b2b"
                    ctx.fillRect(0, y2, blackW, Math.max(1, root.rowHeight - 1))
                }

                ctx.restore()

                // separator
                ctx.strokeStyle = root.gridColor
                ctx.lineWidth = 1
                ctx.beginPath()
                ctx.moveTo(w - 0.5, 0)
                ctx.lineTo(w - 0.5, h)
                ctx.stroke()
            }
        }

        // ruler + grid + notes
        Item {
            id: rollBody

            anchors.top: parent.top
            anchors.left: keyboardCanvas.right
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            Canvas {
                id: rulerCanvas

                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: root.rulerHeight

                onPaint: {
                    var ctx = getContext("2d")
                    var w = width
                    var h = height

                    ctx.clearRect(0, 0, w, h)
                    ctx.fillStyle = root.backgroundColor
                    ctx.fillRect(0, 0, w, h)

                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(0, 0, w, h)
                    ctx.clip()

                    var spans = root.measures.length > 0
                                ? root.measures
                                : [{ "tick": 0, "endTick": root.totalTicks, "index": 0 }]

                    ctx.font = "9px sans-serif"
                    ctx.textAlign = "left"
                    ctx.textBaseline = "middle"

                    for (var i = 0; i < spans.length; ++i) {
                        var m = spans[i]
                        var x0 = root.xForTick(m.tick)
                        var x1 = root.xForTick(m.endTick)
                        if (x1 < 0 || x0 > w) {
                            continue
                        }

                        if (i % 2 === 1) {
                            ctx.fillStyle = root.panelColor
                            ctx.fillRect(x0, 0, x1 - x0, h)
                        }

                        ctx.strokeStyle = root.barLineColor
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(Math.round(x0) + 0.5, 0)
                        ctx.lineTo(Math.round(x0) + 0.5, h)
                        ctx.stroke()

                        if (x1 - x0 > 22) {
                            ctx.fillStyle = root.dimTextColor
                            ctx.fillText(String(m.index + 1), x0 + 4, h / 2)
                        }
                    }

                    ctx.restore()

                    ctx.strokeStyle = root.gridColor
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    ctx.moveTo(0, h - 0.5)
                    ctx.lineTo(w, h - 0.5)
                    ctx.stroke()
                }
            }

            Canvas {
                id: gridCanvas

                anchors.top: rulerCanvas.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                onPaint: {
                    var ctx = getContext("2d")
                    var w = width
                    var h = height

                    ctx.clearRect(0, 0, w, h)
                    ctx.fillStyle = root.backgroundColor
                    ctx.fillRect(0, 0, w, h)

                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(0, 0, w, h)
                    ctx.clip()

                    var rowH = root.rowHeight

                    // black-key rows
                    for (var pitch = root.lowestPitch; pitch <= root.highestPitch; ++pitch) {
                        if (!root.isBlackKey(pitch)) {
                            continue
                        }
                        var y = root.yForPitch(pitch)
                        if (y > h || y + rowH < 0) {
                            continue
                        }
                        ctx.fillStyle = "rgba(128,128,128,0.10)"
                        ctx.fillRect(0, y, w, Math.max(1, rowH - 1))
                    }

                    // horizontal separators (octaves only, to keep it readable)
                    ctx.strokeStyle = root.gridColor
                    ctx.lineWidth = 1
                    for (var p2 = root.lowestPitch; p2 <= root.highestPitch; ++p2) {
                        if (p2 % 12 !== 0) {
                            continue
                        }
                        var y2 = root.yForPitch(p2)
                        if (y2 < 0 || y2 > h) {
                            continue
                        }
                        ctx.beginPath()
                        ctx.moveTo(0, Math.round(y2) + 0.5)
                        ctx.lineTo(w, Math.round(y2) + 0.5)
                        ctx.stroke()
                    }

                    // vertical: measure lines
                    var spans = root.measures
                    for (var i = 0; i < spans.length; ++i) {
                        var x0 = root.xForTick(spans[i].tick)
                        if (x0 < 0 || x0 > w) {
                            continue
                        }
                        ctx.strokeStyle = root.barLineColor
                        ctx.beginPath()
                        ctx.moveTo(Math.round(x0) + 0.5, 0)
                        ctx.lineTo(Math.round(x0) + 0.5, h)
                        ctx.stroke()
                    }

                    // vertical: beat lines (quarter notes)
                    ctx.strokeStyle = root.gridColor
                    var beat = 480
                    var from = Math.floor(root.tickForX(0) / beat) * beat
                    var to = root.tickForX(w)
                    for (var t = from; t <= to; t += beat) {
                        var x = root.xForTick(t)
                        if (x < 0 || x > w) {
                            continue
                        }
                        ctx.beginPath()
                        ctx.moveTo(Math.round(x) + 0.5, 0)
                        ctx.lineTo(Math.round(x) + 0.5, h)
                        ctx.stroke()
                    }

                    // notes
                    var nh = root.noteHeight()
                    var minTick = root.tickForX(-8)
                    var maxTick = root.tickForX(w + 8)

                    for (var n = 0; n < root.notes.length; ++n) {
                        var note = root.notes[n]
                        if (note.tick > maxTick || note.tick + note.durationTicks < minTick) {
                            continue
                        }

                        var previewing = (n === root.dragNoteIndex && root.dragPreviewPitch >= 0)
                        var drawPitch = previewing ? root.dragPreviewPitch : note.pitch
                        var nx = root.xForTick(note.tick)
                        var ny = root.yForPitch(drawPitch)
                        if (ny > h || ny + nh < 0) {
                            continue
                        }

                        var nw = root.noteWidth(note)

                        //! NOTE: Dorico's distinction, in one block: the notated extent is drawn as an
                        //!       outline and the played extent as a solid bar on top of it. A note the
                        //!       user never touched has both at the same place, so it stays a plain
                        //!       solid block exactly as before.
                        var draggingPlay = (n === root.dragNoteIndex && root.dragMode !== root.dragModePitch)
                        var showingPlay = note.hasPlayOverride || draggingPlay
                        var playStart = draggingPlay ? root.dragPreviewPlayStart : note.playTick
                        var playDuration = draggingPlay ? root.dragPreviewPlayDuration : note.playDurationTicks

                        ctx.fillStyle = root.staffColor(note.staffIndex)

                        if (!showingPlay) {
                            ctx.globalAlpha = previewing ? 0.6 : 0.95
                            ctx.fillRect(nx, ny, nw, nh)
                            ctx.globalAlpha = 1.0
                        } else {
                            ctx.globalAlpha = 0.45
                            ctx.lineWidth = 1
                            ctx.strokeStyle = root.staffColor(note.staffIndex)
                            ctx.strokeRect(nx + 0.5, ny + 0.5, Math.max(1, nw - 1), Math.max(1, nh - 1))

                            var px = root.xForTick(playStart)
                            var pw = Math.max(2, playDuration * root.pixelsPerTick - 1)
                            ctx.globalAlpha = previewing ? 0.6 : 0.95
                            ctx.fillRect(px, ny + 1, pw, Math.max(1, nh - 2))
                            ctx.globalAlpha = 1.0
                        }

                        if (n === root.hoveredNoteIndex || previewing) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = 1
                            ctx.strokeRect(nx + 0.5, ny + 0.5, Math.max(1, nw - 1), Math.max(1, nh - 1))
                        }
                    }

                    // playback cursor
                    if (root.isPlaying) {
                        var cx = root.xForTick(root.playbackTick)
                        if (cx >= 0 && cx <= w) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = 2
                            ctx.beginPath()
                            ctx.moveTo(Math.round(cx) + 0.5, 0)
                            ctx.lineTo(Math.round(cx) + 0.5, h)
                            ctx.stroke()
                        }
                    }

                    ctx.restore()
                }
            }

            MouseArea {
                id: rollMouse

                anchors.top: rulerCanvas.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                hoverEnabled: true

                onPressed: function(mouse) {
                    var index = root.noteIndexAt(mouse.x, mouse.y)

                    if (index < 0) {
                        //! NOTE: only the miss is logged, so normal use stays quiet while a "the drag
                        //!       does nothing" report can still be diagnosed from the log file.
                        //!       `console.warn` on purpose: MuseScore records Qt warnings, not plain
                        //!       console.log output, so a log() here would never reach the log file.
                        console.warn("MidiEditorView: press missed at", mouse.x, mouse.y,
                                     "| notes", root.notes.length, "pitchRange", root.lowestPitch, "-", root.highestPitch,
                                     "rowHeight", root.rowHeight, "scrollY", root.scrollY,
                                     "mouseArea", width, "x", height)
                    }

                    root.dragNoteIndex = index
                    if (index >= 0) {
                        var grabbed = root.notes[index]
                        root.dragStartPitch = grabbed.pitch
                        root.dragPreviewPitch = root.dragStartPitch
                        root.dragStartY = mouse.y
                        root.dragStartX = mouse.x
                        root.dragStartPlayStart = grabbed.playTick
                        root.dragStartPlayDuration = grabbed.playDurationTicks
                        root.dragPreviewPlayStart = grabbed.playTick
                        root.dragPreviewPlayDuration = grabbed.playDurationTicks

                        //! NOTE: which part of the block was grabbed decides what the drag edits.
                        //!       The edge test uses the PLAYED bar, not the notated block: that bar is
                        //!       what the user sees on top and aims at, and the two only coincide when
                        //!       the note has no override at all.
                        var grabbedX = root.xForTick(grabbed.playTick)
                        var grabbedW = root.playedWidth(grabbed)
                        var edge = Math.max(4, Math.min(8, grabbedW * 0.25))
                        if (mouse.x >= grabbedX + grabbedW - edge) {
                            root.dragMode = root.dragModePlayLength
                        } else if (mouse.modifiers & Qt.ShiftModifier) {
                            root.dragMode = root.dragModePlayStart
                        } else {
                            root.dragMode = root.dragModePitch
                        }

                        gridCanvas.requestPaint()
                    }
                }

                onPositionChanged: function(mouse) {
                    if (!pressed) {
                        var hovered = root.noteIndexAt(mouse.x, mouse.y)
                        if (hovered !== root.hoveredNoteIndex) {
                            root.hoveredNoteIndex = hovered
                            gridCanvas.requestPaint()
                        }
                        return
                    }

                    if (root.dragNoteIndex < 0) {
                        return
                    }

                    if (root.dragMode === root.dragModePitch) {
                        var deltaRows = Math.round((mouse.y - root.dragStartY) / root.rowHeight)
                        var pitch = root.clamp(root.dragStartPitch - deltaRows, 0, 127)
                        if (pitch !== root.dragPreviewPitch) {
                            root.dragPreviewPitch = pitch
                            gridCanvas.requestPaint()
                        }
                        return
                    }

                    //! NOTE: the played layer snaps to `playSnapTicks`, so a drag lands on musical
                    //!       positions instead of on pixel noise.
                    var snap = root.playSnapTicks
                    var deltaTicks = Math.round((mouse.x - root.dragStartX) / root.pixelsPerTick / snap) * snap

                    if (root.dragMode === root.dragModePlayStart) {
                        var start = Math.max(0, root.dragStartPlayStart + deltaTicks)
                        if (start !== root.dragPreviewPlayStart) {
                            root.dragPreviewPlayStart = start
                            gridCanvas.requestPaint()
                        }
                        return
                    }

                    var duration = Math.max(snap, root.dragStartPlayDuration + deltaTicks)
                    if (duration !== root.dragPreviewPlayDuration) {
                        root.dragPreviewPlayDuration = duration
                        gridCanvas.requestPaint()
                    }
                }

                onReleased: function(mouse) {
                    if (root.dragNoteIndex >= 0 && root.dragNoteIndex < root.notes.length) {
                        var released = root.notes[root.dragNoteIndex]

                        if (root.dragMode === root.dragModePitch) {
                            if (root.dragPreviewPitch >= 0 && root.dragPreviewPitch !== root.dragStartPitch) {
                                //! NOTE: the single submission of the whole drag.
                                root.model.setNotePitch(root.dragNoteIndex, root.dragPreviewPitch)
                            }
                        } else if (root.dragPreviewPlayStart !== released.playTick
                                   || root.dragPreviewPlayDuration !== released.playDurationTicks) {
                            root.model.setNotePlayOverride(root.dragNoteIndex,
                                                           root.dragPreviewPlayStart,
                                                           root.dragPreviewPlayDuration,
                                                           released.playVelocityPercent)
                        }
                    }

                    root.dragNoteIndex = -1
                    root.dragPreviewPitch = -1
                    root.dragMode = root.dragModePitch
                    gridCanvas.requestPaint()
                }

                onExited: {
                    if (root.hoveredNoteIndex !== -1) {
                        root.hoveredNoteIndex = -1
                        gridCanvas.requestPaint()
                    }
                }

                onWheel: function(wheel) {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        root.zoomBy(wheel.angleDelta.y > 0 ? 1.15 : 1 / 1.15, wheel.x)
                        return
                    }

                    if (wheel.modifiers & Qt.ShiftModifier) {
                        root.scrollX = root.clamp(root.scrollX - wheel.angleDelta.y * 0.6, 0, root.maxScrollX)
                        return
                    }

                    root.scrollY = root.clamp(root.scrollY - wheel.angleDelta.y * 0.6, 0, root.maxScrollY)
                }
            }
        }

        // empty state
        Text {
            anchors.centerIn: parent
            visible: !root.hasScore
            text: qsTrc("notationscene", "Open a score to edit it as MIDI")
            color: root.dimTextColor
            font: ui.theme.bodyFont
        }
    }

    // ── velocity lane ────────────────────────────────────────────────────────
    Item {
        id: velocityLane

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.velocityLaneHeight
        visible: root.velocityLaneVisible

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: root.gridColor
        }

        Item {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: root.keyboardWidth

            Canvas {
                id: velocityCanvas

                anchors.fill: parent

                onPaint: {
                    var ctx = getContext("2d")
                    var w = width
                    var h = height

                    ctx.clearRect(0, 0, w, h)
                    ctx.fillStyle = root.backgroundColor
                    ctx.fillRect(0, 0, w, h)

                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(0, 0, w, h)
                    ctx.clip()

                    var minTick = root.tickForX(-8)
                    var maxTick = root.tickForX(w + 8)

                    //! NOTE: one horizontal band per staff. Alternate a faint tint and draw the
                    //! separators first, so an empty band is still visible and the user can tell
                    //! which staff a bar belongs to before touching it.
                    var laneCount = root.staffCount
                    var laneH = h / laneCount

                    for (var band = 0; band < laneCount; ++band) {
                        var bandTop = band * laneH

                        if (band % 2 === 1) {
                            ctx.fillStyle = root.panelColor
                            ctx.globalAlpha = 0.5
                            ctx.fillRect(0, bandTop, w, laneH)
                            ctx.globalAlpha = 1.0
                        }

                        if (band > 0) {
                            ctx.strokeStyle = root.gridColor
                            ctx.lineWidth = 1
                            ctx.beginPath()
                            ctx.moveTo(0, Math.round(bandTop) + 0.5)
                            ctx.lineTo(w, Math.round(bandTop) + 0.5)
                            ctx.stroke()
                        }
                    }

                    for (var i = 0; i < root.notes.length; ++i) {
                        var note = root.notes[i]
                        if (note.tick > maxTick || note.tick < minTick) {
                            continue
                        }

                        var lane = root.clamp(note.staffIndex, 0, laneCount - 1)
                        var laneBottom = (lane + 1) * laneH - 2

                        var x = root.xForTick(note.tick)
                        var barH = Math.max(1, (note.velocity / 127) * (laneH - 3))
                        var active = root.velocityDragging && root.velocityDragTick === note.tick
                                     && root.velocityDragStaff === note.staffIndex
                        var own = note.hasVelocityOverride

                        //! NOTE: a thin, faint bar means "this note has no velocity of its own, so it
                        //!       follows the dynamic marks (pp/ff, hairpins)" - which is the default
                        //!       for almost every note. A thick solid bar means the note was given its
                        //!       own velocity here, overriding the dynamics. Right-click clears it.
                        ctx.fillStyle = active ? root.cursorColor : root.staffColor(note.staffIndex)
                        ctx.globalAlpha = active ? 1.0 : (own ? 0.9 : 0.3)
                        ctx.fillRect(own || active ? x - 0.5 : x + 0.5, laneBottom - barH, own || active ? 4 : 2, barH)
                        ctx.globalAlpha = 1.0

                        // a small cap so an overridden note is recognisable even when short
                        if (own) {
                            ctx.fillStyle = root.cursorColor
                            ctx.fillRect(x - 0.5, laneBottom - barH - 2, 4, 2)
                        }
                    }

                    ctx.strokeStyle = root.gridColor
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    ctx.moveTo(0, 0.5)
                    ctx.lineTo(w, 0.5)
                    ctx.stroke()

                    ctx.restore()
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton

                //! NOTE: right-click clears the per-note velocity, so the note goes back to following
                //!       the dynamic marks. Without this a tweak would be one-way.
                //!       Only the band that was clicked is affected - another staff may well have a
                //!       note on the very same tick.
                onClicked: function(mouse) {
                    if (mouse.button !== Qt.RightButton) {
                        return
                    }

                    var lane = root.velocityLaneAt(mouse.y)
                    var tick = root.velocityAt(mouse.x, lane)
                    if (tick < 0) {
                        return
                    }

                    for (var i = root.notes.length - 1; i >= 0; --i) {
                        if (root.notes[i].tick === tick && root.notes[i].staffIndex === lane
                                && root.notes[i].hasVelocityOverride) {
                            root.model.setNoteVelocity(i, 0)
                        }
                    }
                }

                onPressed: function(mouse) {
                    if (mouse.button !== Qt.LeftButton) {
                        return
                    }

                    var lane = root.velocityLaneAt(mouse.y)
                    var tick = root.velocityAt(mouse.x, lane)
                    if (tick < 0) {
                        return
                    }
                    root.velocityDragTick = tick
                    root.velocityDragStaff = lane
                    root.velocityDragging = true
                    velocityCanvas.requestPaint()
                }

                onPositionChanged: function(mouse) {
                    if (root.velocityDragging) {
                        velocityCanvas.requestPaint()
                    }
                }

                onReleased: function(mouse) {
                    if (mouse.button !== Qt.LeftButton || !root.velocityDragging) {
                        return
                    }

                    var velocity = root.velocityForY(mouse.y)
                    var tick = root.velocityDragTick
                    var staff = root.velocityDragStaff

                    root.velocityDragging = false
                    root.velocityDragTick = -1
                    root.velocityDragStaff = -1

                    //! NOTE: a chord of THIS staff is edited as a whole - one submission per drag,
                    //!       and nothing outside the staff the drag started in.
                    for (var i = root.notes.length - 1; i >= 0; --i) {
                        if (root.notes[i].tick === tick && root.notes[i].staffIndex === staff) {
                            root.model.setNoteVelocity(i, velocity)
                        }
                    }

                    velocityCanvas.requestPaint()
                }
            }
        }
    }
}
