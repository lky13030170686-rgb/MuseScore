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
#include <QSet>

#include "engraving/dom/chord.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/factory.h"
#include "engraving/editing/editkeysig.h"
#include "engraving/editing/edittimesig.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/sig.h"
#include "engraving/dom/score.h"
#include "engraving/dom/textbase.h"
#include "engraving/editing/navigation.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/noteval.h"
#include "engraving/dom/rest.h"
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
    //!
    //! ⛔⛔ STRIP THE WHOLE PREFIX, not "everything up to the first dash". The first version did the
    //! latter, which is correct for `dotted-half` and WRONG for `double-dotted-half`: the first dash is
    //! the one inside `double-dotted`, so the name came out as `dotted-half` and the caller was told
    //! "`double-dotted-half` is not a duration I know" - about a spelling this function documents as
    //! accepted. The unit suite caught it; nothing in the running program had tried the double form.
    int dots = 0;
    const struct {
        const char* prefix;
        int dots;
    } prefixes[] = {
        { "double-dotted-", 2 },
        { "double dotted ", 2 },
        { "dotted-", 1 },
        { "dotted ", 1 },
    };

    for (const auto& p : prefixes) {
        const QString prefix = QString::fromLatin1(p.prefix);
        if (name.startsWith(prefix)) {
            dots = p.dots;
            name = name.mid(prefix.size());
            break;
        }
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

//! Whether `duration` fits in the space from `chord` to the end of its measure.
//!
//! ⛔⛔ WHY THIS GATE EXISTS, and it is the M5 "G1" gate in the plan: `Score::undoChangeChordRestLen`
//! sets `DURATION_TYPE_WITH_DOTS` and `DURATION` and NOTHING ELSE. It does not check that the result
//! fits, and it does not make room. So asking for a longer duration than the space remaining writes a
//! chord whose `ticks()` run past the barline - the measure is no longer full, `sanityCheck()` has
//! something to complain about, and the score can end up in a state the editor will not reopen.
//!
//! ⚠️ The interactive path does not have this problem because it goes through `Score::changeCRlen`,
//! which SPLITS measures to make room. That is right for a user dragging a note longer - it is the
//! behaviour they expect - and wrong for an addressed write, where "make this note a half note" must
//! either succeed in place or say it cannot. Silently splitting the bar is an edit nobody asked for.
//!
//! ⚠️ Ties and tuplets are deliberately NOT special-cased: a tie means the note's SOUND continues, but
//! its WRITTEN duration still has to fit the measure, and a tuplet's written duration is already scaled
//! by `ticks()`. Special-casing either would be inventing a rule.
//! A key signature as a musician reads it: the number of sharps or flats, named.
//!
//! ⚠️ Reported as "3 sharps" rather than as a key NAME (A major) on purpose. The notation layer stores a
//! COUNT, and the count is what was asked for; naming the major key would silently pick one of the two
//! modes that share every signature (A major and F# minor are both three sharps), and a caller who meant
//! the minor one would be told something that is not what it set.
QString describeKeySignature(int fifths)
{
    if (fifths == 0) {
        return QStringLiteral("no sharps or flats");
    }
    const int count = qAbs(fifths);
    return QStringLiteral("%1 %2").arg(count).arg(fifths > 0
                                                  ? (count == 1 ? QStringLiteral("sharp") : QStringLiteral("sharps"))
                                                  : (count == 1 ? QStringLiteral("flat") : QStringLiteral("flats")));
}
bool durationFits(mu::engraving::ChordRest* chord, const mu::engraving::TDuration& duration)
{
    if (!chord) {
        return false;
    }

    const mu::engraving::Measure* measure = chord->findMeasure();
    if (!measure) {
        return false;
    }

    const mu::engraving::Fraction wanted = duration.type() == mu::engraving::DurationType::V_MEASURE
                                           ? measure->ticks()
                                           : duration.fraction();
    const mu::engraving::Fraction available = measure->endTick() - chord->tick();

    return wanted <= available;
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

    //! ⛔ THE GATE, before anything is written. See the note on `durationFits` for why this cannot be
    //! left to the write itself: `undoChangeChordRestLen` does not check, and the interactive
    //! alternative (`changeCRlen`) would SPLIT the bar - an edit nobody asked for.
    //!
    //! The message names the space available, because "it does not fit" without a number leaves the
    //! caller guessing whether it overshot by a beat or by a whole bar.
    if (!durationFits(chord, parsed.value)) {
        const mu::engraving::Measure* measure = chord->findMeasure();
        const mu::engraving::Fraction available = measure ? measure->endTick() - chord->tick()
                                                          : mu::engraving::Fraction(0, 1);
        return RecipeResult::failure(
            //! ⚠️ The space left is reported as a FRACTION OF A WHOLE NOTE (`3/4`), not as a duration
            //! name and not as a tick count. A duration name is wrong for anything that is not a single
            //! note - three quarters is not "a dotted half", and a caller who reads it that way will ask
            //! for one and be refused again. A tick count is worse: "only 1440 tick(s)" tells a musician
            //! nothing. (Both were tried; the first live run printed the tick count.)
            QStringLiteral("a %1 does not fit at %2 - only %3 of a whole note is left in the measure "
                           "after this beat. A shorter duration, or moving the note earlier, would fit. "
                           "(The bar is NOT split to make room: that would be an edit you did not ask "
                           "for.)")
            .arg(parsed.name, formatAddress(address),
                 QStringLiteral("%1/%2").arg(available.numerator()).arg(available.denominator())));
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
                           "than a rest. Use note_to_rest to silence this beat instead.")
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
namespace {
//! The styles this build accepts, with whether each one hangs off a position.
//!
//! ⚠️ The list is deliberately SHORT and hand-picked rather than "all of TextStyleType". Most of that
//! enum is not something a caller adds directly - it is the style OF an element that already exists
//! (`DYNAMICS` styles a dynamic, `LYRICS_ODD` styles a lyric, `TUPLET` styles a tuplet number). Offering
//! those here would invite a caller to "add a dynamics text" and get a plain text box wearing a
//! dynamic's font - which looks right and is not a dynamic.
struct TextStyleEntry {
    const char* name;
    mu::engraving::TextStyleType type;
    bool needsAddress;
};

const TextStyleEntry kTextStyles[] = {
    //! Frame text: belongs to the score, not to a beat.
    { "title", mu::engraving::TextStyleType::TITLE, false },
    { "subtitle", mu::engraving::TextStyleType::SUBTITLE, false },
    { "composer", mu::engraving::TextStyleType::COMPOSER, false },
    { "lyricist", mu::engraving::TextStyleType::LYRICIST, false },
    //! Attached text: hangs off a chord or rest, so it needs an address.
    { "rehearsal-mark", mu::engraving::TextStyleType::REHEARSAL_MARK, true },
    { "system", mu::engraving::TextStyleType::SYSTEM, true },
    { "staff", mu::engraving::TextStyleType::STAFF, true },
    { "expression", mu::engraving::TextStyleType::EXPRESSION, true },
};

const TextStyleEntry* findTextStyle(const QString& style)
{
    for (const TextStyleEntry& entry : kTextStyles) {
        if (style == QLatin1String(entry.name)) {
            return &entry;
        }
    }
    return nullptr;
}
} // namespace

QStringList muse::agentharness::textStyleNames()
{
    QStringList names;
    for (const TextStyleEntry& entry : kTextStyles) {
        names.append(QString::fromLatin1(entry.name));
    }
    return names;
}

bool muse::agentharness::textStyleNeedsAddress(const QString& style, bool& known)
{
    const TextStyleEntry* entry = findTextStyle(style);
    known = entry != nullptr;
    return entry && entry->needsAddress;
}

RecipeResult muse::agentharness::addText(mu::engraving::Score* score, const ScoreAddress& address,
                                        const QString& style, const QString& text)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (text.trimmed().isEmpty()) {
        //! An empty text box is invisible and unselectable - it is an object in the score that nobody
        //! can find again. Refused rather than created.
        return RecipeResult::failure(QStringLiteral("no text given; an empty text is invisible in the "
                                                    "score and cannot be selected again"));
    }

    bool known = false;
    const bool needsAddress = textStyleNeedsAddress(style, known);
    if (!known) {
        return RecipeResult::failure(QStringLiteral("`%1` is not a text style I know. Use one of: %2")
                                     .arg(style, textStyleNames().join(QStringLiteral(", "))));
    }

    mu::engraving::EngravingItem* destination = nullptr;

    if (needsAddress) {
        const NoteLookup found = chordAt(score, address, 0);
        if (!found.found()) {
            return RecipeResult::failure(found.problem.isEmpty()
                                         ? QStringLiteral("the lookup failed without saying why "
                                                          "(internal) - nothing was changed")
                                         : found.problem);
        }
        destination = found.chord ? static_cast<mu::engraving::EngravingItem*>(found.chord)
                                  : static_cast<mu::engraving::EngravingItem*>(found.rest);

        //! ⛔⛔ THE GUARD. `Score::addText` does NOT check this: it calls `chordOrRest(destination)` and
        //! dereferences the result, so a null destination is a null dereference inside upstream. Getting
        //! here means the address resolved to something that is neither a chord nor a rest, which the
        //! locator should already have excluded - so this is a backstop, not the normal path.
        if (!destination) {
            return RecipeResult::failure(QStringLiteral("%1 holds nothing that text can attach to")
                                         .arg(formatAddress(address)));
        }
    }

    const TextStyleEntry* entry = findTextStyle(style);
    mu::engraving::TextBase* added = score->addText(entry->type, destination);
    if (!added) {
        return RecipeResult::failure(QStringLiteral("the %1 could not be created").arg(style));
    }

    //! ⚠️ The text is set AFTER `addText`, because `addText` creates the element with its style's default
    //! text (for a title, the score's own title placeholder). Setting it through `undoChangeProperty` is
    //! what makes the content change part of the same undo step as the creation - assigning the string
    //! directly would leave the element's text invisible to the undo stack and to the information field.
    //!
    //! ⚠️ `muse::String(text)` and not `muse::String::fromUtf8(text.toUtf8())`: `fromUtf8` has overloads
    //! for `const char*`, `std::string_view` and `ByteArray`, and a `QByteArray` argument matches more
    //! than one of them. `String` has a `QString` constructor, which is the conversion actually wanted.
    //!
    //! ⚠️ Three arguments, because `TextBase` overrides the 3-argument form and its
    //! `using EngravingObject::undoChangeProperty` does not re-expose the 2-argument one.
    added->undoChangeProperty(mu::engraving::Pid::TEXT,
                              mu::engraving::PropertyValue(muse::String(text)),
                              mu::engraving::PropertyFlags::STYLED);

    if (needsAddress) {
        return RecipeResult::success(QStringLiteral("%1: added %2 text \"%3\"")
                                     .arg(formatAddress(address), style, text));
    }
    return RecipeResult::success(QStringLiteral("added %1 text \"%2\"").arg(style, text));
}
RecipeResult muse::agentharness::setKeySignature(mu::engraving::Score* score, int measureNumber, int fifths)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    //! ⛔ Validated against the enum's own bounds rather than a literal 7, so that if upstream ever widens
    //! the range this starts accepting the new values instead of silently refusing them.
    const int lowest = int(mu::engraving::Key::MIN);
    const int highest = int(mu::engraving::Key::MAX);
    if (fifths < lowest || fifths > highest) {
        return RecipeResult::failure(
            QStringLiteral("%1 is not a key signature. Use a number of sharps (1 to %2) or flats (-1 to "
                           "%3), or 0 for no accidentals.").arg(fifths).arg(highest).arg(-lowest));
    }

    if (measureNumber < 1) {
        return RecipeResult::failure(QStringLiteral("measure numbers start at 1 (got %1)")
                                     .arg(measureNumber));
    }

    mu::engraving::Measure* measure = nullptr;
    int index = 0;
    for (mu::engraving::Measure* m = score->firstMeasure(); m; m = m->nextMeasure(), ++index) {
        if (index + 1 == measureNumber) {
            measure = m;
            break;
        }
    }
    if (!measure) {
        return RecipeResult::failure(QStringLiteral("there is no measure %1; the score has %2")
                                     .arg(measureNumber).arg(index));
    }

    mu::engraving::KeySigEvent event;
    event.setKey(mu::engraving::Key(fifths));
    //! ⚠️ `setCustom(false)` matters: a custom key signature is one with an explicit accidental list, and
    //! marking a plain -3 as custom would make the score claim it carries a signature the user built by
    //! hand. The notation layer distinguishes the two, and so does the file format.
    event.setCustom(false);

    //! ⚠️ The transaction comes from the score rather than being opened here. `runNoteRecipe` already
    //! prepared one, and `EditKeySig` needs the SAME transaction so that the change joins the undo step
    //! the harness opened - opening a second one would either be reused silently or end the outer one.
    mu::engraving::Transaction& tx = score->transactionManager()->currentOrDummyTransaction();

    const QString was = describeKeySignature(int(score->staff(0)->key(measure->tick())));
    for (mu::engraving::Staff* staff : score->staves()) {
        mu::engraving::EditKeySig::undoChangeKeySig(tx, score, staff, measure->tick(), event);
    }

    const QString now = describeKeySignature(int(score->staff(0)->key(measure->tick())));
    if (was == now) {
        return RecipeResult::success(QStringLiteral("measure %1 is already in %2")
                                     .arg(measureNumber).arg(now));
    }

    return RecipeResult::success(QStringLiteral("measure %1: %2 -> %3 (from this measure on)")
                                 .arg(measureNumber).arg(was, now));
}

