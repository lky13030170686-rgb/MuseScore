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

//! Make the chord at an address have exactly these pitches - e.g. turn a single note into a C major
//! triad in one call.
//!
//! WHY THIS IS NOT JUST `addNoteToChord` CALLED REPEATEDLY: the interesting case is REPLACING a chord,
//! where some of the old notes must go and some new ones must arrive. Doing that as a sequence of adds
//! and removes from outside means the caller has to work out the order, and the obvious order
//! (remove first) can leave the chord EMPTY in the middle - which `removeNote` refuses, for good reason.
//! Doing it here means the invariant "never empty in the middle" is maintained in one place.
//!
//! ⛔ REFUSED WHEN THE LIST IS EMPTY or holds a duplicate. An empty list is "silence this beat", which is
//! `changeToRest`; a duplicate is the same note twice, which is not a chord.
RecipeResult setChordPitches(mu::engraving::Score* score, const ScoreAddress& address, int voice,
                             const QVector<int>& midiPitches);

//! ── Tie recipes ───────────────────────────────────────────────────────────────────────────────
//! A tie joins a note to the next note OF THE SAME PITCH, which is what makes it different from a slur
//! (a slur joins different pitches). Both of these act on the note at an address; the second note is
//! found by walking forward, not by asking the caller for another address.

//! Tie the note at an address to the next note of the same pitch.
//!
//! ⛔ REFUSED when the note is already tied forward. Tying an already-tied note would either replace the
//! existing tie (silently redirecting it) or create a second one, and both are wrong in ways the caller
//! cannot see from the result. The message says the note is already tied, which is also the honest
//! answer to "did my earlier call work".
RecipeResult addTie(mu::engraving::Score* score, const ScoreAddress& address, int voice, int noteIndex);

//! Remove the tie starting at the note at an address. Refused when there is none, rather than reported
//! as a success - "nothing to do" and "done" are different answers.
RecipeResult removeTie(mu::engraving::Score* score, const ScoreAddress& address, int voice, int noteIndex);

//! Add a tie if there is none, remove it if there is. The one operation that always leaves the score in
//! the state the caller asked for, whichever it was.
RecipeResult toggleTie(mu::engraving::Score* score, const ScoreAddress& address, int voice, int noteIndex);

//! ── Slur recipes ──────────────────────────────────────────────────────────────────────────────
//! A SLUR IS NOT A TIE, and the difference is the whole reason both exist:
//!   - a tie joins two notes OF THE SAME PITCH and changes how they SOUND (one longer note);
//!   - a slur joins any two notes and changes how they are PLAYED (legato).
//! So a slur may connect different pitches - which is exactly the case `addTie` refuses - and the two
//! operations must not be confused. `note_tie` says so in its own description, and the slur tool says
//! it again from the other side, because a model that conflates them will produce a score that reads
//! correctly and sounds wrong.

//! Slur the note at an address to the next note.
//!
//! ⛔ REFUSED when a slur already starts there. Adding a second slur over the same pair is not a
//! thicker slur - it is two slurs drawn on top of each other, and the caller cannot see that from the
//! result.
RecipeResult addSlur(mu::engraving::Score* score, const ScoreAddress& address, int voice);

//! Remove the slur starting at the note at an address. Refused when there is none, rather than
//! reported as a success - "nothing to do" and "done" are different answers.
RecipeResult removeSlur(mu::engraving::Score* score, const ScoreAddress& address, int voice);

//! Add a slur if there is none, remove it if there is.
RecipeResult toggleSlur(mu::engraving::Score* score, const ScoreAddress& address, int voice);

