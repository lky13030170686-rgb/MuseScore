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
#include "scoredigest.h"

#include <algorithm>
#include <QMap>
#include <QStringList>

#include "engraving/dom/score.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/sig.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/note.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/volta.h"
#include "engraving/types/symnames.h"
#include "engraving/types/typesconv.h"

//! `describeKeySignature` - the SAME wording the write path uses, so "what key is it in" and "what did
//! you just set" cannot describe the same key two different ways (第 132 条).
#include "scorerecipes.h"

using namespace mu::engraving;

namespace muse::agentharness {
QString pitchName(int midiPitch)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    if (midiPitch < 0 || midiPitch > 127) {
        return QStringLiteral("?");
    }

    //! Scientific pitch notation: MIDI 60 is C4. Getting this off by an octave is the kind of error
    //! that reads as plausible in a digest and misleads every downstream decision, so it is pinned
    //! by a unit test rather than reasoned about at each call site.
    const int octave = midiPitch / 12 - 1;
    return QString::fromLatin1(names[midiPitch % 12]) + QString::number(octave);
}

namespace {
//! Per-staff accumulation, filled by one traversal.
struct StaffFacts
{
    int notes = 0;
    int rests = 0;
    int chords = 0;
    int lowestPitch = 128;
    int highestPitch = -1;
    int voicesUsed = 0;
    //! One bit per voice, so "which voices are in use" costs nothing extra.
    unsigned voiceMask = 0;
};

//! The traversal. NOTE the shape: `ntracks()` on the outside, `segment->element(track)` on the
//! inside. That is the only correct way to see everything at a tick - `Segment::elist()` is sized
//! `nstaves * VOICES` and a chord's own `notes()` only expands the chord you already have.
//! `Score::tick2segment()` is NOT a substitute: it returns a segment only when the tick lands
//! exactly on one.
void collectStaffFacts(const Score* score, QMap<int, StaffFacts>& out, QMap<int, int>& measureNoteCounts)
{
    int measureIndex = 0;

    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure(), ++measureIndex) {
        int inThisMeasure = 0;

        for (const Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            for (track_idx_t track = 0; track < score->ntracks(); ++track) {
                EngravingItem* item = segment->element(track);
                if (!item) {
                    continue;
                }

                const int staff = int(track / VOICES);
                const int voice = int(track % VOICES);
                StaffFacts& facts = out[staff];

                if (item->isRest()) {
                    facts.rests++;
                    facts.voiceMask |= (1u << voice);
                    continue;
                }

                if (!item->isChord()) {
                    continue;
                }

                const Chord* chord = toChord(item);
                //! Grace notes have no independent position in time; counting them would inflate
                //! the note count and skew the pitch range with ornament pitches.
                if (chord->isGrace()) {
                    continue;
                }

                facts.chords++;
                facts.voiceMask |= (1u << voice);

                for (const Note* note : chord->notes()) {
                    facts.notes++;
                    inThisMeasure++;
                    const int pitch = note->pitch();
                    facts.lowestPitch = std::min(facts.lowestPitch, pitch);
                    facts.highestPitch = std::max(facts.highestPitch, pitch);
                }
            }
        }

        measureNoteCounts[measureIndex] = inThisMeasure;
    }

    for (auto it = out.begin(); it != out.end(); ++it) {
        unsigned mask = it.value().voiceMask;
        int count = 0;
        while (mask) {
            count += int(mask & 1u);
            mask >>= 1;
        }
        it.value().voicesUsed = count;
    }
}

//! Where the time signature changes, as "m<N> <n>/<d>" entries. Only the changes: a digest that
//! repeated the same 4/4 for every measure would be mostly noise.
QStringList timesigChanges(const Score* score)
{
    QStringList out;
    QString previous;

    int measureIndex = 0;
    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure(), ++measureIndex) {
        const TimeSigFrac timesig(measure->timesig());
        const QString current = QStringLiteral("%1/%2").arg(timesig.numerator()).arg(timesig.denominator());
        if (current != previous) {
            out.append(QStringLiteral("m%1 %2").arg(measureIndex + 1).arg(current));
            previous = current;
        }
    }

    return out;
}