RecipeResult muse::agentharness::setTimeSignature(mu::engraving::Score* score, int measureNumber,
                                                 int numerator, int denominator)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (numerator < 1 || denominator < 1) {
        return RecipeResult::failure(QStringLiteral("%1/%2 is not a time signature; both numbers must be "
                                                    "at least 1").arg(numerator).arg(denominator));
    }
    //! A power of two is not a stylistic preference - the notation layer stores the denominator as a
    //! power of two, so 6/3 would be accepted here and then silently stored as something else.
    if ((denominator & (denominator - 1)) != 0) {
        return RecipeResult::failure(QStringLiteral("the lower number of a time signature must be a power "
                                                    "of two (2, 4, 8, 16...); %1 is not")
                                     .arg(denominator));
    }

    if (measureNumber < 1) {
        return RecipeResult::failure(QStringLiteral("measure numbers start at 1 (got %1)")
                                     .arg(measureNumber));
    }

    mu::engraving::Measure* measure = nullptr;
    int index = 0;
    for (mu::engraving::Measure* m = score->firstMeasure(); m; m = m->nextMeasure(), ++index) {
        if (index + 1 == measureNumber) {
            measure = m;
            break;
        }
    }
    if (!measure) {
        return RecipeResult::failure(QStringLiteral("there is no measure %1; the score has %2")
                                     .arg(measureNumber).arg(index));
    }

    const mu::engraving::Fraction wanted(numerator, denominator);
    if (measure->timesig() == wanted) {
        return RecipeResult::success(QStringLiteral("measure %1 is already %2/%3")
                                     .arg(measureNumber).arg(numerator).arg(denominator));
    }

    mu::engraving::TimeSig* ts = mu::engraving::Factory::createTimeSig(score->dummy()->segment());
    ts->setSig(wanted, mu::engraving::TimeSigType::NORMAL);

    mu::engraving::Transaction& tx = score->transactionManager()->currentOrDummyTransaction();

    //! ⛔⛔ `addTimeSig` REWRITES THE FOLLOWING MEASURES ITSELF - the first version of this recipe called
    //! `rewriteMeasures` after it, which was wrong twice over: the two-argument form does not exist (it
    //! takes a `staff_idx_t`), and the reflow had already happened inside `addTimeSig`.
    //!
    //! ⚠️ It is also the ONLY thing to call, because the reflow is not optional from outside: `addTimeSig`
    //! rewrites first and adds the signature only if that succeeded, and it calls
    //! `activeTransaction()->unwind()` to abandon the whole change when it did not. Doing the steps in any
    //! other order would put a signature on measures that were never reflowed - bars that do not match
    //! their own signature, which is the "Agent 把工程写坏" state the plan lists first.
    const int oldNumerator = measure->timesig().numerator();
    const int oldDenominator = measure->timesig().denominator();
    //! ⚠️ Captured BEFORE the call, because the reflow inside `addTimeSig` REMOVES AND RECREATES the
    //! measures - so any `Measure*` taken before it is dangling afterwards, and the tick is the only
    //! stable handle. (Reading a stale `Measure*` is what made the first diagnostic report "still 4/4"
    //! for a signature that had in fact been applied.)
    const mu::engraving::Fraction tick = measure->tick();

    mu::engraving::EditTimeSig::addTimeSig(tx, score, measure, 0, ts, /*local*/ false);

    //! ⛔⛔ VERIFIED BY EFFECT, AND BY TICK - both halves of that matter.
    //!
    //! ⚠️ BY EFFECT, because `addTimeSig` returns void and can abandon the change internally (when the
    //! signature is already there, or when the reflow refuses). "I called it" is not evidence.
    //!
    //! ⛔⛔ BY TICK, because the reflow REMOVES AND RECREATES the measures. Any `Measure*` taken before
    //! `addTimeSig` is DANGLING afterwards, and reading through it reports whatever the freed memory
    //! happens to hold. That is not theoretical: this check originally read `measure->timesig()` and
    //! reported "still 4/4" for a signature that HAD been applied, which sent the debugging after the
    //! wrong thing for several rounds. The tick is the stable handle; the measure must be looked up again.
    mu::engraving::Measure* after = score->tick2measure(tick);
    if (!after || after->timesig() != wanted) {
        return RecipeResult::failure(
            QStringLiteral("the %1/%2 signature was not applied at measure %3 - the measures could not be "
                           "rewritten to the new signature, so nothing was changed. (A signature on bars "
                           "that were not reflowed would leave the score inconsistent.)")
            .arg(numerator).arg(denominator).arg(measureNumber));
    }

    return RecipeResult::success(QStringLiteral("measure %1: %2/%3 -> %4/%5 (from this measure on)")
                                 .arg(measureNumber).arg(oldNumerator).arg(oldDenominator)
                                 .arg(numerator).arg(denominator));
}
RecipeResult muse::agentharness::setChordPitches(mu::engraving::Score* score, const ScoreAddress& address,
                                                int voice, const QVector<int>& midiPitches)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (midiPitches.isEmpty()) {
        //! ⛔ An empty list means "make this beat silent", which is `changeToRest` - and an empty CHORD is
        //! not a rest, it is a broken object. Saying which tool does what the caller wants is more useful
        //! than refusing flatly.
        return RecipeResult::failure(
            QStringLiteral("no pitches given. To silence this beat use note_to_rest; a chord with no notes "
                           "is not a rest."));
    }

    //! ⛔ DUPLICATES REFUSED BEFORE ANYTHING IS WRITTEN. Two notes at the same pitch draw as one
    //! notehead while every later read of the chord sees two, which reads as "the chord has a note I
    //! cannot see". Catching it here means the refusal happens before the score is touched at all.
    QSet<int> wanted;
    for (int pitch : midiPitches) {
        if (pitch < 0 || pitch > 127) {
            return RecipeResult::failure(QStringLiteral("%1 is not a MIDI pitch; it must be 0-127")
                                         .arg(pitch));
        }
        if (wanted.contains(pitch)) {
            return RecipeResult::failure(QStringLiteral("%1 appears twice in the pitch list; a chord "
                                                        "cannot hold the same note twice")
                                         .arg(pitchName(pitch)));
        }
        wanted.insert(pitch);
    }

    const NoteLookup found = chordAt(score, address, voice);
    if (!found.found()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    if (!found.chord) {
        //! A rest is not a chord, and turning one into a chord is `note_add` on a note - which requires a
        //! note to add to. Refused with that direction rather than half-done.
        return RecipeResult::failure(
            QStringLiteral("%1 is a rest. To put notes here, this operation needs an existing chord - a "
                           "rest cannot be given pitches.").arg(formatAddress(address)));
    }

    mu::engraving::Chord* chord = found.chord;

    //! ⛔ THE ORDER MATTERS, and this is the whole reason the recipe exists. Removing first can empty the
    //! chord - which `removeNote` refuses and which would be a broken object if it did not. So:
    //!
    //!   1. add every wanted pitch that is not already there   (the chord only grows)
    //!   2. then remove every existing pitch that is not wanted (the chord never empties, because step 1
    //!      guaranteed at least the overlap is present... and if there is NO overlap, step 1 added all of
    //!      them, so the chord holds old+new and removing the old still leaves the new)
    //!
    //! The invariant is: **the chord is non-empty at every point**, which is what keeps it a legal object
    //! throughout rather than only at the end.
    QStringList added;
    for (int pitch : midiPitches) {
        if (chord->findNote(pitch)) {
            continue;
        }
        const RecipeResult result = addNoteToChord(score, address, voice, pitch);
        if (!result.ok) {
            return result;
        }
        added.append(pitchName(pitch));
    }

    QStringList removed;
    //! `notes()` is re-read on every iteration because the removals mutate it - iterating a snapshot while
    //! deleting from the live vector is how a "skip every other element" bug happens.
    for (int i = int(chord->notes().size()) - 1; i >= 0; --i) {
        mu::engraving::Note* note = chord->notes()[size_t(i)];
        if (wanted.contains(note->pitch())) {
            continue;
        }
        const QString was = pitchName(note->pitch());
        const RecipeResult result = removeNote(score, address, voice, i);
        if (!result.ok) {
            return result;
        }
        removed.append(was);
    }

    if (added.isEmpty() && removed.isEmpty()) {
        //! Not an error: the chord already is what was asked for.
        return RecipeResult::success(QStringLiteral("%1 already has exactly those notes")
                                     .arg(formatAddress(address)));
    }

    QStringList parts;
    if (!added.isEmpty()) {
        parts.append(QStringLiteral("added %1").arg(added.join(QStringLiteral(", "))));
    }
    if (!removed.isEmpty()) {
        parts.append(QStringLiteral("removed %1").arg(removed.join(QStringLiteral(", "))));
    }

    return RecipeResult::success(QStringLiteral("%1: %2 (the chord now has %3 note(s))")
                                 .arg(formatAddress(address), parts.join(QStringLiteral(", ")))
                                 .arg(chord->notes().size()));
}
RecipeResult muse::agentharness::changeToRest(mu::engraving::Score* score, const ScoreAddress& address, int voice)
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

    mu::engraving::Chord* chord = found.chord;

    if (found.isRest()) {
        //! Not an error: it is already silent. Reported as success so the caller does not go looking for
        //! another way to do what is already done.
        return RecipeResult::success(QStringLiteral("%1 is already a rest").arg(formatAddress(address)));
    }

    if (!chord) {
        return RecipeResult::failure(QStringLiteral("%1 holds something that is neither a chord nor a "
                                                    "rest, so it cannot be silenced")
                                     .arg(formatAddress(address)));
    }

    //! The duration comes from the chord, so the beat keeps its length. See the note in the header on
    //! why changing it as well would blur two different failures into one.
    const mu::engraving::Fraction ticks = chord->ticks();
    const QString was = describeDuration(chord->durationType());

    //! ⛔ `Score::setNoteRest` with a default `NoteVal` (pitch -1, i.e. `isRest()`), which is the
    //! engraving layer's own primitive for "put this duration at this position as a rest". It handles
    //! the removal of the existing chord and the gap arithmetic, and it is what upstream's own rest
    //! entry goes through - so the undo stack and the layout see a normal edit.
    //!
    //! ⚠️ NOT `Score::cmdEnterRest`: that reads the global input state (where the cursor is), appends a
    //! measure when the position is past the end, and moves the cursor afterwards. An addressed write
    //! must not depend on, or disturb, where the user's cursor happens to be.
    mu::engraving::NoteVal restVal;   //! pitch == -1 -> a rest
    score->setNoteRest(chord->segment(), chord->track(), restVal, ticks);

    return RecipeResult::success(QStringLiteral("%1: replaced the chord with a %2 rest")
                                 .arg(formatAddress(address), was));
}
RecipeResult muse::agentharness::addSlur(mu::engraving::Score* score, const ScoreAddress& address, int voice)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const NoteLookup found = chordAt(score, address, voice);
    if (!found.found()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    //! ⚠️ A slur attaches to the CHORD REST, not to a note inside it - which is why this takes the
    //! chord and does not need a note index. A caller on a rest is refused: a slur needs something to
    //! start on, and "slur the silence" is not a thing.
    mu::engraving::ChordRest* start = found.chord ? static_cast<mu::engraving::ChordRest*>(found.chord)
                                                  : found.rest;
    if (!start || start->isRest()) {
        return RecipeResult::failure(QStringLiteral("%1 is a rest, so there is nothing to slur from")
                                     .arg(formatAddress(address)));
    }

    //! ⛔ ALREADY SLURRED: refused, not stacked. Two slurs over the same pair are not a thicker slur -
    //! they are two slurs drawn on top of each other, and the caller cannot see that from the result.
    if (start->slur()) {
        return RecipeResult::failure(
            QStringLiteral("%1 is already slurred to the next note. To slur it somewhere else, remove "
                           "the existing slur first.").arg(formatAddress(address)));
    }

    //! ⛔ CHECK THAT THERE IS SOMETHING TO SLUR TO, BEFORE calling `addSlur`.
    //!
    //! `Score::addSlur` does not fail when the note is the last one - it falls back to slurring the note
    //! TO ITSELF (`secondChordRest = firstChordRest`), which is the let-ring notation. That is a
    //! legitimate mark and upstream is right to produce it when asked, but it is NOT what "slur this
    //! note to the next note" means, and producing it silently would change the meaning of the request
    //! without saying so.
    //!
    //! ⚠️ The unit suite caught this: the test asserting "the last note cannot be slurred" failed
    //! because the call SUCCEEDED and made a self-slur. The check is here rather than after the call
    //! because a self-slur is indistinguishable from a real one by its return value alone.
    mu::engraving::ChordRestNavigateOptions options;
    options.disableOverRepeats = true;
    mu::engraving::ChordRest* target = mu::engraving::Navigation::nextChordRest(start, options);
    if (!target || !target->isChord()) {
        return RecipeResult::failure(
            QStringLiteral("there is no later note to slur %1 to - the next thing after it is a rest or "
                           "the end of the score. (A slur from a note to itself is the let-ring mark, "
                           "which is a different notation and is not made by this operation.)")
            .arg(formatAddress(address)));
    }

    //! ⛔ `Score::addSlur`, NOT a hand-built `Slur`. It does the wiring this would otherwise have to
    //! repeat - tick2, track2, the staff/part test that decides whether the slur crosses staves, and
    //! the slur segment without which the slur exists but is never drawn.
    mu::engraving::Slur* slur = score->addSlur(start, target, nullptr);
    if (!slur) {
        return RecipeResult::failure(QStringLiteral("the slur from %1 could not be created")
                                     .arg(formatAddress(address)));
    }

    return RecipeResult::success(QStringLiteral("%1: slurred to the next note").arg(formatAddress(address)));
}

