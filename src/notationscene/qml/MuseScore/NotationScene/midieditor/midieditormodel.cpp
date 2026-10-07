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

#include "midieditormodel.h"

#include <algorithm>

#include <QDateTime>

#include "engraving/dom/measure.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"

#include "midi/midievent.h"

#include "context/iglobalcontext.h"
#include "context/iplaybackstate.h"

#include "log.h"
#include "translation.h"

#include "notation/imasternotation.h" // IWYU pragma: keep
#include "notation/inotation.h"
#include "notation/inotationelements.h" // IWYU pragma: keep
#include "notation/inotationinteraction.h"
#include "notation/inotationnoteinput.h"
#include "notation/inotationplayback.h"
#include "notation/inotationsolomutestate.h"
#include "notation/inotationundostack.h"

using namespace mu::engraving;
using namespace mu::notation;
using namespace muse;

//! 把 MIDI 队列搬到主线程的间隔（毫秒）。与上游 `NotationMidiInput::PROCESS_INTERVAL` 同一个量级：
//! 再小也只是更频繁地空转 —— 录制的精度由**事件的到达时刻**保证，与"什么时候处理"无关。
static constexpr int MIDI_PROCESS_INTERVAL_MS = 10;

//! 实时预览的重算节流（毫秒）：按住不放的音要"长出来"，但没必要每 10ms 重算整串音。
static constexpr int RECORD_OVERLAY_INTERVAL_MS = 60;

MidiEditorModel::MidiEditorModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

MidiEditorModel::~MidiEditorModel()
{
    disconnectFromCurrentScore();
}

Score* MidiEditorModel::currentScore() const
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return nullptr;
    }

    return notation->elements()->msScore();
}

INotationPlaybackPtr MidiEditorModel::currentPlayback() const
{
    INotationPtr notation = context()->currentNotation();
    return notation ? notation->masterNotation()->playback() : nullptr;
}

//! 视图状态按**乐谱**分开记（与记谱页的视图状态一样）。键取工程文件路径；
//! 还没存过盘的谱子用 EID 兜底 —— 同一份谱子在会话里始终是同一个键。
static QString viewStateKeyForScore(const Score* score)
{
    const MasterScore* master = score ? score->masterScore() : nullptr;
    if (!master) {
        return QString();
    }

    const IFileInfoProviderPtr fileInfo = master->fileInfo();
    if (fileInfo && !fileInfo->path().empty()) {
        return fileInfo->path().toQString();
    }

    return QString::fromStdString(master->eid().toStdString());
}

void MidiEditorModel::setViewState(const QVariantMap& state)
{
    m_viewState = state;

    if (!m_viewStateKey.isEmpty()) {
        m_viewStateByScore.insert(m_viewStateKey, state);
    }
}

Note* MidiEditorModel::noteAt(int row) const
{
    if (row < 0 || row >= int(m_entries.size())) {
        return nullptr;
    }

    return m_entries[size_t(row)].note;
}

void MidiEditorModel::init()
{
    context()->currentNotationChanged().onNotify(this, [this]() {
        //! 换工程时正在录制的那一场没有意义了：**放弃**它（而不是把它写进新打开的那份谱子）。
        if (m_isRecording) {
            finishRecording(false);
        }

        connectToCurrentScore();
        reload();
    });

    context()->playbackState()->playbackPositionChanged().onReceive(this, [this](muse::audio::secs_t pos) {
        //! 录制用的参考时钟（见 recordTickAt()）。⚠️ 这个回调可能不在主线程上，所以这里
        //! **只写原子成员**，不碰任何 QML/模型状态。
        m_recordPosSecs.store(double(pos));
        m_recordPosWallMs.store(nowMs());
        m_recordPosRunning.store(context()->playbackState()->isPlaying());

        updatePlaybackState();
    });

    context()->playbackState()->playbackStatusChanged().onReceive(this, [this](muse::audio::PlaybackStatus status) {
        updatePlaybackState();

        //! ── 播放自己停下来 = 这一次录制到此为止 ────────────────────────────────────
        //! 用户按空格、或曲子放到了末尾，都走**与按停止按钮同一条**收尾路径（封口 → 量化 →
        //! 一个事务写进乐谱）。`m_recordSawPlaying` 用来跳过起播瞬间那个 Stopped 瞬态 ——
        //! 少了它，每次按下录制的下一刻就会提交一个空 take。
        //! ⚠️ 这里用**排队调用**而不是直接调：这个回调可能在音频线程上，
        //! 而写乐谱必须在主线程（下面 finishRecording 会开事务、改 engraving 模型）。
        if (status == muse::audio::PlaybackStatus::Running) {
            m_recordSawPlaying = true;
        } else if (m_isRecording && m_recordSawPlaying) {
            QMetaObject::invokeMethod(this, [this]() { finishRecording(true); }, Qt::QueuedConnection);
        }
    });

    //! 验证钩子：等播放真的初始化好再动手 —— 播放器是播放初始化时才建出来的，太早 seek 会在
    //! 上游的 `IF_ASSERT_FAILED(currentPlayer())` 上留一条断言（看起来像回归，其实只是太早）。
    m_demoPlaybackPending = qEnvironmentVariableIsSet("MUSE_MIDIEDITOR_DEMO_PLAYBACK");
    m_demoRecordPending = qEnvironmentVariableIsSet("MUSE_MIDIEDITOR_DEMO_RECORD");
    playbackController()->playbackInitedChanged().onReceive(this, [this](bool inited) {
        if (inited) {
            applyDemoPlaybackIfPending();
            applyDemoRecordingIfPending();
        }
    });

    //! ── 实时录制的两条接线 ──────────────────────────────────────────────────────────
    m_midiProcessTimer = new QTimer(this);
    m_midiProcessTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(m_midiProcessTimer, &QTimer::timeout, this, [this]() { processMidiEvents(); });

    if (midiInPort()) {
        //! ⚠️ 这个回调**在 MIDI 端口自己的线程上**跑（WinMM 回调 → WinMidiInPort::doProcess()），
        //! 所以它里面只能做一件事：把事件塞进队列。一个模型成员都不能碰（包括 emit）。
        midiInPort()->eventReceived().onReceive(this, [this](const muse::midi::tick_t, const muse::midi::Event& event) {
            if (!m_isRecording.load()) {
                return;
            }

            const muse::midi::Event::Opcode opcode = event.opcode();
            if (opcode != muse::midi::Event::Opcode::NoteOn && opcode != muse::midi::Event::Opcode::NoteOff) {
                return;
            }

            onMidiPortEvent(int(opcode), event.note(), event.velocity7());
        });

        //! 设备插拔/切换要反映到工具条上（"没有 MIDI 输入设备"这句话就是从这里来的）。
        midiInPort()->availableDevicesChanged().onNotify(this, [this]() { emit recordChanged(); });
        midiInPort()->deviceChanged().onNotify(this, [this]() { emit recordChanged(); });
    }

    connectToCurrentScore();
    reload();

    applyDemoPlaybackIfPending();
    applyDemoRecordingIfPending();
}

