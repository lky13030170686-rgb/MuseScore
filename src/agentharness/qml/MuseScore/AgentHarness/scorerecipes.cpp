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
#include "engraving/automation/automationdata.h"
#include "engraving/automation/automationtypes.h"
#include "engraving/automation/tempovalues.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/hairpin.h"
#include "engraving/automation/automationdata.h"
#include "engraving/automation/automationtypes.h"
#include "engraving/automation/tempovalues.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/masterscore.h"
#include "engraving/editing/editkeysig.h"
#include "engraving/editing/editchord.h"
#include "engraving/editing/editpart.h"
#include "engraving/editing/edithairpin.h"
#include "engraving/editing/edittimesig.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/timesig.h"
#include "engraving/types/symnames.h"
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
namespace {
//! The measure at a 1-based number, or null. Also reports how many there are, for the message.
mu::engraving::Measure* measureAt(mu::engraving::Score* score, int number, int* total)
{
    int index = 0;
    for (mu::engraving::Measure* m = score->firstMeasure(); m; m = m->nextMeasure(), ++index) {
        if (index + 1 == number) {
            if (total) {
                *total = index + 1;
            }
            return m;
        }
    }
    if (total) {
        *total = index;
    }
    return nullptr;
}

int measureCount(mu::engraving::Score* score)
{
    int n = 0;
    for (mu::engraving::Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++n;
    }
    return n;
}
} // namespace
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

    //! ⛔⛔ A REST IS A LEGITIMATE TARGET, AND REACHING FOR `found.chord` HERE WAS A CRASH.
    //!
    //! `chordAt` returns with only `rest` set on a rest - that is what the `rest` field is FOR - so the
    //! unconditional `found.chord->...` that used to be here dereferenced null. The unit suite caught it
    //! with an access violation, the same way the locator suite caught the identical hole in `noteAt`
    //! (第 94 条). The running program never hit it because nobody had asked to change a rest's length.
    //!
    //! ⚠️ And it is not a case to refuse: "make this rest a half rest" is an ordinary request, and
    //! `undoChangeChordRestLen` takes a `ChordRest` - it was always able to do it. The old code simply
    //! never got that far.
    //! ⚠️ `chordRest()` AND NOT `found.chord`. The distinction used to be spelled out here as a
    //! hand-written ternary, and the identical null dereference was written twice anyway (第 94/112 条) -
    //! so it now lives on `NoteLookup`, where a caller cannot forget it.
    mu::engraving::ChordRest* target = found.chordRest();
    if (!target) {
        return RecipeResult::failure(QStringLiteral("%1 holds nothing whose length can be set")
                                     .arg(formatAddress(address)));
    }

    const QString was = describeDuration(target->durationType());

    if (target->durationType().type() == parsed.value.type()
        && target->durationType().dots() == parsed.value.dots()) {
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
    if (!durationFits(target, parsed.value)) {
        const mu::engraving::Measure* measure = target->findMeasure();
        const mu::engraving::Fraction available = measure ? measure->endTick() - target->tick()
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
    score->undoChangeChordRestLen(target, parsed.value);

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

    mu::engraving::Chord* chord = found.asChord(formatAddress(address)).chordOrNull();
    if (!chord) {
        return RecipeResult::failure(found.asChord(formatAddress(address)).problem);
    }
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

    mu::engraving::Chord* chord = found.asChord(formatAddress(address)).chordOrNull();
    if (!chord) {
        return RecipeResult::failure(found.asChord(formatAddress(address)).problem);
    }

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
        destination = static_cast<mu::engraving::EngravingItem*>(found.chordRest());

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
RecipeResult muse::agentharness::setTempo(mu::engraving::Score* score, int measureNumber,
                                         double beatsPerMinute)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (!(beatsPerMinute > 0.0)) {
        return RecipeResult::failure(QStringLiteral("a tempo must be a positive number of beats per minute "
                                                    "(got %1)").arg(beatsPerMinute));
    }

    const mu::engraving::BeatsPerSecond bps(beatsPerMinute / 60.0);

    //! ⛔ VALIDATED AGAINST THE SCORE'S OWN BOUNDS, not against a number written here. `Constants::MIN_TEMPO`
    //! and `MAX_TEMPO` are what the automation layer clamps to, so refusing outside them keeps this recipe
    //! and the layer from disagreeing about what is acceptable - and a tempo silently clamped to a third of
    //! what was asked is exactly the "looks like it worked" failure this project keeps meeting.
    //! ⚠️ Compared through `.val`, not through the type: `BeatsPerSecond` has no ordering operators, so
    //! `bps < MIN_TEMPO` does not compile - and a silent conversion would not have been better.
    if (bps.val < mu::engraving::Constants::MIN_TEMPO.val
        || bps.val > mu::engraving::Constants::MAX_TEMPO.val) {
        return RecipeResult::failure(
            QStringLiteral("%1 BPM is outside the range this score can hold (%2 to %3 BPM)")
            .arg(beatsPerMinute)
            .arg(mu::engraving::Constants::MIN_TEMPO.val * 60.0, 0, 'f', 0)
            .arg(mu::engraving::Constants::MAX_TEMPO.val * 60.0, 0, 'f', 0));
    }

    int total = 0;
    mu::engraving::Measure* measure = measureAt(score, measureNumber, &total);
    if (!measure) {
        return RecipeResult::failure(QStringLiteral("there is no measure %1; the score has %2")
                                     .arg(measureNumber).arg(total));
    }

    //! ⛔⛔ THE VALUE IS NORMALIZED, AND SKIPPING THIS IS THE WHOLE TRAP. `AutomationPoint::outValue` is a
    //! plain number in `[0,1]` with no unit; tempo is stored as a FRACTION OF THE SCORE'S MAXIMUM
    //! (`tempovalues.h`). Writing `120.0` there would ask for a tempo about two hundred times too fast -
    //! and the score would still save, still play, and still look right on the page.
    mu::engraving::AutomationPoint point;
    point.value.outValue = std::clamp(mu::engraving::normalizeTempo(bps),
                                      mu::engraving::MIN_NORMALIZED_TEMPO,
                                      mu::engraving::MAX_NORMALIZED_TEMPO);
    //! ⚠️ `generated = false`: this point was asked for, not derived from a tempo marking on the page. A
    //! point marked generated would be treated as something the app may recompute and drop.
    point.generated = false;

    mu::engraving::AutomationPointEdit edit;
    edit.tick = int(measure->tick().ticks());
    edit.change = mu::engraving::AutomationPointEdit::SetPoint { point };

    mu::engraving::AutomationPointEdits edits { edit };

    //! ⛔⛔ THE CONTROLLER HAS TO BE INITIALIZED FIRST, AND NOTHING SAYS SO.
    //!
    //! `MasterScore::automationData()` returns whatever the controller holds, and that is NULL until
    //! `ensureInitialized` has run - which normally happens as a side effect of building the tempo timeline
    //! for playback. In a session where nothing has asked for a timeline yet, `automationData()` is null and
    //! `editAutomationPoints` does NOTHING, silently: `EditAutomationPoints::redo` starts with
    //! `IF_ASSERT_FAILED(m_controller->automationData()) { return; }`.
    //!
    //! ⚠️ Measured: the first version of this recipe reported "the tempo point was not written" in a unit
    //! test while the same call would have worked in the running app, where playback had already initialized
    //! the controller. That is the worst kind of difference between test and production - the test was
    //! wrong about the world, not the code.
    //!
    //! ⚠️ `ensureInitialized` is idempotent (`if (m_score) return;`), so calling it unconditionally is safe
    //! and costs one branch.
    //! ⚠️ `tempoTimeline()` and not an `ensureInitialized` method: the controller has no public initializer,
    //! and this accessor is the one that does it (`masterscore.cpp:240`). Its return value is not wanted -
    //! the call is made for the side effect, which is stated here rather than left as a bare expression.
    (void)score->masterScore()->tempoTimeline();

    //! ⛔ `AutomationCurveKey::global(Tempo)` - tempo applies to the whole score, which in this API is the
    //! `std::monostate` scope. Building an instrument- or staff-scoped key would put the point on a curve
    //! nothing reads, and the tempo would not change.
    score->editAutomationPoints(mu::engraving::AutomationCurveKey::global(mu::engraving::AutomationType::Tempo),
                                edits, /* undoable */ true);

    //! ⚠️ VERIFIED BY READING THE CURVE BACK, not by the call returning - `editAutomationPoints` returns
    //! void and can silently do nothing, the same shape as `addTimeSig`. And the read-back has to
    //! DENORMALIZE, or it would compare a `[0,1]` fraction against a BPM and always look wrong.
    const mu::engraving::AutomationDataConstPtr automation = score->automationData();
    if (!automation) {
        return RecipeResult::failure(QStringLiteral("the score has no automation data, so the tempo could "
                                                    "not be set"));
    }

    const mu::engraving::AutomationCurve& curve =
        automation->curve(mu::engraving::AutomationCurveKey::global(mu::engraving::AutomationType::Tempo));
    const auto at = curve.find(int(measure->tick().ticks()));
    if (at == curve.end()) {
        return RecipeResult::failure(QStringLiteral("the tempo point was not written at measure %1")
                                     .arg(measureNumber));
    }

    const double landed = mu::engraving::denormalizeTempo(at->second.value.outValue).val * 60.0;

    return RecipeResult::success(QStringLiteral("measure %1: tempo set to %2 BPM")
                                 .arg(measureNumber).arg(landed, 0, 'f', 1));
}

RecipeResult muse::agentharness::fillMeasureWithRests(mu::engraving::Score* score, int measureNumber)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    int total = 0;
    mu::engraving::Measure* measure = measureAt(score, measureNumber, &total);
    if (!measure) {
        return RecipeResult::failure(QStringLiteral("there is no measure %1; the score has %2")
                                     .arg(measureNumber).arg(total));
    }

    //! ⛔⛔ THE SHORTFALL IS A TOTAL, NOT A GAP, AND THE FIRST TWO VERSIONS OF THIS GOT IT WRONG.
    //!
    //! A bar can be short in two shapes and they need different reasoning:
    //!   - a GAP - nothing between two things, e.g. after a note was removed;
    //!   - a SHORT PIECE - a beat that is simply too brief, e.g. after `note_set_duration` shortened it.
    //!
    //! ⚠️ A short piece leaves NO GAP. The first version filled "from the end of the last thing", which does
    //! nothing at all here. The second walked for the first uncovered tick - and a shortened beat IS covered,
    //! it is just too short, so that did nothing either. Measured both times: the bar stayed at
    //! `Found: 7/8. Expected: 4/4` while the tool reported success.
    //!
    //! ⛔ What both shapes have in common is the thing that actually matters: **how much is missing**. So
    //! this counts what the fullest voice holds and fills the difference at the end - which fixes a gap and
    //! a short piece with the same arithmetic, and cannot be fooled by which shape it happens to be.
    //!
    //! ⚠️ Per voice, and the FULLEST one decides: a bar is complete when every voice reaches the barline, so
    //! the voice holding the most is the one whose shortfall is smallest - and filling that one brings the
    //! bar to its length without overfilling a voice that already held less.
    mu::engraving::Fraction fullest;
    mu::engraving::Fraction fullestEnd = measure->tick();
    bool anyContent = false;
    for (mu::engraving::track_idx_t track = 0; track < score->ntracks(); ++track) {
        mu::engraving::Fraction held;
        mu::engraving::Fraction end = measure->tick();
        for (mu::engraving::Segment* segment = measure->first(mu::engraving::SegmentType::ChordRest); segment;
             segment = segment->next(mu::engraving::SegmentType::ChordRest)) {
            mu::engraving::EngravingItem* item = segment->element(track);
            if (!item || !item->isChordRest()) {
                continue;
            }
            const mu::engraving::ChordRest* chordRest = mu::engraving::toChordRest(item);
            held += chordRest->ticks();
            end = chordRest->endTick();
            anyContent = true;
        }
        if (held > fullest) {
            fullest = held;
            fullestEnd = end;
        }
    }

    const mu::engraving::Fraction measureEnd = measure->endTick();

    if (!anyContent) {
        //! An empty measure: fill the whole bar, and say so as a FULL-MEASURE rest - `useFullMeasureRest`
        //! is what makes it draw as the single centred symbol rather than a rest per beat. That distinction
        //! is the difference between a bar that reads "empty" and a bar that reads "four rests".
        score->setRest(measure->tick(), 0, measure->ticks(), false, nullptr, /* useFullMeasureRest */ true);
        return RecipeResult::success(QStringLiteral("measure %1 was empty; filled it with a full-measure "
                                                    "rest").arg(measureNumber));
    }

    if (fullest == measureEnd - measure->tick()) {
        //! Not an error: the measure already adds up. Reported as success so a caller does not go looking
        //! for another way to do what is already done - the same rule the duration and key-signature
        //! recipes follow.
        return RecipeResult::success(QStringLiteral("measure %1 is already complete")
                                     .arg(measureNumber));
    }

    if (fullest > measureEnd - measure->tick()) {
        //! ⛔ REFUSED, NOT PATCHED. A measure whose content runs past the barline is already broken, and
        //! adding a rest would not fix it - it would add a rest to a bar that has no room for one, making
        //! the breakage harder to see. The caller needs to know the bar is over-full, which is a different
        //! problem from the one this tool solves.
        return RecipeResult::failure(
            QStringLiteral("measure %1 already holds more than it can (%2 of %3), so there is no room for "
                           "rests. Something else made this bar too long; filling it would hide that.")
            .arg(measureNumber)
            .arg(fullest.toString(), measure->ticks().toString()));
    }

    const mu::engraving::Fraction gap = (measureEnd - measure->tick()) - fullest;
    LOGW() << "[agent-fill] m" << measureNumber << "measureTicks=" << measure->ticks().toString()
           << "fullest=" << fullest.toString() << "fullestEnd=" << fullestEnd.ticks()
           << "gap=" << gap.toString() << "any=" << anyContent;

    //! ⚠️ One `setRest` for the whole gap, and `useDots` FALSE: `setRest` splits the gap into the fewest
    //! rests that add up to it, which is what a musician writes. Asking for dots as well would produce a
    //! dotted rest where two plain ones are conventional.
    score->setRest(fullestEnd, 0, gap, /* useDots */ false, nullptr, /* useFullMeasureRest */ false);

    return RecipeResult::success(QStringLiteral("measure %1: filled the last %2 of the bar with rests")
                                 .arg(measureNumber).arg(gap.toString()));
}

