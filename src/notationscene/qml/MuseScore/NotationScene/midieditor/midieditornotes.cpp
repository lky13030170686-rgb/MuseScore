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

#include "midieditornotes.h"

#include <algorithm>

#include "translation.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/editing/editnote.h"

using namespace mu::engraving;
using namespace muse;

namespace mu::notation {
//! NOTE: Same meaning as in the Properties panel (see NotePlaybackModel): 0 means "the user never
//!       set a velocity", and is shown as 64 so that the lane does not look empty.
static constexpr int DEFAULT_VELOCITY = 64;

int midiDisplayVelocity(int userVelocity)
{
    return userVelocity == 0 ? DEFAULT_VELOCITY : userVelocity;
}

std::vector<MidiNoteItem> collectMidiNotes(const Score* score)
{
    std::vector<MidiNoteItem> result;
    if (!score) {
        return result;
    }

    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (const Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            for (track_idx_t track = 0; track < score->ntracks(); ++track) {
                EngravingItem* item = segment->element(track);
                if (!item || !item->isChord()) {
                    continue;
                }

                const Chord* chord = toChord(item);
                if (chord->isGrace()) {
                    //! NOTE: grace notes have no independent time position in a roll.
                    continue;
                }

                const int durationTicks = chord->actualTicks().ticks();
                if (durationTicks <= 0) {
                    continue;
                }

                for (const Note* note : chord->notes()) {
                    MidiNoteItem entry;
                    entry.note = const_cast<Note*>(note);
                    entry.tick = note->tick().ticks();
                    entry.durationTicks = durationTicks;
                    entry.pitch = note->pitch();
                    entry.velocity = midiDisplayVelocity(note->userVelocity());
                    entry.staffIndex = int(note->staffIdx());
                    entry.voice = int(note->voice());

                    result.push_back(entry);
                }
            }
        }
    }

    return result;
}

std::vector<MidiMeasureItem> collectMidiMeasures(const Score* score)
{
    std::vector<MidiMeasureItem> result;
    if (!score) {
        return result;
    }

    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        MidiMeasureItem item;
        item.tick = measure->tick().ticks();
        item.endTick = measure->endTick().ticks();
        result.push_back(item);
    }

    return result;
}

bool applyNotePitch(Score* score, Note* note, int pitch)
{
    if (!score || !note) {
        return false;
    }

    pitch = std::clamp(pitch, 0, 127);
    if (pitch == note->pitch()) {
        return false;
    }

    score->startCmd(TranslatableString("midieditor", "Change pitch"));
    EditNote::undoChangePitch(score, note, pitch, note->tpc1default(pitch), note->tpc2default(pitch));
    score->endCmd();

    return true;
}

bool applyNoteVelocity(Score* score, Note* note, int velocity)
{
    if (!score || !note) {
        return false;
    }

    velocity = std::clamp(velocity, 1, 127);
    if (velocity == note->userVelocity()) {
        return false;
    }

    score->startCmd(TranslatableString("midieditor", "Change velocity"));
    //! NOTE: the same property the Properties panel writes (Pid::USER_VELOCITY), so the two stay
    //!       interchangeable. The propertyFlags handling is copied from
    //!       PropertiesPanelAbstractModel::setPropertyValue.
    PropertyFlags flags = note->propertyFlags(Pid::USER_VELOCITY);
    if (flags == PropertyFlags::STYLED) {
        flags = PropertyFlags::UNSTYLED;
    }
    note->undoChangeProperty(Pid::USER_VELOCITY, PropertyValue(velocity), flags);
    score->endCmd();

    return true;
}
}
