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

    QStringList lines;
    lines.append(QStringLiteral("measures %1..%2 of %3").arg(firstMeasure).arg(last).arg(total));

    //! The key in force, carried across the loop so a change can be reported and a repeat cannot.
    QString previousKey;

    //! The slurs that can touch this window, read once (see `slursInWindow`).
    const Measure* lastMeasurePtr = score->firstMeasure();
    for (int i = 1; lastMeasurePtr && i < last; ++i) {
        lastMeasurePtr = lastMeasurePtr->nextMeasure();
    }
    const Fraction windowEnd = lastMeasurePtr ? lastMeasurePtr->endTick() : Fraction(0, 1);
    const std::vector<SlurSpan> slurs = slursInWindow(score, score->firstMeasure()->tick(), windowEnd);

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

        for (const Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            //! Relative tick inside the measure, because that is what the address layer speaks
            //! (`{measure, beat}`) and repeating an absolute tick would invite arithmetic errors.
            const Fraction rel = segment->tick() - measure->tick();

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

        if (content.isEmpty()) {
            lines.append(QStringLiteral("m%1 %2/%3%4: (empty)")
                         .arg(measureIndex + 1).arg(timesig.numerator()).arg(timesig.denominator())
                         .arg(keyPrefix));
        } else {
            lines.append(QStringLiteral("m%1 %2/%3%4: %5")
                         .arg(measureIndex + 1).arg(timesig.numerator()).arg(timesig.denominator())
                         .arg(keyPrefix)
                         .arg(content.join(QStringLiteral(" | "))));
        }
    }

    return lines.join(QLatin1Char('\n'));
}
} // namespace muse::agentharness