void MidiEditorModel::applyDemoPlaybackIfPending()
{
    if (!m_demoPlaybackPending || !m_hasScore || !playbackController()->isPlaybackInited()) {
        return;
    }

    if (m_measures.size() < 2) {
        return;
    }

    m_demoPlaybackPending = false;

    const int inTick = m_measures[1].toMap()["tick"].toInt();
    const int lastIndex = std::min<int>(3, int(m_measures.size()) - 1);
    const int outTick = m_measures[size_t(lastIndex)].toMap()["endTick"].toInt();

    seekTick(inTick);
    setLoopRange(inTick, outTick);

    //! 验证钩子（**有意保留**，与 MUSE_MIDIEDITOR_DEMO_PLAYBACK 配套）：设好循环后直接起播，
    //! 于是"循环有没有真的无缝"不必点 GUI 就能量（机器不能驱动画布上的手势，见 维护手册.md §7.6）。
    //! 不设这个环境变量时一行都不会执行。
    if (qEnvironmentVariableIsSet("MUSE_MIDIEDITOR_DEMO_LOOP_PLAY")) {
        playbackController()->play(false /*showErrors*/);
    }
}

void MidiEditorModel::connectToCurrentScore()
{
    disconnectFromCurrentScore();

    //! A different score (or a fresh one) owns its own solo state; whatever this page soloed in the
    //! previous project is not ours to carry over.
    m_soloStaff = -1;

    m_notation = context()->currentNotation();
    if (!m_notation) {
        return;
    }

    Score* score = m_notation->elements()->msScore();
    if (!score) {
        return;
    }

    //! 换乐谱：换成那份乐谱自己的视图状态（滚动/缩放/选中的谱表/开关）。
    //! 视图若活着就会收到通知并重新摆位；视图若是刚建的，它会自己来读。
    m_viewStateKey = viewStateKeyForScore(score);
    m_viewState = m_viewStateByScore.value(m_viewStateKey);
    emit viewStateChanged();

    //! NOTE: the notation page and this page share one score, so an edit made there has to show up
    //!       here and vice versa. `changesChannel` is the data-changed channel of the engraving
    //!       model (the one PlaybackCursor also listens to); `notationChanged` is only a repaint
    //!       request for the notation view and would be the wrong thing to reload from.
    score->changesChannel().onReceive(this, [this](const ScoreChanges&) {
        //! Our own edits notify the score too; mutateOnce() rebuilds once when it is done, so
        //! rebuilding here as well would double the work for every single edit.
        if (m_rebuildSuppressed) {
            return;
        }

        reload();
    });

    //! 撤销/重做的可用状态要跟着记谱的 undo stack 走 —— MIDI 页没有记谱页那个 UndoRedoToolBar，
    //! 所以"能不能撤销"这件事得由这一页自己说出来（否则按钮/快捷键无从判断）。
    if (INotationUndoStackPtr undoStack = m_notation->undoStack()) {
        undoStack->stackChanged().onNotify(this, [this]() {
            emit undoRedoChanged();
        });
    }

    //! The loop region is the score's own (the notation page's markers point at the same two ticks),
    //! so the roll has to follow it when it changes there or in the playback toolbar.
    if (INotationPlaybackPtr playback = m_notation->masterNotation()->playback()) {
        playback->loopBoundariesChanged().onNotify(this, [this]() {
            emit loopChanged();
        });

        playback->loopEnabledChanged().onReceive(this, [this](bool) {
            emit loopChanged();
        });
    }

    //! Same for solo: the mixer and this page's "only this staff" toggle write one state, so a change
    //! made in the mixer has to reach the toggle.
    if (INotationSoloMuteStatePtr soloMute = m_notation->soloMuteState()) {
        soloMute->trackSoloMuteStateChanged().onReceive(this, [this](const InstrumentTrackId&, const INotationSoloMuteState::SoloMuteState&) {
            emit soloChanged();
        });
    }
}

void MidiEditorModel::disconnectFromCurrentScore()
{
    if (m_notation) {
        Score* score = m_notation->elements()->msScore();
        if (score) {
            score->changesChannel().disconnect(this);
        }

        if (INotationUndoStackPtr undoStack = m_notation->undoStack()) {
            undoStack->stackChanged().disconnect(this);
        }

        if (INotationPlaybackPtr playback = m_notation->masterNotation()->playback()) {
            playback->loopBoundariesChanged().disconnect(this);
            playback->loopEnabledChanged().disconnect(this);
        }

        if (INotationSoloMuteStatePtr soloMute = m_notation->soloMuteState()) {
            soloMute->trackSoloMuteStateChanged().disconnect(this);
        }
    }

    m_notation = nullptr;
}

bool MidiEditorModel::canUndo() const
{
    INotationPtr notation = context()->currentNotation();
    INotationUndoStackPtr undoStack = notation ? notation->undoStack() : nullptr;

    return undoStack ? undoStack->canUndo() : false;
}

bool MidiEditorModel::canRedo() const
{
    INotationPtr notation = context()->currentNotation();
    INotationUndoStackPtr undoStack = notation ? notation->undoStack() : nullptr;

    return undoStack ? undoStack->canRedo() : false;
}

void MidiEditorModel::undo()
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return;
    }

    //! NOTE: the very same undo stack the notation page uses - the two pages edit one score, so they
    //!       must also be one history. This is what makes Ctrl+Z work on this page at all: the
    //!       command's own shortcut is empty (notationcommandsregister.cpp) and the notation page's
    //!       UndoRedoToolBar, which is where the shortcut actually lives, is not mounted here.
    notation->undoStack()->undo(nullptr);
}

void MidiEditorModel::redo()
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return;
    }

    notation->undoStack()->redo(nullptr);
}