//! The key signature, as a line for the overview: staff 1's changes through the score, and - when another
//! staff disagrees - who disagrees.
//!
//! ⛔ WHY THIS EXISTS AT ALL (第 132 条). Asked "what key is it in?", the agent answered, twice and in its
//! own words, that it could not tell: the overview printed time signatures and pitch spellings but no key
//! signature, so the only honest answer was "every note is a natural, so nothing contradicts C major /
//! A minor, but I will not claim it". That is the tool's gap, not the model's - and "what key is this in"
//! is one of the first questions a musician asks.
//!
//! ⚠️ Reported per STAFF-1 plus a disagreement note rather than as one score-wide value: a Bb clarinet is
//! WRITTEN in a different key from the piano, so a single "the key is X" line would be wrong for whichever
//! staff it did not come from. The common case (everyone agrees) stays one short line.
QString keySignatureLine(const Score* score)
{
    if (!score || score->nstaves() == 0 || !score->firstMeasure()) {
        return QString();
    }

    const Staff* first = score->staff(0);
    if (!first) {
        return QString();
    }

    QStringList changes;
    QString previous;
    int measureIndex = 0;
    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure(), ++measureIndex) {
        const QString current = describeKeySignature(int(first->key(measure->tick())));
        if (current != previous) {
            changes.append(QStringLiteral("m%1 %2").arg(measureIndex + 1).arg(current));
            previous = current;
        }
    }

    if (changes.isEmpty()) {
        return QString();
    }

    QString line = QStringLiteral("key signatures (staff 1): %1").arg(changes.join(QStringLiteral(", ")));

    const Fraction startTick = score->firstMeasure()->tick();
    QStringList others;
    for (size_t i = 1; i < score->nstaves(); ++i) {
        const Staff* staff = score->staff(i);
        if (!staff) {
            continue;
        }
        const QString key = describeKeySignature(int(staff->key(startTick)));
        if (key != describeKeySignature(int(first->key(startTick)))) {
            others.append(QStringLiteral("staff %1: %2").arg(i + 1).arg(key));
        }
    }

    //! ⚠️ Said out loud rather than averaged away: a summary that reported one staff's key as "the" key of
    //! a score whose staves disagree would be a plausible-looking wrong answer, which is the failure mode
    //! this whole subsystem keeps refusing.
    if (!others.isEmpty()) {
        line += QStringLiteral("  ⚠️ other staves differ at m1 - %1").arg(others.join(QStringLiteral("; ")));
    }

    return line;
}

//! ══ Layout facts: clefs, repeat structure, endings (第 134 条) ══════════════════════════════════════
//!
//! ⛔ WHY THIS EXISTS. The window could describe pitch, duration, key, time signature, articulations,
//! dynamics, ties and slurs - and said NOTHING about the two layout facts that decide how the page and the
//! playback are actually organised: **the clef** (which decides what every printed pitch MEANS) and **the
//! repeat structure** (which decides how many times a passage is played at all).
//!
//! Measured 2026-10-11, both halves were reachable and invisible:
//!   * the CLEF is writable by the toolset itself - `command://notation/add-clef-violin` /
//!     `add-clef-bass` are in the command list the agent is given, and they end in
//!     `EditClef::undoChangeClef` - so the agent could change the clef and then be unable to see it;
//!   * the REPEAT signs come from the user's own hands (`Measure::drop` for a barline ->
//!     `undoChangeProperty(Pid::REPEAT_START/REPEAT_END)`), which is the half of the score the timeline
//!     records but no tool can read back.

//! A measure index (1-based) for a tick, or -1 when the tick is in no measure at all.
//! ⚠️ The digest counts measures by POSITION (`firstMeasure()` + `nextMeasure()`), not by
//! `Measure::measureNumber()`: an anacrusis or a numbering offset would make the two disagree, and every
//! other line in this file (and every address the write tools take) speaks positions.
int measurePositionAtTick(const Score* score, const Fraction& tick)
{
    int index = 1;
    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure(), ++index) {
        if (tick >= measure->tick() && tick < measure->endTick()) {
            return index;
        }
    }
    return -1;
}

//! The clef in force on a staff at a tick, as the FILE FORMAT's own tag ("G", "F", "C3", "PERC", "TAB8"...).
//!
//! ⛔ `TConv::toXml(ClefType)`, NOT `TConv::userName`: the user name is TRANSLATABLE ("Treble clef" /
//! 高音谱号), so using it would make the same score read differently depending on the UI language - and a
//! fact that changes with the locale is not something two readers can agree on. The xml tag is also the
//! exact string `TConv::fromXml` accepts, so the read side and any future write side speak one vocabulary
//! (the same rule `describeKeySignature` and `TConv::toXml(DynamicType)` already follow here).
//!
//! ⚠️ Read from `Staff::clef()`, i.e. the CLEF LIST, rather than by scanning `Clef` elements:
//!   * the list is what the engraving layer itself asks when it needs to know which clef applies at a
//!     tick (it is maintained by `Segment::add` whenever a clef element is added - including while the
//!     file is being read), so it cannot disagree with the drawn clef;
//!   * it answers for ticks where NO clef element exists at all: a staff with no explicit clef still has
//!     one, coming from the instrument via `Staff::clefType`'s fallback. A segment scan would report
//!     "no clef" for every ordinary staff in the score.
QString clefNameAt(const Score* score, size_t staffIdx, const Fraction& tick)
{
    const Staff* staff = score ? score->staff(staffIdx) : nullptr;
    if (!staff) {
        return QString();
    }

    return QString(TConv::toXml(staff->clef(tick)).toQLatin1String());
}

