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
#include "notelocator.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"

#include "addressing.h"
#include "scoredigest.h"

#include "log.h"

using namespace muse::agentharness;

namespace {
//! The segment holding a chord or rest at `tick` for `track`, or null.
//!
//! ⛔ `Score::tick2segment()` IS NOT A SUBSTITUTE: it only returns a segment when the tick falls
//! exactly on one, and silently answers null otherwise - which would make "there is no note here" and
//! "this tick does not exist" look the same. Walking the measure's segments lets the caller tell those
//! apart, and that difference is the whole content of the error messages below.
//!
//! ⛔⛔ IT SEARCHES *ALL* SEGMENT TYPES, and that is not laziness. A whole-measure rest does not live on
//! a `SegmentType::ChordRest` segment - it lives on the measure's own segment, which is a different
//! type - so a walk restricted to ChordRest segments CANNOT SEE IT. The measured symptom was a
//! `note_to_rest` call on a bar that was already a rest answering "there is no note at measure 2 beat
//! 1", which is both wrong and misleading: the bar is full of rest, and the caller is told there is
//! nothing there.
//!
//! ⚠️ It also does NOT require the element to be on `track`. A whole-measure rest is stored once, on
//! the measure, and a caller asking about voice 2 of an empty bar should still be told "that is a
//! rest" rather than "there is nothing here".
mu::engraving::Segment* segmentAt(mu::engraving::Score* score, mu::engraving::Measure* measure,
                                  const mu::engraving::Fraction& tick, mu::engraving::track_idx_t track)
{
    Q_UNUSED(score);
    Q_UNUSED(track);

    for (mu::engraving::Segment* seg = measure->first(mu::engraving::SegmentType::All); seg;
         seg = seg->next(mu::engraving::SegmentType::All)) {
        if (seg->tick() > tick) {
            //! Segments are in tick order, so there is nothing at this tick.
            return nullptr;
        }
        if (seg->tick() != tick) {
            continue;
        }

        //! The first segment at this tick that carries anything chord-like. Several segments share a
        //! tick (clef, key, time signature, then the chord/rest), so the type has to be checked rather
        //! than assumed from the tick alone.
        if (seg->isChordRestType() || seg->segmentType() == mu::engraving::SegmentType::ChordRest) {
            return seg;
        }

        const mu::engraving::track_idx_t trackCount = score ? score->ntracks() : 0;
        for (mu::engraving::track_idx_t t = 0; t < trackCount; ++t) {
            if (mu::engraving::EngravingItem* item = seg->element(t)) {
                if (item->isChordRest()) {
                    return seg;
                }
            }
        }
    }
    return nullptr;
}
} // namespace

