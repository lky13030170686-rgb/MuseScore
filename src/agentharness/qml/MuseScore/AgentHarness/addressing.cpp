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
#include "addressing.h"

#include "engraving/dom/score.h"
#include "engraving/dom/measure.h"
//! `TimeSigFrac` (the type that owns the beat-length rule) lives in sig.h, NOT timesig.h - the
//! naming is a trap this project has already recorded once.
#include "engraving/dom/sig.h"

using namespace mu::engraving;

//! ⛔ Everything here is inside an explicit namespace block, NOT relying on a `using namespace`.
//! A `using` directive only affects *name lookup*; it does not put a definition into that namespace.
//! The first version of this file had `using namespace muse::agentharness;` at the top and defined
//! the free functions unqualified, so they landed in the global namespace: the header declared
//! `muse::agentharness::addressForTick` and the object file defined `::addressForTick`. It compiled
//! cleanly and failed at link with three unresolved externals - the kind of mistake that costs a
//! build cycle if the cause is not obvious from the error.
namespace muse::agentharness {
QString ScoreAddress::toString() const
{
    return QStringLiteral("m%1 b%2").arg(measure).arg(beat);
}

int MeasureGrid::totalTicks() const
{
    if (starts.isEmpty()) {
        return 0;
    }

    const int last = starts.size() - 1;
    return starts[last] + lengths[last];
}

int MeasureGrid::measureIndexForTick(int tick) const
{
    if (starts.isEmpty() || tick < 0) {
        return -1;
    }

    //! Binary search rather than a linear scan: a real orchestral score has hundreds of measures and
    //! this runs for every recorded event and every address lookup. `std::upper_bound` gives the
    //! first measure that starts *after* `tick`, so the one before it is the answer.
    const auto begin = starts.begin();
    const auto end = starts.end();
    const auto it = std::upper_bound(begin, end, tick);
    if (it == begin) {
        return -1;
    }

    const int index = int(std::distance(begin, it)) - 1;
    if (index >= starts.size()) {
        return -1;
    }

    //! A tick past the end of the last measure is NOT in it. Returning the last measure anyway would
    //! turn "the model asked about tick 999999" into "the model is editing the final bar", which is
    //! precisely the silent-wrong-target failure this project keeps hitting.
    if (tick >= starts[index] + lengths[index]) {
        return -1;
    }

    return index;
}

ScoreAddress addressForTick(const MeasureGrid& grid, int tick, int staff, int part)
{
    ScoreAddress address;
    address.staff = staff;
    address.part = part;

    const int index = grid.measureIndexForTick(tick);
    if (index < 0) {
        //! Out of range: report the first measure rather than a fabricated one, and leave the tick
        //! offset at 0 so the caller can tell "we clamped" from "we found it" by re-checking.
        address.measure = grid.isEmpty() ? 1 : 1;
        address.beat = 1;
        address.tickInBeat = 0;
        return address;
    }

    address.measure = index + 1;

    const int beatTicks = grid.beatTicks.value(index, 0);
    if (beatTicks <= 0) {
        address.beat = 1;
        address.tickInBeat = 0;
        return address;
    }

    const int offset = tick - grid.starts[index];
    address.beat = offset / beatTicks + 1;
    address.tickInBeat = offset % beatTicks;
    return address;
}

int tickForMeasure(const MeasureGrid& grid, int measureIndex)
{
    if (measureIndex < 0 || measureIndex >= grid.starts.size()) {
        return -1;
    }

    return grid.starts[measureIndex];
}

int tickForBeat(const MeasureGrid& grid, int measureIndex, int beat)
{
    if (measureIndex < 0 || measureIndex >= grid.starts.size()) {
        return -1;
    }

    const int beatTicks = grid.beatTicks.value(measureIndex, 0);
    const int measureTicks = grid.measureTicks.value(measureIndex, 0);
    if (beatTicks <= 0 || measureTicks <= 0) {
        return -1;
    }

    if (beat < 1 || (beat - 1) * beatTicks >= measureTicks) {
        return -1;
    }

    return grid.starts[measureIndex] + (beat - 1) * beatTicks;
}

QString formatAddress(const ScoreAddress& address)
{
    return QStringLiteral("m%1b%2").arg(address.measure).arg(address.beat);
}

//! ── The one function here that touches engraving ─────────────────────────────────────────────

MeasureGrid buildMeasureGrid(const Score* score)
{
    MeasureGrid grid;
    if (!score) {
        return grid;
    }

    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        const Fraction tick = measure->tick();
        const Fraction length = measure->ticks();

        grid.starts.append(tick.ticks());
        grid.lengths.append(length.ticks());

        //! Beat length comes from the measure's own time signature, and is obtained by *asking*
        //! `TimeSigFrac::beatTicks()` rather than from a note-value table of our own. That is the
        //! same discipline the write side is held to (`isWritableMidiLength()` asks upstream instead
        //! of listing writable durations): this project has already been burned once by a hand-made
        //! table disagreeing with the rule's real owner (维护手册.md §4.8, the "duration that cannot
        //! be written" trap). It also gets compound metres right - 6/8 has two dotted-quarter beats,
        //! not six - which a naive `4 * DIVISIONS / denominator` would silently get wrong.
        const TimeSigFrac timesig(measure->timesig());

        grid.beatTicks.append(timesig.beatTicks());
        grid.measureTicks.append(length.ticks());
    }

    return grid;
}
} // namespace muse::agentharness
