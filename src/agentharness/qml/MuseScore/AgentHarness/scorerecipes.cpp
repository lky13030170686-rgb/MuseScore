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

#include <QHash>

#include "engraving/dom/chord.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/noteval.h"
#include "engraving/editing/transpose.h"
#include "engraving/editing/editnote.h"
#include "engraving/editing/noteinput.h"
#include "engraving/editing/transaction/transaction.h"

#include "addressing.h"
#include "notelocator.h"
#include "scoredigest.h"

#include "log.h"

using namespace muse::agentharness;

namespace {
//! A parsed duration, or why it could not be parsed.
struct DurationParse
{
    bool ok = false;
    mu::engraving::TDuration value;
    //! The canonical name, for the success message ("dotted-quarter" rather than what was typed).
    QString name;
    QString problem;
};

//! Map one duration name to its type.
bool baseDurationType(const QString& name, mu::engraving::DurationType& out)
{
    using DT = mu::engraving::DurationType;
    static const QHash<QString, DT> table = {
        { QStringLiteral("long"), DT::V_LONG },
        { QStringLiteral("breve"), DT::V_BREVE },
        { QStringLiteral("whole"), DT::V_WHOLE },
        { QStringLiteral("half"), DT::V_HALF },
        { QStringLiteral("quarter"), DT::V_QUARTER },
        { QStringLiteral("eighth"), DT::V_EIGHTH },
        { QStringLiteral("16th"), DT::V_16TH },
        { QStringLiteral("32nd"), DT::V_32ND },
        { QStringLiteral("64th"), DT::V_64TH },
        { QStringLiteral("128th"), DT::V_128TH },
        { QStringLiteral("256th"), DT::V_256TH },
        { QStringLiteral("512th"), DT::V_512TH },
        { QStringLiteral("1024th"), DT::V_1024TH },
        { QStringLiteral("measure"), DT::V_MEASURE },
    };
    const auto it = table.constFind(name);
    if (it == table.constEnd()) {
        return false;
    }
    out = it.value();
    return true;
}

//! Parse a duration name, with dots in any of the spellings a model actually produces.
//!
//! ⛔ WHY NOT TAKE A FRACTION OR A TICK COUNT: a model asked for "a dotted quarter" that has to compute
//! `3/8` will eventually compute something else, and the failure will look like a wrong duration
//! rather than a wrong conversion - which is much harder to notice and to explain. Accepting the name
//! a musician uses removes that whole class of error.
DurationParse parseDuration(const QString& raw)
{
    DurationParse result;

    QString name = raw.trimmed().toLower();
    if (name.isEmpty()) {
        result.problem = QStringLiteral("no duration given");
        return result;
    }

    //! Dots, in the three spellings seen in practice: a prefix (`dotted-quarter`), a suffix word
    //! (`quarter dotted`), and the musical shorthand (`quarter.`).
    int dots = 0;
    if (name.startsWith(QLatin1String("double-dotted-")) || name.startsWith(QLatin1String("double dotted "))) {
        dots = 2;
        name = name.mid(name.indexOf(QLatin1Char('-')) >= 0 ? name.indexOf(QLatin1Char('-')) + 1
                                                            : int(qstrlen("double dotted ")));
    } else if (name.startsWith(QLatin1String("dotted-")) || name.startsWith(QLatin1String("dotted "))) {
        dots = 1;
        name = name.mid(name.indexOf(QLatin1Char('-')) >= 0 ? name.indexOf(QLatin1Char('-')) + 1
                                                            : int(qstrlen("dotted ")));
    }

    while (name.endsWith(QLatin1Char('.'))) {
        ++dots;
        name.chop(1);
    }
    if (name.endsWith(QLatin1String(" dotted"))) {
        ++dots;
        name.chop(int(qstrlen(" dotted")));
    }

    mu::engraving::DurationType type = mu::engraving::DurationType::V_INVALID;
    if (!baseDurationType(name, type)) {
        result.problem = QStringLiteral("`%1` is not a duration I know. Use one of: %2 (optionally "
                                        "dotted, e.g. `dotted-quarter`).")
                         .arg(raw, durationNames().join(QStringLiteral(", ")));
        return result;
    }

    //! A measure-long chord is complete by definition; a dot on it is meaningless rather than harmful,
    //! but accepting it silently would suggest it did something.
    if (type == mu::engraving::DurationType::V_MEASURE && dots > 0) {
        result.problem = QStringLiteral("`measure` is already the whole measure, so it cannot be dotted");
        return result;
    }

    if (dots > 3) {
        result.problem = QStringLiteral("at most three dots are supported; `%1` has %2").arg(raw).arg(dots);
        return result;
    }

    result.value = mu::engraving::TDuration(mu::engraving::DurationTypeWithDots(type, dots));
    result.name = dots == 0 ? name
                  : QStringLiteral("%1%2").arg(QString(dots == 2 ? QStringLiteral("double-dotted-")
                                                                 : QStringLiteral("dotted-")), name);
    result.ok = true;
    return result;
}

//! A readable name for a duration, for "was -> now" messages. Falls back to the tick count, which is
//! still better than an enum number.
QString describeDuration(const mu::engraving::TDuration& d)
{
    for (const QString& candidate : durationNames()) {
        DurationParse parsed = parseDuration(candidate);
        if (parsed.ok && parsed.value.type() == d.type() && parsed.value.dots() == d.dots()) {
            return candidate;
        }
    }
    return QStringLiteral("%1 tick(s)").arg(d.ticks().ticks());
}

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

RecipeResult muse::agentharness::setChordDuration(mu::engraving::Score* score, const ScoreAddress& address,
                                                 int voice, const QString& duration)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const NoteLookup found = chordAt(score, address, voice);
    if (!found.found()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the chord lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    const DurationParse parsed = parseDuration(duration);
    if (!parsed.ok) {
        return RecipeResult::failure(parsed.problem);
    }

    mu::engraving::Chord* chord = found.chord;
    const QString was = describeDuration(chord->durationType());

    if (chord->durationType().type() == parsed.value.type()
        && chord->durationType().dots() == parsed.value.dots()) {
        //! Not an error: it is already that long. Reported as success so a model does not go looking
        //! for another way to do what it already did.
        return RecipeResult::success(QStringLiteral("%1 is already %2")
                                     .arg(formatAddress(address), parsed.name));
    }

    //! ⛔ `Score::undoChangeChordRestLen`, not `chord->setDurationType()`. The latter changes the chord
    //! without telling the undo stack, so Ctrl+Z would not take it back and the information field would
    //! never see it - the edit would be real but invisible to everything that watches the score.
    //!
    //! ⚠️ It sets DURATION_TYPE_WITH_DOTS and DURATION as two properties. That is upstream's own
    //! sequence; doing only the first leaves `ticks()` stale, which shows up as a chord that draws
    //! short and overlaps the next beat.
    score->undoChangeChordRestLen(chord, parsed.value);

    return RecipeResult::success(QStringLiteral("%1: %2 -> %3")
                                 .arg(formatAddress(address), was, parsed.name));
}

RecipeResult muse::agentharness::removeNote(mu::engraving::Score* score, const ScoreAddress& address,
                                            int voice, int noteIndex)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const NoteLookup found = noteAt(score, address, voice, noteIndex);
    if (!found.ok()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the note lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::Chord* chord = found.chord;
    mu::engraving::Note* note = found.note;

    //! ⛔ REFUSED when it is the last note. A chord with no notes is not a rest - it is a broken chord
    //! that will draw as nothing and may trip assertions later. A caller who wants silence wants a
    //! rest, which is a different operation (`command://notation/...`), and saying so is more useful
    //! than either doing it silently or corrupting the score.
    if (chord->notes().size() <= 1) {
        return RecipeResult::failure(
            QStringLiteral("%1 has only one note, so removing it would leave an empty chord rather "
                           "than a rest. To silence this beat, replace the note with a rest instead.")
            .arg(formatAddress(address)));
    }

    const QString was = pitchName(note->pitch());

    //! ⛔ `Score::undoRemoveElement`, which is what upstream's own note deletion uses
    //! (`editvoice.cpp`, `edit.cpp`). It also drops the note's ties, which a bare `chord->remove()`
    //! would leave dangling - a tie pointing at a note that no longer exists.
    score->undoRemoveElement(note);

    return RecipeResult::success(QStringLiteral("%1: removed %2 (the chord now has %3 note(s))")
                                 .arg(formatAddress(address), was).arg(chord->notes().size()));
}

RecipeResult muse::agentharness::addNoteToChord(mu::engraving::Score* score, const ScoreAddress& address,
                                                int voice, int midiPitch)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (!pitchInRange(midiPitch)) {
        return RecipeResult::failure(QStringLiteral("%1 is not a MIDI pitch; it must be 0-127 "
                                                    "(60 is middle C)").arg(midiPitch));
    }

