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

#include "engraving/dom/measure.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"

#include "context/iglobalcontext.h"
#include "context/iplaybackstate.h"

#include "notation/imasternotation.h" // IWYU pragma: keep
#include "notation/inotation.h"
#include "notation/inotationelements.h" // IWYU pragma: keep
#include "notation/inotationplayback.h"
#include "notation/inotationsolomutestate.h"
#include "notation/inotationundostack.h"

using namespace mu::engraving;
using namespace mu::notation;
using namespace muse;

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
        connectToCurrentScore();
        reload();
    });

    context()->playbackState()->playbackPositionChanged().onReceive(this, [this](muse::audio::secs_t) {
        updatePlaybackState();
    });

    context()->playbackState()->playbackStatusChanged().onReceive(this, [this](muse::audio::PlaybackStatus) {
        updatePlaybackState();
    });

    //! 验证钩子：等播放真的初始化好再动手 —— 播放器是播放初始化时才建出来的，太早 seek 会在
    //! 上游的 `IF_ASSERT_FAILED(currentPlayer())` 上留一条断言（看起来像回归，其实只是太早）。
    m_demoPlaybackPending = qEnvironmentVariableIsSet("MUSE_MIDIEDITOR_DEMO_PLAYBACK");
    playbackController()->playbackInitedChanged().onReceive(this, [this](bool inited) {
        if (inited) {
            applyDemoPlaybackIfPending();
        }
    });

    connectToCurrentScore();
    reload();

    applyDemoPlaybackIfPending();
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
