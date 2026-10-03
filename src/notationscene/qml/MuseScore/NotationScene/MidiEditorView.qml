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
import QtQuick.Controls

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
    //! The lane is a single strip, Cubase-style - NOT one band per staff. Bands looked tidy with two
    //! staves and became unclickable slivers with ten, and "the band the pointer happens to be in"
    //! was never an obvious answer to "which staff am I editing".
    readonly property real velocityLaneHeight: velocityLaneVisible ? 96 : 0
    readonly property real rulerHeight: 24
    readonly property real toolBarHeight: 36

    // ── model ────────────────────────────────────────────────────────────────
    readonly property bool hasScore: model !== null && model.hasScore
    //! NOTE: lowestPitch / highestPitch are defined further down - they follow the VISIBLE staff's
    //! notes rather than the whole score's range.
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

    property bool velocityDragging: false
    //! Which staff the current velocity drag edits. The lane is split into one horizontal band per
    //! staff, so a drag must never touch another staff's notes that happen to sit on the same tick.
    property int velocityDragStaff: -1

    //! tick -> velocity, filled in as the pointer sweeps across the lane. This is what makes the
    //! gesture feel like DRAWING rather than "pick one value, release, see it jump": every note the
    //! brush passes over keeps the height the pointer had at that moment, and the bars follow the
    //! pointer instead of only changing on release.
    property var velocityTrail: ({})

    //! True between the release and the model reporting the new values. The trail is kept for that
    //! moment: dropping it straight away would make the bars fall back to their pre-drag heights for
    //! a frame, so the lane would visibly jump back before jumping forward again.
    property bool velocityPending: false

    //! The staff being edited. Cubase-style: the lane shows every staff's bars in that staff's own
    //! colour so they can be compared, but only the current staff is solid - and only the current
    //! staff is ever edited. That makes "which staff does this drag touch" a thing you choose,
    //! rather than a thing you infer from where the pointer happens to be.
    property int currentStaff: 0

    readonly property int staffCount: (model !== null && model.hasScore) ? Math.max(1, model.staffCount) : 1

    readonly property var staffNames: (model !== null && model.hasScore) ? model.staffNames : []

    readonly property string currentStaffName: {
        if (currentStaff < staffOptions.length) {
            return staffOptions[currentStaff]
        }
        return qsTrc("notationscene", "Staff") + " " + (currentStaff + 1)
    }

    //! One entry per staff for the selector, with a fallback name so a score whose part names the
    //! model could not resolve still gets a usable list.
    readonly property var staffOptions: {
        var out = []
        for (var i = 0; i < staffCount; ++i) {
            out.push((i < staffNames.length && staffNames[i])
                     ? staffNames[i]
                     : (qsTrc("notationscene", "Staff") + " " + (i + 1)))
        }
        return out
    }

    //! Only the selected staff is drawn. Cubase shows one track's notes at a time, and mixing every
    //! instrument of a score into a single grid is what made the roll unreadable in the first place.
    //!
    //! Each entry carries `row`, its index in `notes` - that is what the model's edit calls take,
    //! and after filtering the two indexes are no longer the same.
    readonly property var visibleRows: {
        var out = []
        for (var i = 0; i < notes.length; ++i) {
            if (notes[i].staffIndex === currentStaff) {
                out.push({ "note": notes[i], "row": i })
            }
        }
        return out
    }

    //! The pitch range follows the VISIBLE notes, not the whole score - otherwise selecting an
    //! instrument with a narrow range would squeeze its notes into the middle of a mostly empty grid.
    readonly property int lowestPitch: {
        var list = visibleRows
        if (list.length === 0) {
            return 60
        }
        var lo = 127
        for (var i = 0; i < list.length; ++i) {
            lo = Math.min(lo, list[i].note.pitch)
        }
        return Math.max(0, lo - 2)
    }

    readonly property int highestPitch: {
        var list = visibleRows
        if (list.length === 0) {
            return 72
        }
        var hi = 0
        for (var i = 0; i < list.length; ++i) {
            hi = Math.max(hi, list[i].note.pitch)
        }
        return Math.min(127, hi + 2)
    }

    function selectStaff(index) {
        if (index >= 0 && index < staffCount) {
            currentStaff = index
        }
    }

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
        //! Returns an index into visibleRows - NOT into notes, since only the selected staff is drawn
        //! and clickable. Callers map it back with visibleRows[i].row.
        //! NOTE: iterate backwards because later notes are painted on top.
        var list = visibleRows
        for (var i = list.length - 1; i >= 0; --i) {
            var note = list[i].note
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

    function velocityAt(x) {
        //! NOTE: only the visible staff is drawn in the lane, so only its notes can be hit. A chord
        //!       at that time position is edited as a whole, which is what makes dragging usable.
        var list = visibleRows
        var best = -1
        var bestDist = 6
        for (var i = 0; i < list.length; ++i) {
            var dist = Math.abs(xForTick(list[i].note.tick) - x)
            if (dist <= bestDist) {
                bestDist = dist
                best = list[i].note.tick
            }
        }
        return best
    }

    function velocityForY(y) {
        //! One strip, so the whole lane height maps onto 1..127.
        var lane = velocityCanvas.height
        return clamp(Math.round((1.0 - y / lane) * 127), 1, 127)
    }

    //! One stroke of the brush: every note under the pointer keeps the height the pointer has right
    //! now. Called on press and on every move, so the lane tracks the pointer instead of waiting for
    //! the release.
    function paintVelocityAt(x, y) {
        var velocity = velocityForY(y)
        var trail = velocityTrail
        var list = visibleRows
        var touched = false

        for (var i = 0; i < list.length; ++i) {
            var note = list[i].note
            //! Either the pointer is over the note, or close enough to its onset that a fast sweep
            //! must not skip it - a brush that leaves gaps feels broken.
            if (hitHorizontally(note, x) || Math.abs(xForTick(note.tick) - x) <= 8) {
                trail[note.tick] = velocity
                touched = true
            }
        }

        if (touched) {
            //! Reassign so the property change reaches the canvas.
            velocityTrail = trail
        }
    }

    function clearVelocityTrail() {
        velocityDragging = false
        velocityDragStaff = -1
        velocityTrail = ({})
    }

    onScrollXChanged: repaintAll()
    onScrollYChanged: repaintAll()
    onRowHeightChanged: repaintAll()
    onPixelsPerTickChanged: repaintAll()
    onNotesChanged: {
        //! The model has reported back, so the stored values now match what was painted; the trail
        //! has done its job and the bars switch over to the real data without a visible step.
        if (velocityPending) {
            velocityPending = false
            velocityTrail = ({})
        }
        repaintAll()
    }
    onStaffCountChanged: {
        //! A different score can have fewer staves; keep the selection inside the range.
        if (currentStaff >= staffCount) {
            currentStaff = 0
        }
        repaintAll()
    }
    onStaffNamesChanged: repaintAll()
    onCurrentStaffChanged: repaintAll()
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

            text: qsTrc("notationscene", "Drag a note's right edge = played length · Shift+drag = played start · velocity lane: pick a staff in the toolbar, drag = own velocity, right-click = follow dynamics")

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

            //! Cubase-style track selector: the lane and the roll always show every staff, but edits
            //! only ever land on the one named here. Hidden for single-staff scores, where there is
            //! nothing to choose.
            Rectangle {
                id: staffSelector

                visible: root.staffCount > 1
                width: staffSelectorRow.width + 18
                height: 22
                radius: 3
                color: staffSelectorMouse.containsMouse ? ui.theme.buttonColor : "transparent"
                border.width: 2
                border.color: root.staffColor(root.currentStaff)

                Row {
                    id: staffSelectorRow

                    anchors.centerIn: parent
                    spacing: 5

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 1
                        anchors.verticalCenter: parent.verticalCenter
                        color: root.staffColor(root.currentStaff)
                    }

                    Text {
                        id: staffSelectorLabel

                        anchors.verticalCenter: parent.verticalCenter
                        text: root.currentStaffName
                        color: root.textColor
                        font: ui.theme.bodyFont
                    }
                }

                MouseArea {
                    id: staffSelectorMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: staffPopup.open()
                }
            }

            //! The staff list. A Popup rather than an inline panel because the toolbar is only 36px
            //! tall - anything drawn inside it would be clipped.
            Popup {
                id: staffPopup

                parent: staffSelector
                x: 0
                y: staffSelector.height + 2
                width: Math.max(staffSelector.width, 180)
                padding: 4
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                background: Rectangle {
                    color: root.panelColor
                    border.width: 1
                    border.color: root.gridColor
                    radius: 3
                }

                contentItem: Column {
                    spacing: 2

                    Repeater {
                        model: root.staffOptions

                        delegate: Rectangle {
                            id: staffOption

                            required property string modelData
                            required property int index

                            width: staffPopup.width - 8
                            height: 24
                            radius: 2
                            color: (staffOption.index === root.currentStaff || staffOptionMouse.containsMouse)
                                   ? ui.theme.buttonColor : "transparent"

                            Row {
                                anchors.left: parent.left
                                anchors.leftMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Rectangle {
                                    width: 8
                                    height: 8
                                    radius: 1
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: root.staffColor(staffOption.index)
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: staffOption.modelData
                                    color: root.textColor
                                    font: ui.theme.bodyFont
                                }
                            }

                            MouseArea {
                                id: staffOptionMouse

                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    root.selectStaff(staffOption.index)
                                    staffPopup.close()
                                }
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

                    // notes - only the selected staff is drawn
                    var nh = root.noteHeight()
                    var minTick = root.tickForX(-8)
                    var maxTick = root.tickForX(w + 8)
                    var visible = root.visibleRows

                    for (var n = 0; n < visible.length; ++n) {
                        var note = visible[n].note
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

                        //! No fading any more: only the selected staff reaches this loop at all.
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
                    //! An index into visibleRows - only the selected staff is drawn and clickable.
                    var index = root.noteIndexAt(mouse.x, mouse.y)

                    if (index < 0) {
                        //! NOTE: only the miss is logged, so normal use stays quiet while a "the drag
                        //!       does nothing" report can still be diagnosed from the log file.
                        //!       `console.warn` on purpose: MuseScore records Qt warnings, not plain
                        //!       console.log output, so a log() here would never reach the log file.
                        console.warn("MidiEditorView: press missed at", mouse.x, mouse.y,
                                     "| visibleNotes", root.visibleRows.length, "of", root.notes.length,
                                     "pitchRange", root.lowestPitch, "-", root.highestPitch,
                                     "rowHeight", root.rowHeight, "scrollY", root.scrollY,
                                     "mouseArea", width, "x", height)
                    }

                    root.dragNoteIndex = index
                    if (index >= 0) {
                        var grabbed = root.visibleRows[index].note
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
                    if (root.dragNoteIndex >= 0 && root.dragNoteIndex < root.visibleRows.length) {
                        var entry = root.visibleRows[root.dragNoteIndex]
                        var released = entry.note

                        //! The model takes indexes into `notes`, not into the filtered view.
                        if (root.dragMode === root.dragModePitch) {
                            if (root.dragPreviewPitch >= 0 && root.dragPreviewPitch !== root.dragStartPitch) {
                                //! NOTE: the single submission of the whole drag.
                                root.model.setNotePitch(entry.row, root.dragPreviewPitch)
                            }
                        } else if (root.dragPreviewPlayStart !== released.playTick
                                   || root.dragPreviewPlayDuration !== released.playDurationTicks) {
                            root.model.setNotePlayOverride(entry.row,
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

        //! No labels column here any more: with one strip there is nothing to label. Which staff is
        //! being edited is shown in the toolbar instead, and the bars themselves are colour-coded.

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
                    var visible = root.visibleRows

                    for (var i = 0; i < visible.length; ++i) {
                        var note = visible[i].note
                        if (note.tick > maxTick || note.tick < minTick) {
                            continue
                        }

                        var x = root.xForTick(note.tick)

                        //! While the brush is down, a bar follows the pointer's height instead of the
                        //! stored value - that live feedback is the whole point of the gesture.
                        var trail = root.velocityTrail
                        var hasTrail = trail.hasOwnProperty(note.tick)
                        var brushed = root.velocityDragging && hasTrail

                        //! Between the release and the model's answer the painted value is kept, but
                        //! drawn in the staff colour rather than the brush colour: the gesture reads
                        //! as finished immediately, and the height does not jump back in the meantime.
                        var pending = root.velocityPending && hasTrail
                        var shownVelocity = (brushed || pending) ? trail[note.tick] : note.velocity

                        var barH = Math.max(1, (shownVelocity / 127) * (h - 4))
                        var own = note.hasVelocityOverride
                        var solid = own || brushed || pending

                        //! NOTE: a thin, faint bar means "this note has no velocity of its own, so it
                        //!       follows the dynamic marks (pp/ff, hairpins)" - which is the default
                        //!       for almost every note. A thick solid bar means the note was given its
                        //!       own velocity here, overriding the dynamics. Right-click clears it.
                        ctx.fillStyle = brushed ? root.cursorColor : root.staffColor(note.staffIndex)
                        ctx.globalAlpha = (brushed || pending) ? 1.0 : (own ? 0.9 : 0.55)
                        ctx.fillRect(solid ? x - 0.5 : x + 0.5, h - barH, solid ? 4 : 2, barH)
                        ctx.globalAlpha = 1.0

                        // a small cap so an overridden note is recognisable even when short
                        if (solid) {
                            ctx.fillStyle = root.cursorColor
                            ctx.fillRect(x - 0.5, h - barH - 2, 4, 2)
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
                //!       Only the visible staff is drawn, so only its notes can be hit.
                onClicked: function(mouse) {
                    if (mouse.button !== Qt.RightButton) {
                        return
                    }

                    var tick = root.velocityAt(mouse.x)
                    if (tick < 0) {
                        return
                    }

                    var list = root.visibleRows
                    for (var i = list.length - 1; i >= 0; --i) {
                        if (list[i].note.tick === tick && list[i].note.hasVelocityOverride) {
                            root.model.setNoteVelocity(list[i].row, 0)
                        }
                    }
                }

                onPressed: function(mouse) {
                    if (mouse.button !== Qt.LeftButton) {
                        return
                    }

                    var tick = root.velocityAt(mouse.x)
                    if (tick < 0) {
                        return
                    }

                    //! Remembered at press time: switching staff mid-drag must not retarget the edit.
                    root.velocityDragStaff = root.currentStaff
                    root.velocityTrail = ({})
                    root.velocityDragging = true

                    //! Lay down the first stroke immediately, so a plain click already shows its
                    //! effect instead of appearing to do nothing until the button comes up.
                    root.paintVelocityAt(mouse.x, mouse.y)
                    velocityCanvas.requestPaint()
                }

                onPositionChanged: function(mouse) {
                    if (!root.velocityDragging) {
                        return
                    }

                    //! Every move paints, which is what makes the bars follow the pointer.
                    root.paintVelocityAt(mouse.x, mouse.y)
                    velocityCanvas.requestPaint()
                }

                onReleased: function(mouse) {
                    if (mouse.button !== Qt.LeftButton || !root.velocityDragging) {
                        return
                    }

                    //! One last stroke at the release position, so the value under the pointer is the
                    //! value that gets stored.
                    root.paintVelocityAt(mouse.x, mouse.y)

                    var trail = root.velocityTrail
                    var staff = root.velocityDragStaff

                    root.velocityDragging = false
                    root.velocityDragStaff = -1

                    //! NOTE: every note the brush passed over is submitted - that is what "drawing"
                    //!       means here - but in ONE call. Submitting them one at a time made the
                    //!       model rebuild its note list per note, which is what made a sweep lag.
                    //!       The staff check still matters: the drag may have started before a staff
                    //!       switch, and it must not land on the newly selected one.
                    var rows = []
                    var values = []
                    var list = root.visibleRows
                    for (var i = list.length - 1; i >= 0; --i) {
                        var note = list[i].note
                        if (note.staffIndex === staff && trail.hasOwnProperty(note.tick)) {
                            rows.push(list[i].row)
                            values.push(trail[note.tick])
                        }
                    }

                    if (rows.length > 0) {
                        //! The trail stays until the model answers - see velocityPending.
                        root.velocityPending = true
                        root.model.setNoteVelocities(rows, values)
                    } else {
                        root.clearVelocityTrail()
                    }

                    velocityCanvas.requestPaint()
                }
            }
        }
    }
}