void MidiEditorModel::reload()
{
    m_notes.clear();
    m_measures.clear();
    m_staffNames.clear();
    m_scoreName.clear();

    m_hasScore = false;
    m_lowestPitch = 60;
    m_highestPitch = 72;
    m_totalTicks = 0;

    Score* score = currentScore();
    if (!score) {
        m_entries.clear();
        emit scoreChanged();
        return;
    }

    m_hasScore = true;
    m_scoreName = score->name().toQString();     //! NOTE: the score (work) name shown in the tab

    for (size_t i = 0; i < score->nstaves(); ++i) {
        const Staff* staff = score->staff(i);
        const Part* part = staff ? staff->part() : nullptr;
        m_staffNames << (part ? part->partName().toQString() : QString());
    }

    const Measure* lastMeasure = score->lastMeasure();
    m_totalTicks = lastMeasure ? lastMeasure->endTick().ticks() : 0;

    m_entries = collectMidiNotes(score);

    int lowest = 127;
    int highest = 0;

    m_notes.reserve(int(m_entries.size()));
    for (const MidiNoteItem& entry : m_entries) {
        QVariantMap note;
        note["tick"] = entry.tick;
        note["durationTicks"] = entry.durationTicks;
        note["pitch"] = entry.pitch;
        note["velocity"] = entry.velocity;
        note["hasVelocityOverride"] = entry.hasVelocityOverride;
        note["staffIndex"] = entry.staffIndex;
        note["voice"] = entry.voice;
        note["hasPlayOverride"] = entry.hasPlayOverride;
        note["playTick"] = entry.playTick;
        note["playDurationTicks"] = entry.playDurationTicks;
        note["playVelocityPercent"] = entry.playVelocityPercent;
        m_notes << note;

        lowest = std::min(lowest, entry.pitch);
        highest = std::max(highest, entry.pitch);
    }

    for (const MidiMeasureItem& measure : collectMidiMeasures(score)) {
        QVariantMap item;
        item["tick"] = measure.tick;
        item["endTick"] = measure.endTick;
        item["index"] = int(m_measures.size());
        m_measures << item;
    }

    if (!m_entries.empty()) {
        //! NOTE: a little air above and below so that notes are not glued to the edges.
        m_lowestPitch = std::max(0, lowest - 2);
        m_highestPitch = std::min(127, highest + 2);
    }

    emit scoreChanged();
    updatePlaybackState();

    //! ⚠️ 录制那组属性（能不能录 / 上一次结果 / 设备名）也要跟着工程走：
    //! `canRecord()` 里的 `m_hasScore` 就是在这里翻成 true 的，而**只有这一条通知**能让
    //! 走带里那个录制键重新求值。少了它，录制键在"先开程序、后开工程"的常见顺序下会一直是灰的
    //! （2026-10-07 实拍发现的：图标画成了不可用，按下去也没反应）。
    emit recordChanged();
}

void MidiEditorModel::updatePlaybackState()
{
    INotationPtr notation = context()->currentNotation();
    INotationPlaybackPtr playback = notation ? notation->masterNotation()->playback() : nullptr;

    const double tick = playback
                        ? double(playback->secToTick(context()->playbackState()->playbackPosition()))
                        : 0.0;
    const bool playing = context()->playbackState()->isPlaying();

    if (qFuzzyCompare(tick, m_playbackTick) && playing == m_isPlaying) {
        return;
    }

    m_playbackTick = tick;
    m_isPlaying = playing;

    emit playbackTickChanged();

    //! 录制中：还按着的那个音要跟着播放头**长出来**（重算按 RECORD_OVERLAY_INTERVAL_MS 节流）。
    if (m_isRecording) {
        updateRecordedNotes(false);
    }
}

// ── playback: where it plays from, what it loops, and what it plays ──────────────────────────────

int MidiEditorModel::loopInTick() const
{
    INotationPlaybackPtr playback = currentPlayback();
    return playback ? playback->loopBoundaries().loopInTick.ticks() : 0;
}

int MidiEditorModel::loopOutTick() const
{
    INotationPlaybackPtr playback = currentPlayback();
    return playback ? playback->loopBoundaries().loopOutTick.ticks() : 0;
}

bool MidiEditorModel::loopEnabled() const
{
    INotationPlaybackPtr playback = currentPlayback();
    return playback ? playback->isLoopEnabled() : false;
}

void MidiEditorModel::seekTick(int tick)
{
    const int wanted = std::max(0, tick);

    playbackController()->seekTick(wanted);

    //! Publish the new position right away rather than waiting for the audio player to report it: while
    //! stopped it may never do so (upstream's seekRawTick() updates its own tick for exactly this
    //! reason), and the playhead is the one thing the user is looking at after clicking in the ruler.
    if (!qFuzzyCompare(double(wanted) + 1.0, m_playbackTick + 1.0)) {
        m_playbackTick = double(wanted);
        emit playbackTickChanged();
    }
}

void MidiEditorModel::playNote(int row)
{
    //! 行 → 音符**现在**解析一次（不缓存指针）：`notes` 每次乐谱变更都会重建，而这一页所有的
    //! 编辑接口都是"拿 row 现查"（见 noteAt() 的说明）—— 试听走同一条规矩，就不会有悬空指针。
    Note* note = noteAt(row);
    if (!note) {
        return;
    }

    //! ⚠️ 只管"哪个音"，不管"要不要响"：**开关与时长都由播放层自己判**
    //! （`playElements()` 里查 `playNotesWhenEditing()`）。记谱页那一声也是这么来的，
    //! 所以这一页不需要、也不该另建一套试听设置。
    playbackController()->playElements({ note });
}

void MidiEditorModel::playNoteAtPitch(int row, int pitch)
{
    Note* note = noteAt(row);
    if (!note) {
        return;
    }

    //! 拖动中只预览、不写谱，所以这里必须拿**临时音符**去播（详见 midiNoteToAudition()）：
    //! 直接播原音符的话，响的永远是拖动前的音高。
    const MidiAuditionNote audition = midiNoteToAudition(currentScore(), note, pitch);
    if (!audition.isValid()) {
        return;
    }

    //! ⚠️ 先播、再删。`playElements()` 同步走完渲染与发送，所以事件已经出去了；而删除和弦会把它的
    //! 音符一起删掉（`Chord::~Chord()`），所以这里**只删和弦**，不能再删 `audition.note`。
    playbackController()->playElements({ audition.note });
    delete audition.chord;
}

void MidiEditorModel::togglePlay()
{
    //! NOTE: 与记谱页按空格最终走到的是**同一个**调用（`PLAY_TOGGLE_COMMAND` 的实现就是它），
    //!       所以"播放 / 暂停 / 从头播"的语义两页一致。为什么这一页要自己接空格：见头文件。
    playbackController()->togglePlay();
}

