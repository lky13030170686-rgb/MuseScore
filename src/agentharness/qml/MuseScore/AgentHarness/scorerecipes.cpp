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
#include "scorerecipes.h"

#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/editing/editnote.h"

#include "addressing.h"
#include "notelocator.h"
#include "scoredigest.h"

#include "log.h"

using namespace muse::agentharness;

namespace {
//! Reject a MIDI pitch that cannot exist. Doing this here rather than trusting the caller means the
//! failure is a sentence about the input instead of a corrupt note.
bool pitchInRange(int midiPitch)
{
    return midiPitch >= 0 && midiPitch <= 127;
}

//! The spelling to give a note of `pitch`, chosen by the staff's key at the note's position.
//!
//! ⛔ WHY THE KEY MATTERS, and why this is not just "pitch % 12": the same MIDI number can be written
//! as C# or Db, and which one is right depends on the key signature. `pitch2tpc` is upstream's own
//! answer to that question; computing it here would be a second opinion that disagrees in exactly the
//! keys where it matters. It is the same reasoning as asking `TimeSigFrac::beatTicks()` for compound
//! metres instead of multiplying.
int spellingFor(mu::engraving::Note* note, int pitch)
{
    if (!note || !note->chord() || !note->staff()) {
        return mu::engraving::pitch2tpc(pitch, mu::engraving::Key::C, mu::engraving::Prefer::NEAREST);
    }

    const mu::engraving::Key key = note->staff()->concertKey(note->chord()->tick());
    return mu::engraving::pitch2tpc(pitch, key, mu::engraving::Prefer::NEAREST);
}
} // namespace

RecipeResult muse::agentharness::setNotePitch(mu::engraving::Score* score, const ScoreAddress& address,
                                              int voice, int noteIndex, int midiPitch)
{
    //! ⚠️ The entry trace is here rather than in the caller because an EMPTY `problem` is otherwise
    //! indistinguishable from "the recipe never ran": the controller reports `outcome.problem`, and a
    //! default-constructed `RecipeResult` has an empty one. That cost a round - the log said
    //! `recipe "Set pitch" FAILED revision=1 ""` with nothing to go on.
    LOGW() << "[agent-recipe] setNotePitch m" << address.measure << "b" << address.beat
           << "staff" << address.staff << "voice" << voice << "note" << noteIndex
           << "pitch" << midiPitch;

    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (!pitchInRange(midiPitch)) {
        return RecipeResult::failure(QStringLiteral("%1 is not a MIDI pitch; it must be 0-127 "
                                                    "(60 is middle C)").arg(midiPitch));
    }

    //! ⚠️ Traced on BOTH sides of the call. `chordAt` was traced and succeeded while `noteAt`'s own
    //! trace never appeared, which leaves only "the call did not happen" or "it returned without
    //! reaching its trace" - and those need different fixes, so guessing between them wastes rounds.
    LOGW() << "[agent-recipe] calling noteAt";
    const NoteLookup found = noteAt(score, address, voice, noteIndex);
    LOGW() << "[agent-recipe] noteAt returned ok=" << found.ok()
           << "note=" << (found.note != nullptr)
           << "problem=\"" << found.problem << "\"";
    if (!found.ok()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the note lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::Note* note = found.note;
    const int oldPitch = note->pitch();

    if (oldPitch == midiPitch) {
        //! Not an error: the note is already what was asked for. Reported as success with a detail that
        //! says so, because a model told "failed" here would try something else for no reason.
        return RecipeResult::success(QStringLiteral("the note at %1 is already %2")
                                     .arg(formatAddress(address), pitchName(midiPitch)));
    }

    const int tpc = spellingFor(note, midiPitch);

    //! ⛔ The recipe, not a direct `note->setPitch()`. `EditNote::undoChangePitch` pushes a
    //! `ChangePitch` for the note AND every note it is linked to (the same note in other parts), which
    //! is what keeps a score with parts consistent. Setting the pitch directly would edit one part and
    //! silently desynchronise the others.
    mu::engraving::EditNote::undoChangePitch(score, note, midiPitch, tpc, tpc);

    return RecipeResult::success(QStringLiteral("%1: %2 -> %3")
                                 .arg(formatAddress(address), pitchName(oldPitch), pitchName(midiPitch)));
}

RecipeResult muse::agentharness::transposeNote(mu::engraving::Score* score, const ScoreAddress& address,
                                               int voice, int noteIndex, int semitones)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (semitones == 0) {
        //! Refused rather than treated as a no-op. A caller that asks to transpose by nothing has
        //! misunderstood something, and a silent success would let it carry on believing that.
        return RecipeResult::failure(QStringLiteral("transposing by 0 semitones would change nothing; "
                                                    "say how far to move the note"));
    }

    const NoteLookup found = noteAt(score, address, voice, noteIndex);
    if (!found.ok()) {
        return RecipeResult::failure(found.problem);
    }

    mu::engraving::Note* note = found.note;
    const int oldPitch = note->pitch();
    const int newPitch = oldPitch + semitones;

    if (!pitchInRange(newPitch)) {
        //! Out of range is reported with both numbers, so the caller can see how far it overshot
        //! instead of guessing.
        return RecipeResult::failure(QStringLiteral("moving %1 by %2 semitones would give %3, which is "
                                                    "outside the MIDI range 0-127")
                                     .arg(pitchName(oldPitch)).arg(semitones).arg(newPitch));
    }

    const int tpc = spellingFor(note, newPitch);
    mu::engraving::EditNote::undoChangePitch(score, note, newPitch, tpc, tpc);

    return RecipeResult::success(QStringLiteral("%1: %2 -> %3 (%4%5 semitones)")
                                 .arg(formatAddress(address), pitchName(oldPitch), pitchName(newPitch),
                                      semitones > 0 ? QStringLiteral("+") : QString())
                                 .arg(semitones));
}
