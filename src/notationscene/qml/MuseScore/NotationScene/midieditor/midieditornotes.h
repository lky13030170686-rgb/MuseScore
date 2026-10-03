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

#include <utility>
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

    //! Whether this note carries its OWN velocity (Properties `Pid::USER_VELOCITY`).
    //!
    //! This is what makes "tweak one note" possible without touching the engine's driving logic: an
    //! own velocity becomes `ExpressionContext::velocityOverride`, which the synthesisers prefer over
    //! the dynamic level. A note WITHOUT one keeps following the dynamic marks (pp/ff, hairpins).
    //! So this flag is exactly "this note no longer follows the dynamics".
    bool hasVelocityOverride = false;

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
//!
//! `velocity` may be 0, which means "no own velocity" - the note goes back to following the dynamic
//! marks of the score. That is the only way to undo a per-note tweak, so it is allowed on purpose.
bool applyNoteVelocity(engraving::Score* score, engraving::Note* note, int velocity);

//! One velocity write for each pair, all under a SINGLE command.
//!
//! This is what a brush stroke needs, and the difference is not cosmetic. Every `startCmd`/`endCmd`
//! pair notifies the whole score, and the subscribers to that notification rebuild things that cost
//! O(score) - the notation view repaints, the playback events are rebuilt. Opening one command per
//! note therefore made a stroke over N notes cost N full-score rebuilds, which is exactly why
//! drawing more notes took proportionally longer. One command means one notification.
//!
//! A pair whose note already has that velocity is skipped, so the count returned may be smaller than
//! the number of pairs. Returns how many notes were actually changed.
int applyNoteVelocities(engraving::Score* score, const std::vector<std::pair<engraving::Note*, int> >& changes);

//! Writes the "played" timing of a note - the piano roll's played layer, i.e. `ontime` and `len` of
//! the first `NoteEvent`. `startTick` and `durationTicks` are absolute ticks; `velocityPercent` is
//! the velocity multiplier in percent (100 = untouched). Uses the existing `ChangeNoteEventList`
//! undo command, so it is undoable and does not invent a second data model.
//! Returns false when nothing changes.
bool applyNotePlayOverride(engraving::Score* score, engraving::Note* note,
                           int startTick, int durationTicks, int velocityPercent);

//! One point of the Dynamics automation curve of a staff: a tick, and a level in 0..1.
struct MidiAutomationPoint {
    int tick = 0;
    double value = 0.0;

    //! True when the point is the user's own, i.e. the lane may remove it again. False when the score
    //! derived it from an engraving item - a Dynamic mark, a hairpin - because those are regenerated on
    //! every rebuild, so removing one is not the lane's to do.
    //! Only meaningful when READ back; a write always writes an authored point.
    bool authored = true;
};

//! The Dynamics automation curve of one staff, in tick order - what a crescendo, a diminuendo or an
//! fp really is.
//!
//! NOTE: this is not a second curve of our own. It is the one the notation page draws next to the
//! mixer, addressed with the same key (`AutomationCurveKey::staff(Dynamics, staff->id())`), and the one
//! `MuseSamplerSequencer::loadDynamicEvents()` plays - so both pages edit one thing rather than two.
//!
//! Deliberately a free function taking a plain `Score*`, for the same reason `collectMidiNotes` is one:
//! it needs no IoC context, so "both pages address the same curve" is pinned by a unit test instead of
//! by a comment.
std::vector<MidiAutomationPoint> collectAutomationPoints(const engraving::Score* score, int staffIndex);

//! Writes points of that curve as ONE undoable command - one notification for the whole stroke, for the
//! reason spelled out at `applyNoteVelocities`.
//!
//! A point that already holds that value is left alone; when nothing at all changes, nothing is written,
//! because a stroke that changes nothing must not cost an undo step either. Returns how many points were
//! written.
int applyAutomationPoints(engraving::Score* score, int staffIndex, const std::vector<MidiAutomationPoint>& points);

//! Removes the point at `tick`, if it is the user's own.
//!
//! A point the score derived from a Dynamic mark or a hairpin is NOT removable: the score regenerates it
//! on the next rebuild, so erasing it here would look like a dead gesture - and would take the mark's own
//! shape away until that rebuild. The notation page's lane refuses exactly the same points
//! (NotationAutomationController::requestRemovePoint), and the two pages must not disagree about what is
//! the user's to delete. Returns false when nothing was removed.
bool eraseAutomationPoint(engraving::Score* score, int staffIndex, int tick);

//! The default velocity shown for a note the user has never given an explicit velocity.
int midiDisplayVelocity(int userVelocity);
}
