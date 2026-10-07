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

#pragma once

#include <QObject>
#include <QHash>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlintegration.h>

#include <atomic>
#include <functional>
#include <mutex>

#include "async/asyncable.h"

#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "midi/imidiinport.h"

#include "notation/inotation.h"

#include "playback/iplaybackcontroller.h"

#include "midieditornotes.h"
#include "midirecorder.h"

namespace mu::engraving {
class Score;
class Note;
}

namespace mu::notation {
//! NOTE: The "MIDI" page (musescore://midi) shows the very same score as the notation page,
//!       but as a piano roll. This model is the only thing between that view and the engraving
//!       model: it flattens the score into a list of note rectangles and writes edits back
//!       through the regular property/undo machinery.
class MidiEditorModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT
    QML_ELEMENT;

    Q_PROPERTY(bool hasScore READ hasScore NOTIFY scoreChanged)
    Q_PROPERTY(QVariantList notes READ notes NOTIFY scoreChanged)
    Q_PROPERTY(QVariantList measures READ measures NOTIFY scoreChanged)
    Q_PROPERTY(int lowestPitch READ lowestPitch NOTIFY scoreChanged)
    Q_PROPERTY(int highestPitch READ highestPitch NOTIFY scoreChanged)
    Q_PROPERTY(int totalTicks READ totalTicks NOTIFY scoreChanged)
    Q_PROPERTY(int staffCount READ staffCount NOTIFY scoreChanged)
    Q_PROPERTY(QStringList staffNames READ staffNames NOTIFY scoreChanged)
    Q_PROPERTY(QString scoreName READ scoreName NOTIFY scoreChanged)
    Q_PROPERTY(double playbackTick READ playbackTick NOTIFY playbackTickChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playbackTickChanged)

    //! The loop region of the score, as the roll's ruler sets it. These are the very loop in/out points
    //! the notation page's markers and the playback toolbar use - one loop, two views.
    Q_PROPERTY(int loopInTick READ loopInTick NOTIFY loopChanged)
    Q_PROPERTY(int loopOutTick READ loopOutTick NOTIFY loopChanged)
    Q_PROPERTY(bool loopEnabled READ loopEnabled NOTIFY loopChanged)

    //! True while the roll is soloing "the staff being edited" (see setSoloStaff()).
    Q_PROPERTY(bool soloActive READ soloActive NOTIFY soloChanged)

    //! ── 实时录制 ────────────────────────────────────────────────────────────────────
    //!
    //! 数据流：**播放**（录制时自动起播）→ **演奏**（MIDI 键盘）→ **实时记录**（本模型按端口时间戳
    //! 记下未量化的 tick）→ **量化**（停止时按网格与强度夹到格子上）→ 一次事务写进乐谱。
    //! 这一页**不另造发声机制**：演奏时能听见自己弹，是上游记谱页的 MIDI 输入试听在管
    //! （`NotationMidiInput`，非音符输入模式下每个 MIDI 音都会试听）—— 这里只管"记下来"。

    Q_PROPERTY(bool isRecording READ isRecording NOTIFY recordChanged)

    //! 本次已经记下的音数（含还按着的）。工具条上的读数就是它。
    Q_PROPERTY(int recordedNoteCount READ recordedNoteCount NOTIFY recordChanged)
    Q_PROPERTY(int recordedHeldCount READ recordedHeldCount NOTIFY recordChanged)

    //! 实时预览：**按当前量化设置算出来**的一串 { tick, durationTicks, pitch, velocity, held }。
    //! 画在卷帘窗里就是"停止之后会写成什么样"，所以中途改量化设置，预览会跟着变。
    Q_PROPERTY(QVariantList recordedNotes READ recordedNotes NOTIFY recordedNotesChanged)

    //! 量化设置（下拉框的选项与当前档位）。改它只影响**提交**，不会动已经记下的演奏。
    Q_PROPERTY(QVariantList quantizeGrids READ quantizeGrids NOTIFY recordSettingsChanged)
    Q_PROPERTY(int quantizeGridIndex READ quantizeGridIndex WRITE setQuantizeGridIndex NOTIFY recordSettingsChanged)
    Q_PROPERTY(int quantizeStrength READ quantizeStrength WRITE setQuantizeStrength NOTIFY recordSettingsChanged)

    //! 上一次录制写进了什么（"录了 12 个音，写入 12 个，跳过 0 个"）。空 = 这一会话还没录过。
    Q_PROPERTY(QString lastTakeSummary READ lastTakeSummary NOTIFY recordChanged)