RecipeResult muse::agentharness::appendStaff(mu::engraving::Score* score, int partIndex)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const std::vector<mu::engraving::Part*>& parts = score->parts();
    if (partIndex < 0 || partIndex >= int(parts.size())) {
        return RecipeResult::failure(
            QStringLiteral("there is no part %1; the score has %2 (0 to %3)")
            .arg(partIndex).arg(parts.size()).arg(int(parts.size()) - 1));
    }

    mu::engraving::Part* part = parts[size_t(partIndex)];
    const size_t before = part->nstaves();

    //! ⛔ `EditPart::appendStaff`, upstream's own primitive. It does the wiring this would otherwise have
    //! to repeat - and each piece of that wiring is a way to produce a staff that EXISTS but does not work:
    //!   - `undoInsertStaff` puts it on the undo stack (a hand-linked `Staff` would not be undoable);
    //!   - `adjustKeySigs` gives it the part's key signature (without it the new staff has none, so it
    //!     reads in a different key from the staff above it);
    //!   - `updateBracesAndBarlines` joins it to the part's brace and barline group (without it the staff
    //!     hangs outside the bracket, which reads as a separate instrument);
    //!   - and the instrument's clef list is extended (without it the staff has no clef of its own).
    mu::engraving::Staff* added = mu::engraving::EditPart::appendStaff(score, part);
    if (!added) {
        return RecipeResult::failure(QStringLiteral("the staff could not be added to part %1")
                                     .arg(partIndex));
    }

    //! ⚠️ VERIFIED BY EFFECT: `appendStaff` returns a pointer, and a non-null pointer is not evidence that
    //! the part grew - the same shape as the measure insert. Counting is.
    if (part->nstaves() != before + 1) {
        return RecipeResult::failure(QStringLiteral("the part still has %1 staff/staves after the call, so "
                                                    "nothing was added").arg(part->nstaves()));
    }

    return RecipeResult::success(QStringLiteral("added a staff to part %1, which now has %2 (the new one is "
                                                "staff %3 of the score)")
                                 .arg(partIndex).arg(part->nstaves()).arg(added->idx() + 1));
}

