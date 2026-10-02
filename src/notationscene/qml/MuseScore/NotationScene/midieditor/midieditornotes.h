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

#include <vector>

namespace mu::engraving {
class Score;
class Note;
}

namespace mu::notation {
//! NOTE: One rectangle of the piano roll. It keeps the `Note*` it came from, because that is what
//!       the edits are applied to.
struct MidiNoteItem {
    engraving::Note* note = nullptr;

    int tick = 0;
    int durationTicks = 0;
    int pitch = 0;
    int velocity = 0;       //!< as shown in the roll: an unset velocity (0) is shown as 64
    int staffIndex = 0;
    int voice = 0;

    //! The "played" layer (`Note::playEvents()`): where the note actually sounds and for how long.
    //! When `hasPlayOverride` is false the played values equal the notated ones and the roll draws a
    //! plain block. Dorico makes the same distinction between played and notated durations.
    bool hasPlayOverride = false;
    int playTick = 0;
    int playDurationTicks = 0;
    int playVelocityPercent = 100;      //!< velocityMultiplier in percent; 100 means untouched
};

//! The span of one measure, used for the bar lines and the measure numbers of the ruler.
struct MidiMeasureItem {
    int tick = 0;
    int endTick = 0;
};

//! Flattens a score into the note rectangles the roll draws.
//!
//! Deliberately a free function that takes a plain `Score*`: it needs no IoC context, so it can be
//! unit tested against a real score (which is how "the MIDI page and the notation page share one
//! data source" is pinned down).
std::vector<MidiNoteItem> collectMidiNotes(const engraving::Score* score);

//! The measure spans of a score, in order.
std::vector<MidiMeasureItem> collectMidiMeasures(const engraving::Score* score);

//! Writes a pitch back into the score, reusing the very command the notation editor uses, so the
//! linked notes stay in sync and undo keeps working. No-op (returns false) when nothing changes.
bool applyNotePitch(engraving::Score* score, engraving::Note* note, int pitch);

//! Writes a velocity back into the score, through the same property the Properties panel writes.
bool applyNoteVelocity(engraving::Score* score, engraving::Note* note, int velocity);

//! Writes the "played" timing of a note - the piano roll's played layer, i.e. `ontime` and `len` of
//! the first `NoteEvent`. `startTick` and `durationTicks` are absolute ticks; `velocityPercent` is
//! the velocity multiplier in percent (100 = untouched). Uses the existing `ChangeNoteEventList`
//! undo command, so it is undoable and does not invent a second data model.
//! Returns false when nothing changes.
bool applyNotePlayOverride(engraving::Score* score, engraving::Note* note,
                           int startTick, int durationTicks, int velocityPercent);

//! The default velocity shown for a note the user has never given an explicit velocity.
int midiDisplayVelocity(int userVelocity);
}