QVariantMap MidiEditorModel::loopRangeFromDrag(int draggedTick, int releasedTick, int snapTicks) const
{
    const MidiLoopRange range = midiLoopRangeFromDrag(draggedTick, releasedTick, m_totalTicks, snapTicks);

    QVariantMap result;
    result["inTick"] = range.inTick;
    result["outTick"] = range.outTick;
    result["valid"] = range.valid;
    return result;
}

void MidiEditorModel::setLoopRange(int inTick, int outTick)
{
    INotationPlaybackPtr playback = currentPlayback();
    if (!playback || outTick <= inTick) {
        return;
    }

    int in = std::max(0, inTick);
    int out = std::max(in + 1, outTick);

    //! ⚠️ Upstream reads a boundary tick of 0, 1 or 2 as a `BoundaryTick` (see inotationplayback.h):
    //! 1 would mean "wherever the score cursor is" and 2 "the end of the score" - the loop would land
    //! somewhere nobody asked for. midiLoopRangeFromDrag() cannot produce those, since it snaps to a
    //! grid step; this is the belt-and-braces for any other caller. 0 is safe either way
    //! (FirstScoreTick is the start of the score).
    if (in == 1 || in == 2) {
        in = 0;
    }
    if (out == 1 || out == 2) {
        out = 3;
    }

    //! In first: addLoopIn() pushes an out point at or before it to the end of the score, so writing
    //! them in this order never leaves the loop momentarily inside out.
    playback->addLoopBoundary(LoopBoundaryType::LoopIn, in);
    playback->addLoopBoundary(LoopBoundaryType::LoopOut, out);
    playback->setLoopBoundariesEnabled(true);
    emit loopChanged();
}

void MidiEditorModel::clearLoop()
{
    INotationPlaybackPtr playback = currentPlayback();
    if (!playback) {
        return;
    }

    //! Clear the two points rather than only the enable flag: the notation page draws its markers from
    //! them, and "remove the loop" has to look the same on both pages. Out == In == 0 is the "no loop"
    //! state the score starts in.
    playback->addLoopBoundary(LoopBoundaryType::LoopIn, 0);
    playback->addLoopBoundary(LoopBoundaryType::LoopOut, 0);
    playback->setLoopBoundariesEnabled(false);
    emit loopChanged();
}

bool MidiEditorModel::staffIsSoloed(int staffIndex) const
{
    Score* score = currentScore();
    if (!score || staffIndex < 0 || staffIndex >= int(score->nstaves())) {
        return false;
    }

    const Staff* staff = score->staff(staffIndex);
    const Part* part = staff ? staff->part() : nullptr;
    if (!part) {
        return false;
    }

    const InstrumentTrackIdList trackIds = part->instrumentTrackIdList();
    if (trackIds.empty()) {
        return false;
    }

    for (const InstrumentTrackId& trackId : trackIds) {
        //! Read back rather than remembering: a solo the user switched off in the mixer must reach
        //! this page's toggle too.
        if (!playbackController()->trackSoloMuteState(trackId).solo) {
            return false;
        }
    }

    return true;
}

void MidiEditorModel::setStaffSolo(int staffIndex, bool solo)
{
    Score* score = currentScore();
    if (!score || staffIndex < 0 || staffIndex >= int(score->nstaves())) {
        return;
    }

    const Staff* staff = score->staff(staffIndex);
    const Part* part = staff ? staff->part() : nullptr;
    if (!part) {
        return;
    }

    //! NOTE: this writes the mixer's own solo state (the same one the mixer panel and the notation
    //! page's mixer edit), so the channel strip lights up and the engine mutes the others - "only this
    //! staff" is not a second, private notion of solo.
    for (const InstrumentTrackId& trackId : part->instrumentTrackIdList()) {
        INotationSoloMuteState::SoloMuteState state = playbackController()->trackSoloMuteState(trackId);
        state.solo = solo;
        playbackController()->setTrackSoloMuteState(trackId, state);
    }
}

bool MidiEditorModel::soloActive() const
{
    return m_soloStaff >= 0 && staffIsSoloed(m_soloStaff);
}

void MidiEditorModel::setSoloStaff(int staffIndex)
{
    Score* score = currentScore();
    if (!score || staffIndex < 0 || staffIndex >= int(score->nstaves())) {
        return;
    }

    //! Moving to another staff hands the solo over: the one this page took is released, so the solo
    //! follows the staff being edited rather than piling up.
    if (m_soloStaff >= 0 && m_soloStaff != staffIndex) {
        setStaffSolo(m_soloStaff, false);
    }

    setStaffSolo(staffIndex, true);
    m_soloStaff = staffIndex;
    emit soloChanged();
}

void MidiEditorModel::clearSoloStaff()
{
    if (m_soloStaff < 0) {
        return;
    }

    const int staff = m_soloStaff;
    m_soloStaff = -1;
    setStaffSolo(staff, false);
    emit soloChanged();
}

double MidiEditorModel::followScrollX(double scrollX, double playheadX, double viewportWidth, double maxScrollX) const
{
    return midiFollowScrollX(scrollX, playheadX, viewportWidth, maxScrollX);
}

void MidiEditorModel::setNotePitch(int row, int pitch)
{
    //! ⚠️ 事务由 **notation 的 undo stack** 开（不是写入函数自带的那条）：只有它会在提交后通知
    //! "栈变了"，撤销/重做命令的状态（以及主菜单与 Ctrl+Z）才会跟上 —— 见头文件里 openCommand 的说明。
    mutateOnce([this, row, pitch]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Change pitch"),
                                               [this, row, pitch](engraving::Transaction&) {
                applyNotePitch(currentScore(), noteAt(row), pitch, /*openCommand*/ false);
            });
        }
    });
}

void MidiEditorModel::setNoteVelocity(int row, int velocity)
{
    mutateOnce([this, row, velocity]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Change velocity"),
                                               [this, row, velocity](engraving::Transaction&) {
                applyNoteVelocity(currentScore(), noteAt(row), velocity, /*openCommand*/ false);
            });
        }
    });
}

void MidiEditorModel::setNoteVelocities(const QVariantList& rows, const QVariantList& velocities)
{
    if (rows.size() != velocities.size() || rows.isEmpty()) {
        return;
    }

    //! NOTE: deferred by one event-loop turn ON PURPOSE. Applying the edits inline blocks the event
    //!       loop, so the canvas repaint the view queued on release cannot run until the work is
    //!       finished - and the bars therefore stayed in the brush colour until the model was done.
    //!       That gap is the delay that could be felt. Deferring lets the repaint through first, so
    //!       the gesture looks finished the moment the button comes up.
    QMetaObject::invokeMethod(this, [this, rows, velocities]() {
        applyVelocityBatch(rows, velocities);
    }, Qt::QueuedConnection);
}

