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

    //! 混音器面板是页面上的一个 DockPanel（见 MidiEditorPage.qml），视图不拥有它 ——
    //! 这里只接两条线：`mixerOpen` 由页面喂进来（面板的真实开合状态，按钮据此显形），
    //! 点按钮则发一个请求回去（由页面调面板自己的 open()/close()）。
    property bool mixerOpen: false
    signal mixerToggleRequested()

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

    // ── 状态记忆：离开 MIDI 页再回来不该回到初始态 ────────────────────────────
    //!
    //! ⚠️ 这一页的 QML **只在页面可见时存在**：`DockPage.qml` 里 central 是个 `Loader`，
    //! `sourceComponent: (completed && visible) ? central : null` —— 一切到别的页面，本视图就被销毁，
    //! 再回来是全新的一份，于是滚动/缩放/选中的谱表/两个开关全回初始态（用户 2026-10-06 报的）。
    //! 记谱页没有这个问题，因为它的缩放/滚动存在 C++ 的 `NotationViewState` 里，不随视图销毁。
    //! 这里照同样的思路：状态交给**模型**（它声明在 `MidiEditorPage.qml` 的页面对象里，跨页面切换存活）。
    //!
    //! 记的是"视图自己的"状态：选中的谱表、横向缩放、两个滚动偏移、力度车道与 Curve 两个开关。
    //! ⚠️ 其余状态本来就不需要记 —— 播放头在播放控制器里、循环区间在乐谱上（loop in/out）、
    //! solo 在混音器那条轨上、混音器面板的开合由 dock 自己按页面存取。
    property var appliedStateKey: null
    property real pendingScrollX: 0
    property real pendingScrollY: 0
    //! 恢复的滚动还没"落定"（视口尺寸可能还没量出来，见 applyPendingScroll）
    property bool scrollRestorePending: false
    //! 视图是否已经"摆好位"（读完模型里的状态）。
    //! ⚠️ **它是一道闸**：新视图刚建出来时，所有属性都还是默认值，而这时候任何一次属性变化都会
    //! 触发 `syncViewState()` —— **拿默认值把模型里记着的状态覆盖掉** ✗（探针实测：存进去
    //! `scrollX=600`，再读出来是 0）。所以只有摆好位之后才允许写回。
    property bool viewReady: false

    //! 状态一变就交回模型。
    //!
    //! ⚠️ **闸在 `viewReady` 上**（见上面那条说明）：视图刚建出来 / 正在被拆解时，属性都是默认值，
    //! 那些变化也会走到这里 —— 放过去就会用默认值把模型里记着的东西覆盖掉。
    //! 闸门由 `Component.onDestruction` 关上（销毁时只碰自己的属性，不碰模型 ✓）。
    function syncViewState() {
        if (model === null || !viewReady) {
            return
        }

        model.setViewState({
            "staff": currentStaff,
            "pixelsPerTick": pixelsPerTick,
            "scrollX": scrollX,
            "scrollY": scrollY,
            "velocityLane": velocityLaneVisible,
            "automationMode": automationMode,
            "gridIndex": gridIndex,
            "playedChannel": velocityPlayedChannel
        })
    }

    //! 把模型里记着的那份状态摆回来。
    //! 顺序有讲究：先谱表（音域跟着它变）→ 再缩放（内容宽度跟着它变）→ 最后滚动
    //! （`maxScrollX/Y` 要等布局算完，所以推到下一帧）。
    function applyViewState() {
        if (model === null || !hasScore) {
            return
        }

        var state = model.viewState
        if (!state) {
            return
        }

        if (state.velocityLane !== undefined) {
            velocityLaneVisible = state.velocityLane
        }
        if (state.automationMode !== undefined) {
            automationMode = state.automationMode
        }
        //! 🆕 网格与力度车道通道：与别的开关一样按乐谱分开记（离开这一页再回来不该被重置）。
        if (state.gridIndex !== undefined) {
            gridIndex = clamp(state.gridIndex, 0, gridOptions.length - 1)
        }
        if (state.playedChannel !== undefined) {
            velocityPlayedChannel = state.playedChannel
        }
        if (state.pixelsPerTick !== undefined) {
            pixelsPerTick = clamp(state.pixelsPerTick, 0.01, 4.0)
        }
        if (state.staff !== undefined) {
            selectStaff(state.staff)
        }

        pendingScrollX = state.scrollX !== undefined ? state.scrollX : 0
        pendingScrollY = state.scrollY !== undefined ? state.scrollY : 0
        scrollRestorePending = true
        viewReady = true
        Qt.callLater(applyPendingScroll)
    }

    //! ⚠️ **这里不夹取**：恢复的那一刻布局可能还没算完（`maxScrollX/Y` 还是 0），
    //! 夹一下就把位置夹成 0 —— 用户明明拉到中间，回来却回到最左边（真事，探针量到过：
    //! 存进去 600、读出来 0）。正常交互时的夹取由 `clampScroll()` / 滚轮 / 缩放负责。
    function applyPendingScroll() {
        scrollX = Math.max(0, pendingScrollX)
        scrollY = Math.max(0, pendingScrollY)
    }

    //! 视口量出来之后把刚恢复的滚动夹回有效范围（把"夹取"从错误的时机挪到正确的时机）。
    function settleRestoredScroll() {
        if (!scrollRestorePending) {
            return
        }
        if (maxScrollX <= 0 && maxScrollY <= 0) {
            return // 还没量出来，继续等下一次
        }

        scrollRestorePending = false
        clampScroll()
    }

    //! 只在"这一份乐谱还没摆过状态"时摆一次 —— 模型在每次编辑后都会发 `scoreChanged`，
    //! 若每次都摆，用户一边编辑一边滚动就会被打回原位。
    function applyViewStateIfNeeded() {
        if (model === null || !hasScore) {
            return
        }

        var key = model.viewStateKey
        if (appliedStateKey === key) {
            return
        }

        appliedStateKey = key
        applyViewState()
    }

    Component.onCompleted: {
        applyViewStateIfNeeded()
        //! 🆕 这一页一打开就把键盘焦点拿住：电脑键盘弹音（`c.d.e.f.g`）与 Ctrl+Z 都靠它。
        forceActiveFocus()
    }

    //! 销毁时把闸关上：属性在拆解过程中可能被复位，绝不能让那些默认值写回模型
    //! （探针实测过：存进去的 `scrollX=600` 就是这样被 0 覆盖掉的）。
    Component.onDestruction: viewReady = false

    Connections {
        target: root.model

        function onScoreChanged() {
            root.applyViewStateIfNeeded()
        }

        //! 模型换了乐谱（或刚把某份乐谱的状态读出来）：允许重新摆一次。
        function onViewStateChanged() {
            root.appliedStateKey = null
            root.applyViewStateIfNeeded()
        }

        //! 🆕 选中变了（点、Ctrl 点、框选、删掉、结构编辑换了对象都会走这里）。
        //! 视图**不自己维护**选中，只把模型报回来的行号收成查表 —— 一份真相。
        function onSelectionChanged() {
            root.pullSelection()
        }

        function onEditStaffChanged() {
            //! 模型那边换了谱表（例如工程换了、或视图还没同步过）：视图跟上，两处不能各说各话。
            if (root.model !== null && root.currentStaff !== root.model.editStaff) {
                root.currentStaff = root.model.editStaff
            }
        }
    }

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

    //! Ctrl+Z / Ctrl+Shift+Z —— **只有一道**：全局 `Shortcuts`（`AppWindow.qml` 里的
    //! `Shortcuts { }`）负责，这里只留一个**不参与 Shortcut 冲突**的 `Keys.onPressed` 兜底。
    //!
    //! ⛔⛔ **千万不要在这一页再挂 `Shortcut`** —— 2026-10-05 用 Qt 自己的
    //! `qt.gui.shortcutmap` 日志把它钉死了：
    //!
    //! ```
    //! 17:36:17  QShortcutMap::dispatchEvent(): Sending QShortcutEvent("Ctrl+Z", -159, false)
    //!           → ShortcutsController::activate | Ctrl+Z                       ← 正常触发 ✓
    //! 17:37:18  addShortcut(QQuickShortcut(…f90), QKeySequence("Ctrl+Z"), Qt::ApplicationShortcut)
    //!           ← 点开 MIDI 页（页面 QML 此时才创建）后多出第二个注册者
    //! 17:37:24  The following shortcuts are about to be activated ambiguously:
    //!           - QKeySequence("Ctrl+Z") (belonging to QQuickShortcut(…4600))  ← 全局
    //!           - QKeySequence("Ctrl+Z") (belonging to QQuickShortcut(…f90))   ← 本页
    //!           QShortcutMap::dispatchEvent(): Sending QShortcutEvent("Ctrl+Z", -159, true)
    //!           （此后 activate 再也不出现）                                  ← 三个全废 ✗
    //! ```
    //!
    //! 机理（Qt 源码 `qshortcutmap.cpp` + `qquickshortcut.cpp`）：
    //!  * `QShortcutMap::dispatchEvent()` 把**所有 context 匹配的注册者**合成**一个**事件，
    //!    多于一个就把 `QShortcutEvent` 标成 **ambiguous**；
    //!  * `QQuickShortcut::event()` 对 ambiguous **只发 `activatedAmbiguously()`** ——
    //!    `Shortcut.onActivated` **永远不触发**；
    //!  * 而 `tryShortcut()` 对 ExactMatch **返回 true**（事件被消费）⇒ 连
    //!    `Keys.onPressed` 也收不到。
    //!  ⇒ **"抢不到"是不会发生的：只要有两个注册者，就同归于尽。**
    //!  这也正是第 56 条起"按键毫无反应、日志里连痕迹都没有"的真因（当时误判成
    //!  "按键没进入 Qt 的快捷键匹配"，方向正好相反）。
    //!
    //! ⚠️ 另一个坑：**别用 `enabled` 去绑 `canUndo`** —— `enabled: false` 的 Shortcut 完全不
    //! 拦截按键，只要那个属性有一次没刷新，快捷键就"永远没反应"。能不能撤销交给模型判断
    //! （没得撤销时 `undoStack()->undo()` 本来就是 no-op）。
    //!
    //! 为什么这里保留 `Keys.onPressed`：它**不是 `Shortcut`**，不参与上面的冲突。
    //! 全局 Shortcut 正常时按键已被 shortcut map 先消费 ⇒ 这里不会触发（**不会双触发**）；
    //! 全局那条若因 `enabled: false`（`shortcutsModel.active`）而不匹配，Qt **不消耗**按键
    //! （`tryShortcut` 在没有任何 identical 注册者时返回 false）⇒ 这道兜底接管 ✓
    focus: true
    Keys.onPressed: function(event) {
        //! ⛔ **空格不在这里处理**（2026-10-07 试过、实测无效）：`nav-trigger-control` 已经全局注册了
        //! `Space`，Qt 在**快捷键匹配阶段**就把按键消费掉了 ⇒ 这一页的 `Keys.onPressed` **根本收不到**
        //! （日志证据：按空格后只有 `try call action: nav-trigger-control`，没有本页的探针）。
        //! 正解在**快捷键上下文**那一层：走带类动作（play/pause/rewind/loop）改用
        //! `CTX_PROJECT_PAGE_OPENED`，MIDI 页也算"工程页打开着" ⇒ 全局那条 `Space` 在**这一页生效**，
        //! 而且**不需要**新增注册者（新增会让 Qt 判 ambiguous 而同归于尽，见下面的 Ctrl+Z 注释）。
        //!
        //! 🆕 **电脑键盘弹音**（用户 2026-10-07 报的「录制时按 c.d.e.f.g 没反应」）：
        //! `C D E F G A B` = 音名（与记谱页"按 C 输入 C 音"同一套约定），`Z`/`X` = 换八度。
        //! 按下的音**既发声也进录制**（见 `MidiEditorModel::playVirtualKey()`）—— 没有 MIDI 键盘
        //! 的人也能用这条路录。
        //! ⚠️ 这些字母**全局已经注册过**（`note-c`…`note-b`，见 `shortcuts.xml`）：Qt 的快捷键匹配
        //! 会先把按键消费掉，`Keys.onPressed` 根本收不到（与空格那次同源）。所以必须在
        //! `onShortcutOverride` 里**认领**它们（见下），那是 Qt 给"这个按键我要当普通按键用"的正路，
        //! 而且不新增注册者 ⇒ 不会撞 ambiguous。
        //! 🆕 **选中与结构编辑的按键**。它们与下面的字母键一样，全都已经在 `shortcuts.xml` 里
        //! 全局注册过（`Del`/`Backspace` = `action://delete`、`Esc` = `action://cancel`、
        //! `Ctrl+C/V/A` = copy/paste/select-all）⇒ **必须在 `onShortcutOverride` 里先认领**，
        //! 否则 Qt 的快捷键匹配会把按键吃掉，这里根本收不到（第 71 / 73 条的同一个根因）。
        if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            console.warn("[midi-select] delete", root.selectedCount, "note(s)")
            root.model.deleteSelectedNotes()
            event.accepted = true
            return
        }

        if (event.key === Qt.Key_Escape) {
            root.model.clearSelection()
            event.accepted = true
            return
        }

        if ((event.modifiers & Qt.ControlModifier) !== 0) {
            if (event.key === Qt.Key_C) {
                root.model.copySelection()
                console.warn("[midi-select] copy", root.selectedCount, "note(s)")
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_V) {
                //! 粘到**播放头**：粘到哪永远看得见（而不是"粘到看不见的某个地方"）。
                //! 吸附到网格，这样粘出来的位置与画布上画的一致。
                var pasteTick = root.snapTick(Math.round(root.playbackTick))
                console.warn("[midi-select] paste at tick", pasteTick)
                root.model.pasteAtTick(root.currentStaff, pasteTick)
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_A) {
                var allRows = []
                var all = root.visibleRows
                for (var a = 0; a < all.length; ++a) {
                    allRows.push(all[a].row)
                }
                root.model.setSelectedRows(allRows, false)
                console.warn("[midi-select] select all ->", allRows.length)
                event.accepted = true
                return
            }
        }

        if ((event.modifiers & Qt.ControlModifier) === 0) {
            if (handleVirtualKeyPressed(event)) {
                return
            }
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

    //! 松开要配对：不接这个的话，"按一下"录进来的音永远不封口（会一直挂到停止）。
    Keys.onReleased: function(event) {
        if (event.isAutoRepeat) {
            event.accepted = true
            return
        }

        var pitch = root.virtualKeyPitch(event.key)
        if (pitch >= 0 && root.model !== null) {
            root.model.playVirtualKey(pitch, false)
        }

        if (pitch >= 0 || event.key === Qt.Key_Z || event.key === Qt.Key_X) {
            event.accepted = true
        }
    }

    //! ⚠️ **必须认领**：`C D E F G A B` 在 `shortcuts.xml` 里是记谱页的"输入音符"快捷键
    //! （`note-c`…`note-b`，`Qt.ApplicationShortcut`）—— 不认领的话 Qt 的快捷键匹配先把按键吃掉，
    //! `Keys.onPressed` 与 `onReleased` 都收不到（用户报的"按音没反应"就是这个）。
    //! 认领 = `event.accepted = true`，Qt 便不再交给快捷键表，按键按普通按键送到本项。
    //! 只有**带修饰键**的组合留给快捷键（Ctrl+Z 撤销等照旧）。
    Keys.onShortcutOverride: function(event) {
        //! 🆕 选中 / 结构编辑类的按键**也要在这里认领**，理由与下面那组字母键**完全相同**：
        //! 它们全都已经在 `shortcuts.xml` 里注册过（`Del` / `Backspace` = `action://delete`、
        //! `Esc` = `action://cancel`、`Ctrl+C/V` = copy/paste、`Ctrl+A` = select-all），
        //! Qt 的快捷键匹配会先把按键消费掉 —— 不认领的话这一页**收不到**，表现是"按删除没反应"。
        //! ⚠️ 认领（`event.accepted = true`）是**唯一**正确的做法：再挂一个 `Shortcut` 只会让
        //! Qt 把两个注册者一起判 ambiguous 而同归于尽（第 60 条的真事）。
        if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace || event.key === Qt.Key_Escape) {
            event.accepted = true
            return
        }

        //! ⚠️ 只认领**纯粹的** Ctrl 组合：带 Alt/Meta 的留给系统与全局快捷键（例如 Ctrl+Alt+A
        //! 是"选择和弦里的音"）。Ctrl+Z / Ctrl+Shift+Z 也**不认领** —— 撤销由全局那一条负责。
        if ((event.modifiers & Qt.ControlModifier) !== 0
                && (event.modifiers & (Qt.AltModifier | Qt.MetaModifier)) === 0
                && (event.key === Qt.Key_C || event.key === Qt.Key_V || event.key === Qt.Key_A)) {
            event.accepted = true
            return
        }

        if ((event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) !== 0) {
            return
        }

        if (root.virtualKeyPitch(event.key) >= 0 || event.key === Qt.Key_Z || event.key === Qt.Key_X) {
            event.accepted = true
        }
    }

    //! NOTE: **No `Shortcut` here on purpose** - see the comment on `Keys.onPressed` above.
    //! A second Ctrl+Z registration makes Qt dispatch the key ambiguously and kills them all.

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

    // ── playback: 定位 / 循环 / 只播这个谱表 ─────────────────────────────────
    //!
    //! ⚠️ 这三件事**都不是这一页自己的状态**：定位走 `IPlaybackController::seekTick()`，循环写的是
    //! 乐谱自己的 loop in/out（记谱页那两个循环标记指的就是这两个 tick），独奏写的是**混音器那条
    //! 轨的 solo**。所以下面全是"从模型读回来"，而不是在 QML 里另存一份 —— 两页/混音器必须始终
    //! 是同一个状态。

    //! 播放头的位置就是上面那个 `playbackTick`（停止时也画：点一下标尺定位之后，
    //! 用户要能看见自己点在哪 —— 原来的实现只在 `isPlaying` 时画，点完什么都看不到）。

    readonly property int loopInTick: (model !== null && model.hasScore) ? model.loopInTick : 0
    readonly property int loopOutTick: (model !== null && model.hasScore) ? model.loopOutTick : 0
    readonly property bool loopEnabled: model !== null && model.hasScore && model.loopEnabled

    //! 正在标尺上拖出来的循环区间（-1 = 没在拖）：拖动中只预览，松手才发一次命令。
    property int loopDragAnchorTick: -1
    property int loopDragPreviewTick: -1

    //! 循环区间的吸附：1/16 音符（480 tick = 四分音符）。比这更细的循环没有音乐意义，
    //! 而且吸附也是**绕开上游 `BoundaryTick` 陷阱**的手段（见 midiLoopRangeFromDrag 的说明）。
    readonly property int loopSnapTicks: 120

    //! "只播放当前谱表"—— 打开时把选中谱表的轨道 solo 掉（就是混音器上那个 solo）。
    readonly property bool soloActive: model !== null && model.hasScore && model.soloActive

    // ── theme ────────────────────────────────────────────────────────────────
    readonly property color backgroundColor: ui.theme.backgroundPrimaryColor
    readonly property color panelColor: ui.theme.backgroundSecondaryColor
    readonly property color gridColor: ui.theme.strokeColor
    readonly property color barLineColor: ui.theme.fontSecondaryColor
    readonly property color textColor: ui.theme.fontPrimaryColor
    readonly property color dimTextColor: ui.theme.fontSecondaryColor
    readonly property color noteColor: ui.theme.accentColor
    readonly property color cursorColor: ui.theme.fontPrimaryColor

    //! 录制预览的颜色。刻意**不**用音符那套调色板：这些音还没写进乐谱，按停止才会真的落下去，
    //! 画成一样的颜色会让人以为"已经在谱里了"。
    readonly property color recordColor: "#e2554f"
    readonly property color recordHeldColor: "#ff8a5c"

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

    //! 拖动中**上一次已经试听过**的音高（-1 = 还没响过）。用它当判据，音高真的变了才再响一声 ——
    //! 否则鼠标每移动一像素都会发一次音。记谱页拖动音符也是"音高变了才播"（见 `dragPlayedPitch`
    //! 的用法，以及 `维护手册.md` §4.8 里"试听"那一行）。
    property int dragPlayedPitch: -1

    //! What the current drag edits. Dorico draws the played extent over the notated one; grabbing the
    //! right edge of a block edits the played length, Shift+dragging edits the played start, and a
    //! plain drag still edits the pitch.
    //!
    //! 🆕 **记谱层那两条（Alt）**：卷帘窗里一个音有**两套时值**，而用户要能分别改（用户 2026-10-07
    //! 明确说：保留现在这两种状态 —— 外框 = 记谱时值、实心条 = 演奏时值）：
    //!   * **外框**（空心轮廓）= `ChordRest` 的时值，改它走上游的 `Score::changeCRlen()`：
    //!     变短自动补休止符、变长自动按小节切开并生成连音线、谱面不够长自动加小节。
    //!     它是**记谱**的，所以乐谱页看到的就是它。
    //!   * **实心条**（颜色）= `NoteEvent::len`（千分比），只影响播放与 MIDI 导出，**谱面不动**。
    //! 两条手势分开：**无修饰键**拖右边缘 = 演奏时值；**Alt** + 拖右边缘 = 记谱时值。
    //! 加 Alt 而不是让无修饰键同时管两件事，是因为记谱时值会**改谱面结构**（连音线、休止符、
    //! 小节），误触的代价远大于演奏层。
    readonly property int dragModePitch: 0
    readonly property int dragModePlayStart: 1
    readonly property int dragModePlayLength: 2
    readonly property int dragModeNotatedMove: 3
    readonly property int dragModeNotatedLength: 4
    property int dragMode: 0

    property real dragStartX: 0
    property int dragStartPlayStart: 0
    property int dragStartPlayDuration: 0
    property int dragPreviewPlayStart: 0
    property int dragPreviewPlayDuration: 0

    //! 🆕 记谱层拖动的预览值（外框画的就是它们；松手才提交一次）。
    property int dragStartNotatedTick: 0
    property int dragStartNotatedDuration: 0
    property int dragPreviewNotatedTick: 0
    property int dragPreviewNotatedDuration: 0

    //! 🆕 多选拖动：这两条是**整批**的增量，画所有选中的音时都加上去（所以整块一起动）。
    property int dragDeltaPitch: 0
    property int dragDeltaTicks: 0

    //! 按下了但还没移动（用来区分"点一下 = 选中/定位"与"真的拖了"。拖动提交的判据仍然是
    //! "值真的变了"，这个只是让"点一下已选中的音 = 收成单选"成立）。
    property bool dragMoved: false

    //! 🆕 框选（在空白处按下并拖动）：矩形 + 是否叠加（Ctrl）。
    property bool marqueeActive: false
    property real marqueeX0: 0
    property real marqueeY0: 0
    property real marqueeX1: 0
    property real marqueeY1: 0
    property bool marqueeAdditive: false

    //! 🆕 选中态：由模型给的行号（`notes()` 的下标）算出一张查表，绘制时 O(1) 判断。
    //! **不能反过来在绘制循环里逐个问模型**（那是每个音一次跨语言调用，画布每次重绘都付一遍）。
    property var selectionRows: []
    property var selectionIndex: ({})

    //! ── 网格（吸附粒度 + 插入时值）──────────────────────────────────────────────────
    //!
    //! 一条下拉同时管三件事，因为它们在用户心里本来就是一件事（"现在按多大的格子编辑"）：
    //!  * 演奏层拖动（起点/时长）吸附到它；
    //!  * 记谱层拖动（Alt）吸附到它；
    //!  * **插入音符的时值**就是它（双击空白画出来的音有多长）。
    //! 默认 **1/32（60 tick）** —— 与改动前的 `playSnapTicks` 一模一样，所以"没碰过这个下拉"的
    //! 用户不会感觉到任何行为变化；要画四分音符的人在下拉里选 1/4 即可。
    //! ⚠️ 三连音档位是 `480/3 = 160`、`480/6 = 80`，**不是**"二连音的一半"：写错的话吸附会把
    //! 三连音吸到最近的二连音格子上（`维护手册.md` §4.8.4 那条量化陷阱，同一份道理）。
    property int gridIndex: 3
    readonly property var gridOptions: [
        { "label": "1/4", "ticks": 480 },
        { "label": "1/8", "ticks": 240 },
        { "label": "1/16", "ticks": 120 },
        { "label": "1/32", "ticks": 60 },
        { "label": "1/8T", "ticks": 160 },
        { "label": "1/16T", "ticks": 80 }
    ]
    readonly property int snapTicks: {
        var index = Math.max(0, Math.min(gridIndex, gridOptions.length - 1))
        return gridOptions[index].ticks
    }

    property int hoveredNoteIndex: -1

    //! 网格上"点了一下空白"的定位（见 rollMouse 的 onPressed/onReleased）：
    //! 按下时记下位置，松手时若既没抓到音符、也没怎么移动，就把播放位置挪到那儿 ——
    //! 记谱页也是"点空白 = 把游标挪过去"，这一页没理由不一样。
    property real rollPressX: 0
    property int rollPressSeekTick: -1

    property bool velocityDragging: false
    //! Which staff the current velocity drag edits. The lane is split into one horizontal band per
    //! staff, so a drag must never touch another staff's notes that happen to sit on the same tick.
    property int velocityDragStaff: -1

    //! 🆕 力度车道的**第二个通道**（工具条上的 `Played` 开关）。
    //!
    //!  * `false` = 每个音**自己的力度**（`Pid::USER_VELOCITY`）：它是"覆盖"语义 —— 有值就不再跟随
    //!    表情记号（pp/ff、渐强线），所以柱子画成"细 + 半透明 / 粗 + 实心 + 小帽"两种。
    //!  * `true` = **演奏力度**（`NoteEvent::velocityMultiplier`，百分比）：它是"乘一下"语义 ——
    //!    这个音本来该多响，再乘这个系数。**不覆盖**表情记号，所以它和上面那条可以同时存在
    //!    （`维护手册.md` §4.8 有专门一条讲这两条通路怎么共存）。
    //!
    //! 两个通道各画各的柱子、各刷各的值、右击各清各的（力度 → 0 = 回到跟随表情记号；
    //! 演奏力度 → 100% = 回到"没调过"）。**同一时刻只编辑一个通道**：混在一起刷会让人分不清
    //! 自己到底改了哪条 —— 而这两条在合成器里是两套语义。
    property bool velocityPlayedChannel: false

    //! 演奏力度通道的基准线（100% = 没调过）在这条车道里的比例位置。
    //! 画柱子时以它为零点：往上 = 更响、往下 = 更轻，一眼看得出"这是相对量，不是绝对力度"。
    readonly property real playedVelocityBaseline: 0.5
    readonly property int playedVelocityMin: 10
    readonly property int playedVelocityMax: 200

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

    //! ── 实时录制（工具条上那组控件就是它的全部入口）──────────────────────────────────
    //!
    //! 谁在做主：**模型**。这一页只做三件事：把"录到哪个谱表"（视图才知道的选中）传进去、
    //! 画模型给出来的实时预览、显示状态。量化的档位与强度、时间戳怎么变成 tick、
    //! 停止时怎么写进乐谱，全在 `MidiEditorModel` / `MidiRecorder` 里。
    //!
    //! 录到哪个谱表在**开始时**就定死（`recordStaff`），中途换谱表不会把预览画到别的谱表上。
    property int recordStaff: 0

    readonly property bool recording: model !== null && model.isRecording
    readonly property var recordedRows: (model !== null) ? model.recordedNotes : []
    readonly property var quantizeGrids: (model !== null) ? model.quantizeGrids : []
    readonly property string takeSummary: (model !== null) ? model.lastTakeSummary : ""

    readonly property string quantizeLabel: {
        if (quantizeGrids.length === 0) {
            return "—"
        }
        var index = Math.max(0, Math.min(model.quantizeGridIndex, quantizeGrids.length - 1))
        return quantizeGrids[index].label
    }

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
    //!
    //! ⚠️ 录制的实时预览**也要算进来**：空谱表上录一个低音时，只按已有音符算范围的话它会被画到
    //! 屏幕外面去 —— 用户看到的是"按了键什么都没发生"（而音其实录到了）。
    readonly property var rangeRows: (recording && currentStaff === recordStaff) ? recordedRows : []

    readonly property int lowestPitch: {
        var lo = 127
        var list = visibleRows
        for (var i = 0; i < list.length; ++i) {
            lo = Math.min(lo, list[i].note.pitch)
        }
        for (var r = 0; r < rangeRows.length; ++r) {
            lo = Math.min(lo, rangeRows[r].pitch)
        }
        if (lo > 127) {
            return 60
        }
        return Math.max(0, lo - 2)
    }

    readonly property int highestPitch: {
        var hi = 0
        var list = visibleRows
        for (var i = 0; i < list.length; ++i) {
            hi = Math.max(hi, list[i].note.pitch)
        }
        for (var r = 0; r < rangeRows.length; ++r) {
            hi = Math.max(hi, rangeRows[r].pitch)
        }
        if (hi <= 0) {
            return 72
        }
        return Math.min(127, hi + 2)
    }

    function selectStaff(index) {
        if (index >= 0 && index < staffCount) {
            currentStaff = index
        }
    }

    // ── 选中 ────────────────────────────────────────────────────────────────────────
    //!
    //! 选中态存在**模型**里（按音符身份，见 `MidiEditorModel` 的说明），视图只保留一张行号查表。
    //! 视图侧要做的三件事：press/release 时告诉模型点到哪一行、框选时把矩形里的行号成批递过去、
    //! 绘制时 O(1) 判断"这个音选中没有"。

    //! 行号数组 → 查表。**一次重建、整帧复用** —— 若改成在绘制循环里逐个问模型，
    //! 每次重绘就是"音符数 × 一次跨语言调用"。
    function selectionLookup(rows) {
        var map = ({})
        for (var i = 0; i < rows.length; ++i) {
            map[rows[i]] = true
        }
        return map
    }

    function isSelectedRow(row) {
        return selectionIndex[row] === true
    }

    //! 视图此刻选中了几个（工具条上的按钮用它灰显）。
    readonly property int selectedCount: (model !== null) ? model.selectedCount : 0

    //! 把模型报回来的行号接进视图状态。
    function pullSelection() {
        selectionRows = (model !== null && model.hasScore) ? model.selectedRows : []
        selectionIndex = selectionLookup(selectionRows)
        gridCanvas.requestPaint()
    }

    //! 框选：矩形与"音高 × 时间"都相交的音才算选中（与画出来的方块一致）。
    function rowsInMarquee(x0, y0, x1, y1) {
        var left = Math.min(x0, x1)
        var right = Math.max(x0, x1)
        var top = Math.min(y0, y1)
        var bottom = Math.max(y0, y1)

        var out = []
        var list = visibleRows
        for (var i = 0; i < list.length; ++i) {
            var note = list[i].note

            var nx = xForTick(note.tick)
            var ny = yForPitch(note.pitch)
            if (nx > right || nx + noteWidth(note) < left) {
                continue
            }
            if (ny > bottom || ny + noteHeight() < top) {
                continue
            }
            out.push(list[i].row)
        }
        return out
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

    // ── 电脑键盘弹音（没有 MIDI 键盘也能录）──────────────────────────────────────
    //!
    //! 字母键 = **音名**：`C`→C、`D`→D、`E`→E、`F`→F、`G`→G、`A`→A、`B`→B ——
    //! 与记谱页"按 C 输入 C 音"**同一套约定**（用户 2026-10-07 报的就是「按 c.d.e.f.g」）。
    //! `Z` / `X` = **降 / 升八度**（默认 C4 那一带）。
    //!
    //! 不抄 DAW 那套 `ZSXDCVGBHNJM`（下排 = 一个八度）：用户按的是**音名**，
    //! 而 MuseScore 自己的音符输入也是"字母即音名" —— 两处一致比抄 DAW 更不容易让人按错。
    property int keyboardOctave: 4

    //! 这个按键对应哪个音高？-1 = 不是弹音键。
    //! ⚠️ 用 `Qt.Key_*`（**逻辑键**）而不是字符：与快捷键表同一套判据，换个键盘布局也对得上。
    function virtualKeyPitch(key) {
        var semitone = -1
        if (key === Qt.Key_C) {
            semitone = 0
        } else if (key === Qt.Key_D) {
            semitone = 2
        } else if (key === Qt.Key_E) {
            semitone = 4
        } else if (key === Qt.Key_F) {
            semitone = 5
        } else if (key === Qt.Key_G) {
            semitone = 7
        } else if (key === Qt.Key_A) {
            semitone = 9
        } else if (key === Qt.Key_B) {
            semitone = 11
        } else {
            return -1
        }

        var pitch = (keyboardOctave + 1) * 12 + semitone    // C4 = 60
        if (pitch < 0 || pitch > 127) {
            return -1
        }
        return pitch
    }

    //! 按下：弹音或换八度。返回 true = 这个按键我们认领了（调用方不用再管）。
    function handleVirtualKeyPressed(event) {
        var playable = virtualKeyPitch(event.key) >= 0 || event.key === Qt.Key_Z || event.key === Qt.Key_X
        if (!playable) {
            return false
        }

        //! ⚠️ 自动重复**不是**"又按了一下"：当成新音头会在谱面上录出一串颤音。
        if (!event.isAutoRepeat) {
            var pitch = virtualKeyPitch(event.key)
            if (pitch >= 0) {
                if (model !== null) {
                    model.playVirtualKey(pitch, true)
                }
            } else if (event.key === Qt.Key_Z) {
                keyboardOctave = clamp(keyboardOctave - 1, 0, 8)
                console.warn("[midi-keys] octave down ->", keyboardOctave)
            } else {
                keyboardOctave = clamp(keyboardOctave + 1, 0, 8)
                console.warn("[midi-keys] octave up ->", keyboardOctave)
            }
        }

        event.accepted = true
        return true
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

    //! 试听：按**可见行**下标响一声（`visibleIndex` = `noteIndexAt()` 的返回值）。
    //!
    //! 收成一个函数是**有意为之**：按下音符块与拖动改音高是两条路径，而"可见行下标 → 模型要的
    //! `notes()` 下标"这个映射**只在这里写一次** —— 两个调用点各写一遍迟早会分叉，而分叉的表现是
    //! "点这个响那个"（不报错、不崩，最难查的一类）。模型侧的两个入口都自带越界保护。
    //!
    //! `pitch` 不给（< 0）时响**谱面上那个音**；拖动时传**拖到的音高** —— 拖动中卷帘窗不写谱，
    //! 所以必须让模型造个临时音符才响得出"拖到的那个音"（用户 2026-10-07 报的「上下拖动响的是
    //! 同一个音」）。记谱页拖动响的是拖到的音，这里与它一致。
    function auditionNoteAt(visibleIndex, pitch) {
        var list = visibleRows
        if (visibleIndex < 0 || visibleIndex >= list.length) {
            return
        }

        if (pitch === undefined || pitch < 0) {
            model.playNote(list[visibleIndex].row)
        } else {
            model.playNoteAtPitch(list[visibleIndex].row, pitch)
        }
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

    // ── playback: 定位 / 循环 / 跟随 ─────────────────────────────────────────

    //! 把播放位置移到某个 tick —— 点标尺（或网格空白处）就是这一下。
    //! 不做吸附：在钢琴卷帘里"从这里开始播"要能落在两个音之间，吸附反而挡住这件事。
    function seekToTick(tick) {
        if (!hasScore || model === null) {
            return
        }

        model.seekTick(clamp(Math.round(tick), 0, totalTicks))
    }

    //! 标尺上拖出来的一段 → 循环区间。**数学在模型那一侧**（`midiLoopRangeFromDrag`，有单测）：
    //! 顺序无关、按网格吸附、越界夹住，而且"没跨过一格"会返回 valid=false ⇒ 那就是一次点击。
    function applyLoopDrag(anchorTick, releasedTick) {
        if (!hasScore || model === null) {
            return false
        }

        var range = model.loopRangeFromDrag(Math.round(anchorTick), Math.round(releasedTick), loopSnapTicks)
        if (!range.valid) {
            return false
        }

        model.setLoopRange(range.inTick, range.outTick)
        return true
    }

    //! 播放时让视口跟着播放头走 —— **算在模型里**（`midiFollowScrollX`，有单测），这里只喂当前
    //! 视口的四个数。没在播放时一个像素都不动：用户自己滚到哪里就停在哪里。
    function followPlayhead() {
        if (!isPlaying || !hasScore) {
            return
        }

        var x = xForTick(playbackTick)
        var next = model !== null
                   ? model.followScrollX(scrollX, x, viewportWidth, maxScrollX)
                   : scrollX
        if (Math.abs(next - scrollX) > 0.5) {
            scrollX = next
        }
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

    //! 演奏力度通道的值：车道中线 = **100%**（没调过），往上更响、往下更轻。
    //! 以中线为基准是刻意的 —— 这个量是**相对**的，画成"从底部量"会让人误以为它和力度是一回事。
    function playedVelocityForY(y) {
        var lane = Math.max(1, velocityCanvas.height)
        var baseline = lane * playedVelocityBaseline
        var span = Math.max(1, baseline - 2)                   //!< 中线到顶 = 100 个百分点
        var percent = 100 + Math.round((baseline - y) / span * 100)
        return clamp(percent, playedVelocityMin, playedVelocityMax)
    }

    //! 一个音的演奏力度在车道上画多高（从哪到哪）—— 柱子以中线为起点，向上或向下长。
    function playedVelocityBar(percent, laneHeight) {
        var baseline = laneHeight * playedVelocityBaseline
        var span = Math.max(1, baseline - 2)
        var h = Math.abs(percent - 100) / 100 * span
        return { "y": percent >= 100 ? baseline - h : baseline, "height": Math.max(1, h) }
    }

    //! 一个音在这条车道上"当前显示的值"：按通道取（力度 1..127 / 演奏力度 10..200）。
    function laneValueForNote(note) {
        return velocityPlayedChannel ? note.playVelocityPercent : note.velocity
    }

    //! One stroke of the brush: every note under the pointer keeps the height the pointer has right
    //! now. Called on press and on every move, so the lane tracks the pointer instead of waiting for
    //! the release.
    function paintVelocityAt(x, y) {
        var value = velocityPlayedChannel ? playedVelocityForY(y) : velocityForY(y)
        var trail = velocityTrail
        var list = visibleRows
        var touched = false

        for (var i = 0; i < list.length; ++i) {
            var note = list[i].note
            //! Either the pointer is over the note, or close enough to its onset that a fast sweep
            //! must not skip it - a brush that leaves gaps feels broken.
            if (hitHorizontally(note, x) || Math.abs(xForTick(note.tick) - x) <= 8) {
                trail[note.tick] = value
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
        var snap = snapTicks
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

    onScrollXChanged: {
        repaintAll()
        syncViewState()
    }
    onScrollYChanged: {
        repaintAll()
        syncViewState()
    }
    onRowHeightChanged: repaintAll()
    onPixelsPerTickChanged: {
        repaintAll()
        syncViewState()
    }
    onVelocityLaneVisibleChanged: syncViewState()
    onGridIndexChanged: {
        repaintAll()
        syncViewState()
    }
    onVelocityPlayedChannelChanged: {
        repaintAll()
        syncViewState()
    }
    onNotesChanged: {
        //! The model has reported back, so the stored values now match what was painted; the trail
        //! has done its job and the bars switch over to the real data without a visible step.
        if (velocityPending) {
            velocityPending = false
            velocityTrail = ({})
        }
        //! 数据重建 = 行号可能整体挪过位（插入/删除音符），所以选中那张查表必须跟着重建 ——
        //! 模型给的 `selectedRows` 是**当场算的**，视图照抄即可。
        pullSelection()
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
        syncViewState()
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

        //! "只播放当前谱表"跟着选中的谱表走：换谱表时把独奏交接过去（模型只释放它自己 solo 的那条轨，
        //! 用户在混音器里自己按下的 solo 不动）。
        if (soloActive && model !== null) {
            model.setSoloStaff(currentStaff)
        }

        //! 🆕 **编辑哪个谱表**也在这里交给模型：录制 / 插入音符 / 粘贴都要问"往哪个谱表写"，
        //! 而模型看不到视图的 `currentStaff` —— 三处必须同一个来源（见 `editStaff` 的说明）。
        if (model !== null) {
            model.setEditStaff(currentStaff)
        }

        repaintAll()
        syncViewState()
    }
    onAutomationPointsChanged: repaintAll()
    onRecordedRowsChanged: {
        //! 录制的实时预览每一帧都在变（还按着的音在长），所以这一条必须便宜：
        //! `repaintAll()` 只请求四个画布重绘，不重建任何数据，也不写视图状态
        //! （滚动/缩放没变，写了反而是每 60ms 一次无用的回写）。
        repaintAll()
    }
    onRecordingChanged: {
        if (recording) {
            //! 起录时把"录到哪个谱表"定死（视图才知道这个选中），并让模型知道 ——
            //! 模型看不到视图的 `currentStaff`，所以由这里传进去。
            recordStaff = currentStaff
            if (model !== null) {
                model.setEditStaff(currentStaff)
            }

            //! 🆕 **把键盘焦点收回来**：用户是按走带上那个录制键进来的，焦点此刻在**那个按钮**上 ——
            //! 不收的话接下来按 `c.d.e.f.g` 全被按钮吃掉（方向键还会在按钮之间跳），
            //! 表现就是用户报的"录制时按音没反应"。
            forceActiveFocus()
        }
        repaintAll()
    }
    onPlaybackTickChanged: {
        //! 播放时视口跟着走；停止时**一个像素都不动**（用户滚到哪儿就停在哪儿，
        //! 点标尺定位也不会把视图弹走 —— 他点的位置本来就在屏幕上）。
        followPlayhead()
        repaintAll()
    }
    onLoopInTickChanged: repaintAll()
    onLoopOutTickChanged: repaintAll()
    onLoopEnabledChanged: repaintAll()
    onHeightChanged: repaintAll()
    onWidthChanged: repaintAll()
    //! 内容尺寸/视口一变，就把"刚恢复、还没落定"的滚动夹回有效范围（见 settleRestoredScroll）。
    onMaxScrollXChanged: settleRestoredScroll()
    onMaxScrollYChanged: settleRestoredScroll()

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
            id: hintLabel

            anchors.left: titleLabel.right
            anchors.leftMargin: 16
            //! 右边**必须**让给按钮行：工具栏这一行的高度只有 36px，而提示文字比按钮行还长 ——
            //! 让它自由伸展的话，多一个按钮（Solo、混音器开关）就会把文字压到按钮底下，
            //! 两边叠在一起（2026-10-06 实拍发现：窄窗口下 "clear loop" 的碎片从按钮缝里露出来）。
            //! 现在文字有确定的宽度、放不下就省略号，按钮行永远是完整的。
            anchors.right: toolButtons.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter

            visible: root.hasScore
            elide: Text.ElideRight

            text: qsTrc("notationscene", "Click = select · Ctrl+click or drag a box = multi-select · drag a note = pitch, its right edge = played length, Shift+drag = played start · Alt+drag = move in time, Alt+right edge = notated length · double-click empty space = insert · Del/Ctrl+C/Ctrl+V · velocity lane: drag = own velocity, right-click = follow dynamics · ruler: click = play from here, drag = loop, right-click = clear loop")

            color: root.dimTextColor
            font: ui.theme.bodyFont
        }

        Row {
            id: toolButtons

            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            //! ── 实时录制 ─────────────────────────────────────────────────────────────
            //!
            //! 一个按钮管"开始/停止"，一个下拉管量化网格，一个开关管节拍器。
            //! 三个都用**真控件**（`FlatButton` / 真 `MouseArea` 的矩形）而不是画布上的自绘：
            //! 本环境里合成鼠标到不了 Qt Quick 画布，但工具条上的控件进得了无障碍树 ——
            //! `tools/ui-probe.ps1 -Action click -Name Record -ControlType Button` 点得中它，
            //! "按一下能不能真的开始录"因此是**机器可验**的（见 `维护手册.md` §7.6）。
            //! ── 实时录制（**按钮本身不在这里**）────────────────────────────────────────
            //!
            //! ⚠️ 录制键做在**走带按钮行**上、节拍器图标左边（`MidiEditorPage.qml` 往
            //! `PlaybackToolBar.extraItem` 里塞的那一个）—— 因为它与播放/循环/节拍器本来就是
            //! 一件事：按下它 = 从这里开始播 + 开始记。这一页只留**量化网格**与**状态读数**。
            //!
            //! 为什么量化不跟着搬过去：它是"这一页怎么记谱"的设置，不是走带的一部分；
            //! 而走带那条属性是一个通用插槽（见 `PlaybackToolBarModel::extraItem`），
            //! 往里塞两个控件就不是"一个按钮"了。
            //
            //! 节拍器开关也**不再重复**：走带行里那个就是同一个开关（`toggleMetronome()` 写的是
            //! 同一份记谱配置），两个按钮说同一件事只会让人怀疑哪个才算数。

            //! 量化网格。用 Popup 而不是 ComboBox：与旁边的谱表选择器同一套做法，
            //! 不受控件样式影响，而且这一行的 36px 高度也放不下一个下拉框。
            Rectangle {
                id: quantizeSelector

                height: 22
                width: Math.max(52, quantizeLabelText.implicitWidth + 20)
                radius: 3
                color: quantizePopup.opened ? ui.theme.buttonColor : "transparent"
                border.width: 1
                border.color: root.gridColor

                Text {
                    id: quantizeLabelText

                    anchors.centerIn: parent
                    text: root.quantizeLabel
                    color: root.textColor
                    font: ui.theme.bodyFont
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: quantizePopup.open()
                }

                ToolTip {
                    text: qsTrc("notationscene", "Quantize grid applied when the take is written")
                    visible: quantizePopup.opened === false && quantizeSelectorHover.containsMouse
                    delay: 600
                }

                MouseArea {
                    id: quantizeSelectorHover

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }

                Popup {
                    id: quantizePopup

                    parent: quantizeSelector
                    x: 0
                    y: quantizeSelector.height + 2
                    width: Math.max(quantizeSelector.width, 120)
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
                            model: root.quantizeGrids

                            delegate: Rectangle {
                                id: quantizeOption

                                required property var modelData
                                required property int index

                                width: quantizePopup.width - 8
                                height: 24
                                radius: 2
                                color: (index === root.model.quantizeGridIndex || quantizeOptionMouse.containsMouse)
                                       ? ui.theme.buttonColor : "transparent"

                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: quantizeOption.modelData.label
                                    color: root.textColor
                                    font: ui.theme.bodyFont
                                }

                                MouseArea {
                                    id: quantizeOptionMouse

                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        root.model.quantizeGridIndex = quantizeOption.index
                                        quantizePopup.close()
                                    }
                                }
                            }
                        }
                    }
                }
            }

            //! 🆕 **网格**：吸附粒度 + 插入时值（一条下拉，因为它们在用户心里是一件事）。
            //! 与左边的量化分别放在两个下拉里是**有意**的：量化是"录完之后怎么摆"，
            //! 网格是"现在拖动/插入按多大格子" —— 混成一个的话，改录制量化会顺手改掉拖动手感。
            //! 用 Popup 而不是 ComboBox：与旁边的谱表/量化选择器同一套做法，不受控件样式影响，
            //! 而且这一行的 36px 高度也放不下一个下拉框。
            Rectangle {
                id: gridSelector

                height: 22
                width: Math.max(52, gridLabelText.implicitWidth + 20)
                radius: 3
                color: gridPopup.opened ? ui.theme.buttonColor : "transparent"
                border.width: 1
                border.color: root.gridColor

                Text {
                    id: gridLabelText

                    anchors.centerIn: parent
                    text: qsTrc("notationscene", "Grid") + " " + root.gridOptions[Math.max(0, Math.min(root.gridIndex,
                                                                                                      root.gridOptions.length - 1))].label
                    color: root.textColor
                    font: ui.theme.bodyFont
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: gridPopup.open()
                }

                ToolTip {
                    text: qsTrc("notationscene", "Snap of every drag, and the length of a note inserted by double-clicking")
                    visible: gridPopup.opened === false && gridSelectorHover.containsMouse
                    delay: 600
                }

                MouseArea {
                    id: gridSelectorHover

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }

                Popup {
                    id: gridPopup

                    parent: gridSelector
                    x: 0
                    y: gridSelector.height + 2
                    width: Math.max(gridSelector.width, 110)
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
                            model: root.gridOptions

                            delegate: Rectangle {
                                id: gridOption

                                required property var modelData
                                required property int index

                                width: gridPopup.width - 8
                                height: 24
                                radius: 2
                                color: (index === root.gridIndex || gridOptionMouse.containsMouse)
                                       ? ui.theme.buttonColor : "transparent"

                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: gridOption.modelData.label
                                    color: root.textColor
                                    font: ui.theme.bodyFont
                                }

                                MouseArea {
                                    id: gridOptionMouse

                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        root.gridIndex = gridOption.index
                                        gridPopup.close()
                                        syncViewState()

                                        //! 观测点（验证用）：网格同时决定拖动手感与插入时值，
                                        //! "改了没生效"时这一行就是答案。
                                        console.warn("[midi-grid] snap =", root.snapTicks, "tick")
                                    }
                                }
                            }
                        }
                    }
                }
            }

            //! 录制的状态读数：
            //!  * 录制中 → "● 已录 N 个音"（红点 + 计数，一眼看出还在记）；
            //!  * 停下后 → 上一次的结果（"录了 4 个音，写入 4 个，跳过 0 个"）；空串 = 这次会话还没录过。
            //! 放在工具条上而不是弹对话框：录制是"看一眼就知道成不成"的事，弹窗只会多一次点击。
            Text {
                id: takeSummaryLabel

                anchors.verticalCenter: parent.verticalCenter
                visible: root.hasScore && text.length > 0
                text: root.recording
                      ? "● " + qsTrc("notationscene", "Recording") + " " + root.model.recordedNoteCount
                      : root.takeSummary
                color: root.recording ? root.recordColor : root.dimTextColor
                font: ui.theme.bodyFont
                elide: Text.ElideRight
                width: Math.min(implicitWidth, 220)
            }

            //! 混音器开关。面板本身在页面上（MidiEditorPage.qml），这里只是它的入口 ——
            //! 与记谱页的 View → Mixer 是同一个混音器（同一份轨道、同一份音量/静音/独奏）。
            //!
            //! ⚠️ 这两个开关刻意用 `FlatButton` 而不是旁边那种"Rectangle + MouseArea"：
            //!  * **键盘/无障碍可达**（工具条本来就有导航区，自绘矩形进不去）；
            //!  * **机器可验** —— 只有真实的 Button 才会出现在无障碍树里，于是
            //!    `tools/ui-probe.ps1 -Action click -Name Solo -ControlType Button` 点得中它。
            //!    本环境里**画布上的合成鼠标是无效的**（见 `维护手册.md` §7.6），
            //!    工具条上的按钮却是可以的 —— 这正是"只播当前谱表"这条链路能被自动验的原因。
            FlatButton {
                id: mixerButton

                height: 22

                text: qsTrc("notationscene", "Mixer")
                transparent: !root.mixerOpen
                accentButton: root.mixerOpen

                toolTipTitle: qsTrc("notationscene", "Mixer")
                toolTipDescription: qsTrc("notationscene", "Show or hide the mixer - the very tracks the score plays through")

                accessible.name: text + "  " + (root.mixerOpen ? qsTrc("global", "On") : qsTrc("global", "Off"))

                onClicked: root.mixerToggleRequested()
            }

            //! 只播放当前谱表：打开后**只有选中谱表的轨道**发声（写的就是混音器上那个 solo，
            //! 所以混音器里那一路会同时亮起来）。单谱表时没有可挑的，隐藏。
            FlatButton {
                id: soloButton

                visible: root.staffCount > 1
                height: 22

                text: qsTrc("notationscene", "Solo")
                transparent: !root.soloActive
                accentButton: root.soloActive

                toolTipTitle: qsTrc("notationscene", "Solo")
                toolTipDescription: qsTrc("notationscene", "Play only the staff being edited (turns the mixer's solo on for it)")

                accessible.name: text + "  " + (root.soloActive ? qsTrc("global", "On") : qsTrc("global", "Off"))

                onClicked: {
                    if (root.model === null) {
                        return
                    }

                    if (root.soloActive) {
                        root.model.clearSoloStaff()
                    } else {
                        root.model.setSoloStaff(root.currentStaff)
                    }
                }
            }

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

            //! 🆕 选中集合上的三个动作。用**真 `FlatButton`**而不是旁边的自绘矩形：
            //! 只有真 Button 才进得了无障碍树，`tools/ui-probe.ps1 -Action click` 才点得中它 ——
            //! 本环境里**画布上的合成鼠标是无效的**（`维护手册.md` §7.6），所以"删除选中的音"
            //! 这条链路要机器可验，入口就必须在这里。
            //! ⚠️ 快捷键（Del / Ctrl+C / Ctrl+V）是**另一条**路：那几个键全局都注册过，
            //! 要在本页 `Keys.onShortcutOverride` 里认领才收得到（见那里的长注释）。
            FlatButton {
                id: deleteButton

                height: 22
                //! ⚠️ 只有文字的 `FlatButton` 默认是 `TextOnly`：`minWidth = 132`、`margins = 16`
                //! ⇒ 三个这样的按钮要 400px，会把工具条那一行撑得放不下（这一行只有 36px 高，
                //! 左边的提示文字靠"省略号"给它腾地方，见 `hintLabel` 的 anchors）。
                //! `Horizontal` 那一档是 `minWidth = 24` / `margins = 12` —— 三个短标签该有的宽度。
                buttonType: FlatButton.Horizontal
                text: qsTrc("notationscene", "Del")
                transparent: true
                enabled: root.selectedCount > 0

                toolTipTitle: qsTrc("notationscene", "Delete the selected notes")
                toolTipDescription: qsTrc("notationscene", "The last note of a chord becomes a rest of the same length, so the measure stays complete (same as Delete on the notation page)")

                accessible.name: text + "  " + qsTrc("global", "Delete")

                onClicked: root.model.deleteSelectedNotes()
            }

            FlatButton {
                id: copyButton

                height: 22
                buttonType: FlatButton.Horizontal
                text: qsTrc("notationscene", "Copy")
                transparent: true
                enabled: root.selectedCount > 0

                toolTipTitle: qsTrc("notationscene", "Copy the selected notes")
                toolTipDescription: qsTrc("notationscene", "Pitch, length, velocity and the played layer - everything that makes the note sound the way it does")

                onClicked: root.model.copySelection()
            }

            FlatButton {
                id: pasteButton

                height: 22
                buttonType: FlatButton.Horizontal
                text: qsTrc("notationscene", "Paste")
                transparent: true
                enabled: model !== null && model.hasClipboard

                toolTipTitle: qsTrc("notationscene", "Paste at the playhead")
                toolTipDescription: qsTrc("notationscene", "Pastes on the staff being edited, starting at the playback position, and selects what it pasted")

                onClicked: root.model.pasteAtTick(root.currentStaff, root.snapTick(Math.round(root.playbackTick)))
            }

            Rectangle {
                id: velocityToggle

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
                    id: velocityToggleArea

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.velocityLaneVisible = !root.velocityLaneVisible
                }

                ToolTip {
                    text: qsTrc("notationscene", "Show or hide the lane under the roll")
                    visible: velocityToggleArea.containsMouse
                    delay: 600
                }
            }

            //! 🆕 力度车道的**通道开关**：关 = 每个音自己的力度（`Pid::USER_VELOCITY`，覆盖表情记号），
            //! 开 = **演奏力度**（`NoteEvent::velocityMultiplier`，百分比，乘在本来该有的力度上）。
            //! 两个通道是两套语义（`维护手册.md` §4.8 有专条），所以界面上也要能一眼看出在编辑哪个。
            //!
            //! ⚠️ 用**真 `FlatButton`**（而不是旁边 `Velocity` 那种自绘矩形）：自绘开关**不在无障碍树里**，
            //! 而本环境**画布上的合成鼠标无效** ⇒ "切到演奏力度通道"这条链路要机器可验，入口就必须是
            //! 真控件（`tools/ui-probe.ps1 -Action click -Name "Played %" -ControlType Button`，见 §7.6）。
            FlatButton {
                id: playedChannelButton

                visible: root.velocityLaneVisible
                height: 22
                buttonType: FlatButton.Horizontal

                text: qsTrc("notationscene", "Played %")
                transparent: !root.velocityPlayedChannel
                accentButton: root.velocityPlayedChannel

                toolTipTitle: qsTrc("notationscene", "Played velocity lane")
                toolTipDescription: qsTrc("notationscene", "Velocity lane channel: off = each note's own velocity (overrides the dynamics), "
                                          + "on = played velocity in percent (multiplies what the note would get). "
                                          + "100% (the middle line) means untouched; right-click resets it")

                accessible.name: text + "  " + (root.velocityPlayedChannel ? qsTrc("global", "On") : qsTrc("global", "Off"))

                onClicked: {
                    root.velocityPlayedChannel = !root.velocityPlayedChannel
                    //! 换通道 = 换一套值：留着上一条通道的笔迹会画出"看不见来源"的柱子。
                    root.velocityTrail = ({})
                    root.velocityPending = false
                    velocityCanvas.requestPaint()

                    console.warn("[midi-velocity] channel =",
                                 root.velocityPlayedChannel ? "played %" : "own velocity")
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

                    //! 循环区间：标尺上这一段染色 + 两条边界线。它与记谱页那两个循环标记指的是
                    //! **同一对 tick**（乐谱自己的 loop in/out），所以在哪一页设的循环都看得见。
                    //! 拖动中画的是**预览**（松手才写模型），因此拖的时候就已经知道要循环哪一段。
                    var loopIn = root.loopInTick
                    var loopOut = root.loopOutTick
                    var showingLoop = root.loopEnabled
                    if (root.loopDragAnchorTick >= 0) {
                        loopIn = Math.min(root.loopDragAnchorTick, root.loopDragPreviewTick)
                        loopOut = Math.max(root.loopDragAnchorTick, root.loopDragPreviewTick)
                        showingLoop = loopOut > loopIn
                    }

                    if (showingLoop && loopOut > loopIn) {
                        var lx0 = root.xForTick(loopIn)
                        var lx1 = root.xForTick(loopOut)

                        ctx.fillStyle = ui.theme.accentColor
                        ctx.globalAlpha = 0.22
                        ctx.fillRect(lx0, 0, lx1 - lx0, h)
                        ctx.globalAlpha = 1.0

                        ctx.fillRect(lx0, 0, Math.max(1, lx1 - lx0), 3)   //! 顶上一条，像 DAW 的循环条
                        ctx.fillRect(lx0, 0, 1, h)
                        ctx.fillRect(lx1 - 1, 0, 1, h)
                    }

                    //! 播放头：**停止时也画**。原来的实现只在 `isPlaying` 时画，于是"点标尺定位"
                    //! 这一下在画面上没有任何反馈（用户会以为没生效）。播放中画粗一点，
                    //! 一眼分得清"正在播"和"停在这里"。
                    if (root.hasScore) {
                        var phx = root.xForTick(root.playbackTick)
                        if (phx >= -1 && phx <= w + 1) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = root.isPlaying ? 2 : 1
                            ctx.beginPath()
                            ctx.moveTo(Math.round(phx) + 0.5, 0)
                            ctx.lineTo(Math.round(phx) + 0.5, h)
                            ctx.stroke()

                            ctx.fillStyle = root.cursorColor
                            ctx.beginPath()
                            ctx.moveTo(phx - 4, 0)
                            ctx.lineTo(phx + 4, 0)
                            ctx.lineTo(phx, 6)
                            ctx.closePath()
                            ctx.fill()
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

            //! 标尺上的手势：**点一下 = 从这儿播** · **拖一段 = 循环这一段** · **右击 = 取消循环**。
            //! 三条手势共用一条 24px 高的带子，所以"点"和"拖"的区分交给 `midiLoopRangeFromDrag`
            //! （没跨过一格就是点击）—— 判据与单测都在模型那一侧，这里不做第二套判断。
            MouseArea {
                id: rulerMouse

                anchors.fill: rulerCanvas
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                hoverEnabled: true

                onPressed: function(mouse) {
                    if (mouse.button !== Qt.LeftButton) {
                        return
                    }

                    root.loopDragAnchorTick = Math.round(root.clamp(root.tickForX(mouse.x), 0, root.totalTicks))
                    root.loopDragPreviewTick = root.loopDragAnchorTick
                }

                onPositionChanged: function(mouse) {
                    if (!pressed || root.loopDragAnchorTick < 0) {
                        return
                    }

                    var tick = Math.round(root.clamp(root.tickForX(mouse.x), 0, root.totalTicks))
                    if (tick !== root.loopDragPreviewTick) {
                        root.loopDragPreviewTick = tick
                        rulerCanvas.requestPaint()
                    }
                }

                onReleased: function(mouse) {
                    if (mouse.button !== Qt.LeftButton || root.loopDragAnchorTick < 0) {
                        return
                    }

                    var anchor = root.loopDragAnchorTick
                    var released = root.loopDragPreviewTick

                    root.loopDragAnchorTick = -1
                    root.loopDragPreviewTick = -1

                    //! 拖出区间 → 循环；只是点了一下 → 把播放位置挪到那儿。
                    if (!root.applyLoopDrag(anchor, released)) {
                        root.seekToTick(anchor)
                    }

                    rulerCanvas.requestPaint()
                }

                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton && root.model !== null) {
                        //! 右击 = 取消循环（与力度条上"右击 = 取消覆盖"是同一种"把它拿走"）。
                        root.model.clearLoop()
                    }
                }

                ToolTip {
                    text: qsTrc("notationscene", "Click = play from here · drag = loop this range · right-click = clear the loop")
                    visible: rulerMouse.containsMouse
                    delay: 600
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

                    //! 循环区间（画在音符**下面**：它是一块"这片区域会反复播"的底色，
                    //! 不该把音符盖住）。与标尺上那一段是同一对 tick。
                    var loopIn = root.loopInTick
                    var loopOut = root.loopOutTick
                    var showingLoop = root.loopEnabled
                    if (root.loopDragAnchorTick >= 0) {
                        loopIn = Math.min(root.loopDragAnchorTick, root.loopDragPreviewTick)
                        loopOut = Math.max(root.loopDragAnchorTick, root.loopDragPreviewTick)
                        showingLoop = loopOut > loopIn
                    }

                    if (showingLoop && loopOut > loopIn) {
                        var lx0 = root.xForTick(loopIn)
                        var lx1 = root.xForTick(loopOut)
                        ctx.fillStyle = ui.theme.accentColor
                        ctx.globalAlpha = 0.12
                        ctx.fillRect(lx0, 0, lx1 - lx0, h)
                        ctx.globalAlpha = 1.0
                    }

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

                        //! 多选拖动：整批一起走，所以**每个选中的音**都要加上这次手势的增量。
                        //! 增量只在拖动中非 0，平时这里是恒等变换（画面与数据一致）。
                        var dragging = (root.dragNoteIndex >= 0 && root.dragMoved)
                        var selected = root.isSelectedRow(visible[n].row)
                        var batch = dragging && selected

                        var noteTick = note.tick
                        var noteDuration = note.durationTicks
                        var notePitch = note.pitch
                        var notePlayTick = note.playTick
                        var notePlayDuration = note.playDurationTicks

                        if (n === root.dragNoteIndex && root.dragMoved) {
                            //! 被抓住的那一个：用它的**专属**预览值（记谱位置/时值各一条手势）。
                            if (root.dragMode === root.dragModeNotatedMove) {
                                noteTick = root.dragPreviewNotatedTick
                            } else if (root.dragMode === root.dragModeNotatedLength) {
                                noteDuration = root.dragPreviewNotatedDuration
                            }
                        } else if (batch) {
                            //! 同一批里的其它音：跟着**增量**走。
                            noteTick = note.tick + root.dragDeltaTicks
                            notePitch = root.clamp(note.pitch + root.dragDeltaPitch, 0, 127)
                        }

                        if (n === root.dragNoteIndex && root.dragPreviewPitch >= 0
                                && root.dragMode === root.dragModePitch) {
                            notePitch = root.dragPreviewPitch
                        }

                        if (noteTick > maxTick || noteTick + noteDuration < minTick) {
                            continue
                        }

                        var previewing = (n === root.dragNoteIndex && root.dragMoved)
                        var nx = root.xForTick(noteTick)
                        var ny = root.yForPitch(notePitch)
                        if (ny > h || ny + nh < 0) {
                            continue
                        }

                        var nw = Math.max(3, noteDuration * root.pixelsPerTick - 1)

                        //! NOTE: Dorico's distinction, in one block: the notated extent is drawn as an
                        //!       outline and the played extent as a solid bar on top of it. A note the
                        //!       user never touched has both at the same place, so it stays a plain
                        //!       solid block exactly as before.
                        //! 🆕 记谱层拖动（Alt）改的就是**外框**：`nw` 与 `nx` 已经是预览值了。
                        var draggingPlay = (n === root.dragNoteIndex && root.dragMoved
                                            && (root.dragMode === root.dragModePlayStart
                                                || root.dragMode === root.dragModePlayLength))
                        var showingPlay = note.hasPlayOverride || draggingPlay
                        var playStart = draggingPlay ? root.dragPreviewPlayStart : notePlayTick
                        var playDuration = draggingPlay ? root.dragPreviewPlayDuration : notePlayDuration

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

                        //! 🆕 选中态：画一圈**垫底 + 描边**（不换填充色，否则会和"谱表分色"打架，
                        //! 用户就分不出这是哪个乐器了）。垫底色先画，所以描边在深浅底上都看得见。
                        if (selected) {
                            ctx.strokeStyle = root.backgroundColor
                            ctx.lineWidth = 3
                            ctx.strokeRect(nx + 0.5, ny + 0.5, Math.max(1, nw - 1), Math.max(1, nh - 1))
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = 2
                            ctx.strokeRect(nx + 0.5, ny + 0.5, Math.max(1, nw - 1), Math.max(1, nh - 1))
                        }

                        if (n === root.hoveredNoteIndex || previewing) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = 1
                            ctx.strokeRect(nx + 0.5, ny + 0.5, Math.max(1, nw - 1), Math.max(1, nh - 1))
                        }
                    }

                    //! 🆕 框选矩形：画在音符**上面**（否则框到音上就看不见边界了），
                    //! 用半透明填充 + 亮边：既看得见框住了哪些音，也看得见框本身。
                    if (root.marqueeActive) {
                        var mx = Math.min(root.marqueeX0, root.marqueeX1)
                        var my = Math.min(root.marqueeY0, root.marqueeY1)
                        var mw = Math.abs(root.marqueeX1 - root.marqueeX0)
                        var mh = Math.abs(root.marqueeY1 - root.marqueeY0)

                        ctx.fillStyle = ui.theme.accentColor
                        ctx.globalAlpha = 0.18
                        ctx.fillRect(mx, my, mw, mh)
                        ctx.globalAlpha = 1.0

                        ctx.strokeStyle = root.cursorColor
                        ctx.lineWidth = 1
                        ctx.setLineDash([4, 3])
                        ctx.strokeRect(mx + 0.5, my + 0.5, mw, mh)
                        ctx.setLineDash([])
                    }

                    //! 录制实时预览：**还没写进乐谱**的音。
                    //! 画的是"按停止会写成什么样"（模型已经按当前量化设置算过），所以换网格时
                    //! 这一层会跟着动 —— 用户能在提交之前就看见量化会把他的演奏挪到哪。
                    //! 用红色而不是谱表调色板：这些音还没落进谱里，画成一样会让人以为已经在谱里了。
                    if (root.recording && root.currentStaff === root.recordStaff) {
                        var live = root.recordedRows
                        for (var r = 0; r < live.length; ++r) {
                            var take = live[r]
                            if (take.pitch < root.lowestPitch || take.pitch > root.highestPitch) {
                                continue
                            }

                            var tx = root.xForTick(take.tick)
                            var tw = Math.max(2, take.durationTicks * root.pixelsPerTick - 1)
                            if (tx > w || tx + tw < 0) {
                                continue
                            }

                            var ty = root.yForPitch(take.pitch)
                            if (ty > h || ty + nh < 0) {
                                continue
                            }

                            ctx.fillStyle = take.held ? root.recordHeldColor : root.recordColor
                            ctx.globalAlpha = take.held ? 0.9 : 0.7
                            ctx.fillRect(tx, ty, tw, nh)
                            ctx.globalAlpha = 1.0
                        }
                    }

                    //! 播放头：停止时也画（点标尺定位之后必须看得见点在哪），播放中画粗一点。
                    if (root.hasScore) {
                        var cx = root.xForTick(root.playbackTick)
                        if (cx >= 0 && cx <= w) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = root.isPlaying ? 2 : 1
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
                    //! 🆕 点一下画布就把键盘焦点收回来 —— 否则"先点了播放键、再想用电脑键盘弹"
                    //! 的按键会全落在那个按钮上（电脑键盘弹音与 Ctrl+Z 都要这一句）。
                    root.forceActiveFocus()

                    //! An index into visibleRows - only the selected staff is drawn and clickable.
                    var index = root.noteIndexAt(mouse.x, mouse.y)

                    //! 空白处：先只**记下**位置，松手时再决定是不是要定位（见 onReleased）——
                    //! 免得"想拖音符但没点中"的那一下把播放位置甩到别处去。
                    root.rollPressX = mouse.x
                    root.rollPressSeekTick = (index < 0 && root.hasScore)
                                             ? Math.round(root.clamp(root.tickForX(mouse.x), 0, root.totalTicks))
                                             : -1

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
                    root.dragMoved = false
                    root.dragDeltaPitch = 0
                    root.dragDeltaTicks = 0
                    if (index >= 0) {
                        var grabbed = root.visibleRows[index].note
                        var grabbedRow = root.visibleRows[index].row

                        //! 🆕 **选中**（按下时先定下来，之后整条手势都作用于这个集合）：
                        //!  * Ctrl + 点 = 把这个音加进/移出选中（记谱页与文件管理器同一套约定）；
                        //!  * 点一个**没被选中**的音 = 改成只选它；
                        //!  * 点一个**已被选中**的音 = 选中**不动** —— 因为它可能是多选里的一员，
                        //!    接下来很可能是"整块一起拖"。若最终只是点了一下（没拖动），
                        //!    松手时会收成"只选这一个"（见 onReleased）。
                        if (mouse.modifiers & Qt.ControlModifier) {
                            root.model.toggleSelectedRow(grabbedRow)
                        } else if (!root.isSelectedRow(grabbedRow)) {
                            root.model.setSelectedRows([grabbedRow], false)
                        }

                        root.dragStartPitch = grabbed.pitch
                        root.dragPreviewPitch = root.dragStartPitch
                        root.dragStartY = mouse.y
                        root.dragStartX = mouse.x
                        root.dragStartPlayStart = grabbed.playTick
                        root.dragStartPlayDuration = grabbed.playDurationTicks
                        root.dragPreviewPlayStart = grabbed.playTick
                        root.dragPreviewPlayDuration = grabbed.playDurationTicks
                        root.dragStartNotatedTick = grabbed.tick
                        root.dragStartNotatedDuration = grabbed.durationTicks
                        root.dragPreviewNotatedTick = grabbed.tick
                        root.dragPreviewNotatedDuration = grabbed.durationTicks

                        //! 试听：**按下就出声** —— 记谱页点音符是同一个动作
                        //! （`NotationViewInputController::handleLeftClick()` 里那句
                        //! `playbackController()->playElements({ hitElement })`），这里走模型的
                        //! `playNote()`，而它调的就是同一个播放接口，所以"编辑时播放音符"这个
                        //! 设置两页一起生效。行号映射收在 `auditionNoteAt()` 里（只写一次）。
                        //! 记在 `dragPlayedPitch` 上：拖动中只在音高**真的变了**时才再响一声，
                        //! 免得鼠标每动一像素都发一次音（记谱页拖动也是这个判据）。
                        root.dragPlayedPitch = root.dragStartPitch
                        root.auditionNoteAt(index)

                        //! NOTE: which part of the block was grabbed decides what the drag edits.
                        //!       The edge test uses the PLAYED bar, not the notated block: that bar is
                        //!       what the user sees on top and aims at, and the two only coincide when
                        //!       the note has no override at all.
                        //! ⚠️ **Alt 把两条手势整个换到记谱层**（外框那条）：Alt + 右边缘 = 改记谱时值、
                        //! Alt + 其它 = 左右移动记谱位置。故意用修饰键而不是"水平拖 = 移动"：
                        //! 记谱层会改谱面结构（连音线/休止符/小节），误触代价比演奏层大得多。
                        var grabbedX = root.xForTick(grabbed.playTick)
                        var grabbedW = root.playedWidth(grabbed)
                        var edge = Math.max(4, Math.min(8, grabbedW * 0.25))
                        if (mouse.modifiers & Qt.AltModifier) {
                            if (mouse.x >= grabbedX + grabbedW - edge) {
                                root.dragMode = root.dragModeNotatedLength
                            } else {
                                root.dragMode = root.dragModeNotatedMove
                            }
                        } else if (mouse.x >= grabbedX + grabbedW - edge) {
                            root.dragMode = root.dragModePlayLength
                        } else if (mouse.modifiers & Qt.ShiftModifier) {
                            root.dragMode = root.dragModePlayStart
                        } else {
                            root.dragMode = root.dragModePitch
                        }

                        gridCanvas.requestPaint()
                    } else {
                        //! 🆕 空白处按下 = **框选**（拖动时）或**定位**（只是点一下，见 onReleased）。
                        //! 两者共用同一条起手式，判据是"移动超过 3px 没有" —— 与标尺上
                        //! "点 = 定位 / 拖 = 循环"完全同一种做法，用户不用记两套。
                        root.marqueeActive = false
                        root.marqueeX0 = mouse.x
                        root.marqueeY0 = mouse.y
                        root.marqueeX1 = mouse.x
                        root.marqueeY1 = mouse.y
                        root.marqueeAdditive = (mouse.modifiers & Qt.ControlModifier) !== 0

                        //! Ctrl + 点空白 = **取消全部选中**（与"Ctrl 点音符 = 加减一个"配对）。
                        if ((mouse.modifiers & Qt.ControlModifier) !== 0) {
                            root.model.clearSelection()
                        }
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
                        //! 🆕 空白处拖动 = **框选**。判据 3px（与"点空白 = 定位"共用起手式）——
                        //! 比这更小的位移仍然算"点了一下"，不会甩出一个几乎看不见的框。
                        if (!root.marqueeActive
                                && (Math.abs(mouse.x - root.marqueeX0) > 3
                                    || Math.abs(mouse.y - root.marqueeY0) > 3)) {
                            root.marqueeActive = true
                        }

                        if (root.marqueeActive) {
                            root.marqueeX1 = mouse.x
                            root.marqueeY1 = mouse.y
                            gridCanvas.requestPaint()
                        }
                        return
                    }

                    root.dragMoved = true

                    if (root.dragMode === root.dragModePitch) {
                        var deltaRows = Math.round((mouse.y - root.dragStartY) / root.rowHeight)
                        var pitch = root.clamp(root.dragStartPitch - deltaRows, 0, 127)
                        root.dragDeltaPitch = pitch - root.dragStartPitch
                        if (pitch !== root.dragPreviewPitch) {
                            root.dragPreviewPitch = pitch
                            //! 试听**拖到的那个音高**（不是谱面上原有的那个）：与记谱页拖动音符
                            //! 一致 —— 拖到哪个音就响哪个音。卷帘窗拖动中不写谱，所以模型会造一个
                            //! 临时音符来发声（`playNoteAtPitch`）。判据仍是"音高真的变了才响"，
                            //! 免得鼠标每动一像素都发一次音；真正的写入仍在松手那一次。
                            if (pitch !== root.dragPlayedPitch) {
                                root.dragPlayedPitch = pitch
                                root.auditionNoteAt(root.dragNoteIndex, pitch)
                            }
                            gridCanvas.requestPaint()
                        }
                        return
                    }

                    //! NOTE: the played layer snaps to the grid, so a drag lands on musical
                    //!       positions instead of on pixel noise.
                    var snap = root.snapTicks
                    var deltaTicks = Math.round((mouse.x - root.dragStartX) / root.pixelsPerTick / snap) * snap

                    //! 🆕 记谱层两条：**左右移动**与**改时值**。它们改的是外框，所以预览值是
                    //! 绝对 tick / 绝对时值（而不是演奏层那种"起点 + 时长"）。
                    if (root.dragMode === root.dragModeNotatedMove) {
                        var movedTick = Math.max(0, root.dragStartNotatedTick + deltaTicks)
                        root.dragDeltaTicks = movedTick - root.dragStartNotatedTick
                        if (movedTick !== root.dragPreviewNotatedTick) {
                            root.dragPreviewNotatedTick = movedTick
                            gridCanvas.requestPaint()
                        }
                        return
                    }

                    if (root.dragMode === root.dragModeNotatedLength) {
                        //! 下限 = 一格：比一格格子还短的记谱时值没有意义，而且吸附本来也落不上去。
                        var notatedDuration = Math.max(snap, root.dragStartNotatedDuration + deltaTicks)
                        if (notatedDuration !== root.dragPreviewNotatedDuration) {
                            root.dragPreviewNotatedDuration = notatedDuration
                            gridCanvas.requestPaint()
                        }
                        return
                    }

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
                        var multi = root.selectedCount > 1 && root.isSelectedRow(entry.row)

                        //! The model takes indexes into `notes`, not into the filtered view.
                        if (root.dragMode === root.dragModePitch) {
                            if (root.dragPreviewPitch >= 0 && root.dragPreviewPitch !== root.dragStartPitch) {
                                if (multi && root.dragDeltaPitch !== 0) {
                                    //! 🆕 多选：整批一起挪同样的半音数，**一个命令**。
                                    //! 逐个 setNotePitch 会让 N 个音 = N 次全谱通知（§4.8 那条）。

                                    //! 行号在这里**当场收集**：模型补完缓存后会重发行号，
                                    //! 但收集要用的是"这一帧的行号"，所以先收齐再一次性递进去。
                                    var pitchRows = []
                                    var pitchValues = []
                                    var list = root.visibleRows
                                    for (var i = 0; i < list.length; ++i) {
                                        if (root.isSelectedRow(list[i].row)) {
                                            pitchRows.push(list[i].row)
                                            pitchValues.push(root.clamp(list[i].note.pitch + root.dragDeltaPitch, 0, 127))
                                        }
                                    }
                                    root.model.setNotePitches(pitchRows, pitchValues)
                                } else {
                                    //! NOTE: the single submission of the whole drag.
                                    root.model.setNotePitch(entry.row, root.dragPreviewPitch)
                                }
                            }
                        } else if (root.dragMode === root.dragModeNotatedMove) {
                            if (root.dragDeltaTicks !== 0) {
                                //! 🆕 记谱层移动：模型对整个**选中集合**施加同一个增量，
                                //! 一次事务、一次撤销（结构编辑，见 moveMidiNotes 的边界说明）。
                                root.model.moveSelectedNotes(root.dragDeltaTicks)
                            }
                        } else if (root.dragMode === root.dragModeNotatedLength) {
                            var notatedDelta = root.dragPreviewNotatedDuration - root.dragStartNotatedDuration
                            if (notatedDelta !== 0) {
                                //! 🆕 记谱层时值：模型按增量改整个选中集合（歌剧院之外的部分由
                                //! `changeCRlen()` 自己补休止符 / 连音线）。
                                root.model.resizeSelectedNotes(notatedDelta)
                            }
                        } else if (root.dragPreviewPlayStart !== released.playTick
                                   || root.dragPreviewPlayDuration !== released.playDurationTicks) {
                            root.model.setNotePlayOverride(entry.row,
                                                           root.dragPreviewPlayStart,
                                                           root.dragPreviewPlayDuration,
                                                           released.playVelocityPercent)
                        }

                        //! 点一下**已选中**的音（没拖动）= 收成"只选它"。
                        //! 这条与按下时"选中的不动"配对：多选整块拖动与单选点击因此能共存。
                        if (!root.dragMoved && root.selectedCount > 1 && !(mouse.modifiers & Qt.ControlModifier)) {
                            root.model.setSelectedRows([entry.row], false)
                        }
                    }

                    //! 🆕 框选提交：**松手那一次**把矩形里的行号成批交给模型（拖动中只画框）。
                    if (root.marqueeActive) {
                        var rows = root.rowsInMarquee(root.marqueeX0, root.marqueeY0, root.marqueeX1, root.marqueeY1)
                        root.model.setSelectedRows(rows, root.marqueeAdditive)
                        root.marqueeActive = false
                        console.warn("[midi-select] marquee ->", rows.length, "note(s), additive=", root.marqueeAdditive)
                    }

                    root.dragNoteIndex = -1
                    root.dragPreviewPitch = -1
                    root.dragPlayedPitch = -1
                    root.dragMode = root.dragModePitch
                    root.dragMoved = false
                    root.dragDeltaPitch = 0
                    root.dragDeltaTicks = 0

                    //! 点空白 = 把播放位置挪到这儿（记谱页同款）。判据两条：没抓到音符，
                    //! 而且几乎没移动过 —— 拖动空白不是定位手势。
                    if (root.rollPressSeekTick >= 0 && Math.abs(mouse.x - root.rollPressX) <= 3) {
                        root.seekToTick(root.rollPressSeekTick)
                    }
                    root.rollPressSeekTick = -1

                    gridCanvas.requestPaint()
                }

                //! 🆕 **双击空白 = 插入一个音符**（时值 = 工具条上的网格）。
                //! 双击**音符**不插入（那是试听/编辑的手势，插入会让人误以为点坏了）。
                onDoubleClicked: function(mouse) {
                    if (!root.hasScore || root.model === null || mouse.button !== Qt.LeftButton) {
                        return
                    }

                    if (root.noteIndexAt(mouse.x, mouse.y) >= 0) {
                        return
                    }

                    var tick = Math.round(root.clamp(root.tickForX(mouse.x), 0, root.totalTicks))
                    var pitch = root.clamp(root.pitchForY(mouse.y), 0, 127)
                    tick = root.clamp(root.snapTick(tick), 0, Math.max(0, root.totalTicks - 1))

                    console.warn("[midi-notes] insert at tick", tick, "pitch", pitch,
                                 "duration", root.snapTicks, "staff", root.currentStaff)
                    root.model.insertNoteAt(root.currentStaff, tick, pitch, root.snapTicks)
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

                    //! 循环区间也铺在这条车道上（很淡，柱子仍然看得清）：改力度时最想知道的就是
                    //! "我现在改的这段会不会反复播"。与标尺上那一段是同一对 tick。
                    var loopIn = root.loopInTick
                    var loopOut = root.loopOutTick
                    var showingLoop = root.loopEnabled
                    if (root.loopDragAnchorTick >= 0) {
                        loopIn = Math.min(root.loopDragAnchorTick, root.loopDragPreviewTick)
                        loopOut = Math.max(root.loopDragAnchorTick, root.loopDragPreviewTick)
                        showingLoop = loopOut > loopIn
                    }

                    if (showingLoop && loopOut > loopIn) {
                        var lx0 = root.xForTick(loopIn)
                        var lx1 = root.xForTick(loopOut)
                        ctx.fillStyle = ui.theme.accentColor
                        ctx.globalAlpha = 0.10
                        ctx.fillRect(lx0, 0, lx1 - lx0, h)
                        ctx.globalAlpha = 1.0
                    }

                    //! 🆕 **演奏力度通道**的车道基准线（100% = 没调过）：画一条细线当地平线 ——
                    //! 这一通道是**相对量**，没有基准线的话"110%"和"90%"看起来只是两根不同的柱子，
                    //! 看不出谁比"原样"更响。
                    if (root.velocityPlayedChannel) {
                        var baselineY = Math.round(h * root.playedVelocityBaseline) + 0.5
                        ctx.strokeStyle = root.gridColor
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(0, baselineY)
                        ctx.lineTo(w, baselineY)
                        ctx.stroke()
                    }

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

                        //! 🆕 选中的音在车道上也要看得出来：柱子加一圈亮边 —— 用户刷之前得知道
                        //! 自己选中的是哪些音（选中的音在画布上是同一套描边，两处要一致）。
                        var selectedLaneNote = root.isSelectedRow(visible[i].row)

                        if (root.velocityPlayedChannel) {
                            //! 演奏力度：以中线为起点、向上（更响）或向下（更轻）长。
                            var shownPercent = (brushed || pending) ? trail[note.tick] : note.playVelocityPercent
                            var bar = root.playedVelocityBar(shownPercent, h)
                            var ownPlayed = note.hasPlayOverride

                            ctx.fillStyle = brushed ? root.cursorColor : root.staffColor(note.staffIndex)
                            ctx.globalAlpha = root.automationMode ? 0.18 : ((brushed || pending) ? 1.0 : (ownPlayed ? 0.9 : 0.55))
                            ctx.fillRect(x - 0.5, bar.y, 4, bar.height)
                            ctx.globalAlpha = 1.0

                            if (selectedLaneNote) {
                                ctx.strokeStyle = root.cursorColor
                                ctx.lineWidth = 1
                                ctx.strokeRect(x - 1.5, bar.y - 1.5, 6, bar.height + 3)
                            }
                            continue
                        }

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

                        if (selectedLaneNote) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = 1
                            ctx.strokeRect(x - 1.5, h - barH - 1.5, 6, barH + 3)
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

                                //! ⚠️ 手柄要和控制点**一眼分得开**：记谱页车道上的同一个手柄也改了
                                //! （用户 2026-10-06 报「曲点与节点样式有点相似」）。
                                //! 只把轮廓从圆改成方不够 —— 6px 下一圈描边的圆和一个方块几乎一样。
                                //! 所以：**大小**（9px vs 半径 3 的 6px）+ **实心方块 vs 圆** +
                                //! 先描一圈背景色当"垫圈"（曲线从方块下穿过时不会和它糊在一起）。
                                ctx.fillStyle = root.backgroundColor
                                ctx.fillRect(bend.x - 6, bend.y - 6, 12, 12)
                                ctx.fillStyle = root.cursorColor
                                ctx.fillRect(bend.x - 4.5, bend.y - 4.5, 9, 9)
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

                    //! 播放头在这条车道上也画一条：力度是"跟着播放听"才改得准的。
                    if (root.hasScore) {
                        var phx = root.xForTick(root.playbackTick)
                        if (phx >= 0 && phx <= w) {
                            ctx.strokeStyle = root.cursorColor
                            ctx.lineWidth = root.isPlaying ? 2 : 1
                            ctx.beginPath()
                            ctx.moveTo(Math.round(phx) + 0.5, 0)
                            ctx.lineTo(Math.round(phx) + 0.5, h)
                            ctx.stroke()
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

                    //! 🆕 两个通道各清各的：
                    //!  * **演奏力度**通道 → 回到 **100%**（= 没调过）。它不是覆盖语义，
                    //!    所以"清掉"就是让乘子回到 1.0；写 0 会把这个音彻底静音，那是另一回事。
                    //!  * 力度通道 → 回到 **0** = "没有自己的力度"，从而**重新跟随表情记号**。
                    if (root.velocityPlayedChannel) {
                        var playedList = root.visibleRows
                        var playedRows = []
                        var playedValues = []
                        for (var p = playedList.length - 1; p >= 0; --p) {
                            if (playedList[p].note.tick === tick && playedList[p].note.playVelocityPercent !== 100) {
                                playedRows.push(playedList[p].row)
                                playedValues.push(100)
                            }
                        }
                        if (playedRows.length > 0) {
                            //! 同 tick 的音（和弦）**一起清**：与刷力度时"和弦整体改"是同一条语义。
                            root.model.setNotePlayVelocities(playedRows, playedValues)
                        }
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
                        //! 🆕 按**当前通道**提交：两个通道是两套语义（覆盖 vs 乘子），
                        //! 写错通道不会报错，只会让"刷了没反应"或"改错了东西"。
                        if (root.velocityPlayedChannel) {
                            root.model.setNotePlayVelocities(rows, values)
                        } else {
                            root.model.setNoteVelocities(rows, values)
                        }
                    } else {
                        root.clearVelocityTrail()
                    }

                    velocityCanvas.requestPaint()
                }
            }
        }
    }
}