    //! 能不能开始录制：有工程、有 MIDI 输入设备（没设备时录制按钮画成禁用并说明原因）。
    Q_PROPERTY(bool canRecord READ canRecord NOTIFY recordChanged)
    Q_PROPERTY(QString midiInputDeviceName READ midiInputDeviceName NOTIFY recordChanged)

    //! MIDI 页自己报"能不能撤销"：记谱页的 UndoRedoToolBar（Ctrl+Z 真正绑定的地方）不挂在这一页，
    //! 而 `UNDO_COMMAND` 自己的 `InputSchema()` 是空的 —— 所以这一页必须自己给入口。
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoRedoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoRedoChanged)

    //! 视图状态（滚动 / 横向缩放 / 选中的谱表 / 力度车道与 Curve 两个开关）。
    //!
    //! ⚠️ 为什么记在**模型**上：这一页的 QML（central）**只在页面可见时存在** —— `DockPage.qml`
    //! 里 central 是个 `Loader`，`sourceComponent: visible ? central : null`，所以一离开 MIDI 页
    //! 整个 `MidiEditorView` 就被销毁、再回来是全新的一份 ⇒ 状态全回初始态。
    //! 模型不是：`MidiEditorModel {}` 声明在 `MidiEditorPage.qml`（页面对象本身）里，跨页面切换存活。
    //! 记谱页之所以"记得住"，也是同一个道理 —— 它的缩放/滚动存在 C++ 的 `NotationViewState` 里。
    //!
    //! 按乐谱分开记：和记谱页一样，"换一份谱子就是另一套视图"。
    Q_PROPERTY(QVariantMap viewState READ viewState NOTIFY viewStateChanged)
    Q_PROPERTY(QString viewStateKey READ viewStateKey NOTIFY viewStateChanged)

    muse::ContextInject<context::IGlobalContext> context = { this };

    //! The playback position and the solo state are the playback module's, not ours: this page only
    //! asks for them (see seekTick() / setSoloStaff()).
    muse::ContextInject<playback::IPlaybackController> playbackController = { this };

    //! 录制要听的 MIDI 输入端口。⚠️ **全球注入**（与 `MidiInputOutputController` 拿的是同一个）：
    //! 端口是进程级的，不是某一页的。
    muse::GlobalInject<muse::midi::IMidiInPort> midiInPort;