void MidiEditorModel::applyVelocityBatch(const QVariantList& rows, const QVariantList& velocities)
{
    Score* score = currentScore();
    if (!score) {
        return;
    }

    //! Collect first, then write: applyNoteVelocities() wraps the whole stroke in ONE command, so the
    //! score is notified once instead of once per note. The notification is what costs - its
    //! subscribers repaint the notation view and rebuild the playback events, both O(score) - and
    //! that is why drawing more notes used to take proportionally longer.
    std::vector<std::pair<Note*, int> > changes;
    changes.reserve(size_t(rows.size()));

    std::vector<int> appliedRows;
    std::vector<int> appliedValues;
    appliedRows.reserve(size_t(rows.size()));
    appliedValues.reserve(size_t(rows.size()));

    for (int i = 0; i < rows.size(); ++i) {
        const int row = rows[i].toInt();
        if (row < 0 || row >= int(m_entries.size())) {
            continue;
        }

        //! Kept side by side: skipping an out-of-range row would otherwise shift the two lists apart.
        changes.emplace_back(noteAt(row), velocities[i].toInt());
        appliedRows.push_back(row);
        appliedValues.push_back(velocities[i].toInt());
    }

    const bool wasSuppressed = m_rebuildSuppressed;
    m_rebuildSuppressed = true;
    int changed = 0;
    //! 整笔一个命令，且事务开在 **notation 的 undo stack** 上（同 setNotePitch 的理由）。
    if (INotationPtr notation = context()->currentNotation()) {
        notation->undoStack()->transaction(TranslatableString("midieditor", "Draw velocities"),
                                           [this, score, &changes, &changed](engraving::Transaction&) {
            changed = applyNoteVelocities(score, changes, /*openCommand*/ false);
        });
    }
    m_rebuildSuppressed = wasSuppressed;

    if (changed == 0) {
        return;
    }

    //! NOTE: only the velocities changed, so patch the cached lists instead of collecting the whole
    //!       score again. A full reload walks every note of every staff and builds a QVariantMap per
    //!       note - none of which can have changed here, and all of which costs time the user can
    //!       feel. The maps still have to be re-emitted so the view sees the new values.
    for (size_t i = 0; i < appliedRows.size(); ++i) {
        const int row = appliedRows[i];
        const int velocity = std::clamp(appliedValues[i], 0, 127);

        m_entries[size_t(row)].velocity = midiDisplayVelocity(velocity);
        m_entries[size_t(row)].hasVelocityOverride = (velocity > 0);

        QVariantMap note = m_notes[row].toMap();
        note["velocity"] = midiDisplayVelocity(velocity);
        note["hasVelocityOverride"] = (velocity > 0);
        m_notes[row] = note;
    }

    emit scoreChanged();
}

//! NOTE: the automation curve itself - which points it has, which of them are the user's to remove, and
//!       how a stroke is written as one command - lives in midieditornotes.cpp as free functions over a
//!       plain `Score*`, so that "both pages address the same curve" is covered by a unit test.
QVariantList MidiEditorModel::automationPoints(int staffIndex) const
{
    QVariantList result;

    for (const MidiAutomationPoint& point : collectAutomationPoints(currentScore(), staffIndex)) {
        QVariantMap item;
        item["tick"] = point.tick;
        item["value"] = point.value;
        item["authored"] = point.authored;
        item["hasEase"] = point.hasEase;
        item["controlT"] = point.controlT;
        item["controlValue"] = point.controlValue;
        item["arrival"] = point.arrival;
        result << item;
    }

    return result;
}

void MidiEditorModel::setAutomationPoint(int staffIndex, int tick, double value)
{
    const std::vector<MidiAutomationPoint> points { MidiAutomationPoint { tick, value } };

    //! NOTE: write through the engraving model, rebuild once - see mutateOnce.
    //! ⚠️ 事务由 **notation 的 undo stack** 开（不是自由函数自带的那条）：只有它会在提交后通知
    //! "栈变了"，撤销/重做命令的状态、主菜单与 Ctrl+Z 才会跟上（见头文件里 openCommand 的说明）。
    mutateOnce([this, staffIndex, points]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Draw dynamics curve"),
                                               [this, staffIndex, points](engraving::Transaction&) {
                applyAutomationPoints(currentScore(), staffIndex, points, /*openCommand*/ false);
            });
        }
    });
}

void MidiEditorModel::setAutomationPoints(int staffIndex, const QVariantList& points)
{
    std::vector<MidiAutomationPoint> drawn;
    drawn.reserve(points.size());

    for (const QVariant& entry : points) {
        const QVariantMap point = entry.toMap();

        MidiAutomationPoint written;
        written.tick = point.value("tick").toInt();
        written.value = point.value("value").toDouble();
        drawn.push_back(written);
    }

    if (drawn.empty()) {
        return;
    }

    mutateOnce([this, staffIndex, drawn]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Add dynamics point"),
                                               [this, staffIndex, drawn](engraving::Transaction&) {
                applyAutomationPoints(currentScore(), staffIndex, drawn, /*openCommand*/ false);
            });

            //! 观测点：**事务提交之后**点还在不在。用户报「新建的点切页后消失」（= 模型里没有），
            //! 这一行与 applyAutomationPoints 里那行配合，就能分清是"没写进去"还是"写进去又被清掉"。
            LOGW() << "[midi-automation] after transaction: points="
                   << collectAutomationPoints(currentScore(), staffIndex).size();
        }
    });
}

void MidiEditorModel::removeAutomationPoint(int staffIndex, int tick)
{
    mutateOnce([this, staffIndex, tick]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Remove dynamics point"),
                                               [this, staffIndex, tick](engraving::Transaction&) {
                eraseAutomationPoint(currentScore(), staffIndex, tick, /*openCommand*/ false);
            });
        }
    });
}

void MidiEditorModel::setAutomationPointEase(int staffIndex, int tick, double t, double value)
{
    mutateOnce([this, staffIndex, tick, t, value]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Bend dynamics curve"),
                                               [this, staffIndex, tick, t, value](engraving::Transaction&) {
                applyAutomationPointEase(currentScore(), staffIndex, tick, t, value, /*openCommand*/ false);
            });
        }
    });
}

