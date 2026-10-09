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
#include <gtest/gtest.h>

#include <memory>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/score.h"
#include "engraving/dom/sig.h"
#include "engraving/tests/utils/scorerw.h"

#include "agentharness/qml/MuseScore/AgentHarness/addressing.h"

using namespace mu::engraving;
using namespace muse::agentharness;

namespace {
//! A 4/4 score, 2 bars. The same file the notationscene suite uses, copied into this module's data
//! directory, so "the grid agrees with the score" is checked against a real engraving project and not
//! against a fixture we invented.
//! NOTE the path is relative: `ScoreRW::readScore` prefixes its own root path (which the suite
//! environment sets from `agentharness_tests_DATA_ROOT`). Building the absolute path by hand here
//! would both duplicate that logic and not compile - `muse::String` has no `const char*` `operator+`.
const muse::String GRID_SCORE(u"data/test.mscx");
constexpr int QUARTER = 480;   //!< MuseScore's DIVISIONS; a 4/4 bar is 1920 ticks.

MeasureGrid gridFor()
{
    const std::shared_ptr<MasterScore> score = std::shared_ptr<MasterScore>(mu::engraving::ScoreRW::readScore(GRID_SCORE));
    EXPECT_TRUE(score != nullptr) << "could not load the test score";
    return buildMeasureGrid(score.get());
}
} // namespace

//! ── The grid mirrors the score ───────────────────────────────────────────────────────────────

TEST(AgentHarness_Addressing, GridMatchesTheScoresOwnMeasures)
{
    const std::shared_ptr<MasterScore> score = std::shared_ptr<MasterScore>(mu::engraving::ScoreRW::readScore(GRID_SCORE));
    ASSERT_TRUE(score != nullptr);
    const MeasureGrid grid = buildMeasureGrid(score.get());

    ASSERT_FALSE(grid.isEmpty());

    //! Every measure's start and length must equal what the score itself says. This is the assertion
    //! that catches the whole class of "our flattened copy drifted from the truth" bugs.
    int index = 0;
    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure(), ++index) {
        ASSERT_LT(index, grid.measureCount()) << "grid has fewer measures than the score";
        EXPECT_EQ(grid.starts[index], m->tick().ticks()) << "measure " << index << " start";
        EXPECT_EQ(grid.lengths[index], m->ticks().ticks()) << "measure " << index << " length";
    }
    EXPECT_EQ(index, grid.measureCount()) << "grid has more measures than the score";
}

TEST(AgentHarness_Addressing, BeatLengthComesFromTheTimeSignature)
{
    const MeasureGrid grid = gridFor();
    ASSERT_FALSE(grid.isEmpty());

    //! In 4/4 a beat is a quarter note. Derived from `TimeSigFrac::beatTicks()` rather than assumed,
    //! so this also pins the fact that we ask upstream instead of using our own table.
    for (int i = 0; i < grid.measureCount(); ++i) {
        EXPECT_EQ(grid.beatTicks[i], QUARTER) << "measure " << i;
        EXPECT_EQ(grid.measureTicks[i], 4 * QUARTER) << "measure " << i;
    }
}

TEST(AgentHarness_Addressing, CompoundMetreHasCompoundBeats)
{
    //! 6/8 has TWO dotted-quarter beats, not six eighth beats. A naive
    //! `4 * DIVISIONS / denominator` gets this wrong (it would say 240), which is exactly why the
    //! implementation asks `TimeSigFrac`. Built by hand because the point is the rule, not a file.
    const TimeSigFrac sixEight(6, 8);
    EXPECT_TRUE(sixEight.isCompound());
    EXPECT_EQ(sixEight.beatTicks(), 3 * QUARTER / 2) << "a 6/8 beat is a dotted quarter";
    EXPECT_EQ(sixEight.beatsPerMeasure(), 2);

    const TimeSigFrac fourFour(4, 4);
    EXPECT_FALSE(fourFour.isCompound());
    EXPECT_EQ(fourFour.beatTicks(), QUARTER);
    EXPECT_EQ(fourFour.beatsPerMeasure(), 4);
}