    const NoteLookup found = chordAt(score, address, voice);
    if (!found.found()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the chord lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::Chord* chord = found.chord;

    //! ⛔ A duplicate pitch is not a chord, it is the same note twice. Upstream will create it without
    //! complaint, and the result draws as ONE notehead while every later read of the chord sees two
    //! notes at the same pitch - which reads as "the chord has a note I cannot see". Refusing also
    //! settles the common case where the caller meant a different octave.
    for (mu::engraving::Note* existing : chord->notes()) {
        if (existing->pitch() == midiPitch) {
            return RecipeResult::failure(
                QStringLiteral("%1 already has a %2, so adding it again would put the same note in the "
                               "chord twice. If you meant a different octave, give the MIDI number for "
                               "that octave (60 is middle C).")
                .arg(formatAddress(address), pitchName(midiPitch)));
        }
    }

    //! The spelling is derived from the key, exactly as `setNotePitch` does it - a chord added to in C
    //! major should spell a black key the way that key spells it.
    const mu::engraving::Key key = chord->staff() ? chord->staff()->concertKey(chord->tick())
                                                  : mu::engraving::Key::C;
    const int tpc = mu::engraving::pitch2tpc(midiPitch, key, mu::engraving::Prefer::NEAREST);

    mu::engraving::NoteVal nval(midiPitch);
    nval.tpc1 = tpc;
    //! ⚠️ `tpc2` is the TRANSPOSED spelling, and it must be set: leaving it at TPC_INVALID trips an
    //! assert inside `Note::setPitch`. For a non-transposing instrument the two spellings are the same,
    //! and for a transposing one the staff's interval is what converts between them.
    if (chord->staff()) {
        mu::engraving::Interval v = chord->staff()->transpose(chord->tick());
        if (v.isZero()) {
            nval.tpc2 = tpc;
        } else {
            v.flip();
            nval.tpc2 = mu::engraving::Transpose::transposeTpc(tpc, v, true);
        }
    } else {
        nval.tpc2 = tpc;
    }

    const size_t before = chord->notes().size();

    //! ⛔ `NoteInput::addPitchToChord`, NOT `chord->add(note)`. This is the engraving layer's own
    //! primitive for putting a pitch into a chord (the same one the user's note entry goes through),
    //! and it pushes the right `UndoableCommand`s. A bare `chord->add()` changes the chord without
    //! telling the undo stack - the note would appear and Ctrl+Z would not take it back.
    mu::engraving::NoteInput::addPitchToChord(score->transactionManager()->currentOrDummyTransaction(),
                                              score, nval, chord);

    const size_t after = chord->notes().size();
    if (after <= before) {
        //! The primitive reported no change. Saying so is better than a success message the caller
        //! cannot reconcile with what it reads back.
        return RecipeResult::failure(QStringLiteral("adding %1 to %2 did not change the chord "
                                                    "(it still has %3 note(s))")
                                     .arg(pitchName(midiPitch), formatAddress(address)).arg(after));
    }

    return RecipeResult::success(QStringLiteral("%1: added %2 (the chord now has %3 note(s))")
                                 .arg(formatAddress(address), pitchName(midiPitch)).arg(after));
}
namespace {
//! Create a tie between two notes and put it on the undo stack.
//!
//! ⛔ THIS MIRRORS `createAndAddTie` in `src/engraving/editing/edittie.cpp`, deliberately. That function
//! is file-static, and the public `EditTie::cmdAddTie` is not a substitute: it reads the current
//! SELECTION, mutates the global input state, and will even append a measure when the note is at the
//! end of the score. None of that is acceptable for an addressed write - the caller named a note, not
//! "whatever is selected", and an edit that silently appends a measure is an edit nobody asked for.
//!
//! The alternative was making the upstream helper public, which means touching a header under
//! `src/engraving/` and paying an 8-16 minute rebuild for every downstream target (维护手册.md §9.3).
//! A local copy of eleven lines is the cheaper and more honest trade; if upstream's version changes,
//! this is the place that has to be compared against it.
mu::engraving::Tie* createAndAddTie(mu::engraving::Score* score, mu::engraving::Note* startNote,
                                    mu::engraving::Note* endNote)
{
    mu::engraving::Tie* tie = mu::engraving::Factory::createTie(startNote);
    tie->setStartNote(startNote);
    tie->setTrack(startNote->track());
    tie->setTick(startNote->chord()->segment()->tick());

    if (endNote->tieBack()) {
        //! The end note may already be tied from something else. Removing that first is what upstream
        //! does, and it is what keeps a note from having two ties arriving at it.
        score->undoRemoveElement(endNote->tieBack());
    }

    tie->setEndNote(endNote);
    tie->setTicks(endNote->chord()->segment()->tick() - startNote->chord()->segment()->tick());

    score->undoAddElement(tie);

    //! Jump points are the partial-tie bookkeeping (a tie split across a system break). Upstream calls
    //! this immediately after adding; skipping it leaves a tie that draws but does not survive a
    //! relayout.
    tie->addTiesToJumpPoints();

    return tie;
}
} // namespace

RecipeResult muse::agentharness::addTie(mu::engraving::Score* score, const ScoreAddress& address,
                                        int voice, int noteIndex)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const NoteLookup found = noteAt(score, address, voice, noteIndex);
    if (!found.ok()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the note lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::Note* note = found.note;

    //! ⛔ Already tied forward: refused, not replaced. Silently redirecting an existing tie is an edit
    //! the caller cannot see in the result, and the caller may well be asking precisely because it
    //! wants to know whether the earlier call worked.
    if (note->tieFor()) {
        return RecipeResult::failure(
            QStringLiteral("%1 is already tied to the next note. To tie it somewhere else, remove the "
                           "existing tie first.").arg(formatAddress(address)));
    }

    const NoteLookup target = nextNote(score, note);
    if (!target.ok()) {
        return RecipeResult::failure(target.problem.isEmpty()
                                     ? QStringLiteral("no later note to tie to")
                                     : target.problem);
    }

    createAndAddTie(score, note, target.note);

    return RecipeResult::success(QStringLiteral("%1: tied %2 to the next %2")
                                 .arg(formatAddress(address), pitchName(note->pitch())));
}

RecipeResult muse::agentharness::removeTie(mu::engraving::Score* score, const ScoreAddress& address,
                                           int voice, int noteIndex)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const NoteLookup found = noteAt(score, address, voice, noteIndex);
    if (!found.ok()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the note lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::Note* note = found.note;

    if (!note->tieFor()) {
        //! Refused rather than reported as done: "there was nothing to remove" and "I removed it" are
        //! different answers, and a caller that gets the second when the first is true will believe it
        //! changed something.
        return RecipeResult::failure(QStringLiteral("%1 is not tied to the next note, so there is "
                                                    "nothing to remove").arg(formatAddress(address)));
    }

    //! ⛔ `Score::undoRemoveElement`, which is what upstream's own tie removal uses. A bare
    //! `note->setTieFor(nullptr)` would leave the tie in the score pointing at a note that no longer
    //! references it.
    score->undoRemoveElement(note->tieFor());

    return RecipeResult::success(QStringLiteral("%1: removed the tie from %2")
                                 .arg(formatAddress(address), pitchName(note->pitch())));
}

RecipeResult muse::agentharness::toggleTie(mu::engraving::Score* score, const ScoreAddress& address,
                                           int voice, int noteIndex)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    //! One lookup, then dispatch: `addTie`/`removeTie` do their own lookups, and doing it here as well
    //! would be three walks of the score for one keystroke's worth of work.
    const NoteLookup found = noteAt(score, address, voice, noteIndex);
    if (!found.ok()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the note lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    if (found.note->tieFor()) {
        return removeTie(score, address, voice, noteIndex);
    }

    return addTie(score, address, voice, noteIndex);
}
QStringList muse::agentharness::durationNames(){
    return {
        QStringLiteral("long"), QStringLiteral("breve"), QStringLiteral("whole"), QStringLiteral("half"),
        QStringLiteral("quarter"), QStringLiteral("eighth"), QStringLiteral("16th"), QStringLiteral("32nd"),
        QStringLiteral("64th"), QStringLiteral("128th"), QStringLiteral("256th"), QStringLiteral("512th"),
        QStringLiteral("1024th"), QStringLiteral("measure"),
    };
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