void MidiEditorModel::moveAutomationPoint(int staffIndex, int fromTick, int toTick, double value)
{
    mutateOnce([this, staffIndex, fromTick, toTick, value]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Move dynamics point"),
                                               [this, staffIndex, fromTick, toTick, value](engraving::Transaction&) {
                applyAutomationPointMove(currentScore(), staffIndex, fromTick, toTick, value, /*openCommand*/ false);
            });
        }
    });
}

//! NOTE: writing through the engraving model notifies the score, and our notification handler
//!       rebuilds the whole note list - so one edit would rebuild twice, and a batch of N would
//!       rebuild 2N times. That is what made a brush stroke lag: sweeping over ten notes cleared and
//!       rebuilt the note list (and with it the QML list and the canvas) about twenty times.
void MidiEditorModel::mutateOnce(const std::function<void()>& mutate)
{
    if (!currentScore()) {
        return;
    }

    const bool wasSuppressed = m_rebuildSuppressed;
    m_rebuildSuppressed = true;
    mutate();
    m_rebuildSuppressed = wasSuppressed;

    reload();
}

void MidiEditorModel::setNotePlayOverride(int row, int startTick, int durationTicks, int velocityPercent)
{
    mutateOnce([this, row, startTick, durationTicks, velocityPercent]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Change played timing"),
                                               [this, row, startTick, durationTicks, velocityPercent](engraving::Transaction&) {
                applyNotePlayOverride(currentScore(), noteAt(row), startTick, durationTicks, velocityPercent,
                                      /*openCommand*/ false);
            });
        }
    });
}

// ── 实时录制 ────────────────────────────────────────────────────────────────────────────────────
//
// 四段，各管一段：
//   播放   录制开始时从播放头起播（`startRecording()` 里那一句 `play(false)`）
//   演奏   上游记谱页的 MIDI 输入试听（`NotationMidiInput`）—— 这一页**不另造发声机制**
//   记录   本文件：端口事件 → 队列 → 主线程 → `MidiRecorder`（未量化的 tick）
//   量化   停止时按网格与强度算一遍，再一次事务写进乐谱（`applyRecordedChords`）

qint64 MidiEditorModel::nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

//! 当前量化档位（下拉框下标 → 真实 tick 数）。
static int quantizeGridTicksAt(int index)
{
    const std::vector<MidiQuantizeGrid>& grids = midiQuantizeGrids();
    const int clamped = std::clamp(index, 0, int(grids.size()) - 1);
    return grids[size_t(clamped)].ticks;
}

MidiQuantizeSettings MidiEditorModel::quantizeSettings() const
{
    MidiQuantizeSettings settings;
    settings.gridTicks = quantizeGridTicksAt(m_quantizeGridIndex);
    settings.strengthPercent = m_quantizeStrength;
    //! 结束位置也吸附：录下来的长度几乎不可能正好落在格子上，只量起点的话会得到一串
    //! "120、118、123"这样的时值，谱面上会变成一堆奇怪的附点音符。
    settings.quantizeEnd = true;
    return settings;
}

QVariantList MidiEditorModel::quantizeGrids() const
{
    QVariantList result;

    const std::vector<MidiQuantizeGrid>& grids = midiQuantizeGrids();
    for (size_t i = 0; i < grids.size(); ++i) {
        QVariantMap entry;
        //! 只有"不量化"这一档需要翻译，其余的（1/4、1/8T…）是记号，两种语言里一样。
        entry["label"] = i == 0 ? muse::qtrc("notationscene", "Off") : QString::fromUtf8(grids[i].label);
        entry["ticks"] = grids[i].ticks;
        result << entry;
    }

    return result;
}

void MidiEditorModel::setQuantizeGridIndex(int index)
{
    const int clamped = std::clamp(index, 0, int(midiQuantizeGrids().size()) - 1);
    if (clamped == m_quantizeGridIndex) {
        return;
    }

    m_quantizeGridIndex = clamped;
    emit recordSettingsChanged();

    //! 中途换网格要**看得见**：预览马上按新网格重算（记下的演奏本身一个数都没动）。
    updateRecordedNotes(true);
}

void MidiEditorModel::setQuantizeStrength(int percent)
{
    const int clamped = std::clamp(percent, 0, 100);
    if (clamped == m_quantizeStrength) {
        return;
    }

    m_quantizeStrength = clamped;
    emit recordSettingsChanged();

    updateRecordedNotes(true);
}

bool MidiEditorModel::canRecord() const
{
    if (!m_hasScore || !midiInPort()) {
        return false;
    }

    //! ⚠️ `isConnected()` **不够**：没有设备时端口连的是 `NONE_DEVICE_ID`（"-1"），它同样让
    //! `isConnected()` 返回真（见 WinMidiInPort::connect 的 else 分支）。只看它的话，
    //! 没插键盘的机器上录制按钮是亮的、按下去却一个音都收不到 —— 正是"按了没反应"那类最难查的失败。
    const muse::midi::MidiDeviceID deviceId = midiInPort()->deviceID();
    return midiInPort()->isConnected() && !deviceId.empty() && deviceId != muse::midi::NONE_DEVICE_ID;
}

QString MidiEditorModel::midiInputDeviceName() const
{
    if (!midiInPort()) {
        return QString();
    }

    const muse::midi::MidiDeviceID id = midiInPort()->deviceID();
    for (const muse::midi::MidiDevice& device : midiInPort()->availableDevices()) {
        if (device.id == id) {
            return QString::fromStdString(device.name);
        }
    }

    return QString();
}

//! 端口线程 → 队列。**这里碰的每一个东西都必须是线程安全的**（`nowMs` 是静态的，队列有锁）。
void MidiEditorModel::onMidiPortEvent(int opcode, int note, int velocity)
{
    PendingMidiEvent event;
    event.opcode = opcode;
    event.note = note;
    event.velocity = velocity;
    event.arrivalMs = nowMs();      //!< 在端口线程上取：这是"这个音真的到了"的时刻

    std::lock_guard<std::mutex> lock(m_midiMutex);
    m_midiQueue.push_back(event);
}

double MidiEditorModel::recordTickAt(qint64 eventMs) const
{
    INotationPlaybackPtr playback = currentPlayback();
    if (!playback) {
        return 0.0;
    }

    const double positionSecs = m_recordPosSecs.load();
    const qint64 positionWallMs = m_recordPosWallMs.load();
    const bool running = m_recordPosRunning.load();

    double secs = positionSecs;
    if (running && positionWallMs > 0) {
        //! 事件与"最近一次播放位置"之间那一段用**墙钟**补上：播放位置是每块音频才报一次的
        //! （几十毫秒），直接拿它当事件时间的话每个音都会晚小半格 —— 量化之后就是"整体偏晚"。
        //! 夹到 ±200ms：万一系统时间被改（或端口与我们的时钟不同源），结果只是"不补"，
        //! 而不是把一个音甩到曲子另一头。
        const double deltaMs = std::clamp(double(eventMs - positionWallMs), -200.0, 200.0);
        secs = positionSecs + (deltaMs / 1000.0) * playbackController()->tempoMultiplier();
    }

    //! 结果刻意是 double（未量化的 tick）：1/32 三连音这一级的时间差也留得住，
    //! 量化是**提交时**才做的事 —— 用户还能改主意。
    return double(playback->secToTick(muse::audio::secs_t(std::max(0.0, secs))));
}

