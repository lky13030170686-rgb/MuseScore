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

namespace mu::engraving {
class Score;
class Note;
class Chord;
}

namespace muse::agentharness {
struct ScoreAddress;

//! Finding the thing to edit, and saying so when it is not there.
//!
//! WHY THIS IS A SEPARATE LAYER FROM THE COMMAND DISPATCH: the command layer addresses actions by URI
//! and takes whatever parameters a command declares - it has no notion of "the note at m3 b2". Note
//! editing needs to resolve a musical position to an object first, and that resolution is where all
//! the failure modes live (no such measure, no such beat, an empty beat, several notes on the beat).
//! Keeping it in one file means the failure modes are enumerated in one place instead of being
//! rediscovered by every recipe.
//!
//! ⛔ EVERYTHING HERE IS A POINTER INTO THE SCORE and is valid only until the next transaction. Do not
//! store the result of `noteAt` across a write - that is the "never cache an EngravingObject*"
//! invariant, and a stale `Note*` is how a write lands on the wrong note after an undo.

//! Why a lookup failed, in terms the model can act on.
struct NoteLookup
{
    mu::engraving::Note* note = nullptr;
    mu::engraving::Chord* chord = nullptr;
    //! Empty when the lookup succeeded.
    QString problem;

    //! Whether a NOTE was found. `chordAt` legitimately returns with only `chord` set, so this is not
    //! the right question to ask about its result.
    bool ok() const { return note != nullptr; }

    //! Whether the lookup itself succeeded - a chord for `chordAt`, a note for `noteAt`.
    //!
    //! ⛔ THE TWO ARE NOT THE SAME, and conflating them cost several rebuilds: `noteAt` began with
    //! `if (!result.ok()) return result;` on `chordAt`'s result, and `ok()` asks for a NOTE - which
    //! `chordAt` never sets. So every lookup bailed out immediately, returning a default-constructed
    //! `problem`, and the caller reported an empty failure. The symptom (a failure with no message) was
    //! indistinguishable from "the recipe never ran", which is what made it expensive.
    bool found() const { return note != nullptr || chord != nullptr; }
};

//! The note at an address.
//!
//! @param voice 1-based voice within the staff; 0 means "the first voice that has a note on this beat"
//! @param index 0-based index within the chord, for a beat that holds several notes (a chord or a
//!              multi-note tremolo). Negative means "the only note", and is an ERROR when the chord
//!              holds more than one - picking one arbitrarily would edit a note the caller did not
//!              name, which is the failure this whole struct exists to make impossible.
NoteLookup noteAt(mu::engraving::Score* score, const ScoreAddress& address, int voice, int index);

//! The chord at an address, without needing a note to be present. Used by recipes that create notes.
NoteLookup chordAt(mu::engraving::Score* score, const ScoreAddress& address, int voice);

//! The first note at or after `from`, on the same track, or null.
//!
//! WHY THIS IS HERE AND NOT "ask for the next address and look it up": a tie goes to the next NOTE, and
//! the next note is not necessarily on the next beat - it may be several beats away (the rest of the
//! bar is rests), in the next measure, or several measures away. Computing that address in the caller
//! would mean every caller re-deriving "where is the next thing that sounds", and getting it wrong
//! would tie to a rest or to nothing.
//!
//! @param includeSelf when true, `from` itself is a candidate. Used by tie recipes that want to know
//!                    whether a note is already the last one.
NoteLookup nextNote(mu::engraving::Score* score, mu::engraving::Note* from, bool includeSelf = false);

} // namespace muse::agentharness
