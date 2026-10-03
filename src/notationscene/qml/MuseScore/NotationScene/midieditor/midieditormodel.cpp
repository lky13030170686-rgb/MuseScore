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

    connectToCurrentScore();
    reload();
}

void MidiEditorModel::connectToCurrentScore()
{
    disconnectFromCurrentScore();

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
}

void MidiEditorModel::disconnectFromCurrentScore()
{
    if (m_notation) {
        Score* score = m_notation->elements()->msScore();
        if (score) {
            score->changesChannel().disconnect(this);
        }
    }

    m_notation = nullptr;
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

void MidiEditorModel::setNotePitch(int row, int pitch)
{
    mutateOnce([this, row, pitch]() {
        applyNotePitch(currentScore(), noteAt(row), pitch);
    });
}

void MidiEditorModel::setNoteVelocity(int row, int velocity)
{
    mutateOnce([this, row, velocity]() {
        applyNoteVelocity(currentScore(), noteAt(row), velocity);
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
    const int changed = applyNoteVelocities(score, changes);
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
        result << item;
    }

    return result;
}

void MidiEditorModel::setAutomationPoint(int staffIndex, int tick, double value)
{
    const std::vector<MidiAutomationPoint> points { MidiAutomationPoint { tick, value } };

    //! NOTE: write through the engraving model, rebuild once - see mutateOnce.
    mutateOnce([this, staffIndex, points]() {
        applyAutomationPoints(currentScore(), staffIndex, points);
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
        applyAutomationPoints(currentScore(), staffIndex, drawn);
    });
}

void MidiEditorModel::removeAutomationPoint(int staffIndex, int tick)
{
    mutateOnce([this, staffIndex, tick]() {
        eraseAutomationPoint(currentScore(), staffIndex, tick);
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
        applyNotePlayOverride(currentScore(), noteAt(row), startTick, durationTicks, velocityPercent);
    });
}