void MidiEditorModel::processMidiEvents()
{
    std::vector<PendingMidiEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_midiMutex);
        events.swap(m_midiQueue);
    }

    if (!m_isRecording) {
        return;
    }

    for (const PendingMidiEvent& event : events) {
        const double tick = recordTickAt(event.arrivalMs);

        //! ⚠️ `velocity == 0` 的 NoteOn 就是 NoteOff（MIDI 1.0 的约定；MIDI 2.0 那条路上
        //! 端口会替我们改成 NoteOff，但这里必须两种都认 —— 漏了就会有一个音一直不封口）。
        if (event.opcode == int(muse::midi::Event::Opcode::NoteOff) || event.velocity == 0) {
            m_recorder.noteOff(event.note, tick);
        } else {
            m_recorder.noteOn(event.note, event.velocity, tick);
        }
    }

    updateRecordedNotes(!events.empty());
}

void MidiEditorModel::updateRecordedNotes(bool force)
{
    const qint64 now = nowMs();
    if (!force && now - m_lastOverlayMs < RECORD_OVERLAY_INTERVAL_MS) {
        return;
    }
    m_lastOverlayMs = now;

    const MidiQuantizeSettings settings = quantizeSettings();
    const int minDuration = quantizeMinDurationTicks(settings);

    QVariantList overlay;

    //! 已经抬起来的音：按当前设置量化之后再画 —— 用户看到的就是"停止之后会写成什么样"。
    for (const MidiQuantizedNote& note : quantizeRecordedNotes(m_recorder.notes(), settings)) {
        QVariantMap item;
        item["tick"] = note.tick;
        item["durationTicks"] = note.durationTicks;
        item["pitch"] = note.pitch;
        item["velocity"] = note.velocity;
        item["held"] = false;
        overlay << item;
    }

    //! 还按着的音：也摆到格子上，末端跟着播放头长（它没有"停止时的长度"，所以只在录制中有意义）。
    if (m_isRecording) {
        const double endTick = recordTickAt(now);
        for (const MidiRecordedNote& note : m_recorder.notes()) {
            if (!note.isHeld()) {
                continue;
            }

            const int start = quantizeTickValue(note.startTick, settings.gridTicks, settings.strengthPercent);
            const int end = quantizeTickValue(endTick, settings.gridTicks, settings.strengthPercent);

            QVariantMap item;
            item["tick"] = start;
            item["durationTicks"] = std::max(minDuration, end - start);
            item["pitch"] = note.pitch;
            item["velocity"] = note.velocity;
            item["held"] = true;
            overlay << item;
        }
    }

    m_recordedNotes = overlay;
    emit recordedNotesChanged();

    //! 工具条上"Stop (N)"里那个 N 跟着走。`recordChanged` 原本只在**开始/停止**时发，
    //! 录制中它就一直停在 0 —— 实拍看到 "Stop (0)" 而画布上明明有两个音，
    //! 用户会以为"按键没记上"（2026-10-07 的截图里就是这个样子）。
    const int count = m_recorder.noteCount();
    if (count != m_lastRecordedCount) {
        m_lastRecordedCount = count;
        emit recordChanged();
    }
}

void MidiEditorModel::toggleRecording()
{
    if (m_isRecording) {
        finishRecording(true);
        return;
    }

    //! 第二道闸：录制键在没有设备时画成不可用，但快捷键/脚本仍可能打进来 ——
    //! 那种情况下"开始录"只会录到一个空 take，不如当场说清楚。
    if (!canRecord()) {
        LOGW() << "[midi-record] refused: no MIDI input device connected"
               << "(device=" << midiInputDeviceName() << ")";
        return;
    }

    startRecording(m_recordStaff);
}

void MidiEditorModel::setRecordStaff(int staffIndex)
{
    Score* score = currentScore();
    if (!score) {
        return;
    }

    //! 夹到合法范围（视图切谱表时会传过来；换了工程之后旧的选中可能已经不在了）。
    const int clamped = std::clamp(staffIndex, 0, int(score->nstaves()) - 1);
    if (clamped == m_recordStaff) {
        return;
    }

    m_recordStaff = clamped;
    emit recordChanged();
}

void MidiEditorModel::startRecording(int staffIndex)
{
    Score* score = currentScore();
    if (!score) {
        return;
    }

    const int staff = (staffIndex >= 0 && size_t(staffIndex) < score->nstaves()) ? staffIndex : 0;

    //! 记谱页停在音符输入模式时，同一串 MIDI 事件**也会**被写进谱面光标处
    //! （`NotationMidiInput::addNoteToScore()`）—— 那会在谱上凭空多出一份，而且不在你录的地方。
    //! 所以起录之前先把音符输入收掉。这是**保护**，不是顺手改用户的状态（所以留一条日志）。
    if (INotationPtr notation = context()->currentNotation()) {
        if (INotationInteractionPtr interaction = notation->interaction()) {
            INotationNoteInputPtr noteInput = interaction->noteInput();
            if (noteInput && noteInput->isNoteInputMode()) {
                LOGW() << "[midi-record] note input mode was on - ending it so the take is not also written at the score cursor";
                noteInput->endNoteInput(false);
            }
        }
    }

    m_recordStaff = staff;
    m_recorder.start();
    m_recordedNotes.clear();
    m_lastOverlayMs = 0;
    m_lastRecordedCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_midiMutex);
        m_midiQueue.clear();
    }

    m_isRecording = true;
    m_recordSawPlaying = false;

    //! 参考时钟**立刻**初始化：第一个音可能比第一帧播放位置还早，那时用 0 会让它落在曲子开头。
    //! `running = false` 是刻意的 —— 还没开始走带时事件不该被外推。
    m_recordPosSecs.store(double(context()->playbackState()->playbackPosition()));
    m_recordPosWallMs.store(nowMs());
    m_recordPosRunning.store(false);

    if (m_midiProcessTimer) {
        m_midiProcessTimer->start(MIDI_PROCESS_INTERVAL_MS);
    }

    emit recordChanged();
    emit recordedNotesChanged();

    LOGW() << "[midi-record] start: staff=" << staff << "quantize=" << quantizeGridTicksAt(m_quantizeGridIndex)
           << "ticks, strength=" << m_quantizeStrength << "%, device=" << midiInputDeviceName();

    //! 录制 = **从播放头开始播**：这才谈得上"跟着伴奏弹"。
    //! `showErrors = false`：起播失败就是没声音，不值得再弹一个对话框把人拦住。
    playbackController()->play(false);
}