public:
    explicit MidiEditorModel(QObject* parent = nullptr);
    ~MidiEditorModel() override;

    bool hasScore() const { return m_hasScore; }
    QVariantList notes() const { return m_notes; }
    QVariantList measures() const { return m_measures; }
    int lowestPitch() const { return m_lowestPitch; }
    int highestPitch() const { return m_highestPitch; }
    int totalTicks() const { return m_totalTicks; }
    int staffCount() const { return int(m_staffNames.size()); }
    QStringList staffNames() const { return m_staffNames; }
    QString scoreName() const { return m_scoreName; }
    double playbackTick() const { return m_playbackTick; }
    bool isPlaying() const { return m_isPlaying; }

    int loopInTick() const;
    int loopOutTick() const;
    bool loopEnabled() const;
    bool soloActive() const;

    bool canUndo() const;
    bool canRedo() const;

    QVariantMap viewState() const { return m_viewState; }
    QString viewStateKey() const { return m_viewStateKey; }

    //! 视图离开页面时把自己的状态交回来（QML 侧在 `Component.onDestruction` 里调）。
    //! 之后重建的视图会用 `viewState` 把自己摆回原样。
    Q_INVOKABLE void setViewState(const QVariantMap& state);

    Q_INVOKABLE void init();

    //! Undo / redo：走**记谱页同一套** undo stack（两页编辑同一份乐谱，也就该是同一条历史）。
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    //! Playback -------------------------------------------------------------------------------------
    //!
    //! Moves the playback position to an exact score tick - what a click in the roll's ruler does.
    //!
    //! Neither seek upstream already had fits a piano roll: `seekElement()` aims at a notation element
    //! (the roll only has ticks) and `seekBeat()` lands on a beat, which at a slow tempo can be most of
    //! a second away from where the user clicked. Hence the third entry point, `IPlaybackController::
    //! seekTick()`.
    Q_INVOKABLE void seekTick(int tick);

    //! 试听一个音符：**按下音符块就出声**。
    //!
    //! 走的是**记谱页点音符时那条一模一样的路** —— `IPlaybackController::playElements()`：
    //! 记谱页在 `NotationViewInputController::handleLeftClick()` 里对点中的元素就是这一句，
    //! 所以「编辑时播放音符」（`score/note/playOnClick`，默认开）、试听时长
    //! （`notePlayDurationMilliseconds`）这些设置对两页同时生效，不需要在这一页再抄一份开关。
    //!
    //! ⚠️ 与记谱页唯一的差别是**传谁**：记谱页点中的是**和弦**（`isChord()` → 整串音一起响），
    //! 而卷帘窗里一个方块就是**一个音**，所以这里传 `Note` —— 点哪个响哪个，这也正是卷帘窗
    //! 里试听该有的语义（想听整个和弦，记谱页那边点一下即可）。
    //!
    //! `row` 是 `notes()` 的下标（视图侧要先用 `visibleRows[i].row` 把**可见列表**的下标映射回来）。
    //! 越界、音符已失效、或这一页没有工程时都是**无操作**：试听不该因为一次失手而改变任何状态。
    Q_INVOKABLE void playNote(int row);

    //! 播放 / 暂停（**空格键**走的最终调用**不是**它 —— 见下）。
    //!
    //! 空格的正解在**快捷键上下文**那一层：`play` 动作原来带 `CTX_NOTATION_FOCUSED`（= 记谱页有焦点），
    //! 而 MIDI 页解析出的 UI 上下文是 `UiCtxUnknown`（`UiContextResolver::resolveCurrentUiContext()`
    //! 只认 notation/publish/devtools 三个 URI）⇒ 那条全局 `Space` 快捷键在**这一页被过滤掉**，
    //! 空格于是落到同样绑着 `Space` 的全局 `nav-trigger-control`（"激活当前聚焦控件"）上 ✗。
    //! 实测证据（2026-10-07，`ActionsDispatcher::doDispatch` 日志）：
    //! 记谱页按空格 → `try call action: play` → `command://playback/play-toggle` ✓；
    //! MIDI 页按空格 → `try call action: nav-trigger-control` ✗。
    //!
    //! 所以走带类动作改用 `CTX_PROJECT_PAGE_OPENED`（记谱页**或** MIDI 页都算"工程页打开着"），
    //! 全局那条 `Space` 于是自动在这一页生效 —— **不必**新增注册者（新增会让 Qt 判 ambiguous）。
    //!
    //! ⚠️ 试过在这页的 `Keys.onPressed` 里接空格：**无效**，因为 Qt 在快捷键匹配阶段就消费了按键，
    //! QML 的按键处理器根本收不到（日志里没有探针、只有 `nav-trigger-control`）。
    //!
    //! 这个方法保留下来是给**页面内**的入口用（例如以后加一个"播放"按钮），
    //! 调的是记谱页最终走的**同一个** `IPlaybackController::togglePlay()`（`PLAY_TOGGLE_COMMAND` 的实现）。
    Q_INVOKABLE void togglePlay();

    //! 试听**某个音高**上的那个音符 —— 拖动改音高时用。
    //!
    //! 与 `playNote()` 的区别只有一个：音高。拖动中卷帘窗**不写谱**（松手才提交一次），所以原音符
    //! 还是拖动前的音高，直接播它就永远响同一个音（用户 2026-10-07 报的正是这个）。这里让模型造一个
    //! 临时音符（`midiNoteToAudition()`，track/staff/voice/位置都从原音符抄），音高用**拖到的**那个 ——
    //! 于是听感与记谱页拖动一致：**拖到哪个音就响哪个音**。
    //!
    //! 仍然是同一个播放接口（`playElements()`），所以"编辑时播放音符"这个设置照旧管着它。
    Q_INVOKABLE void playNoteAtPitch(int row, int pitch);

    //! Turns a drag in the ruler (two raw ticks, in whichever direction) into a loop range, using the
    //! maths of `midiLoopRangeFromDrag()`: snapping, clamping, order, and "was that a click?".
    //! Returns { inTick, outTick, valid }.
    Q_INVOKABLE QVariantMap loopRangeFromDrag(int draggedTick, int releasedTick, int snapTicks) const;

    //! Sets the loop region and turns looping on, so that dragging a range in the ruler can be heard
    //! right away. Writes the score's own loop in/out points (the notation page's markers follow).
    Q_INVOKABLE void setLoopRange(int inTick, int outTick);

    //! Removes the loop region again (right-click in the ruler). Clears the points, not just the
    //! enable flag, so that both pages stop showing it.
    Q_INVOKABLE void clearLoop();

    //! "Only play the staff I am editing."
    //!
    //! This is the mixer's own solo state, not a second one: the track(s) of that staff get `solo =
    //! true`, the playback controller then mutes everything else, and the mixer shows the same button
    //! lit. The staff it soloed is remembered so that switching the toggle off, or moving to another
    //! staff, releases exactly the track it took - a solo the user set in the mixer is left alone.
    Q_INVOKABLE void setSoloStaff(int staffIndex);

    //! Releases the solo this page set (no-op when it set none).
    Q_INVOKABLE void clearSoloStaff();

    //! ── 实时录制 ────────────────────────────────────────────────────────────────────

    bool isRecording() const { return m_isRecording.load(); }
    int recordedNoteCount() const { return m_recorder.noteCount(); }
    int recordedHeldCount() const { return m_recorder.heldCount(); }
    QVariantList recordedNotes() const { return m_recordedNotes; }

    QVariantList quantizeGrids() const;
    int quantizeGridIndex() const { return m_quantizeGridIndex; }
    void setQuantizeGridIndex(int index);
    int quantizeStrength() const { return m_quantizeStrength; }
    void setQuantizeStrength(int percent);

    QString lastTakeSummary() const { return m_lastTakeSummary; }

    bool canRecord() const;
    QString midiInputDeviceName() const;

    //! **开始 / 停止**（同一个入口 —— 走带按钮行上那个录制键、以及页面内的调用都是它）。
    //!
    //! 开始：从**当前播放头**起播，同时开始采集；录到**当前选中的谱表**上（`setRecordStaff()`，
    //! 视图负责把它的选中同步进来 —— 模型看不到视图的选中）。
    //! 停止：把还在按着的音封口 → 量化 → **一次事务**写进乐谱 → 停止播放。
    //!
    //! ⚠️ 播放自己停下来（放到曲子末尾、或用户按了空格）也算停止：见 `init()` 里对播放状态的订阅。
    //! ⚠️ 没有 MIDI 输入设备时**拒绝开始**（只写一条日志）：录制键在那种情况下本来就画成不可用，
    //! 这里是第二道闸（快捷键/脚本也可能打进来）。
    Q_INVOKABLE void toggleRecording();

    //! 录到哪个谱表（视图在切换选中谱表时同步进来）。越界时会夹到合法范围。
    Q_INVOKABLE void setRecordStaff(int staffIndex);

    //! 停止并**放弃**这一次采集（一个音都不写）。
    Q_INVOKABLE void cancelRecording();

    //! 把队列里剩下的 MIDI 事件收尾、停止采集，并按 `commit` 决定写不写谱。
    void finishRecording(bool commit);

    //! Where to scroll so that the playhead stays visible while playing - see `midiFollowScrollX()`.
    //! Exposed so the view uses the tested maths rather than a copy of it.
    Q_INVOKABLE double followScrollX(double scrollX, double playheadX, double viewportWidth, double maxScrollX) const;

    //! NOTE: `row` is an index into notes(), and is only stable until the score changes.
    //!       The view is expected to submit one edit when the mouse is released (not on every
    //!       mouse move), which is what the audio lane drag does as well.
    Q_INVOKABLE void setNotePitch(int row, int pitch);
    Q_INVOKABLE void setNoteVelocity(int row, int velocity);

    //! The same edit for many notes at once, which is what a brush stroke needs. Setting them one by
    //! one made the model rebuild its whole note list - twice - per note, so sweeping over a phrase
    //! cost dozens of rebuilds. Here the rebuild happens once, at the end.
    Q_INVOKABLE void setNoteVelocities(const QVariantList& rows, const QVariantList& velocities);

    //! The Dynamics AUTOMATION curve of one staff - where the loudness should be over time. This is
    //! what a crescendo, a diminuendo or an fp inside a single note is, and it is the same curve the
    //! notation page draws next to the mixer, so the two stay one thing rather than two.
    //!
    //! Returns a list of { tick, value, authored, hasEase, controlT, controlValue } with the values in
    //! 0..1. `authored` is false for a point the score derived from a Dynamic mark or a hairpin - those
    //! cannot be removed from here, exactly as the notation page's lane refuses them.
    //! `hasEase` / `controlT` / `controlValue` are the arrival segment's bend - the quadratic Bezier
    //! control the lane draws as a draggable handle (`muse::mpe::AutomationPoint::Ease`).
    Q_INVOKABLE QVariantList automationPoints(int staffIndex) const;

    //! Writes one point of that curve. Goes through the score's own undoable automation command, so
    //! it undoes and saves exactly like the same edit made on the notation page.
    Q_INVOKABLE void setAutomationPoint(int staffIndex, int tick, double value);

    //! Writes a whole set of points at once, from a list of { tick, value }. One command for the batch,
    //! for the same reason the velocity brush needs one: every command notifies the whole score.
    Q_INVOKABLE void setAutomationPoints(int staffIndex, const QVariantList& points);

    //! Removes the point at that tick, if there is one and it is the user's own.
    Q_INVOKABLE void removeAutomationPoint(int staffIndex, int tick);

    //! 拖手柄：把该点"到达段"的弯折控制改成 (t, value) —— 这就是二次贝塞尔曲线的手柄。
    Q_INVOKABLE void setAutomationPointEase(int staffIndex, int tick, double t, double value);

    //! 拖控制点：把它移到另一个 tick（可同时改值）。
    Q_INVOKABLE void moveAutomationPoint(int staffIndex, int fromTick, int toTick, double value);

    //! The "played" layer: where the note actually sounds (tick) and for how long, plus the velocity
    //! multiplier in percent. Absolute ticks, so the view does not need to know about thousandths.
    Q_INVOKABLE void setNotePlayOverride(int row, int startTick, int durationTicks, int velocityPercent);