//! One clef change: the tick it TAKES EFFECT at, which staff, and what it is.
struct ClefChange
{
    Fraction tick;
    int staff = 0;          //!< 0-based, printed as `s<staff + 1>`
    QString name;
};

//! Every clef change that takes effect inside `[from, to)`, all staves, in tick order.
//!
//! ⛔ WHY NOT "sample the clef at each measure start and report the difference". A clef change is NOT
//! measure-aligned: MuseScore writes the clef that starts the next bar at the END of the previous one
//! (that is where it is drawn - before the barline), and a change in the middle of a measure is ordinary
//! notation. Sampling measure starts would report a mid-measure change one measure LATE - a
//! plausible-looking wrong bar number, which is the one failure this subsystem exists to refuse.
//!
//! ⚠️ The walk goes through `Staff::nextClefTick()`, which returns `score->endTick()` as its "there is no
//! next clef" sentinel. That sentinel can never become a phantom change here: the range is half-open, so
//! a tick equal to the window's end is excluded, and the window's end is never past the score's.
std::vector<ClefChange> clefChangesIn(const Score* score, const Fraction& from, const Fraction& to)
{
    std::vector<ClefChange> out;
    if (!score || to <= from) {
        return out;
    }

    for (size_t i = 0; i < score->nstaves(); ++i) {
        const Staff* staff = score->staff(i);
        if (!staff) {
            continue;
        }

        Fraction cursor = from;
        while (true) {
            const Fraction next = staff->nextClefTick(cursor);
            //! `next <= cursor` cannot happen with the current implementation (the lookup is an
            //! `upper_bound`), but a loop whose exit depends on that is a hang waiting for an upstream
            //! change - so it is checked rather than assumed.
            if (next <= cursor || next >= to) {
                break;
            }

            const QString name = clefNameAt(score, i, next);
            if (!name.isEmpty()) {
                out.push_back(ClefChange { next, int(i), name });
            }
            cursor = next;
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const ClefChange& a, const ClefChange& b) {
        return a.tick < b.tick;
    });

    return out;
}

//! `1st`, `2nd`, `3rd`, `11th`... for the volta endings. Plain English on purpose: the digest does not go
//! through the translation layer (see `clefNameAt`), and "ending 2" alone does not say whether it is a
//! number of an ending or a count of them.
QString ordinal(int n)
{
    const int lastTwo = n % 100;
    QString suffix = QStringLiteral("th");
    if (lastTwo < 11 || lastTwo > 13) {
        switch (n % 10) {
        case 1: suffix = QStringLiteral("st");
            break;
        case 2: suffix = QStringLiteral("nd");
            break;
        case 3: suffix = QStringLiteral("rd");
            break;
        default:
            break;
        }
    }

    return QString::number(n) + suffix;
}

//! The endings (voltas, "1st/2nd time bars") that INTERSECT `[from, to)`, as `1st ending m3-m4`.
//!
//! ⛔ An ending is a SPAN, which is why this is a window-level line rather than a per-measure token: a
//! token printed only where the ending starts hides that the following measures are inside it, and one
//! repeated on every measure it covers costs a token per bar to say the same thing. Two explicit measure
//! numbers answer "how does this repeat work" in one line.
//!
//! ⛔ `tick2()` is the tick the ending ENDS AT, i.e. the beginning of the measure after it - so the last
//! covered measure is the one containing `tick2 - 1 tick`, not the one containing `tick2`. Getting this
//! wrong prints an ending one measure too long, and a range that is off by one reads completely ordinary
//! (第 101 条 was exactly this shape with a hairpin's end).
QStringList voltaSpans(const Score* score, const Fraction& from, const Fraction& to)
{
    QStringList out;
    if (!score || !score->firstMeasure()) {
        return out;
    }

    for (const auto& entry : score->spannerMap().map()) {
        const Spanner* spanner = entry.second;
        if (!spanner || !spanner->isVolta()) {
            continue;
        }

        const Fraction start = spanner->tick();
        const Fraction end = spanner->tick2();
        //! Half-open on both sides: an ending that merely touches the window is not in it.
        if (end <= from || start >= to) {
            continue;
        }

        const Volta* volta = toVolta(spanner);
        QStringList names;
        for (int ending : volta->endings()) {
            names.append(ordinal(ending));
        }
        const QString what = names.isEmpty()
                             ? QStringLiteral("unnumbered ending")
                             : QStringLiteral("%1 ending").arg(names.join(QLatin1Char('+')));

        const int firstMeasure = measurePositionAtTick(score, start);
        const int lastMeasure = measurePositionAtTick(score, Fraction::fromTicks(std::max(end.ticks() - 1, start.ticks())));
        if (firstMeasure < 1 || lastMeasure < 1) {
            //! Said rather than guessed: an ending anchored outside any measure is a broken score, and
            //! inventing a measure number for it would be the digest making something up.
            out.append(QStringLiteral("%1 (measures could not be resolved)").arg(what));
        } else {
            out.append(QStringLiteral("%1 m%2-m%3").arg(what).arg(firstMeasure).arg(lastMeasure));
        }
    }

    return out;
}