void MidiEditorModel::finishRecording(bool commit)
{
    if (!m_isRecording) {
        return;
    }

    //! 先落旗：下面 `stop()` 会回调播放状态，而那个回调里也有"停止录制"的判断 ——
    //! 不先落旗就会递归进来一次。
    m_isRecording = false;

    //! 收尾：队列里还没处理的音**必须**处理掉。用户按下停止的手比 10ms 的定时器快是很正常的，
    //! 少了这一句就是"最后几个音不见了"（而且是静默的）。
    processMidiEvents();

    //! 还按着的音按"停止这一刻"封口（手还按着就按停止时，长度是 0 而不是负数）。
    m_recorder.stop(recordTickAt(nowMs()));

    if (m_midiProcessTimer) {
        m_midiProcessTimer->stop();
    }

    if (playbackController()->isPlaying()) {
        playbackController()->stop();
    }

    const int recorded = m_recorder.noteCount();

    if (!commit || recorded == 0) {
        LOGW() << "[midi-record] stop: recorded=" << recorded << "commit=" << commit << "(nothing written)";
        m_recorder.clear();
        updateRecordedNotes(true);
        emit recordChanged();
        return;
    }

    const MidiQuantizeSettings settings = quantizeSettings();
    const std::vector<MidiQuantizedNote> notes = quantizeRecordedNotes(m_recorder.notes(), settings);
    const std::vector<MidiRecordedChord> chords = buildRecordedChords(notes, int(VOICES));

    //! 整场演奏**一个事务**：与这一页其它编辑同一条规矩（`INotationUndoStack::transaction()`，
    //! 写入函数一律 `openCommand = false`）。一次 Ctrl+Z 撤销整次录制，而不是一个音一个音地撤。
    MidiRecordedWriteResult written;
    mutateOnce([this, &chords, &written]() {
        if (INotationPtr notation = context()->currentNotation()) {
            notation->undoStack()->transaction(TranslatableString("midieditor", "Record MIDI"),
                                               [this, &chords, &written](engraving::Transaction&) {
                written = applyRecordedChords(currentScore(), m_recordStaff, 0, chords, /*openCommand*/ false);
            });
        }
    });

    m_lastTakeSummary = muse::qtrc("notationscene", "Recorded %1 notes, wrote %2, skipped %3")
                        .arg(recorded).arg(written.notesWritten).arg(written.chordsSkipped);

    //! ⚠️ 日志用 `console.warn` 那一族（`LOGW`）：`console.log` 进不了日志文件
    //! （见 `维护手册.md` §7.1）。这一行是"录进去了没有"最直接的证据。
    LOGW() << "[midi-record] stop: recorded=" << recorded
           << "chords=" << chords.size()
           << "written=" << written.chordsWritten << "notes=" << written.notesWritten
           << "skipped=" << written.chordsSkipped
           << "lastTick=" << written.lastTick;

    m_recorder.clear();
    updateRecordedNotes(true);
    emit recordChanged();
}

void MidiEditorModel::cancelRecording()
{
    finishRecording(false);
}

//! 验证钩子（**有意保留**，见 `维护手册.md` §7.6）：这一页的录制入口要么需要一台 MIDI 键盘、
//! 要么需要"左手按播放右手弹"，两者在自动化环境里都做不到 —— 于是"录制这条链路通不通"
//! 没法用脚本量。设了 `MUSE_MIDIEDITOR_DEMO_RECORD=1` 时，模型在工程打开后**自己**喂进一小段
//! 演奏：事件走的是与真实端口**同一条**采集路径（`onMidiPortEvent` → 队列 → 主线程 →
//! `MidiRecorder`），时刻是**真墙钟**，提交也走真事务。所以日志里的 tick 与 PNG 里的音符
//! 都是这条链路的真实产物。不设这个环境变量时一行都不会执行。
void MidiEditorModel::applyDemoRecordingIfPending()
{
    if (!m_demoRecordPending || !m_hasScore || !playbackController()->isPlaybackInited()) {
        return;
    }

    m_demoRecordPending = false;

    //! 起播前的等待（`MUSE_MIDIEDITOR_DEMO_RECORD_DELAY` 毫秒，默认 0）。
    //! 存在的理由：**"录制中"这一帧要能拍到**。这一页的视图只在页面可见时才创建
    //! （`DockPage` 的 central 是 Loader），而脚本切到 MIDI 页要用掉几秒 —— 不留这段时间，
    //! 拍到的一定是"已经写完"的状态，看不到实时预览那一层。
    const int delayMs = qEnvironmentVariableIntValue("MUSE_MIDIEDITOR_DEMO_RECORD_DELAY");
    if (delayMs > 0) {
        QTimer::singleShot(delayMs, this, [this]() { runDemoRecording(); });
        return;
    }

    runDemoRecording();
}

void MidiEditorModel::runDemoRecording()
{
    if (!m_hasScore || m_isRecording) {
        return;
    }

    seekTick(0);
    startRecording(0);

    struct Step {
        int delayMs;
        int note;
        int velocity;   //!< 0 = 抬起
    };

    //! C4 E4 G4 C5：每个音按住 400ms、间隔 50ms —— 无论速度是多少，量化之后都该落在格子上。
    static const std::vector<Step> steps {
        { 200, 60, 90 }, { 600, 60, 0 },
        { 650, 64, 80 }, { 1050, 64, 0 },
        { 1100, 67, 70 }, { 1500, 67, 0 },
        { 1550, 72, 100 }, { 2150, 72, 0 },
    };

    for (const Step& step : steps) {
        QTimer::singleShot(step.delayMs, this, [this, step]() {
            if (!m_isRecording) {
                return;
            }

            onMidiPortEvent(int(step.velocity > 0 ? muse::midi::Event::Opcode::NoteOn : muse::midi::Event::Opcode::NoteOff),
                            step.note, step.velocity);
        });
    }

    QTimer::singleShot(2600, this, [this]() { finishRecording(true); });
}