RecipeResult muse::agentharness::removeLastStaff(mu::engraving::Score* score, int partIndex)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    const std::vector<mu::engraving::Part*>& parts = score->parts();
    if (partIndex < 0 || partIndex >= int(parts.size())) {
        return RecipeResult::failure(
            QStringLiteral("there is no part %1; the score has %2 (0 to %3)")
            .arg(partIndex).arg(parts.size()).arg(int(parts.size()) - 1));
    }

    mu::engraving::Part* part = parts[size_t(partIndex)];

    //! ⛔ REFUSED WHEN IT IS THE ONLY STAFF, for the same reason the score refuses to lose its last
    //! measure: a part with no staves is not a small part, it is a broken one - the notation layer has
    //! nowhere to put the cursor for that part, and the part's instrument is still there claiming a staff
    //! that does not exist. Removing it is a request to remove the PART, which is a different operation
    //! this tool deliberately does not do.
    if (part->nstaves() <= 1) {
        return RecipeResult::failure(
            QStringLiteral("part %1 has only one staff, and a part cannot have none. To remove the whole "
                           "part, use the score's own staff/part dialog - this tool does not remove "
                           "parts.").arg(partIndex));
    }

    const size_t before = part->nstaves();
    mu::engraving::Staff* last = part->staff(before - 1);
    if (!last) {
        return RecipeResult::failure(QStringLiteral("the part's last staff could not be found"));
    }

    //! `EditPart::removeStaves` rather than `Score::undoRemoveStaff`: the plural form is the one upstream's
    //! own part dialog calls, and it is the one that also fixes up the braces, the barlines and the
    //! instrument's clef list - so the remaining staves are left in a consistent group rather than a group
    //! with a hole in it.
    mu::engraving::EditPart::removeStaves(score, { last });

    if (part->nstaves() != before - 1) {
        return RecipeResult::failure(QStringLiteral("the part still has %1 staff/staves after the call, so "
                                                    "nothing was removed").arg(part->nstaves()));
    }

    return RecipeResult::success(QStringLiteral("removed the last staff of part %1, which now has %2")
                                 .arg(partIndex).arg(part->nstaves()));
}