//! The repeat structure of the whole score: every `|: ... :|` section as `m1-m4 ×2`.
//!
//! ⛔ `repeatStart()` / `repeatEnd()` are the measures' own flags, not the drawn barlines. They are what
//! the file format stores (`<startRepeat/>`, `<endRepeat>N</endRepeat>`), what the user's own "drop a
//! repeat barline" path sets (`Measure::drop` -> `undoChangeProperty(Pid::REPEAT_START/REPEAT_END)`), and
//! what the playback order (`RepeatList`) is computed from. Reading the glyphs instead would answer a
//! different question than "how many times does this actually play".
//!
//! ⚠️ `×N` is the number of times the section is PLAYED (the "play count" MuseScore prints over the
//! barline); ×2 is the ordinary single repeat. It is written out even when it is 2: a bare `end-repeat`
//! would leave the reader to guess a convention that lives only in the tool description.
QString repeatStructure(const Score* score)
{
    if (!score || !score->firstMeasure()) {
        return QStringLiteral("none");
    }

    QStringList sections;
    int openedAt = -1;          //!< 1-based position of the `|:`, -1 while no repeat is open
    int index = 0;

    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure(), ++index) {
        if (measure->repeatStart() && openedAt < 0) {
            openedAt = index + 1;
        }
        if (measure->repeatEnd()) {
            //! An `:|` with no `|:` repeats from the beginning of the piece - that is the notation's own
            //! meaning, so measure 1 is the honest answer rather than a missing one.
            const int start = openedAt >= 0 ? openedAt : 1;
            sections.append(QStringLiteral("m%1-m%2 ×%3").arg(start).arg(index + 1).arg(measure->repeatCount()));
            openedAt = -1;
        }
    }

    //! A `|:` that never closes changes nothing about playback - saying so is better than dropping it
    //! silently, which would read as "this score has no repeats here".
    if (openedAt >= 0) {
        sections.append(QStringLiteral("m%1- (start-repeat with no end-repeat)").arg(openedAt));
    }

    return sections.isEmpty() ? QStringLiteral("none") : sections.join(QStringLiteral(", "));
}

//! The layout tokens of one measure that belong to its HEAD (before the notes): the clef(s) in force at
//! its start, the repeat marks, and any non-normal barline.
//!
//! `lastClef` carries the last clef this digest PRINTED per staff, so the state is written on the window's
//! first measure and afterwards only where it changes - the same rule `key=` follows, and for the same
//! reason: a reader who asks for measures 5..8 alone must still learn the clef, and a reader of the whole
//! score must not be told the same clef eleven times.
QString measureLayoutTokens(const Score* score, const Measure* measure, bool firstOfWindow, QMap<int, QString>& lastClef)
{
    QStringList tokens;

    for (size_t i = 0; i < score->nstaves(); ++i) {
        const QString name = clefNameAt(score, i, measure->tick());
        if (name.isEmpty()) {
            continue;
        }
        if (firstOfWindow || lastClef.value(int(i)) != name) {
            //! Staff 1 bare (`clef=G`), the others named - the same shape `key signatures (staff 1)` uses
            //! in the overview: the common single-staff case stays short, and a grand staff still says
            //! which staff it means.
            tokens.append(i == 0 ? QStringLiteral("clef=%1").arg(name)
                                 : QStringLiteral("clef(s%1)=%2").arg(i + 1).arg(name));
        }
        lastClef[int(i)] = name;
    }

    if (measure->repeatStart()) {
        tokens.append(QStringLiteral("start-repeat"));
    }
    if (measure->repeatEnd()) {
        tokens.append(QStringLiteral("end-repeat×%1").arg(measure->repeatCount()));
    }

    //! Any other non-normal end barline (`double`, `end`, `dashed`, `heavy`, ...). The repeat barline
    //! types are deliberately NOT repeated here: the two tokens above already say it, and saying one fact
    //! twice in two vocabularies invites the reader to look for a difference between them.
    const BarLineType barline = measure->endBarLineType();
    if (barline != BarLineType::NORMAL && barline != BarLineType::START_REPEAT && barline != BarLineType::END_REPEAT
        && barline != BarLineType::END_START_REPEAT) {
        tokens.append(QStringLiteral("barline=%1").arg(QString(TConv::toXml(barline).toQLatin1String())));
    }

    if (tokens.isEmpty()) {
        return QString();
    }

    return QStringLiteral(" ") + tokens.join(QLatin1Char(' '));
}
} // namespace

