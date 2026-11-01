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

#include "agentharness/qml/MuseScore/AgentHarness/selectiondigest.h"

using namespace muse::agentharness;

namespace {
SelectionFacts rangeFacts(int firstMeasure, int firstBeat, int lastMeasure, int lastBeat,
                          int firstStaff, int lastStaff, int notes)
{
    SelectionFacts facts;
    facts.kind = SelectionFacts::Kind::Range;
    facts.firstMeasure = firstMeasure;
    facts.firstBeat = firstBeat;
    facts.lastMeasure = lastMeasure;
    facts.lastBeat = lastBeat;
    facts.firstStaff = firstStaff;
    facts.lastStaff = lastStaff;
    facts.noteCount = notes;
    return facts;
}
} // namespace

//! Nothing selected is an EMPTY string, not the word "none": the caller owns that word (the digest prints
//! `selection: none`, the panel draws a dash). A helper that also chose the wording would force both
//! surfaces to agree with it, and the model-facing and the human-facing sentences are not the same thing.
TEST(AgentHarness_SelectionDigest, NothingSelectedIsEmpty)
{
    SelectionFacts facts;
    EXPECT_TRUE(describeSelection(facts).isEmpty());
}

//! The ordinary case: a drag over whole measures. Both ends are positions, because a box selection can start
//! and end mid-measure and the address is what the write tools take.
TEST(AgentHarness_SelectionDigest, RangeReadsAsAddressesStaffSpanAndNoteCount)
{
    const QString text = describeSelection(rangeFacts(3, 1, 5, 4, 1, 1, 6));

    EXPECT_TRUE(text.contains(QStringLiteral("m3b1..m5b4"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("staff 1"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("6 note(s)"))) << text.toStdString();
}

//! ⛔ THE OFF-BY-ONE THAT MATTERS. `endTick()` is exclusive, so a selection ending exactly on a barline must
//! be reported as ending in the LAST SELECTED measure - the controller subtracts one tick before asking the
//! grid. Here the facts already carry the corrected position; this pins the OTHER half of the rule: a range
//! that ends at the start of a measure must not print that measure as the last one.
TEST(AgentHarness_SelectionDigest, RangeEndingAtABarlineDoesNotClaimTheNextMeasure)
{
    const QString text = describeSelection(rangeFacts(1, 1, 2, 4, 1, 1, 8));

    EXPECT_TRUE(text.contains(QStringLiteral("m1b1..m2b4"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("m3"))) << "the measure after the last selected one leaked in";
}

//! Two staves selected means "staves 1..2"; one staff means "staff 2" - the plural form is not decoration,
//! it is the difference between a range and a single staff, and a reader acting on the wrong span is a real
//! risk on a piano score.
TEST(AgentHarness_SelectionDigest, StaffSpanIsWrittenOnlyWhenThereIsOne)
{
    const QString two = describeSelection(rangeFacts(2, 1, 2, 4, 1, 2, 4));
    EXPECT_TRUE(two.contains(QStringLiteral("staves 1..2"))) << two.toStdString();

    const QString one = describeSelection(rangeFacts(2, 1, 2, 4, 2, 2, 4));
    EXPECT_TRUE(one.contains(QStringLiteral("staff 2"))) << one.toStdString();
    EXPECT_FALSE(one.contains(QStringLiteral("staves"))) << one.toStdString();
}

//! A range of exactly one beat is still a RANGE (it has a span and a note count), not a single element.
TEST(AgentHarness_SelectionDigest, OneBeatRangeStaysARange)
{
    SelectionFacts facts = rangeFacts(3, 2, 3, 2, 1, 1, 1);
    const QString text = describeSelection(facts);

    EXPECT_TRUE(text.startsWith(QStringLiteral("m3b2"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("1 note(s)"))) << text.toStdString();
}

//! A single click on a note: the address plus what is there. "C4" is the part that lets a reader confirm the
//! agent is about to change the pitch they meant, not merely the bar they clicked.
TEST(AgentHarness_SelectionDigest, SingleNoteShowsPositionTypeAndPitch)
{
    SelectionFacts facts;
    facts.kind = SelectionFacts::Kind::Single;
    facts.firstMeasure = 3;
    facts.firstBeat = 2;
    facts.firstStaff = 1;
    facts.lastStaff = 1;
    facts.noteCount = 1;
    facts.elementType = QStringLiteral("Note");
    facts.pitch = QStringLiteral("C4");

    const QString text = describeSelection(facts);
    EXPECT_TRUE(text.contains(QStringLiteral("m3b2"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Note"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("C4"))) << text.toStdString();
}

//! A single click on a measure (the most common "select a bar" gesture) has no pitch and must not print an
//! empty parenthesis or a stray space where the pitch would be.
TEST(AgentHarness_SelectionDigest, SingleMeasureHasNoPitchNoise)
{
    SelectionFacts facts;
    facts.kind = SelectionFacts::Kind::Single;
    facts.firstMeasure = 2;
    facts.firstBeat = 1;
    facts.elementType = QStringLiteral("Measure");

    const QString text = describeSelection(facts);
    EXPECT_EQ(text, QStringLiteral("m2b1 · Measure")) << text.toStdString();
}

//! An unresolvable position must not be invented. The controller resolves positions against the measure grid;
//! if a selection ever comes back pointing outside it, the honest answer is the element type alone, never a
//! confident "m1b1".
TEST(AgentHarness_SelectionDigest, UnknownPositionIsNotGuessed)
{
    SelectionFacts facts;
    facts.kind = SelectionFacts::Kind::Single;
    facts.elementType = QStringLiteral("Dynamic");

    const QString text = describeSelection(facts);
    EXPECT_EQ(text, QStringLiteral("Dynamic")) << text.toStdString();

    SelectionFacts emptyRange = rangeFacts(0, 0, 0, 0, 0, 0, 0);
    EXPECT_TRUE(describeSelection(emptyRange).isEmpty());
}

//! ⛔ THE OFF-BY-ONE, PINNED WHERE IT LIVES. `endTick()` is exclusive, so the last tick INSIDE a selection
//! that ends exactly on a barline is one below it. Reporting the closed bound claims the next measure - and
//! the line still looks completely ordinary, which is why it is a test and not a review note.
TEST(AgentHarness_SelectionDigest, LastTickOfARangeIsInsideIt)
{
    //! A 4/4 bar is 1920 ticks; a selection of exactly two bars is 0..3840 exclusive.
    EXPECT_EQ(rangeLastTick(0, 3840), 3839);
    EXPECT_EQ(rangeLastTick(0, 1920), 1919);

    //! An empty or reversed range answers its own start, never a tick before it (which would address a
    //! measure that was never selected).
    EXPECT_EQ(rangeLastTick(960, 960), 960);
    EXPECT_EQ(rangeLastTick(960, 0), 960);
}

//! ⛔ THE SAME OFF-BY-ONE IN THE OTHER UNIT. A one-staff selection is `start=0, end=1` in the half-open API,
//! and must read "staff 1" - the first version's `end + 1` printed "staves 1..2" on a one-staff score
//! (measured on the real program, 第 173 条).
TEST(AgentHarness_SelectionDigest, StaffSpanIsHalfOpenInTheApiAndInclusiveOnScreen)
{
    const StaffSpan one = staffSpanForDisplay(0, 1);
    EXPECT_EQ(one.first, 1);
    EXPECT_EQ(one.last, 1);

    const StaffSpan two = staffSpanForDisplay(0, 2);
    EXPECT_EQ(two.first, 1);
    EXPECT_EQ(two.last, 2);

    //! The SECOND staff only: indices 1..2 exclusive ⇒ "staff 2", not "staff 2..2" by accident and not
    //! "staves 2..3".
    const StaffSpan second = staffSpanForDisplay(1, 2);
    EXPECT_EQ(second.first, 2);
    EXPECT_EQ(second.last, 2);

    //! No staff information: 0..0, which `describeSelection` then leaves out entirely.
    const StaffSpan none = staffSpanForDisplay(0, 0);
    EXPECT_EQ(none.first, 0);
    EXPECT_EQ(none.last, 0);

    //! And the two names the panel actually shows.
    EXPECT_TRUE(describeSelection(rangeFacts(3, 1, 5, 4, 1, 1, 6)).contains(QStringLiteral("staff 1")));
    EXPECT_TRUE(describeSelection(rangeFacts(3, 1, 5, 4, 1, 2, 6)).contains(QStringLiteral("staves 1..2")));
}