//! ── Text recipes ──────────────────────────────────────────────────────────────────────────────
//! Text in a score comes in TWO KINDS, and which kind a given style is decides whether it needs a
//! position at all:
//!
//!   - **frame text** - title, subtitle, composer, lyricist. It belongs to the score, not to a beat, so
//!     there is no address to give.
//!   - **attached text** - rehearsal mark, system text, staff text, expression. It hangs off a specific
//!     chord or rest, so it needs one.
//!
//! ⛔⛔ THE CRASH THIS FUNCTION EXISTS TO PREVENT: `Score::addText` calls `chordOrRest(destination)` for
//! every attached style and then uses the result WITHOUT checking it. `chordOrRest` returns null for a
//! null destination, so `addText(REHEARSAL_MARK, nullptr)` dereferences null and takes the process down.
//! A model that names a rehearsal mark but forgets the measure reaches that in one call, so the check
//! has to be here.

//! Add text to the score. `address` is ignored for frame styles and REQUIRED for attached ones.
//!
//! @param style one of the names from `textStyleNames()`, e.g. `title`, `composer`, `rehearsal-mark`,
//!              `system`, `staff`, `expression`
RecipeResult addText(mu::engraving::Score* score, const ScoreAddress& address, const QString& style,
                     const QString& text);

//! Whether a style is attached to a position (and therefore needs an address).
//! @param known set to false when the name is not a style this build knows
bool textStyleNeedsAddress(const QString& style, bool& known);

//! The style names `addText` accepts, for the tool's error message and its schema.
QStringList textStyleNames();

//! ── Key and time signature ────────────────────────────────────────────────────────────────────
//! ⛔⛔ THESE HAVE NO COMMAND, and that is not an oversight in the harness - the notation command
//! registry simply has none. `command_list` enumerates every command that exists, and a caller looking
//! for "change the key signature" finds nothing there. So the recipe path is the ONLY path, which is why
//! the plan lists these three (key, time, barline) as the ones that cannot go through `command_dispatch`.

//! Set the key signature at a measure, as a number of sharps (positive) or flats (negative).
//!
//! @param fifths -7 (seven flats) to +7 (seven sharps); 0 is C major / A minor.
//!
//! ⚠️ The value is the number of sharps or flats, NOT a key name, because a key NAME is ambiguous in a
//! way this interface cannot resolve: "C" is C major to one caller and the note C to another, and a
//! caller that meant A minor would have to know that A minor is also zero accidentals. The count is
//! what the notation layer actually stores, so nothing is lost by asking for it directly.
//!
//! ⚠️ `EditKeySig::undoChangeKeySig` applies the change to every staff LINKED to the one given, which is
//! what a user expects from the key signature button - the linked staves show the same key.
RecipeResult setKeySignature(mu::engraving::Score* score, int measureNumber, int fifths);

//! Set the time signature at a measure, e.g. 3/4.
//!
//! ⚠️ `local = false`: the signature applies to the whole score from that measure on, which is what
//! "change the time signature" means. A local signature changes one staff only and is a different,
//! rarer operation - offering it as a flag would make the common case look like it had an option it
//! does not need.
RecipeResult setTimeSignature(mu::engraving::Score* score, int measureNumber, int numerator,
                              int denominator);

//! ── Transposing a passage ─────────────────────────────────────────────────────────────────────
//! ⛔ WHY THIS IS NOT `Transpose::transpose`, WHICH ALREADY EXISTS. That function reads the SCORE'S
//! SELECTION - it has no way to be told "measures 3 to 8" - so calling it from a tool would mean setting a
//! selection the user did not make, transposing, and putting it back. That is a side effect on shared state
//! for the duration of the call, and it would also move what the user had selected.
//!
//! ⚠️ What this does NOT do: transpose the KEY SIGNATURE. `Transpose::transposeKeys` needs a `Transaction&`
//! that a recipe has no way to obtain, and upstream itself does not transpose key signatures for an
//! interval that is a whole number of octaves. So a caller transposing a passage by a non-octave interval
//! gets notes moved and the key left alone - which is exactly what the tool's description says, because a
//! silent half-transposition is worse than a documented one.