RecipeResult muse::agentharness::toggleArticulation(mu::engraving::Score* score, const ScoreAddress& address,
                                                   int voice, int noteIndex, const QString& name)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (name.trimmed().isEmpty()) {
        return RecipeResult::failure(QStringLiteral("no articulation given, e.g. `articStaccatoAbove`"));
    }

    //! ⛔⛔ `SymNames::symIdByName` RETURNS A DEFAULT RATHER THAN FAILING - `SymId::noSym` when the name
    //! is not one it knows. So an unchecked lookup would go on to create an articulation with no symbol,
    //! which draws as nothing and is an object in the score nobody can see or select. The default is
    //! checked for exactly that reason, the same way the dynamic marking's `OTHER` fallback is.
    const mu::engraving::SymId symbol = mu::engraving::SymNames::symIdByName(muse::String(name.trimmed()));
    if (symbol == mu::engraving::SymId::noSym) {
        return RecipeResult::failure(
            QStringLiteral("`%1` is not an articulation name I know. The names are the score format's own "
                           "SMuFL names, e.g. `articStaccatoAbove`, `articAccentAbove`, "
                           "`articTenutoAbove`, `articMarcatoAbove`, `articStaccatissimoAbove`.")
            .arg(name));
    }

    //! ⚠️ The NOTE is required here, unlike the segment-level elements: an articulation hangs off a note,
    //! so a rest has nothing to attach it to. `noteAt` reports that with its own message.
    const NoteLookup found = noteAt(score, address, voice, noteIndex);
    if (!found.ok()) {
        return RecipeResult::failure(found.problem.isEmpty()
                                     ? QStringLiteral("the note lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : found.problem);
    }

    mu::engraving::Chord* chord = found.asChord(formatAddress(address)).chordOrNull();
    if (!chord) {
        return RecipeResult::failure(found.asChord(formatAddress(address)).problem);
    }
    //! `hasArticulation` takes an articulation to compare against, so one is built to ask the question. It
    //! is not added - `EditChord::toggleArticulation` builds its own - so it is deleted immediately;
    //! leaking it here would be a leak per call.
    mu::engraving::Articulation* probe =
        mu::engraving::Factory::createArticulation(score->dummy()->chord());
    probe->setSymId(symbol);
    const bool wasThere = chord->hasArticulation(probe) != nullptr;
    delete probe;

    //! ⛔ `EditChord::toggleArticulation`, upstream's own primitive. It adds when absent and removes when
    //! present, both through the undo stack. Writing the add by hand would leave the "already there" case
    //! producing a SECOND articulation - two staccato dots stacked on one notehead, which is what a
    //! caller asking twice would get.
    mu::engraving::Articulation* articulation =
        mu::engraving::Factory::createArticulation(score->dummy()->chord());
    articulation->setSymId(symbol);
    if (!mu::engraving::EditChord::toggleArticulation(score, found.note, articulation)) {
        //! ⚠️ `toggleArticulation` returns false in TWO different situations - it removed one, and it
        //! could not act at all - so the return value alone cannot be reported. The state read before the
        //! call is what tells them apart, and getting this wrong would report a removal as a failure.
        delete articulation;
        if (wasThere) {
            return RecipeResult::success(QStringLiteral("%1: removed %2").arg(formatAddress(address), name));
        }
        return RecipeResult::failure(QStringLiteral("the articulation could not be added at %1")
                                     .arg(formatAddress(address)));
    }

    return RecipeResult::success(QStringLiteral("%1: added %2").arg(formatAddress(address), name));
}

RecipeResult muse::agentharness::moveNote(mu::engraving::Score* score, const ScoreAddress& from,
                                          int noteIndex, const ScoreAddress& to)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (from.measure == to.measure && from.beat == to.beat && from.staff == to.staff) {
        return RecipeResult::failure(QStringLiteral("%1 and %2 are the same beat, so there is nowhere to "
                                                    "move the note to")
                                     .arg(formatAddress(from), formatAddress(to)));
    }

    //! ── VALIDATE EVERYTHING BEFORE WRITING ANYTHING ───────────────────────────────────────────────
    //!
    //! ⛔ A move is a delete plus an add, so every reason the ADD could fail has to be found while the
    //! note is still in place. Finding it afterwards means the note is already gone and the caller is
    //! told the move failed - the score changed AND the operation reported as not having happened.

    const NoteLookup source = noteAt(score, from, 0, noteIndex);
    if (!source.ok()) {
        return RecipeResult::failure(source.problem.isEmpty()
                                     ? QStringLiteral("the source lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : source.problem);
    }

    const int pitch = source.note->pitch();
    const bool sourceChordWouldEmpty = source.chordOrNull()->notes().size() <= 1;

    const NoteLookup target = chordAt(score, to, 0);
    if (!target.found()) {
        return RecipeResult::failure(target.problem.isEmpty()
                                     ? QStringLiteral("the target lookup failed without saying why "
                                                      "(internal) - nothing was changed")
                                     : target.problem);
    }

    //! ⛔ THE TARGET MUST BE A CHORD. A rest is not something a note can be moved into - the note has to
    //! go ONTO something, and "make this rest a note" is `note_add` on a beat that already has one.
    if (!target.chordOrNull()) {
        return RecipeResult::failure(
            QStringLiteral("%1 is a rest. A note can only be moved onto a beat that already has one; to put "
                           "a note on a silent beat, add it there instead.").arg(formatAddress(to)));
    }

    //! ⛔ THE TARGET MUST NOT ALREADY HAVE THIS PITCH. Two notes at one pitch is not a chord - it draws as
    //! one notehead while every later read sees two, which reads as "the chord has a note I cannot see".
    if (target.chordOrNull()->findNote(pitch)) {
        return RecipeResult::failure(QStringLiteral("%1 already has a %2, so moving the note there would "
                                                    "put two notes at the same pitch")
                                     .arg(formatAddress(to), pitchName(pitch)));
    }

    //! ⛔ AND IF THE SOURCE WOULD BE EMPTIED, THE TARGET HAS TO BE ABLE TO TAKE IT. `removeNote` refuses
    //! to leave an empty chord - correctly, since that is not a rest - so a move that would empty the
    //! source has to be refused HERE, while nothing has been written, rather than half-done.
    if (sourceChordWouldEmpty) {
        return RecipeResult::failure(
            QStringLiteral("%1 holds only this note, so moving it would leave an empty chord - which is "
                           "not a rest. To silence %1 use note_to_rest, then add the note at %2.")
            .arg(formatAddress(from), formatAddress(to)));
    }

    //! ── THE TWO HALVES, IN THE ORDER THAT KEEPS THE NOTE ALIVE ────────────────────────────────────
    //!
    //! The target gains the pitch BEFORE the source loses it. If the add somehow fails anyway, the score
    //! holds the note twice - wrong, but recoverable with one undo, and nothing has been LOST. The other
    //! order would lose the note.

    const RecipeResult added = addNoteToChord(score, to, 0, pitch);
    if (!added.ok) {
        return RecipeResult::failure(QStringLiteral("could not add the note at %1: %2")
                                     .arg(formatAddress(to), added.problem));
    }

    const RecipeResult removed = removeNote(score, from, 0, noteIndex);
    if (!removed.ok) {
        //! ⚠️ Reported as a failure WITH the state spelled out, because the score now holds the note
        //! twice. Saying only "failed" would leave the caller believing nothing happened.
        return RecipeResult::failure(
            QStringLiteral("the note was added at %1 but could not be removed from %2: %3. The score now "
                           "has the note in BOTH places - one undo will take back the addition.")
            .arg(formatAddress(to), formatAddress(from), removed.problem));
    }

    return RecipeResult::success(QStringLiteral("moved %1 from %2 to %3")
                                 .arg(pitchName(pitch), formatAddress(from), formatAddress(to)));
}


RecipeResult muse::agentharness::insertMeasures(mu::engraving::Score* score, int beforeMeasure, int count)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }
    if (count < 1) {
        return RecipeResult::failure(QStringLiteral("`count` must be at least 1 (got %1)").arg(count));
    }

    const int total = measureCount(score);

    //! ⚠️ ONE PAST THE LAST MEASURE IS ALLOWED AND MEANS "APPEND". Refusing it would leave the caller no
    //! way to add a bar at the end - the commonest case of all - and clamping it silently would put the
    //! new bar somewhere the caller did not ask for.
    if (beforeMeasure < 1 || beforeMeasure > total + 1) {
        return RecipeResult::failure(
            QStringLiteral("there is no measure %1 to insert before; the score has %2, so use 1 to %3 "
                           "(where %3 appends at the end)").arg(beforeMeasure).arg(total).arg(total + 1));
    }

    mu::engraving::Measure* before = measureAt(score, beforeMeasure, nullptr);
    for (int i = 0; i < count; ++i) {
        //! ⛔ `Score::insertMeasure`, which wraps upstream's own `InsertMeasures` undo command. Building a
        //! `Measure` by hand and linking it in - which is what `MasterScore` does internally - would put a
        //! measure in the score WITHOUT telling the undo stack, so Ctrl+Z would not take it back.
        if (!score->insertMeasure(mu::engraving::ElementType::MEASURE, before)) {
            return RecipeResult::failure(QStringLiteral("the measure could not be inserted"));
        }
    }

    const int now = measureCount(score);
    return RecipeResult::success(
        QStringLiteral("inserted %1 measure(s) before measure %2; the score now has %3 (measure numbers "
                       "from %2 on have shifted by %1)").arg(count).arg(beforeMeasure).arg(now));
}

