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
    //! Whether the lane edits the velocity of single notes (bars) or the staff's Dynamics AUTOMATION
    //! curve. They are different things: a bar is "this note is this loud, whatever the score says",
    //! while the curve is "the loudness moves like this over time" - which is what a crescendo, a
    //! diminuendo or an fp is. Both need a left-drag in the same strip, hence a mode.
    property bool automationMode: false

    //! The Dynamics automation of the selected staff, as
    //! { tick, value, authored, hasEase, controlT, controlValue, arrival } with the values in 0..1.
    //! Read from the model rather than bound, because it is a Q_INVOKABLE - it is refreshed whenever
    //! the score or the selected staff changes.
    property var automationPoints: []

    //! 正在拖的控制点（它的 tick，-1 = 没在拖）与拖动中的预览值。拖动中**只预览**、松手才提交一次。
    property int automationDragTick: -1
    property int automationDragPreviewTick: -1
    property real automationDragPreviewValue: 0

    //! 正在拖的弯折手柄（它所属段的后一个点的 tick，-1 = 没在拖）与预览的弯折点。
    //! 手柄调的是 `AutomationPoint::Ease` —— 二次贝塞尔曲线的弯折位置/幅度。
    property int automationBendTick: -1
    property real automationBendPreviewT: 0.5
    property real automationBendPreviewValue: 0.5

    //! 按下时记下的谱表：拖动中切换谱表不能把这次编辑落到新谱表上（与力度画笔同一条纪律）。
    property int automationDragStaff: -1

    //! 本次手势收到的**移动事件次数**（观测用）。
    //! `moves == 0` 说明 `MouseArea` 根本没收到移动 → 那是事件层的问题；
    //! `moves > 0` 却 `from == to` 说明鼠标确实没怎么动。2026-10-05 加，用来一次定位"拖不动"。
    property int automationMoveCount: 0

    //! 正在**新建一个点**（按下空白后还没松手）。
    //!
    //! ⚠️ 按下时**不写模型**：整次手势只在松手时发**一个**命令，直接把点建在最终位置。
    //! 曾经的写法是"按下写一次（新增）+ 松手再写一次（移动）"—— 两个命令落在同一个点上，
    //! 第二个会把点弄丢，表现就是"按下冒出一个点、一拖就没了 / 切页后消失"
    //! （2026-10-05 用户实测："不松手直接拖"丢点、"先点击-松开"正常）。
    //! 只发一个命令既修掉它，也保住了"按下即拖"的一次性操作。
    property bool automationNewDragging: false

    //! Ctrl+Z / Ctrl+Shift+Z。
    //!
    //! ⚠️ 这里必须**自己绑**，而且要用 `Qt.ApplicationShortcut`：
    //!  * MuseScore 的快捷键表（`shortcuts.xml`）虽然注册了 `action://undo` = std 11，但它在
    //!    `musescore://midi` 这一页**不触发**（2026-10-03/05 实测：Edit 菜单的 Undo 已经亮了、
    //!    命令状态也正确，按键仍然没反应）；
    //!  * QML `Shortcut` 默认是 `Qt::WindowShortcut`，与 MuseScore 注册的键同级 —— 同级时先注册者
    //!    优先，所以抢不到；`Qt::ApplicationShortcut` **优先级更高**，能稳定拿到（也正因为更高，
    //!    不会与它双触发）。
    //!
    //! ⚠️⚠️ **不要用 `enabled` 去绑 `canUndo`**：`enabled: false` 的 Shortcut **完全不拦截按键**，
    //! 所以只要那个属性有一次没刷新，快捷键就"永远没反应"、而且**日志里连痕迹都没有**（这次
    //! 排查就卡在这里）。改成常开 + 在回调里打印一行观测点，能不能撤销交给模型自己判断
    //! （没得撤销时 `undoStack()->undo()` 本来就是 no-op）。
    //! Ctrl+Z / Ctrl+Shift+Z。
    //!
    //! ⚠️ 这一页**必须自己绑**：MuseScore 的全局快捷键（`AppWindow.qml` 的 `Shortcuts { }`）
    //! 在 `musescore://midi` 这一页**不触发**（2026-10-05 实测：`active=true`、`Ctrl+Z` 已注册、
    //! 上下文检查通过、组件也在主窗口上 —— 链路上每一环都正常，按键就是没反应）。
    //! QML `Shortcut` 默认是 `Qt::WindowShortcut`，与它同级时抢不到；用
    //! **`Qt::ApplicationShortcut`**（优先级更高）才能稳定拿到。
    //! `enabled` **不要**绑 `canUndo`：`enabled: false` 的 Shortcut 完全不拦截按键，
    //! 只要那个属性有一次没刷新，快捷键就"永远没反应"且日志里毫无痕迹。没得撤销时
    //! `undoStack()->undo()` 本来就是 no-op。
    //! ⚠️ Ctrl+Z / Ctrl+Shift+Z —— **两道保险**。
    //!
    //! 第一道是下面的 `Shortcut`（`Qt::ApplicationShortcut`）。实测它在这套架构里**不触发**
    //! （2026-10-05：`active=true`、`Ctrl+Z` 已在快捷键表里、上下文检查通过、全局 `Shortcuts`
    //! 组件也在主窗口上 —— 链路上每一环都正常，按键就是没反应）。
    //!
    //! 所以再加一道 `Keys.onPressed`：**只要按键能到达这一页，就一定能撤销**。
    //! 两条日志（`keys undo` / `shortcut undo`）还能顺带告诉我们按键到底走到了哪一层：
    //!  * 只有 `keys undo`  → 按键到了页面、Shortcut 系统没接 → 兜底生效（快捷键可用）
    //!  * 两条都没有        → 按键在更上层就被吃掉了 → 继续往上查
    focus: true
    Keys.onPressed: function(event) {
        if ((event.modifiers & Qt.ControlModifier) === 0) {
            return
        }

        if (event.key === Qt.Key_Z) {
            console.warn("[midi-automation] keys undo (shift=" + ((event.modifiers & Qt.ShiftModifier) !== 0) + ")")
            if ((event.modifiers & Qt.ShiftModifier) !== 0) {
                root.model.redo()
            } else {
                root.model.undo()
            }
            event.accepted = true
        } else if (event.key === Qt.Key_Y) {
            console.warn("[midi-automation] keys redo")
            root.model.redo()
            event.accepted = true
        }
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        context: Qt.ApplicationShortcut
        enabled: root.model !== null
        onActivated: {
            console.warn("[midi-automation] shortcut undo")
            root.model.undo()
        }
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        context: Qt.ApplicationShortcut
        enabled: root.model !== null
        onActivated: {
            console.warn("[midi-automation] shortcut redo")
            root.model.redo()
        }
    }

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

    //! Reads the selected staff's Dynamics automation into `automationPoints`.
    function reloadAutomation() {
        automationPoints = (model !== null && model.hasScore)
                           ? model.automationPoints(currentStaff)
                           : []
    }

    //! The value under the pointer, in the same 0..1 the automation uses.
    function automationValueForY(y) {
        var lane = velocityCanvas.height
        return clamp(1.0 - (y - 2) / Math.max(1, lane - 4), 0.0, 1.0)
    }

    //! The tick a drawn point lands on. The automation is a time curve, so it snaps like the played
    //! layer does - on the same grid, so a point drawn here lines up with the notes.
    function snapTick(tick) {
        var snap = playSnapTicks
        return Math.max(0, Math.round(tick / snap) * snap)
    }

    //! Where a 0..1 automation value sits in the lane.
    function yForAutomationValue(v, laneHeight) {
        return 2 + (1.0 - v) * (laneHeight - 4)
    }

    //! 一段的值 —— 与 `muse::mpe::evaluateAt()` **同式**：把 [前一点, 本点] 拆成两条在弯折点
    //! (`controlT`, `controlValue`) 相切的二次贝塞尔弧；`Ease::none()`（{0.5, 0.5}）退化成直线。
    //!
    //! ⚠️ 这里照抄那份公式而不是"画个差不多"：屏幕上看到的必须就是合成器听到的那条线，
    //!    否则会出现最难查的一类 bug —— 看着是渐强、听着是台阶。
    function automationSegmentValue(prevValue, point, t) {
        //! `ArrivalFromPrevious` 的到达值就是前一点的值（这一段是平的），显式到达则用它自己的。
        var thisIn = point.hasEase ? point.arrival : prevValue
        var range = thisIn - prevValue

        var bent = point.hasEase
                   && !(Math.abs(point.controlT - 0.5) < 1e-9 && Math.abs(point.controlValue - 0.5) < 1e-9)
        if (!bent || point.controlT <= 0 || point.controlT >= 1) {
            return clamp(prevValue + range * t, 0.0, 1.0)
        }

        var quadratic = function(s, p0, p1, p2) {
            var u = 1.0 - s
            return clamp(u * u * p0 + 2.0 * u * s * p1 + s * s * p2, 0.0, 1.0)
        }

        var fraction = clamp(point.controlValue, 0.0, 1.0)
        var bendValue = prevValue + fraction * range
        var lo = Math.min(prevValue, thisIn)
        var hi = Math.max(prevValue, thisIn)
        var halfSlope = 0.5 * range
        var remainder = 1.0 - point.controlT
        var q1 = clamp(bendValue - point.controlT * halfSlope, lo, hi)

        if (t <= point.controlT) {
            return quadratic(t / point.controlT, prevValue, q1, bendValue)
        }

        var q2 = clamp(bendValue + remainder * halfSlope, lo, hi)
        return quadratic((t - point.controlT) / remainder, bendValue, q2, thisIn)
    }

    //! 画曲线用的点列表：模型里的点 + 拖动中的预览（拖点改位置/值、拖手柄改弯折），按 tick 排序。
    //! 预览必须参与绘制，否则拖动时画面纹丝不动（维护手册 §4.8 记过这个坑）。
    function automationPointsForDraw() {
        var drawn = []

        for (var i = 0; i < automationPoints.length; ++i) {
            var source = automationPoints[i]
            var item = {
                "tick": source.tick,
                "value": source.value,
                "authored": source.authored,
                "hasEase": source.hasEase,
                "controlT": source.controlT,
                "controlValue": source.controlValue,
                "arrival": source.arrival
            }

            if (automationDragTick >= 0 && source.tick === automationDragTick) {
                item.tick = automationDragPreviewTick
                item.value = automationDragPreviewValue
                //! 值改了，到达值跟着改 —— 这一段"结束在本点的值"，与模型里的写法一致。
                if (item.hasEase) {
                    item.arrival = automationDragPreviewValue
                }
            }

            if (automationBendTick >= 0 && source.tick === automationBendTick) {
                item.hasEase = true
                item.controlT = automationBendPreviewT
                item.controlValue = automationBendPreviewValue
                //! 原本是 `ArrivalFromPrevious`（平的段）的点，一旦拖手柄就升级成"到达本点的值"，
                //! 这样这一段才真的弯得起来（range 为 0 的段怎么弯都是平的）。看的是**模型里**的标志。
                if (!source.hasEase) {
                    item.arrival = item.value
                }
            }

            drawn.push(item)
        }

        //! 新建中的点：还没写进模型，但**必须参与绘制** —— 否则按下拖动时看不到它（= 不跟手）。
        if (automationNewDragging) {
            drawn.push({
                "tick": automationDragPreviewTick,
                "value": automationDragPreviewValue,
                "authored": true,
                "hasEase": false,
                "controlT": 0.5,
                "controlValue": 0.5,
                "arrival": automationDragPreviewValue
            })
        }

        drawn.sort(function(a, b) { return a.tick - b.tick })
        return drawn
    }

    //! 某个 tick 处的曲线值（含拖动预览），没有点时返回 -1。
    function automationValueAtTick(tick) {
        return automationValueInList(automationPointsForDraw(), tick)
    }

    //! 列表版求值。绘制循环用这个：列表取一次，逐像素只做求值 ——
    //! 若每个像素都调 `automationValueAtTick`，就会每个像素重建并排序一遍点列表（白烧 CPU）。
    //! 第一个点之前保持第一个点的值 —— 与 `evaluateCurveAt` 的"向前保持"语义一致。
    function automationValueInList(list, tick) {
        if (list.length === 0) {
            return -1
        }

        if (tick <= list[0].tick) {
            return list[0].value
        }

        for (var i = 1; i < list.length; ++i) {
            if (tick <= list[i].tick) {
                var span = list[i].tick - list[i - 1].tick
                if (span <= 0) {
                    return list[i].value
                }

                return automationSegmentValue(list[i - 1].value, list[i], (tick - list[i - 1].tick) / span)
            }
        }

        return list[list.length - 1].value
    }

    //! 第 i 段（list[i-1] → list[i]）的弯折手柄位置：横向在段的 controlT 处、纵向在该处的值上。
    //! 返回 { x, y, tick, prevTick, prevValue, arrival }，或 null（平的段：没有弯折可言）。
    //!
    //! 记号生成的点（空心）**也给手柄**：拖手柄 = **接管**它（模型会把 `generated`/`itemId`
    //! 清掉），所以写上去的曲率不会被下一次重建覆盖。
    function automationHandleAt(list, i) {
        if (i < 1 || i >= list.length) {
            return null
        }

        var point = list[i]
        var prev = list[i - 1]
        var thisIn = point.hasEase ? point.arrival : prev.value
        if (Math.abs(thisIn - prev.value) < 1e-9) {
            return null   // 平的段：没有弯折可言，也就不给手柄
        }

        var t = point.hasEase ? clamp(point.controlT, 0.0, 1.0) : 0.5
        var fraction = point.hasEase ? clamp(point.controlValue, 0.0, 1.0) : 0.5

        return {
            "x": xForTick(prev.tick) + (xForTick(point.tick) - xForTick(prev.tick)) * t,
            "y": yForAutomationValue(prev.value + fraction * (thisIn - prev.value), velocityCanvas.height),
            "tick": point.tick,
            "prevTick": prev.tick,
            "prevValue": prev.value,
            "arrival": thisIn
        }
    }

    //! 命中的控制点（返回它的 tick，-1 = 没命中）。
    //!
    //! ⚠️ **横纵分开判**，纵向给得宽得多：力度轴是 0..1 映射到 ~92px 的一条窄带，
    //! 用户很难在纵向精确点中一个半径 3px 的圆 —— 2026-10-05 的日志里，用户想拖的点
    //! 横向只差 5.4px、**纵向差约 10px**，于是被判成"空白"、走了新增分支，
    //! 表现出来就是"我拖它它不动"（用户报的「回弹」）。
    function automationHitPoint(x, y) {
        var list = automationPointsForDraw()
        var best = -1
        var bestDist = 1e9

        for (var i = 0; i < list.length; ++i) {
            var dx = Math.abs(xForTick(list[i].tick) - x)
            var dy = Math.abs(yForAutomationValue(list[i].value, velocityCanvas.height) - y)
            if (dx > 10 || dy > 18) {
                continue
            }

            //! 都够近时取"更像同一个点"的那个：横向权重更高（时间轴才是主要维度）。
            var dist = dx * dx + dy * dy
            if (dist < bestDist) {
                bestDist = dist
                best = list[i].tick
            }
        }

        return best
    }

    //! 命中的弯折手柄：返回它所属段的后一个点的 tick，-1 = 没命中。
    function automationHitHandle(x, y) {
        var list = automationPointsForDraw()
        var best = -1
        var bestDist = 10

        for (var i = 1; i < list.length; ++i) {
            var handle = automationHandleAt(list, i)
            if (handle === null) {
                continue
            }

            var dx = handle.x - x
            var dy = handle.y - y
            var dist = Math.sqrt(dx * dx + dy * dy)
            if (dist <= bestDist) {
                bestDist = dist
                best = handle.tick
            }
        }

        return best
    }

    //! 模型里某个 tick 上的点（**不含**拖动预览），没有则 null。
    function automationPointAt(tick) {
        for (var i = 0; i < automationPoints.length; ++i) {
            if (automationPoints[i].tick === tick) {
                return automationPoints[i]
            }
        }

        return null
    }

    //! 某个点"到达段"的两端与到达值（**模型里的值**，不含预览 —— 预览正是要反算的东西）。
    //! 返回 { prevTick, prevValue, arrival }；平的段（两端值相同）返回 null：那种段没有弯折可言。
    function automationBendSegment(tick) {
        var prev = null
        var point = null

        for (var i = 0; i < automationPoints.length; ++i) {
            if (automationPoints[i].tick < tick) {
                prev = automationPoints[i]
            } else if (automationPoints[i].tick === tick) {
                point = automationPoints[i]
            }
        }

        if (prev === null || point === null) {
            return null
        }

        var arrival = point.hasEase ? point.arrival : point.value
        if (Math.abs(arrival - prev.value) < 1e-9) {
            return null
        }

        return { "prevTick": prev.tick, "prevValue": prev.value, "arrival": arrival }
    }

    //! 把手柄拖到的位置反算成弯折点 (t, value)：横向落在段内的比例、纵向落在 prevValue..arrival
    //! 之间的比例。`t` 夹在 0.05..0.95 —— 上游把贴到 0/1 的 t 当成"没有弯折"，拖到边上会突然失效。
    function automationBendFromPointer(x, y, tick) {
        var segment = automationBendSegment(tick)
        if (segment === null) {
            return null
        }

        var prevX = xForTick(segment.prevTick)
        var thisX = xForTick(tick)
        if (thisX - prevX < 1) {
            return null
        }

        return {
            "t": clamp((x - prevX) / (thisX - prevX), 0.05, 0.95),
            "value": clamp((automationValueForY(y) - segment.prevValue)
                           / (segment.arrival - segment.prevValue), 0.0, 1.0)
        }
    }

    //! The point nearest to x, or -1. Used by right-click, which only ever removes the user's own
    //! points: one the score derived from a Dynamic mark or a hairpin is put back by the next rebuild,
    //! so removing it is not the lane's to do - the model refuses it too (and the notation page's lane
    //! refuses the same points).
    function automationPointNear(x) {
        var best = -1
        var bestDist = 8
        for (var i = 0; i < automationPoints.length; ++i) {
            if (!automationPoints[i].authored) {
                continue
            }

            var dist = Math.abs(xForTick(automationPoints[i].tick) - x)
            if (dist <= bestDist) {
                bestDist = dist
                best = automationPoints[i].tick
            }
        }
        return best
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
        //! 曲线这边同理：模型回话说明编辑已经落到数据里，预览该让位 —— 留着会画出一个"幽灵点"。
        automationDragTick = -1
        automationBendTick = -1
        //! ⚠️ **`automationDragStaff` 不能在这里清**：它是"这次手势的谱表上下文"，
        //! 而不是数据预览。新增点会立刻触发本处理器（`setAutomationPoints` → `scoreChanged`），
        //! 若把它清成 -1，紧接着的"拖动刚新增的点"就会用 -1 去查曲线键 → 移动被拒绝 → 点弹回。
        //! 这正是"新增后立刻拖动第一次失败、第二次成功"的根因（2026-10-05）。
        //! The automation is a Q_INVOKABLE, not a property, so it has to be re-read rather than
        //! bound - and it changes whenever the score does (the notation page edits the same curve).
        reloadAutomation()
        repaintAll()
    }
    onAutomationModeChanged: {
        //! 换模式时把拖动状态清干净：留着会让下一次进入 Curve 模式时凭空多出一个预览点。
        automationDragTick = -1
        automationBendTick = -1
        automationDragStaff = -1
        automationNewDragging = false
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
    onCurrentStaffChanged: {
        //! Each staff has its own Dynamics curve, so the one on screen has to follow the selection.
        reloadAutomation()
        repaintAll()
    }
    onAutomationPointsChanged: repaintAll()
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

            //! Switches the lane between editing single notes' velocity and drawing the staff's
            //! Dynamics automation curve. Shown next to the lane's own toggle because the two are
            //! about the same strip.
            Rectangle {
                visible: root.velocityLaneVisible
                width: Math.max(28, automationToggleLabel.implicitWidth + 16)
                height: 22
                radius: 3
                color: root.automationMode ? ui.theme.buttonColor : "transparent"
                border.width: 1
                border.color: root.automationMode ? root.cursorColor : root.gridColor

                Text {
                    id: automationToggleLabel

                    anchors.centerIn: parent
                    text: qsTrc("notationscene", "Curve")
                    color: root.textColor
                    font: ui.theme.bodyFont
                }

                MouseArea {
                    id: automationToggleArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        root.automationMode = !root.automationMode
                        root.reloadAutomation()
                    }
                }

                ToolTip {
                    //! 交互是"控制点 + 手柄"，不是刷 —— 一句话说明，省得用户先乱刷一通。
                    text: qsTrc("notationscene", "Click to add a point, drag a point to move it, "
                                + "drag the square handle to bend the curve, right-click to remove. "
                                + "Hollow points belong to a dynamic mark and are read-only")
                    visible: automationToggleArea.containsMouse
                    delay: 600
                }
            }

            //! 撤销 / 重做。放在 Curve 开关旁边，因为曲线编辑就在这条车道上做 —— 而这一页原本
            //! 没有任何撤销入口（见上面 Shortcut 的说明）。不可用时灰显。
            Row {
                spacing: 4

                Repeater {
                    model: [
                        { "label": "↶", "tip": qsTrc("notationscene", "Undo (Ctrl+Z)"),
                          "enabled": root.model !== null && root.model.canUndo, "act": "undo" },
                        { "label": "↷", "tip": qsTrc("notationscene", "Redo (Ctrl+Shift+Z)"),
                          "enabled": root.model !== null && root.model.canRedo, "act": "redo" }
                    ]

                    Rectangle {
                        required property var modelData

                        width: 26
                        height: 22
                        radius: 3
                        color: "transparent"
                        border.width: 1
                        border.color: root.gridColor
                        opacity: modelData.enabled ? 1.0 : 0.4

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: root.textColor
                            font: ui.theme.bodyFont
                        }

                        MouseArea {
                            id: undoRedoArea

                            anchors.fill: parent
                            enabled: modelData.enabled
                            hoverEnabled: true
                            onClicked: modelData.act === "undo" ? root.model.undo() : root.model.redo()
                        }

                        ToolTip {
                            text: modelData.tip
                            visible: undoRedoArea.containsMouse
                            delay: 600
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
                        //! In curve mode the bars step back and become a reference: what is being
                        //! edited is the shape over time, not these individual values.
                        ctx.globalAlpha = root.automationMode ? 0.18
                                          : ((brushed || pending) ? 1.0 : (own ? 0.9 : 0.55))
                        ctx.fillRect(solid ? x - 0.5 : x + 0.5, h - barH, solid ? 4 : 2, barH)
                        ctx.globalAlpha = 1.0

                        // a small cap so an overridden note is recognisable even when short
                        if (solid) {
                            ctx.fillStyle = root.cursorColor
                            ctx.globalAlpha = root.automationMode ? 0.25 : 1.0
                            ctx.fillRect(x - 0.5, h - barH - 2, 4, 2)
                            ctx.globalAlpha = 1.0
                        }
                    }

                    //! The staff's Dynamics automation - the curve a crescendo, a diminuendo or an fp
                    //! inside a note actually is. Drawn over the bars, because it is what the
                    //! synthesiser follows.
                    //!
                    //! 画法：**按像素采样求值**（`automationValueAtTick`，与播放同一份贝塞尔公式），
                    //! 而不是把控制点连成折线 —— 折线会把"弯"画成"折"，看到的就不是听到的那条。
                    if (root.automationMode) {
                        var drawn = root.automationPointsForDraw()

                        if (drawn.length > 0) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = 2
                            ctx.beginPath()

                            var started = false
                            for (var px = 0; px <= w; px += 2) {
                                var curveValue = root.automationValueInList(drawn, root.tickForX(px))
                                if (curveValue < 0) {
                                    continue
                                }

                                var cy = root.yForAutomationValue(curveValue, h)
                                if (started) {
                                    ctx.lineTo(px, cy)
                                } else {
                                    ctx.moveTo(px, cy)
                                    started = true
                                }
                            }
                            ctx.stroke()

                            //! 每个"斜坡段"的弯折手柄：画成小方块，并用细虚线连到段的两端。
                            //! 它拖的是 `AutomationPoint::Ease`，也就是这条二次贝塞尔的弯折点。
                            ctx.save()
                            ctx.setLineDash([3, 3])
                            ctx.lineWidth = 1
                            for (var i = 1; i < drawn.length; ++i) {
                                var handle = root.automationHandleAt(drawn, i)
                                if (handle === null) {
                                    continue
                                }

                                ctx.strokeStyle = root.gridColor
                                ctx.beginPath()
                                ctx.moveTo(root.xForTick(handle.prevTick),
                                           root.yForAutomationValue(handle.prevValue, h))
                                ctx.lineTo(handle.x, handle.y)
                                ctx.lineTo(root.xForTick(handle.tick),
                                           root.yForAutomationValue(handle.arrival, h))
                                ctx.stroke()
                            }
                            ctx.restore()

                            for (var k = 1; k < drawn.length; ++k) {
                                var bend = root.automationHandleAt(drawn, k)
                                if (bend === null) {
                                    continue
                                }

                                ctx.fillStyle = root.cursorColor
                                ctx.fillRect(bend.x - 3, bend.y - 3, 6, 6)
                            }

                            //! 控制点：用户的实心，记号的空心（记号生成的点不给删，画成空心区分）。
                            for (var q = 0; q < drawn.length; ++q) {
                                ctx.beginPath()
                                ctx.arc(root.xForTick(drawn[q].tick), root.yForAutomationValue(drawn[q].value, h), 3, 0, 2 * Math.PI)
                                if (drawn[q].authored) {
                                    ctx.fillStyle = root.cursorColor
                                    ctx.fill()
                                } else {
                                    ctx.strokeStyle = root.cursorColor
                                    ctx.lineWidth = 2
                                    ctx.stroke()
                                }
                            }
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
                //!       In curve mode it removes an automation point instead - the same gesture,
                //!       "take this away".
                onClicked: function(mouse) {
                    if (mouse.button !== Qt.RightButton) {
                        return
                    }

                    if (root.automationMode) {
                        var pointTick = root.automationPointNear(mouse.x)
                        if (pointTick >= 0) {
                            root.model.removeAutomationPoint(root.currentStaff, pointTick)
                        }
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

                    //! Curve mode: 控制点 + 手柄，**不是**刷。
                    //!   1) 命中手柄  → 拖弯折点（改曲率）
                    //!   2) 命中控制点 → 拖它（改 tick / 值）
                    //!   3) 都没命中   → 在空白处新增一个控制点（立即提交，单击即生效）
                    if (root.automationMode) {
                        root.automationDragStaff = root.currentStaff
                        root.automationMoveCount = 0

                        var bendTick = root.automationHitHandle(mouse.x, mouse.y)
                        if (bendTick >= 0) {
                            var segment = root.automationBendSegment(bendTick)
                            if (segment !== null) {
                                root.automationBendTick = bendTick
                                var point = root.automationPointAt(bendTick)
                                root.automationBendPreviewT = point !== null && point.hasEase ? point.controlT : 0.5
                                root.automationBendPreviewValue = point !== null && point.hasEase ? point.controlValue : 0.5
                                //! 观测点（§7.6：用 console.warn，console.log 进不了日志文件）
                                console.warn("[midi-automation] press handle tick=" + bendTick)
                                velocityCanvas.requestPaint()
                                return
                            }
                        }

                        var hitTick = root.automationHitPoint(mouse.x, mouse.y)
                        if (hitTick >= 0) {
                            var hit = root.automationPointAt(hitTick)
                            root.automationDragTick = hitTick
                            root.automationDragPreviewTick = hitTick
                            root.automationDragPreviewValue = hit !== null ? hit.value : 0
                            console.warn("[midi-automation] press point tick=" + hitTick
                                         + " value=" + root.automationDragPreviewValue)
                            velocityCanvas.requestPaint()
                            return
                        }

                        var newTick = root.snapTick(root.tickForX(mouse.x))
                        var newValue = root.automationValueForY(mouse.y)
                        console.warn("[midi-automation] press empty -> pending new tick=" + newTick + " value=" + newValue)

                        //! ⚠️ **按下时不写模型**：只记住"要在哪儿新建"，并进入拖动预览。
                        //! 松手时发**一个**命令，把点直接建在**松手时**的位置 —— 于是
                        //! "单击一下"和"按下就拖"都只发一个命令，而一次手势发两个命令正是
                        //! 点会消失的原因（见 automationNewDragging 的说明）。
                        root.automationNewDragging = true
                        root.automationDragStaff = root.currentStaff
                        root.automationDragTick = -1
                        root.automationDragPreviewTick = newTick
                        root.automationDragPreviewValue = newValue
                        velocityCanvas.requestPaint()
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
                    if (root.automationMode) {
                        ++root.automationMoveCount
                    }

                    //! ⚠️ **新建拖动**必须单独一支：这时 `automationDragTick` 是 -1（点还没建），
                    //! 所以下面"拖已有点"的分支不会命中 —— 漏掉这一支的后果是预览不更新，
                    //! 松手时把点建在**按下的位置**，看起来就是"点击后直接移动失败/回弹"
                    //! （2026-10-05 用户实测）。
                    if (root.automationNewDragging) {
                        root.automationDragPreviewTick = root.snapTick(root.tickForX(mouse.x))
                        root.automationDragPreviewValue = root.automationValueForY(mouse.y)
                        velocityCanvas.requestPaint()
                        return
                    }

                    if (root.automationBendTick >= 0) {
                        var bent = root.automationBendFromPointer(mouse.x, mouse.y, root.automationBendTick)
                        if (bent !== null) {
                            root.automationBendPreviewT = bent.t
                            root.automationBendPreviewValue = bent.value
                        }
                        velocityCanvas.requestPaint()
                        return
                    }

                    if (root.automationDragTick >= 0) {
                        //! 拖动中必须有预览值参与绘制 —— 否则按住期间画面纹丝不动（§4.8 的老坑）。
                        root.automationDragPreviewTick = root.snapTick(root.tickForX(mouse.x))
                        root.automationDragPreviewValue = root.automationValueForY(mouse.y)
                        velocityCanvas.requestPaint()
                        return
                    }

                    if (!root.velocityDragging) {
                        return
                    }

                    //! Every move paints, which is what makes the bars follow the pointer.
                    root.paintVelocityAt(mouse.x, mouse.y)
                    velocityCanvas.requestPaint()
                }

                onReleased: function(mouse) {
                    if (mouse.button !== Qt.LeftButton) {
                        return
                    }

                    //! 无条件留痕：任何一次松手都要能在日志里看见走的是哪个分支 ——
                    //! "按下有记录、松手没记录"曾经让一次排查多绕了一轮（2026-10-05）。
                    console.warn("[midi-automation] release bend=" + root.automationBendTick
                                 + " drag=" + root.automationDragTick
                                 + " moves=" + root.automationMoveCount)

                    //! Curve mode：一次手势 = **一个**命令（拖动中只预览，松手才提交）。
                    //!
                    //! 新建优先：按下空白时只记了"要在哪儿建"，松手时在**最终位置**建一个点 ——
                    //! 一个命令，既保住"按下即拖"，也不会出现"两个命令弄丢点"。
                    if (root.automationNewDragging) {
                        var newTick = root.automationDragPreviewTick
                        var newValue = root.automationDragPreviewValue
                        var newStaff = root.automationDragStaff

                        root.automationNewDragging = false
                        root.automationDragStaff = -1

                        console.warn("[midi-automation] release new tick=" + newTick + " value=" + newValue)
                        root.model.setAutomationPoints(newStaff, [{ "tick": newTick, "value": newValue }])
                        root.reloadAutomation()
                        velocityCanvas.requestPaint()
                        return
                    }

                    if (root.automationBendTick >= 0) {
                        var bendTick = root.automationBendTick
                        var bendT = root.automationBendPreviewT
                        var bendValue = root.automationBendPreviewValue
                        var bendStaff = root.automationDragStaff

                        root.automationBendTick = -1
                        root.automationDragStaff = -1

                        console.warn("[midi-automation] release handle tick=" + bendTick
                                     + " t=" + bendT + " value=" + bendValue)
                        root.model.setAutomationPointEase(bendStaff, bendTick, bendT, bendValue)
                        root.reloadAutomation()
                        velocityCanvas.requestPaint()
                        return
                    }

                    if (root.automationDragTick >= 0) {
                        var fromTick = root.automationDragTick
                        var toTick = root.automationDragPreviewTick
                        var movedValue = root.automationDragPreviewValue
                        var dragStaff = root.automationDragStaff

                        root.automationDragTick = -1
                        root.automationDragStaff = -1

                        console.warn("[midi-automation] release point from=" + fromTick + " to=" + toTick
                                     + " value=" + movedValue)
                        //! 观测点：**提交前**模型里到底有没有这两个 tick ——
                        //! "新增点后立刻拖动"第一次失败、第二次成功（2026-10-05 日志），
                        //! 靠这一行就能看出是"from 不在模型里"还是"to 写不进去"。
                        console.warn("[midi-automation] before move: hasFrom=" + (root.automationPointAt(fromTick) !== null)
                                     + " hasTo=" + (root.automationPointAt(toTick) !== null)
                                     + " total=" + root.automationPoints.length)
                        root.model.moveAutomationPoint(dragStaff, fromTick, toTick, movedValue)
                        //! 立刻按模型里的真实数据重画（模型是同步的）：画面不会停在预览上。
                        root.reloadAutomation()

                        //! 观测点：提交后**读回模型**，一眼看清写入结果（有没有落上、值是多少）。
                        var back = root.automationPointAt(toTick)
                        console.warn("[midi-automation] after move: at toTick=" + (back !== null)
                                     + " value=" + (back !== null ? back.value : -1)
                                     + " fromStillThere=" + (root.automationPointAt(fromTick) !== null))
                        velocityCanvas.requestPaint()
                        return
                    }

                    if (!root.velocityDragging) {
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