//! Transpose every note in a measure range by a number of semitones.
//!
//! @param partIndex 0-based index into `Score::parts()`; only that part's staves are touched
RecipeResult transposeRange(mu::engraving::Score* score, int fromMeasure, int toMeasure, int partIndex,
                            int semitones);

//! ── Tempo ─────────────────────────────────────────────────────────────────────────────────────
//! ⛔ TEMPO IS NOT A NOTATION ELEMENT - it lives in `AutomationData` and changes through its OWN channel.
//! That makes this the one recipe whose edit the information field would not see if only
//! `Score::changesChannel()` were subscribed, which is why it is also the trigger that verifies the second
//! subscription works.

//! Set the tempo, in beats per minute, at a measure.
//!
//! ⚠️ The tempo is stored NORMALIZED - as a fraction of the score's own maximum tempo (`tempovalues.h`) -
//! because `AutomationPoint::outValue` is a plain `[0,1]` number with no unit. Writing a raw BPM there
//! would ask for a tempo hundreds of times too fast, and the score would still play.
RecipeResult setTempo(mu::engraving::Score* score, int measureNumber, double beatsPerMinute);

//! ── Filling a measure ─────────────────────────────────────────────────────────────────────────
//! The one operation whose job is to make a measure ADD UP. Everything else here changes what a bar
//! contains; this one changes whether it is complete.

//! Fill the rest of a measure with rests, so the measure is as long as its time signature says.
//!
//! ⛔ WHY THIS EXISTS AT ALL: a score whose measures do not add up is the state `sanityCheck` reports as
//! "Incomplete measure", and it is reachable through ordinary edits - `note_to_rest` on a beat, removing a
//! note that was holding a bar together, or a paste that did not fill. A caller that has just made a bar
//! short needs one call that says "make this bar whole again", not a sequence of duration guesses.
//!
//! ⚠️ It fills from the END of the last thing in the measure, not from a beat the caller names. "Fill this
//! measure" is about the measure, and asking for a start position would invite a caller to name a beat
//! inside existing content - where the honest answer is that there is nothing to fill.
RecipeResult fillMeasureWithRests(mu::engraving::Score* score, int measureNumber);

//! ── Parts and staves ──────────────────────────────────────────────────────────────────────────
//! Adding and removing whole staves. These change how many lines the score is read on, which is a
//! different kind of edit from anything else here: every other operation touches CONTENT, and this one
//! touches the shape of the page.

//! Append a staff to a part.
//!
//! @param partIndex 0-based index into `Score::parts()`
//! @return a description of what was added, including the staff's new 1-based number
RecipeResult appendStaff(mu::engraving::Score* score, int partIndex);

//! Remove the last staff of a part.
//!
//! ⛔ REFUSED WHEN IT IS THE PART'S ONLY STAFF. A part with no staves is not a small part - it is a broken
//! one, and the same reasoning as "a score needs at least one measure" applies here. Removing the last
//! staff of a part is a request to remove the PART, which is a different operation this tool does not do.
RecipeResult removeLastStaff(mu::engraving::Score* score, int partIndex);

//! ── Articulations ─────────────────────────────────────────────────────────────────────────────
//! Staccato, accent, tenuto and the rest. These attach to a NOTE, and they change how it is played -
//! which makes them the third thing after ties and slurs that a model can confuse with the other two.

//! Add an articulation to the note at an address, or remove it if it is already there.
//!
//! @param name the SMuFL symbol name, e.g. `articStaccatoAbove`. The names are the score format's own, so
//!             the accepted set is what the format accepts rather than a list kept here.
//!
//! ⛔ TOGGLE, NOT ADD. An articulation is on or off - "add a staccato" to a note that already has one
//! should not produce two dots stacked on top of each other, and `EditChord::toggleArticulation` is
//! upstream's own answer to that. The result says which way it went, so a caller that asked to add and
//! got a removal is told rather than left to discover it.
RecipeResult toggleArticulation(mu::engraving::Score* score, const ScoreAddress& address, int voice,
                                int noteIndex, const QString& name);