RecipeResult muse::agentharness::removeMeasures(mu::engraving::Score* score, int first, int last)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }
    if (first < 1 || last < first) {
        return RecipeResult::failure(QStringLiteral("give a range like `first` <= `last`, both at least 1 "
                                                    "(got %1 to %2)").arg(first).arg(last));
    }

    const int total = measureCount(score);
    if (last > total) {
        return RecipeResult::failure(QStringLiteral("there is no measure %1; the score has %2")
                                     .arg(last).arg(total));
    }

    //! ⛔ REFUSED WHEN IT WOULD EMPTY THE SCORE. A score with no measures is not a short score - the
    //! notation layer has nowhere to put the cursor, and several upstream paths assume a first measure
    //! exists. Deleting the last one is the caller getting the arithmetic wrong, and it should hear so.
    if (last - first + 1 >= total) {
        return RecipeResult::failure(
            QStringLiteral("removing measures %1-%2 would leave the score with no measures at all. A score "
                           "needs at least one measure; remove %3 of them at most.")
            .arg(first).arg(last).arg(total - 1));
    }

    mu::engraving::Measure* startMeasure = measureAt(score, first, nullptr);
    mu::engraving::Measure* endMeasure = measureAt(score, last, nullptr);
    if (!startMeasure || !endMeasure) {
        return RecipeResult::failure(QStringLiteral("the measures could not be found"));
    }

    //! ⛔ `Score::undoRemoveMeasures`, which wraps `RemoveMeasures` - the same reasoning as the insert
    //! above. `preserveTies` is left at its default, because a tie running into a bar that no longer
    //! exists is exactly the dangling state this project keeps guarding against.
    score->undoRemoveMeasures(startMeasure, endMeasure);

    const int now = measureCount(score);
    return RecipeResult::success(
        QStringLiteral("removed measures %1-%2; the score now has %3 (measure numbers after %1 have "
                       "shifted back by %4)").arg(first).arg(last).arg(now).arg(last - first + 1));
}

