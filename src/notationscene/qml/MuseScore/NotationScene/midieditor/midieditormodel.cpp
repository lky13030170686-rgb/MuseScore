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
    if (!applyNotePitch(currentScore(), noteAt(row), pitch)) {
        return;
    }

    reload();
}

void MidiEditorModel::setNoteVelocity(int row, int velocity)
{
    if (!applyNoteVelocity(currentScore(), noteAt(row), velocity)) {
        return;
    }

    reload();
}

void MidiEditorModel::setNotePlayOverride(int row, int startTick, int durationTicks, int velocityPercent)
{
    if (!applyNotePlayOverride(currentScore(), noteAt(row), startTick, durationTicks, velocityPercent)) {
        return;
    }

    reload();
}