//! ── Moving a note ─────────────────────────────────────────────────────────────────────────────
//! ⛔⛔ THIS IS A DELETE PLUS AN ADD, AND THAT SHAPE IS THE WHOLE PROBLEM. Done naively - remove from the
//! source, then add to the target - a failure in the second half leaves the note GONE: the score is
//! changed and the caller is told the operation failed, which is the worst of both. So `moveNote`
//! VALIDATES EVERYTHING FIRST and then performs the two halves in the order that keeps the note alive:
//! the target gains it before the source loses it.

//! Move one note's pitch from one beat to another.
//!
//! @param from the beat holding the note
//! @param to the beat that should gain it
//! @param noteIndex which note of the source chord, when it holds several
//!
//! ⛔ REFUSED when the target already has that pitch (two notes at one pitch is not a chord) and when the
//! target is not a chord - turning a rest into a note is `note_add` on an existing note, not a move.
RecipeResult moveNote(mu::engraving::Score* score, const ScoreAddress& from, int noteIndex,
                      const ScoreAddress& to);

//! ── Measures ──────────────────────────────────────────────────────────────────────────────────
//! Adding and removing whole measures. These change the SHAPE of the score rather than its contents, and
//! everything addressed by measure number after the edit moves - which is worth saying in the result.

//! Insert `count` measures before measure `beforeMeasure` (1-based). `beforeMeasure` may be one past the
//! last measure, which appends.
RecipeResult insertMeasures(mu::engraving::Score* score, int beforeMeasure, int count);

//! Remove measures `first` through `last` inclusive (1-based).
//!
//! ⛔ REFUSED WHEN IT WOULD EMPTY THE SCORE. A score with no measures is not a short score, it is a broken
//! one - the notation layer has nowhere to put the cursor and several upstream paths assume a first
//! measure exists.
RecipeResult removeMeasures(mu::engraving::Score* score, int first, int last);

//! ── Dynamics and hairpins ─────────────────────────────────────────────────────────────────────
//! These are the first elements that are NOT notes and NOT text: they attach to a beat and belong to a
//! staff, and they are what makes a score sound like something.

//! Add a dynamic marking (`pp`, `mf`, `sfz`, ...) at a beat.
//!
//! @param mark the marking as written, e.g. `mf`. Parsed by the notation layer's own parser, so the
//!             accepted set is exactly what the score format accepts rather than a list kept here.
RecipeResult addDynamic(mu::engraving::Score* score, const ScoreAddress& address, const QString& mark);

//! Add a crescendo or diminuendo hairpin from one beat to another.
//!
//! @param kind `crescendo` or `diminuendo`
//! @param toAddress the end of the hairpin; when it names the same beat as the start, the hairpin runs to
//!                  the next chord rest, which is what a user gets from the palette
RecipeResult addHairpin(mu::engraving::Score* score, const ScoreAddress& from, const ScoreAddress& to,
                        const QString& kind);

//! ── Rest recipes ──────────────────────────────────────────────────────────────────────────────
//! Replace the chord at an address with a rest of the SAME duration.
//!
//! ⛔ WHY THIS IS THE ANSWER TO "silence this beat" AND `note_remove` IS NOT: removing the last note of
//! a chord leaves an empty chord, which is not a rest - it draws as nothing and is a broken object.
//! `note_remove` refuses that case and points here, so the two tools are complements rather than
//! alternatives, and a caller that wants silence has exactly one place to go.
//!
//! The duration is taken from the chord being replaced, not from an argument: "make this beat a rest"
//! means the beat keeps its length. Changing the length as well is `note_set_duration`'s job, and doing
//! both at once would make "the rest is the wrong length" and "the rest is in the wrong place"
//! indistinguishable in the result.
RecipeResult changeToRest(mu::engraving::Score* score, const ScoreAddress& address, int voice);

//! The duration names `setChordDuration` accepts, for the tool's error message and its schema.
QStringList durationNames();

} // namespace muse::agentharness