NoteLookup muse::agentharness::chordAt(mu::engraving::Score* score, const ScoreAddress& address, int voice)
{
    NoteLookup result;

    //! ⚠️ One trace at the top and one before every return. A failure branch that returns an empty
    //! `problem` is invisible from the caller (it reports `problem`, which is then empty), and the
    //! previous two attempts to find this by tracing the recipe and the note lookup both came back
    //! clean - because the failure is in here, before either of them.
    LOGW() << "[agent-locate] chordAt m" << address.measure << "b" << address.beat
           << "staff" << address.staff << "voice" << voice
           << "score=" << (score != nullptr)
           << "measures=" << (score ? int(score->nmeasures()) : -1)
           << "staves=" << (score ? int(score->nstaves()) : -1);

    if (!score) {
        result.problem = QStringLiteral("no score");
        return result;
    }

    if (address.measure < 1 || address.measure > score->nmeasures()) {
        result.problem = QStringLiteral("there is no measure %1 (the score has %2)")
                         .arg(address.measure).arg(score->nmeasures());
        return result;
    }

    mu::engraving::Measure* measure = mu::engraving::toMeasure(score->measure(address.measure - 1));
    if (!measure) {
        result.problem = QStringLiteral("measure %1 could not be read").arg(address.measure);
        return result;
    }

    //! ⚠️ `ScoreAddress::staff` is 0-BASED (see addressing.h) - the model says "staff 1" and the
    //! boundary converts, which is exactly why that conversion lives in one place. The track then
    //! packs voice on top of staff, and doing that arithmetic here means no recipe has to remember the
    //! packing.
    const int staffIndex = address.staff;
    if (staffIndex < 0 || staffIndex >= int(score->nstaves())) {
        result.problem = QStringLiteral("there is no staff %1 (the score has %2)")
                         .arg(address.staff + 1).arg(score->nstaves());
        return result;
    }

    //! The beat is converted through the SAME addressing layer the read side uses, so "beat 2" means
    //! the same thing whether it is being read or written. Building a grid here is not waste: it is
    //! what makes 6/8's compound beats come out right, because `buildMeasureGrid` asks upstream
    //! (`TimeSigFrac::beatTicks()`) rather than multiplying.
    const MeasureGrid grid = buildMeasureGrid(score);
    const int tickInt = tickForBeat(grid, address.measure - 1, address.beat);

    //! ⚠️ Traced because this is the step that was invisible: `chordAt` entered with a valid score and
    //! returned before any of its error branches ran, which leaves only the conversion between them.
    //! `buildMeasureGrid` is also the one call here that can assert internally, so its result is worth
    //! seeing rather than assuming.
    LOGW() << "[agent-locate] grid measures=" << grid.starts.size()
           << "beat tick=" << tickInt;

    if (tickInt < 0) {
        result.problem = QStringLiteral("beat %1 does not exist in measure %2")
                         .arg(address.beat).arg(address.measure);
        return result;
    }
    const mu::engraving::Fraction tick = mu::engraving::Fraction::fromTicks(tickInt);

    //! A voice of 0 means "whichever voice has something here", which is what a caller who did not
    //! think about voices means. An explicit voice is honoured exactly, so a wrong one is an error
    //! rather than a silent fallback to a different voice's note.
    QVector<mu::engraving::track_idx_t> tracks;
    if (voice > 0) {
        tracks.append(mu::engraving::track_idx_t(staffIndex * mu::engraving::VOICES + (voice - 1)));
    } else {
        for (int v = 0; v < mu::engraving::VOICES; ++v) {
            tracks.append(mu::engraving::track_idx_t(staffIndex * mu::engraving::VOICES + v));
        }
    }

    LOGW() << "[agent-locate] searching" << tracks.size() << "track(s) at tick" << tickInt
           << "in measure" << address.measure;

    for (mu::engraving::track_idx_t track : tracks) {
        if (mu::engraving::Segment* seg = segmentAt(score, measure, tick, track)) {
            //! ⚠️ Look at the element for THIS track, and fall back to whatever chord-like element the
            //! segment carries. A whole-measure rest is stored once on the measure, so asking about
            //! voice 2 of an empty bar finds it only through the fallback - and without that, the
            //! caller is told "there is nothing here" about a bar full of rest.
            mu::engraving::EngravingItem* item = seg->element(track);
            if (!item) {
                for (mu::engraving::track_idx_t t = 0; t < score->ntracks(); ++t) {
                    mu::engraving::EngravingItem* candidate = seg->element(t);
                    if (candidate && candidate->isChordRest()) {
                        item = candidate;
                        break;
                    }
                }
            }

            if (item && item->isChord()) {
                result.chord = mu::engraving::toChord(item);
                LOGW() << "[agent-locate] track" << int(track) << "is a chord with"
                       << result.chord->notes().size() << "note(s)";
                return result;
            }

            if (item && item->isRest()) {
                //! ⛔ A REST IS A LEGITIMATE ANSWER, and returning "nothing here" for one is what made
                //! `changeToRest`'s "already a rest" branch unreachable on the most common rest there
                //! is. `found()` is true because `chord` is set; `ok()` stays false because there is no
                //! note - which is exactly the distinction the two predicates exist to draw.
                result.rest = mu::engraving::toChordRest(item);
                LOGW() << "[agent-locate] track" << int(track) << "is a rest";
                return result;
            }

            LOGW() << "[agent-locate] track" << int(track) << "element is not a chord or rest";
        }
    }

    LOGW() << "[agent-locate] no chord found on any track";

    result.problem = QStringLiteral("there is no note at measure %1 beat %2%3")
                     .arg(address.measure).arg(address.beat)
                     .arg(voice > 0 ? QStringLiteral(" in voice %1").arg(voice) : QString());

    //! ⚠️ Trace every failure branch's RESULT, not just the branch. The caller reports `problem`, and
    //! the earlier attempt to diagnose this by tracing the recipe entry showed the recipe ran and the
    //! problem was empty - which narrowed it to "the lookup returned nothing", but not to which branch.
    LOGW() << "[agent-locate] failed at m" << address.measure << "b" << address.beat
           << "staff" << address.staff << "voice" << voice << ":" << result.problem;

    return result;
}