QString buildScoreOverview(const Score* score)
{
    if (!score) {
        return QStringLiteral("(no score)");
    }

    QStringList lines;

    //! ── Identity and shape ────────────────────────────────────────────────────────────────
    const QString title = score->metaTag(u"workTitle");
    const QString composer = score->metaTag(u"composer");
    lines.append(QStringLiteral("score: %1").arg(score->name().toQString()));
    if (!title.isEmpty()) {
        lines.append(QStringLiteral("title: %1").arg(title));
    }
    if (!composer.isEmpty()) {
        lines.append(QStringLiteral("composer: %1").arg(composer));
    }

    lines.append(QStringLiteral("measures: %1").arg(score->nmeasures()));
    lines.append(QStringLiteral("parts: %1   staves: %2").arg(score->parts().size()).arg(score->nstaves()));

    const QStringList timesigs = timesigChanges(score);
    if (!timesigs.isEmpty()) {
        lines.append(QStringLiteral("time signatures: %1").arg(timesigs.join(QStringLiteral(", "))));
    }

    //! The key, right after the time signature: together they are what a reader needs to make sense of the
    //! pitches below, and "what key is it in" was a question the digest could not answer at all (第 132 条).
    const QString keys = keySignatureLine(score);
    if (!keys.isEmpty()) {
        lines.append(keys);
    }

    //! ── The shape of the piece (第 134 条) ────────────────────────────────────────────────
    //! The repeat structure and the endings are score-level facts: they are what decides how many times a
    //! passage is heard and which bar is played when. Both are printed here (with `none` said out loud
    //! for the repeats, the way `empty measures: none` is) because "this score has no repeats" is itself
    //! an answer a reader needs before counting bars.
    lines.append(QStringLiteral("repeats: %1").arg(repeatStructure(score)));

    //! ⚠️ Guarded rather than passed `firstMeasure()->tick()` directly: a score with no measures at all
    //! (which this function is reachable with - the window builder reports it as "(score has no
    //! measures)") would otherwise be dereferenced on the way into a function that handles it fine.
    if (score->firstMeasure()) {
        const QStringList endings = voltaSpans(score, score->firstMeasure()->tick(), score->endTick());
        if (!endings.isEmpty()) {
            lines.append(QStringLiteral("endings (voltas): %1").arg(endings.join(QStringLiteral(", "))));
        }
    }

    //! ── Per staff ────────────────────────────────────────────────────────────────────────
    QMap<int, StaffFacts> facts;
    QMap<int, int> measureNoteCounts;
    collectStaffFacts(score, facts, measureNoteCounts);

    lines.append(QStringLiteral("staves:"));
    for (size_t i = 0; i < score->nstaves(); ++i) {
        const Staff* staff = score->staff(i);
        if (!staff) {
            continue;
        }

        //! The part name is what a reader recognises ("Piano", "Violin"); the staff's own name is
        //! often empty for single-staff parts.
        //! NOTE the explicit `toQString()`: `partName()` returns the engraving layer's `String`, and
        //! mixing it with a `QString` literal in one conditional is ambiguous to the compiler.
        QString name = staff->part() ? staff->part()->partName().toQString() : QString();
        if (name.isEmpty()) {
            name = QStringLiteral("(unnamed)");
        }

        const StaffFacts& f = facts.value(int(i));
        QString range = QStringLiteral("-");
        if (f.highestPitch >= 0) {
            range = QStringLiteral("%1..%2").arg(pitchName(f.lowestPitch), pitchName(f.highestPitch));
        }

        lines.append(QStringLiteral("  staff %1  %2  notes=%3 rests=%4 chords=%5 voices=%6 range=%7")
                     .arg(i + 1)
                     .arg(name)
                     .arg(f.notes)
                     .arg(f.rests)
                     .arg(f.chords)
                     .arg(f.voicesUsed)
                     .arg(range));
    }

    //! ── Where the notes are ───────────────────────────────────────────────────────────────
    //! Empty measures are the single most useful thing a reader can learn from a summary: they are
    //! where a new phrase can be written, and finding them by reading every bar is exactly the work
    //! the digest exists to remove.
    QStringList emptyMeasures;
    for (auto it = measureNoteCounts.constBegin(); it != measureNoteCounts.constEnd(); ++it) {
        if (it.value() == 0) {
            emptyMeasures.append(QString::number(it.key() + 1));
        }
    }

    if (emptyMeasures.isEmpty()) {
        lines.append(QStringLiteral("empty measures: none"));
    } else {
        //! Capped: a score where most bars are empty would otherwise produce a line longer than the
        //! rest of the digest put together.
        const int cap = 40;
        const bool truncated = emptyMeasures.size() > cap;
        const QStringList shown = emptyMeasures.mid(0, cap);
        lines.append(QStringLiteral("empty measures (%1): %2%3")
                     .arg(emptyMeasures.size())
                     .arg(shown.join(QStringLiteral(", ")))
                     .arg(truncated ? QStringLiteral(" ...") : QString()));
    }

    return lines.join(QLatin1Char('\n'));
}

