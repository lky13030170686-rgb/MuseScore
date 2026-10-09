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

                content.append(QStringLiteral("t%1 s%2 v%3 [%4]/%5")
                               .arg(rel.ticks()).arg(staff).arg(voice)
                               .arg(pitches.join(QLatin1Char(' ')))
                               .arg(chord->ticks().ticks()));
            }
        }

        if (content.isEmpty()) {
            lines.append(QStringLiteral("m%1 %2/%3: (empty)")
                         .arg(measureIndex + 1).arg(timesig.numerator()).arg(timesig.denominator()));
        } else {
            lines.append(QStringLiteral("m%1 %2/%3: %4")
                         .arg(measureIndex + 1).arg(timesig.numerator()).arg(timesig.denominator())
                         .arg(content.join(QStringLiteral(" | "))));
        }
    }

    return lines.join(QLatin1Char('\n'));
}
} // namespace muse::agentharness