NoteLookup muse::agentharness::noteAt(mu::engraving::Score* score, const ScoreAddress& address, int voice, int index)
{
    NoteLookup result = chordAt(score, address, voice);

    //! ⛔ `found()`, NOT `ok()`. `ok()` asks "is there a note", and `chordAt` never sets one - so using
    //! it here made every lookup bail out immediately with an empty `problem`, which the caller then
    //! reported as a failure with no message. See the note on `NoteLookup::found`.
    if (!result.found()) {
        return result;
    }

    const std::vector<mu::engraving::Note*>& notes = result.chord->notes();

    //! ⚠️ Trace, because an empty `problem` is otherwise indistinguishable from "the lookup never ran":
    //! the caller reports whatever is in `problem`, and a default-constructed one is empty. That cost a
    //! round - the log showed `FAILED ""` with nothing to go on.
    LOGW() << "[agent-locate] chord at m" << address.measure << "b" << address.beat
           << "has" << notes.size() << "note(s); index=" << index;

    if (notes.empty()) {
        result.problem = QStringLiteral("the chord at measure %1 beat %2 has no notes")
                         .arg(address.measure).arg(address.beat);
        return result;
    }

    //! ⛔ AN UNSPECIFIED INDEX IS ONLY ACCEPTABLE WHEN THERE IS NOTHING TO CHOOSE BETWEEN. Picking
    //! notes[0] from a chord would edit a note the caller did not name, and the caller would have no
    //! way to tell - the edit would simply be on the wrong pitch. Refusing is the only safe answer, and
    //! the message says how to disambiguate.
    if (index < 0) {
        if (notes.size() > 1) {
            result.problem = QStringLiteral("measure %1 beat %2 holds %3 notes; say which one with "
                                            "`note` (0-based, from the lowest)")
                             .arg(address.measure).arg(address.beat).arg(notes.size());
            result.chord = nullptr;
            return result;
        }
        result.note = notes.front();
        return result;
    }

    if (index >= int(notes.size())) {
        result.problem = QStringLiteral("measure %1 beat %2 has %3 note(s), so there is no note %4")
                         .arg(address.measure).arg(address.beat).arg(notes.size()).arg(index);
        result.chord = nullptr;
        return result;
    }

    result.note = notes[size_t(index)];
    return result;
}

NoteLookup muse::agentharness::nextNote(mu::engraving::Score* score, mu::engraving::Note* from, bool includeSelf)
{
    NoteLookup result;

    if (!score || !from || !from->chord()) {
        result.problem = QStringLiteral("no note to search from");
        return result;
    }

    const mu::engraving::track_idx_t track = from->track();
    const int startMeasure = from->chord()->measure()->index();
    const mu::engraving::Fraction startTick = from->chord()->tick();

    //! ⛔ WALK FORWARD FROM THE GIVEN POSITION, do not re-derive an address. The next note may be
    //! several beats or several measures away, and asking "the note at the next beat" would find a
    //! rest - or nothing - and tie to that.
    //!
    //! ⚠️ The scan is bounded by the score's measure count, so a note in the last measure simply has
    //! no next note. That is the honest answer: there is nothing to tie to, and the caller is told so
    //! rather than being handed the last note again.
    for (int m = startMeasure; m < score->nmeasures(); ++m) {
        mu::engraving::Measure* measure = mu::engraving::toMeasure(score->measure(m));
        if (!measure) {
            continue;
        }

        for (mu::engraving::Segment* seg = measure->first(mu::engraving::SegmentType::ChordRest); seg;
             seg = seg->next(mu::engraving::SegmentType::ChordRest)) {
            if (seg->tick() < startTick || (!includeSelf && seg->tick() == startTick)) {
                continue;
            }

            mu::engraving::EngravingItem* item = seg->element(track);
            if (!item || !item->isChord()) {
                continue;
            }

            mu::engraving::Chord* chord = mu::engraving::toChord(item);
            if (chord->notes().empty()) {
                continue;
            }

            result.chord = chord;
            //! The same pitch, so a tie connects the same note rather than an arbitrary member of the
            //! next chord. A chord whose notes all differ from `from` is NOT a tie target - tying two
            //! different pitches together is not a tie, it is a slur, and upstream would draw it as
            //! one while every later read of the score saw an impossible pair.
            result.note = chord->findNote(from->pitch());
            if (!result.note) {
                result.chord = nullptr;
                continue;
            }

            return result;
        }
    }

    result.problem = QStringLiteral("there is no later %1 to tie to - the note is in the last measure "
                                    "that has one, or every later note on this staff differs in pitch")
                     .arg(pitchName(from->pitch()));
    return result;
}