//! ── Address arithmetic ────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_Addressing, TickToMeasureAndBeat)
{
    const MeasureGrid grid = gridFor();
    ASSERT_GE(grid.measureCount(), 2);

    //! Start of measure 1.
    ScoreAddress a = addressForTick(grid, grid.starts[0]);
    EXPECT_EQ(a.measure, 1);
    EXPECT_EQ(a.beat, 1);
    EXPECT_EQ(a.tickInBeat, 0);

    //! Beat 3 of measure 1 = two quarters in.
    a = addressForTick(grid, grid.starts[0] + 2 * QUARTER);
    EXPECT_EQ(a.measure, 1);
    EXPECT_EQ(a.beat, 3);
    EXPECT_EQ(a.tickInBeat, 0);

    //! An eighth into beat 2: still beat 2, half a beat in.
    a = addressForTick(grid, grid.starts[0] + QUARTER + QUARTER / 2);
    EXPECT_EQ(a.measure, 1);
    EXPECT_EQ(a.beat, 2);
    EXPECT_EQ(a.tickInBeat, QUARTER / 2);

    //! Start of measure 2.
    a = addressForTick(grid, grid.starts[1]);
    EXPECT_EQ(a.measure, 2);
    EXPECT_EQ(a.beat, 1);
}

TEST(AgentHarness_Addressing, LastTickOfAMeasureIsStillInsideIt)
{
    const MeasureGrid grid = gridFor();
    ASSERT_FALSE(grid.isEmpty());

    //! The boundary is the classic off-by-one: the tick one before a measure's end belongs to that
    //! measure, and the tick at its end belongs to the next one. Getting this wrong means an edit is
    //! reported against the wrong bar - the most expensive error this subsystem can make.
    const int end = grid.starts[0] + grid.lengths[0];
    EXPECT_EQ(addressForTick(grid, end - 1).measure, 1);
    if (grid.measureCount() > 1) {
        EXPECT_EQ(addressForTick(grid, end).measure, 2);
    }
}

TEST(AgentHarness_Addressing, OutOfRangeIsRejectedRatherThanClamped)
{
    const MeasureGrid grid = gridFor();
    ASSERT_FALSE(grid.isEmpty());

    //! Past the end of the score: NOT the last measure. Returning the last measure would turn
    //! "the model asked about a tick that does not exist" into "the model is editing the final bar".
    EXPECT_EQ(grid.measureIndexForTick(grid.totalTicks()), -1);
    EXPECT_EQ(grid.measureIndexForTick(grid.totalTicks() + 100000), -1);
    EXPECT_EQ(grid.measureIndexForTick(-1), -1);

    //! And the forward lookups refuse out-of-range measures instead of inventing one.
    EXPECT_EQ(tickForMeasure(grid, -1), -1);
    EXPECT_EQ(tickForMeasure(grid, grid.measureCount()), -1);
    EXPECT_EQ(tickForBeat(grid, 0, 0), -1) << "beats are 1-based";
    EXPECT_EQ(tickForBeat(grid, 0, 99), -1) << "a beat past the end of the bar";
}

TEST(AgentHarness_Addressing, ForwardAndBackwardAgree)
{
    const MeasureGrid grid = gridFor();
    ASSERT_FALSE(grid.isEmpty());

    //! Round-trip every beat of every measure. A one-directional test would pass even if the two
    //! functions disagreed about the base (1-based vs 0-based), which is the likeliest bug here.
    for (int m = 0; m < grid.measureCount(); ++m) {
        const int beats = grid.measureTicks[m] / grid.beatTicks[m];
        for (int b = 1; b <= beats; ++b) {
            const int tick = tickForBeat(grid, m, b);
            ASSERT_GE(tick, 0) << "m" << m << " b" << b;
            const ScoreAddress back = addressForTick(grid, tick);
            EXPECT_EQ(back.measure, m + 1) << "m" << m << " b" << b;
            EXPECT_EQ(back.beat, b) << "m" << m << " b" << b;
            EXPECT_EQ(back.tickInBeat, 0) << "m" << m << " b" << b;
        }
    }
}

TEST(AgentHarness_Addressing, EmptyGridDoesNotInventAMeasure)
{
    //! No score open: the field must still answer something, and the something must be obviously
    //! not-a-real-position rather than a plausible-looking "measure 1".
    const MeasureGrid empty;
    EXPECT_TRUE(empty.isEmpty());
    EXPECT_EQ(empty.totalTicks(), 0);
    EXPECT_EQ(empty.measureIndexForTick(0), -1);
    EXPECT_EQ(tickForMeasure(empty, 0), -1);

    const ScoreAddress a = addressForTick(empty, 1234);
    EXPECT_EQ(a.measure, 1);
    EXPECT_EQ(a.beat, 1);
}

TEST(AgentHarness_Addressing, FormatIsCompactAndStable)
{
    ScoreAddress a;
    a.measure = 3;
    a.beat = 2;
    EXPECT_EQ(formatAddress(a), QStringLiteral("m3b2"));
    EXPECT_EQ(a.toString(), QStringLiteral("m3 b2"));
}