signals:
    void scoreChanged();
    void playbackTickChanged();
    void undoRedoChanged();
    void loopChanged();
    void soloChanged();
    void viewStateChanged();
    void recordChanged();
    void recordedNotesChanged();
    void recordSettingsChanged();

private:
    void reload();
    void updatePlaybackState();

    //! The real work of setNoteVelocities(), run one event-loop turn later (see the .cpp).
    void applyVelocityBatch(const QVariantList& rows, const QVariantList& velocities);

    //! Runs `mutate` with rebuilds suppressed, then rebuilds once. Writing through the engraving
    //! model notifies the score, and the notification handler rebuilds the note list - so a single
    //! edit otherwise rebuilds twice, and a batch of N rebuilds 2N times.
    void mutateOnce(const std::function<void()>& mutate);

    void connectToCurrentScore();
    void disconnectFromCurrentScore();

    engraving::Score* currentScore() const;
    engraving::Note* noteAt(int row) const;

    //! The playback facade of the current score (the same object the notation page's playback uses).
    INotationPlaybackPtr currentPlayback() const;

    //! Solos / unsolos every instrument track of one staff. Writes through the playback controller so
    //! the mixer, the audio engine and this page all see the same state. No-op for an unknown staff.
    void setStaffSolo(int staffIndex, bool solo);

    //! Whether that staff's tracks are soloed right now (read back, so a change made in the mixer is
    //! seen here too).
    bool staffIsSoloed(int staffIndex) const;

    //! ⚠️ 验证用钩子（**有意保留**，见 `维护手册.md` §7.6）：标尺上的定位/循环都是**画布上的鼠标手势**，
    //! 而本环境里合成鼠标到不了 Qt Quick 画布 —— 于是"循环带画出来没有、seek 到底有没有把播放位置
    //! 挪走"这两件事没法用脚本验。设了 `MUSE_MIDIEDITOR_DEMO_PLAYBACK=1` 时，这一页在打开工程后
    //! **自己**进入一个已知状态：播放头落在第 2 小节、循环区间 = 第 2..4 小节。
    //! 只在设了环境变量时走这条路，正常使用一行都不会执行。
    void applyDemoPlaybackIfPending();

    //! Set by the demo hook above; cleared once it has been applied.
    bool m_demoPlaybackPending = false;

    //! 录制版的验证钩子（`MUSE_MIDIEDITOR_DEMO_RECORD`）：没设 = 假，一行都不会执行。
    bool m_demoRecordPending = false;

    //! 验证钩子：喂一小段固定时刻的"演奏"进采集队列，再按当前设置提交。
    void applyDemoRecordingIfPending();

    //! 上面那个钩子的正体（延时到点之后才跑）。
    void runDemoRecording();

    //! ── 实时录制 ────────────────────────────────────────────────────────────────────

    //! 一条待处理的 MIDI 事件。**只有这三个字段 + 到达时刻**：音高/力度是整数，时间戳是墙钟毫秒。
    //!
    //! ⚠️ 为什么不直接用端口给的 `tick`（Windows 上是 `timeGetTime()` 的毫秒）：它与播放位置
    //! （`QDateTime` 墙钟）**不同源**，混用要先测两个时钟的偏移。到达时刻与播放位置同源，
    //! 而 WinMM 回调的调度抖动是毫秒级 —— 对"之后还要量化"的录制来说足够了。
    struct PendingMidiEvent {
        int opcode = 0;
        int note = 0;
        int velocity = 0;
        qint64 arrivalMs = 0;
    };

    //! MIDI 端口在自己的线程上发事件（WinMM 回调 → `WinMidiInPort::doProcess()`），
    //! 而这个模型只被主线程碰。所以**不能**在订阅回调里直接改模型状态：
    //! 事件先进队列（加锁），再由 `m_midiProcessTimer` 搬到主线程处理 ——
    //! 与上游 `NotationMidiInput`（20ms 一个 QTimer）是同一个做法。
    void onMidiPortEvent(int opcode, int note, int velocity);
    void processMidiEvents();

    //! 事件到达时刻 → **未量化**的分数 tick（double）。
    //! 最近一次播放位置（分数秒）+ 它被采到时的墙钟，用事件与它之间的墙钟差补上中间那一段。
    double recordTickAt(qint64 eventMs) const;

    //! 重算实时预览（按当前量化设置）。`force == false` 时按 `RECORD_OVERLAY_INTERVAL_MS` 节流。
    void updateRecordedNotes(bool force);

    void startRecording(int staffIndex);

    //! 当前档位对应的量化设置（网格来自 `midiQuantizeGrids()` 的下标）。
    MidiQuantizeSettings quantizeSettings() const;

    static qint64 nowMs();

    MidiRecorder m_recorder;

    std::mutex m_midiMutex;
    std::vector<PendingMidiEvent> m_midiQueue;
    QTimer* m_midiProcessTimer = nullptr;

    //! ⚠️ **原子**：MIDI 端口线程要用它判断"现在是不是在录"（不在录就整串事件都不收）。
    std::atomic<bool> m_isRecording { false };

    //! 录到哪个谱表（视图把当前选中的谱表传进来，模型看不到视图的选中）。
    int m_recordStaff = 0;

    //! 播放位置 + 采到它时的墙钟。**原子**：位置是从播放线程/音频线程上发出来的，
    //! 主线程只读（写-写竞争会让时间戳与位置对不上，录制就会整体偏一个音频块）。
    std::atomic<double> m_recordPosSecs { 0.0 };
    std::atomic<qint64> m_recordPosWallMs { 0 };
    std::atomic<bool> m_recordPosRunning { false };

    //! 播放真的进入过 Playing 吗。用来区分"刚起播时的 Stopped 瞬态"与"放到头了"——
    //! 前者不能当成"停止录制"（那会让每次录制一开始就立刻提交一个空 take）。
    bool m_recordSawPlaying = false;

    QVariantList m_recordedNotes;
    qint64 m_lastOverlayMs = 0;

    //! 上一次报给界面的音数（录制中那个读数只在**变了**的时候才发通知，见 updateRecordedNotes）。
    int m_lastRecordedCount = 0;

    int m_quantizeGridIndex = 4;    //!< `midiQuantizeGrids()` 的下标：默认 1/16
    int m_quantizeStrength = 100;
    QString m_lastTakeSummary;

    std::vector<MidiNoteItem> m_entries;
    QVariantList m_notes;
    QVariantList m_measures;

    QStringList m_staffNames;
    QString m_scoreName;
    bool m_hasScore = false;

    //! Set while an edit of our own is in flight, so the score's change notification does not
    //! rebuild the list underneath us.
    bool m_rebuildSuppressed = false;

    int m_lowestPitch = 60;
    int m_highestPitch = 72;
    int m_totalTicks = 0;

    double m_playbackTick = 0.0;
    bool m_isPlaying = false;

    //! The staff whose tracks this page soloed (-1 = none). Kept so that releasing the solo, or moving
    //! to another staff, gives back exactly what it took.
    int m_soloStaff = -1;

    INotationPtr m_notation;

    //! 视图状态：当前乐谱的那份 + 按乐谱分开存的所有份（见 viewState 属性的说明）。
    QVariantMap m_viewState;
    QString m_viewStateKey;
    QHash<QString, QVariantMap> m_viewStateByScore;
};
}