namespace {
//! The chord or rest at an address, as a `ChordRest*`, for elements that hang off a beat.
//!
//! ⚠️ Dynamics and hairpins attach to the SEGMENT, not to a note, so no note index is involved - which is
//! why this returns the chord rest rather than going through `noteAt`.
mu::engraving::ChordRest* chordRestAt(mu::engraving::Score* score, const ScoreAddress& address,
                                      QString& problem)
{
    const NoteLookup found = chordAt(score, address, 0);
    if (!found.found()) {
        problem = found.problem.isEmpty()
                  ? QStringLiteral("the lookup failed without saying why (internal) - nothing was changed")
                  : found.problem;
        return nullptr;
    }
    return found.chordRest();
}
} // namespace

RecipeResult muse::agentharness::addDynamic(mu::engraving::Score* score, const ScoreAddress& address,
                                           const QString& mark)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    if (mark.trimmed().isEmpty()) {
        return RecipeResult::failure(QStringLiteral("no dynamic marking given, e.g. `mf`"));
    }

    QString problem;
    mu::engraving::ChordRest* at = chordRestAt(score, address, problem);
    if (!at) {
        return RecipeResult::failure(problem);
    }

    mu::engraving::Dynamic* dynamic = mu::engraving::Factory::createDynamic(at->segment());
    dynamic->setTrack(at->track());

    //! ⛔⛔ VALIDATED AGAINST A LIST, because the parser does NOT report failure.
    //!
    //! `setDynamicType(String)` runs a regex and falls back to `DynamicType::OTHER` for anything it cannot
    //! place, storing the raw text as an uninterpreted blob. So `12345` would come back as a dynamic that
    //! draws as nothing meaningful, and the caller would be told "added the dynamic 12345".
    //!
    //! ⚠️ And the check cannot be "did it return OTHER" either: the regex is `[fmnprsz]+`, and a marking
    //! really can be spelled from those letters - `xyzzy` matches it and yields a non-OTHER type. (That
    //! was the first version of this test's mistake, and the mistake was in the test data, not the parser.)
    //! An explicit list is the only check that means what it says.
    static const QStringList kMarkings = {
        "pppppp", "ppppp", "pppp", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ffff", "fffff",
        "ffffff", "fp", "pf", "sf", "sfz", "sff", "sffz", "sfff", "sfffz", "rf", "rfz", "fz",
    };
    if (!kMarkings.contains(mark.trimmed().toLower())) {
        delete dynamic;
        return RecipeResult::failure(
            QStringLiteral("`%1` is not a dynamic marking I know. Use one of: %2")
            .arg(mark, kMarkings.join(QStringLiteral(", "))));
    }

    dynamic->setDynamicType(muse::String(mark));
    if (dynamic->dynamicType() == mu::engraving::DynamicType::OTHER) {
        //! Belt and braces: the list above and the parser should agree, and if they ever stop agreeing this
        //! is where it shows up rather than in a score carrying a marking nothing can read.
        delete dynamic;
        return RecipeResult::failure(
            QStringLiteral("`%1` is in the accepted list but the score format's own parser did not "
                           "recognise it - that is a bug in this tool, not in your request").arg(mark));
    }

    dynamic->setOwnershipParent(at->segment());
    score->undoAddElement(dynamic);

    return RecipeResult::success(QStringLiteral("%1: added the dynamic %2")
                                 .arg(formatAddress(address), mark));
}

