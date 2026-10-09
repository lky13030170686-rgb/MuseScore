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

#include <QString>
#include <QStringList>

namespace mu::engraving {
class Score;
}

namespace muse::agentharness {
struct ScoreAddress;

//! The outcome of one recipe. `problem` is empty on success.
//!
//! ⛔ A RECIPE REPORTS, IT DOES NOT THROW OR ASSERT. Every one of these can fail for reasons the
//! caller can act on (no note there, a pitch outside the staff, an unspellable pitch), and the message
//! is what the model reads. A crash or a silent no-op would both be worse than a sentence.
struct RecipeResult
{
    bool ok = false;
    QString problem;
    //! What actually happened, for the model to confirm against. Empty when nothing changed.
    QString detail;

    static RecipeResult success(const QString& detail)
    {
        RecipeResult r;
        r.ok = true;
        r.detail = detail;
        return r;
    }

    static RecipeResult failure(const QString& problem)
    {
        RecipeResult r;
        r.ok = false;
        r.problem = problem;
        return r;
    }
};

//! ── Note recipes ──────────────────────────────────────────────────────────────────────────────
//! These are the writes the user can make to a note, expressed as operations on an addressed note
//! rather than as command URIs. WHY BOTH EXIST: the command layer is right when the action is "what
//! the user does" (insert a measure, toggle a rest) and the note is implied by the selection. It is
//! the wrong shape when the action names a note - there is no command URI for "set the note at m3 b2
//! to C#5", and inventing one per recipe would be a table of near-duplicates.
//!
//! ⛔ ALL OF THESE MUST RUN INSIDE A TRANSACTION. They push `UndoableCommand`s; outside a transaction
//! `currentOrDummyTransaction()` hands back a dummy that discards them, so the change would appear and
//! then vanish - the "it worked and nothing happened" shape this project keeps meeting.

//! Set one note's pitch (MIDI number, 0-127). The spelling (tpc) is derived from the staff's key at
//! that position, which is what a musician means by "make it a C#" - not "make it pitch 61 with the
//! spelling I chose".
RecipeResult setNotePitch(mu::engraving::Score* score, const ScoreAddress& address, int voice, int noteIndex,
                          int midiPitch);

//! Move one note by `semitones`, keeping its spelling where the key allows. 0 is refused rather than
//! treated as a no-op: a model that asks to transpose by nothing has misunderstood something, and
//! saying so is more useful than a silent success.
RecipeResult transposeNote(mu::engraving::Score* score, const ScoreAddress& address, int voice, int noteIndex,
                           int semitones);

//! ── Chord recipes ─────────────────────────────────────────────────────────────────────────────
//! These address the whole chord at a position rather than one note, which is what "change this beat's
//! duration" means: a chord's notes all sound for the same length.

//! Set the duration of the chord at an address.
//!
//! @param duration one of the names a musician uses: `whole`, `half`, `quarter`, `eighth`, `16th`,
//!                 `32nd`, `64th`, `breve`, `long`, `measure` (a full-measure rest/chord), optionally
//!                 with dots - `dotted-quarter`, `quarter.`, `double-dotted-half`. Parsed HERE rather
//!                 than taking a raw fraction, because a model that has to compute a tick length will
//!                 eventually compute the wrong one and the error will look like a wrong duration
//!                 rather than a wrong conversion.
RecipeResult setChordDuration(mu::engraving::Score* score, const ScoreAddress& address, int voice,
                              const QString& duration);

//! Remove one note from the chord at an address. Refused when it is the chord's LAST note: a chord with
//! no notes is not a rest, it is a broken chord, and the caller almost certainly meant "make this beat
//! a rest" - which is a different operation with a different command.
RecipeResult removeNote(mu::engraving::Score* score, const ScoreAddress& address, int voice, int noteIndex);

//! Add a note to the chord at an address, making it a chord of several notes.
//!
//! ⛔ REFUSED WHEN THE PITCH IS ALREADY THERE. Adding a duplicate pitch to a chord is not a chord - it
//! is the same note twice, which upstream will happily create and which then draws as one notehead and
//! confuses every later read of the chord. The refusal names the existing note so the caller can tell
//! "already done" from "wrong octave".
RecipeResult addNoteToChord(mu::engraving::Score* score, const ScoreAddress& address, int voice,
                            int midiPitch);

//! The duration names `setChordDuration` accepts, for the tool's error message and its schema.
QStringList durationNames();

} // namespace muse::agentharness