RecipeResult muse::agentharness::removeSlur(mu::engraving::Score* score, const ScoreAddress& address, int voice)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const NoteLookup found = chordAt(score, address, voice);
    if (!found.found()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::ChordRest* start = found.chord ? static_cast<mu::engraving::ChordRest*>(found.chord)
                                                  : found.rest;
    if (!start) {
        return RecipeResult::failure(QStringLiteral("%1 holds nothing to slur")
                                     .arg(formatAddress(address)));
    }

    //! `ChordRest::slur()` with no argument uses the same "next chord rest" navigation `addSlur` does,
    //! so the slur this finds is the slur that was added - rather than a different one that happens to
    //! overlap.
    mu::engraving::Slur* slur = start->slur();
    if (!slur) {
        //! Refused rather than reported as done: "there was nothing to remove" and "I removed it" are
        //! different answers, and a caller that gets the second when the first is true believes it
        //! changed something.
        return RecipeResult::failure(QStringLiteral("%1 has no slur to the next note, so there is "
                                                    "nothing to remove").arg(formatAddress(address)));
    }

    //! ⛔ `Score::undoRemoveElement`, which is what upstream uses for spanners. A bare `delete` would
    //! take the slur out of the score without telling the undo stack - it would vanish and Ctrl+Z
    //! would not bring it back.
    score->undoRemoveElement(slur);

    return RecipeResult::success(QStringLiteral("%1: removed the slur").arg(formatAddress(address)));
}

RecipeResult muse::agentharness::toggleSlur(mu::engraving::Score* score, const ScoreAddress& address, int voice)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const NoteLookup found = chordAt(score, address, voice);
    if (!found.found()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::ChordRest* start = found.chord ? static_cast<mu::engraving::ChordRest*>(found.chord)
                                                  : found.rest;
    if (start && start->slur()) {
        return removeSlur(score, address, voice);
    }

    return addSlur(score, address, voice);
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