RecipeResult muse::agentharness::addHairpin(mu::engraving::Score* score, const ScoreAddress& from,
                                           const ScoreAddress& to, const QString& kind)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    mu::engraving::HairpinType type = mu::engraving::HairpinType::INVALID;
    if (kind == QLatin1String("crescendo")) {
        type = mu::engraving::HairpinType::CRESC_HAIRPIN;
    } else if (kind == QLatin1String("diminuendo")) {
        type = mu::engraving::HairpinType::DIM_HAIRPIN;
    } else {
        return RecipeResult::failure(QStringLiteral("`%1` is not a hairpin kind; use `crescendo` or "
                                                    "`diminuendo`").arg(kind));
    }

    QString problem;
    mu::engraving::ChordRest* start = chordRestAt(score, from, problem);
    if (!start) {
        return RecipeResult::failure(problem);
    }

    //! ⛔ THE END IS RESOLVED HERE, NOT LEFT TO UPSTREAM. Passing a null `cr2` makes `addHairpin` use the
    //! next chord rest, which is right when the caller did not say - but it is ALSO what it does when the
    //! caller named an end that does not resolve, so "I named bar 2" and "I named nothing" would produce
    //! the same hairpin. Resolving it here keeps the two apart, and a wrong address stays an error.
    mu::engraving::ChordRest* end = nullptr;
    if (to.measure == from.measure && to.beat == from.beat && to.staff == from.staff) {
        end = nullptr;   //!< same beat -> let upstream run to the next chord rest
    } else {
        end = chordRestAt(score, to, problem);
        if (!end) {
            return RecipeResult::failure(problem);
        }
    }

    mu::engraving::Transaction& tx = score->transactionManager()->currentOrDummyTransaction();
    mu::engraving::Hairpin* hairpin = mu::engraving::EditHairpin::addHairpin(tx, score, type, start, end);
    if (!hairpin) {
        return RecipeResult::failure(QStringLiteral("the hairpin could not be created at %1")
                                     .arg(formatAddress(from)));
    }

    if (end) {
        return RecipeResult::success(QStringLiteral("added a %1 from %2 to %3")
                                     .arg(kind, formatAddress(from), formatAddress(to)));
    }
    return RecipeResult::success(QStringLiteral("added a %1 from %2 to the next beat")
                                 .arg(kind, formatAddress(from)));
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

    mu::engraving::Chord* chord = found.asChord(formatAddress(address)).chordOrNull();
    if (!chord) {
        return RecipeResult::failure(found.asChord(formatAddress(address)).problem);
    }

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

    //! ⛔⛔ THE REST CHECK COMES FIRST, AND A BLIND CONVERSION PUT IT SECOND - which the existing test
    //! caught immediately (第 93 条的回归守卫).
    //!
    //! `asChord()` FAILS on a rest, by design. Asking for the chord before asking "is this already a rest"
    //! therefore turns the "already silent, nothing to do" answer into "there is no chord to work on" -
    //! i.e. it re-breaks the exact case that made the whole-measure-rest branch reachable in the first
    //! place. The order here is not stylistic: the rest answer is only available to a caller that asks for
    //! it before demanding a chord.
    if (found.isRest()) {
        //! Not an error: it is already silent. Reported as success so the caller does not go looking for
        //! another way to do what is already done.
        return RecipeResult::success(QStringLiteral("%1 is already a rest").arg(formatAddress(address)));
    }

    mu::engraving::Chord* chord = found.chordOrNull();
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
    //! ⚠️ `chordRest()` and not a hand-written ternary: a slur or a text attaches to either, and
    //! the ternary is exactly the shape that was written wrong elsewhere (第 112 条).
    mu::engraving::ChordRest* start = found.chordRest();
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

    //! ⚠️ `chordRest()` and not a hand-written ternary: a slur or a text attaches to either, and
    //! the ternary is exactly the shape that was written wrong elsewhere (第 112 条).
    mu::engraving::ChordRest* start = found.chordRest();
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

    //! ⚠️ `chordRest()` and not a hand-written ternary: a slur or a text attaches to either, and
    //! the ternary is exactly the shape that was written wrong elsewhere (第 112 条).
    mu::engraving::ChordRest* start = found.chordRest();
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