//! One slur, reduced to the two facts a reader needs: where it starts and where it ends.
//! ⚠️ Read from the score's SPANNER MAP rather than from a chord's `startEndSlurs()` flags - that
//! accessor is **non-const** and its flags are set by LAYOUT, so using it here would mean either a
//! `const_cast` or an answer that changes depending on whether the page has been redrawn.
struct SlurSpan
{
    Fraction startTick;
    int startTrack = 0;
    Fraction endTick;
    int endTrack = 0;
};

std::vector<SlurSpan> slursInWindow(const Score* score, const Fraction& from, const Fraction& to)
{
    std::vector<SlurSpan> out;
    if (!score) {
        return out;
    }

    for (const auto& entry : score->spannerMap().map()) {
        const Spanner* spanner = entry.second;
        if (!spanner || !spanner->isSlur()) {
            continue;
        }
        //! Only the ones that can touch this window: a score with hundreds of slurs must not turn every
        //! chord in the window into a scan of all of them.
        if (spanner->tick2() < from || spanner->tick() > to) {
            continue;
        }

        out.push_back(SlurSpan { spanner->tick(), int(spanner->track()),
                                 spanner->tick2(), int(spanner->track2()) });
    }

    return out;
}

QString buildMeasureWindow(const Score* score, int firstMeasure, int lastMeasure)
{
    if (!score) {
        return QStringLiteral("(no score)");
    }

    const int total = int(score->nmeasures());
    if (total <= 0) {
        return QStringLiteral("(score has no measures)");
    }

    //! Out of range is REPORTED, not clamped. Clamping would turn "measure 99 of a 4-bar score" into
    //! a confident answer about measure 4 - the same silent-wrong-target failure the addressing layer
    //! refuses to commit, and the reason it returns -1 rather than the last measure.
    if (firstMeasure < 1 || lastMeasure < firstMeasure || firstMeasure > total) {
        return QStringLiteral("measure range %1..%2 is outside this score (1..%3)")
               .arg(firstMeasure).arg(lastMeasure).arg(total);
    }

    const int last = std::min(lastMeasure, total);

    //! The key in force, carried across the loop so a change can be reported and a repeat cannot.
    QString previousKey;

    //! The window's own tick range, and the measure pointers that define it. Everything that is a SPAN
    //! (slurs, endings) or a tick-keyed fact (clefs) is read once against this range instead of per
    //! measure.
    const Measure* firstMeasurePtr = score->firstMeasure();
    for (int i = 1; firstMeasurePtr && i < firstMeasure; ++i) {
        firstMeasurePtr = firstMeasurePtr->nextMeasure();
    }
    const Measure* lastMeasurePtr = firstMeasurePtr;
    for (int i = firstMeasure; lastMeasurePtr && i < last; ++i) {
        lastMeasurePtr = lastMeasurePtr->nextMeasure();
    }
    const Fraction windowStart = firstMeasurePtr ? firstMeasurePtr->tick() : score->firstMeasure()->tick();
    const Fraction windowEnd = lastMeasurePtr ? lastMeasurePtr->endTick() : Fraction(0, 1);

    QStringList lines;
    lines.append(QStringLiteral("measures %1..%2 of %3").arg(firstMeasure).arg(last).arg(total));

    //! ⛔ The endings (voltas) that touch this window, as a WINDOW-level line rather than a per-measure
    //! token: an ending is a span, so "1st ending m3-m4" answers "how does this repeat work" in one line -
    //! a token printed only where the ending starts would hide that the following bars are inside it, and
    //! one repeated per covered bar would spend a token per bar to say the same thing (第 134 条).
    const QStringList endings = voltaSpans(score, windowStart, windowEnd);
    if (!endings.isEmpty()) {
        lines.append(QStringLiteral("endings (voltas): %1").arg(endings.join(QStringLiteral(", "))));
    }

    //! The slurs and the clef changes that can touch this window, read once (see `slursInWindow` /
    //! `clefChangesIn`).
    const std::vector<SlurSpan> slurs = slursInWindow(score, windowStart, windowEnd);
    const std::vector<ClefChange> clefs = clefChangesIn(score, windowStart, windowEnd);

    //! The clef this digest has already PRINTED per staff (see `measureLayoutTokens`).
    QMap<int, QString> lastClef;

    const auto slurStartsAt = [&slurs](const Fraction& tick, int track) {
        for (const SlurSpan& span : slurs) {
            if (span.startTick == tick && span.startTrack == track) {
                return true;
            }
        }
        return false;
    };
    const auto slurEndsAt = [&slurs](const Fraction& tick, int track) {
        for (const SlurSpan& span : slurs) {
            if (span.endTick == tick && span.endTrack == track) {
                return true;
            }
        }
        return false;
    };

    int measureIndex = 0;
    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure(), ++measureIndex) {
        if (measureIndex + 1 < firstMeasure) {
            continue;
        }
        if (measureIndex + 1 > last) {
            break;
        }

        const TimeSigFrac timesig(measure->timesig());
        QStringList content;

        //! ⛔ Clef changes that take effect INSIDE this measure, in tick order. The one at the measure's
        //! very start is the layout prefix's job; these are printed in front of the notes they apply to,
        //! because a clef changes what the notes AFTER it mean - a token printed after them would read as
        //! if it applied to what came before.
        //! ⚠️ The cursor only moves forward, so a change at a tick where no chordrest segment exists (an
        //! empty beat) is attached to the next segment in the same measure; the token still prints the
        //! clef's OWN tick, so the reader is not told the wrong position.
        std::vector<ClefChange> insideClefs;
        for (const ClefChange& change : clefs) {
            if (change.tick > measure->tick() && change.tick < measure->endTick()) {
                insideClefs.push_back(change);
            }
        }
        size_t clefCursor = 0;

        for (const Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            //! Relative tick inside the measure, because that is what the address layer speaks
            //! (`{measure, beat}`) and repeating an absolute tick would invite arithmetic errors.
            const Fraction rel = segment->tick() - measure->tick();

            while (clefCursor < insideClefs.size() && insideClefs[clefCursor].tick <= segment->tick()) {
                const ClefChange& change = insideClefs[clefCursor++];
                content.append(QStringLiteral("t%1 s%2 clef=%3")
                               .arg((change.tick - measure->tick()).ticks())
                               .arg(change.staff + 1)
                               .arg(change.name));
                //! Recorded as printed, so the next measure's prefix does not repeat a clef the reader
                //! has already been told about.
                lastClef[change.staff] = change.name;
            }

            for (track_idx_t track = 0; track < score->ntracks(); ++track) {
                EngravingItem* item = segment->element(track);
                if (!item) {
                    continue;
                }

                const int staff = int(track / VOICES) + 1;
                const int voice = int(track % VOICES) + 1;

                if (item->isRest()) {
                    const Rest* rest = toRest(item);
                    content.append(QStringLiteral("t%1 s%2 v%3 rest/%4")
                                   .arg(rel.ticks()).arg(staff).arg(voice).arg(rest->ticks().ticks()));
                    continue;
                }

                if (!item->isChord()) {
                    continue;
                }

                const Chord* chord = toChord(item);
                if (chord->isGrace()) {
                    continue;
                }

                QStringList pitches;
                for (const Note* note : chord->notes()) {
                    pitches.append(pitchName(note->pitch()));
                }

                //! ── What the note is DOING, not just how high it is (第 133 条) ─────────────────────
                //! ⛔ Until this round the window showed pitches and durations only, while the write side
                //! could create articulations, ties, slurs and dynamics - so the agent could change those
                //! and then be UNABLE TO SEE ITS OWN WORK. Measured 2026-10-11: asked to add a staccato
                //! and read the measure back, it had to answer that the digest does not report
                //! articulations. A read tool that cannot confirm the writes its own toolset makes is
                //! half a tool.
                //! ⚠️ The articulation name is the SAME SMuFL name the write path took
                //! (`SymNames::nameForSymId` is the exact inverse of the `symIdByName` it validates with),
                //! so "add articStaccatoAbove" and "read it back" speak one vocabulary.
                QString marks;
                for (const Articulation* articulation : chord->articulations()) {
                    //! `nameForSymId` returns the framework's `AsciiStringView`; `toQLatin1String()` is
                    //! its documented Qt conversion (there is no `asQString`).
                    marks += QStringLiteral(" art=%1")
                             .arg(QString(SymNames::nameForSymId(articulation->symId()).toQLatin1String()));
                }

                //! ⚠️ A tie belongs to the NOTE, so "the chord is tied" means every note of it is: a chord
                //! with one tied note is a real thing (a held inner voice), and saying "tie" for it would
                //! overstate. Reported per chord as `tie` only when they all are.
                bool anyTie = false;
                bool allTie = true;
                for (const Note* note : chord->notes()) {
                    if (note->tieFor()) {
                        anyTie = true;
                    } else {
                        allTie = false;
                    }
                }
                if (allTie && anyTie) {
                    marks += QStringLiteral(" tie");
                } else if (anyTie) {
                    marks += QStringLiteral(" tie(part)");
                }

                //! The slur is a SPANNER, so what is knowable per chord is where it starts and where it
                //! ends - which is exactly what a reader needs to tell "there is a slur over these two
                //! notes" from "there is not".
                //! ⛔ NOT `chord->startEndSlurs()`: that accessor is **non-const** and its flags are set by
                //! LAYOUT, so reading them here would mean either a `const_cast` or a dependency on whether
                //! the page has been laid out yet - and the answer would differ before and after a redraw.
                //! The spanner map is const, and it is where `Score::addSlur` puts the slur it creates.
                if (slurStartsAt(chord->tick(), chord->track())) {
                    marks += QStringLiteral(" slur→");
                }
                if (slurEndsAt(chord->tick(), chord->track())) {
                    marks += QStringLiteral(" slur←");
                }

                content.append(QStringLiteral("t%1 s%2 v%3 [%4]/%5%6")
                               .arg(rel.ticks()).arg(staff).arg(voice)
                               .arg(pitches.join(QLatin1Char(' ')))
                               .arg(chord->ticks().ticks())
                               .arg(marks));
            }

            //! Dynamics hang off the SEGMENT (they belong to a beat, not to a notehead), so they are read
            //! from the segment's annotations and printed as their own token in the same list.
            //! ⛔ `TConv::toXml(DynamicType)`, NOT `Dynamic::dynamicText(DynamicType)`: the latter is the
            //! DISPLAY text and comes back as SMuFL markup - measured 2026-10-11, the digest printed
            //! `dyn=<sym>dynamicMezzo</sym><sym>dynamicForte</sym>` for `mf`. `toXml` is the plain marking
            //! name, and it is the exact inverse of the string the write path parses.
            for (EngravingItem* annotation : segment->annotations()) {
                if (!annotation || !annotation->isDynamic()) {
                    continue;
                }

                const Dynamic* dynamic = toDynamic(annotation);
                const int staff = int(dynamic->track() / VOICES) + 1;
                content.append(QStringLiteral("t%1 s%2 dyn=%3")
                               .arg(rel.ticks()).arg(staff)
                               .arg(QString(TConv::toXml(dynamic->dynamicType()).toQLatin1String())));
            }
        }

        //! The key signature is printed on the FIRST measure of the window and whenever it changes - the
        //! same rule the time signature follows by being on every line. ⚠️ The first line matters even
        //! when nothing changes: a reader who asks for measures 5..8 alone must still learn what key those
        //! measures are in (第 132 条 added it to the overview; a window that starts mid-score needs it
        //! just as much).
        QString keyPrefix;
        const Staff* keyStaff = score->staff(0);
        if (keyStaff) {
            const QString key = describeKeySignature(int(keyStaff->key(measure->tick())));
            if (measureIndex == firstMeasure - 1 || key != previousKey) {
                keyPrefix = QStringLiteral(" key=%1").arg(key);
            }
            previousKey = key;
        }

        //! ⚠️ Guarded rather than assumed: with the current `nextClefTick` a change always lands on a
        //! chordrest segment's tick or before one, but a measure whose only clef change sits after its
        //! last segment (and which therefore has no chordrest segment at all) would otherwise be dropped
        //! SILENTLY - and an omission is indistinguishable from "there is no clef change".
        while (clefCursor < insideClefs.size()) {
            const ClefChange& change = insideClefs[clefCursor++];
            content.append(QStringLiteral("t%1 s%2 clef=%3")
                           .arg((change.tick - measure->tick()).ticks())
                           .arg(change.staff + 1)
                           .arg(change.name));
            lastClef[change.staff] = change.name;
        }

        //! ── The layout of this measure (第 134 条) ────────────────────────────────────────────
        //! The clef(s) in force, the repeat marks and any non-normal barline - see `measureLayoutTokens`.
        //! Placed with the key, i.e. as part of the measure's HEAD: these are properties of the whole bar,
        //! not of a note in it.
        const QString layoutPrefix = measureLayoutTokens(score, measure, measureIndex == firstMeasure - 1, lastClef);

        if (content.isEmpty()) {
            lines.append(QStringLiteral("m%1 %2/%3%4%5: (empty)")
                         .arg(measureIndex + 1).arg(timesig.numerator()).arg(timesig.denominator())
                         .arg(keyPrefix)
                         .arg(layoutPrefix));
        } else {
            lines.append(QStringLiteral("m%1 %2/%3%4%5: %6")
                         .arg(measureIndex + 1).arg(timesig.numerator()).arg(timesig.denominator())
                         .arg(keyPrefix)
                         .arg(layoutPrefix)
                         .arg(content.join(QStringLiteral(" | "))));
        }
    }

    return lines.join(QLatin1Char('\n'));
}
} // namespace muse::agentharness
