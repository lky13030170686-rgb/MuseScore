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

#include "agentharness/qml/MuseScore/AgentHarness/eventattribution.h"

using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS (第 139 条).
//!
//! One `split-measure` put FOUR rows in the timeline - three of them automation rebuilds that the split
//! caused - and `revision` climbed by four as well. The rule that fixes it is a three-branch decision, so it
//! is a pure class with tests rather than a handful of lines buried in a Qt slot that needs a running
//! application to exercise.
//!
//! ⛔ AND THE FIRST VERSION OF THAT RULE WAS WRONG IN A WAY ONLY THE REAL APPLICATION COULD SHOW: holding
//! "while a transaction is open" caught two of the split's three changes and missed the third, because
//! `TransactionManager::endTransaction` rebuilds the automation *after* ending the transaction. The tests
//! below pin the rule that survived that measurement: hold for the turn, let the score event absorb.

TEST(AgentHarness_EventAttribution, HeldChangesAreAbsorbedByTheEventOfTheSameTurn)
{
    AutomationAttribution attribution;

    //! The measured shape of one `split-measure`: three automation notifications, then the transaction's own
    //! event. (The first two arrive inside the transaction, the third during the commit - which is exactly
    //! why "is a transaction open" was the wrong test.)
    EXPECT_TRUE(attribution.hold()) << "the first hold is the caller's cue to schedule the release";
    EXPECT_FALSE(attribution.hold()) << "the second one must not schedule a second release";
    EXPECT_FALSE(attribution.hold());
    EXPECT_EQ(attribution.heldCount(), 3);

    EXPECT_EQ(attribution.absorb(), 3) << "the event that describes the action absorbs all three";
    EXPECT_EQ(attribution.heldCount(), 0) << "and nothing is left for extra rows";
}

TEST(AgentHarness_EventAttribution, AnEventWithNothingHeldAbsorbsNothing)
{
    AutomationAttribution attribution;

    //! Every ordinary edit looks like this (a typed note changes the score and nothing else): the event must
    //! report zero absorbed, because a non-zero count would make the field log an absorption that never
    //! happened.
    EXPECT_EQ(attribution.absorb(), 0);
    EXPECT_EQ(attribution.heldCount(), 0);
}

TEST(AgentHarness_EventAttribution, ATurnThatEndsWithoutAnEventRecordsWhatItHeld)
{
    AutomationAttribution attribution;

    //! ⛔ THE CASE THAT KEEPS A CHANGE FROM DISAPPEARING. A standalone automation change (a tempo drag
    //! outside any score edit, the initial build at load) has no event to absorb it - so when the turn ends
    //! it is recorded as its own event after all.
    EXPECT_TRUE(attribution.hold()) << "the first hold of the turn schedules the release";
    EXPECT_FALSE(attribution.hold()) << "a second change in the same turn must not schedule a second one";

    EXPECT_EQ(attribution.release(), 2) << "uncovered changes are handed back to be recorded";
    EXPECT_EQ(attribution.heldCount(), 0) << "and the hold is empty afterwards, not counted twice";

    //! Released exactly once: a second end reports nothing.
    EXPECT_EQ(attribution.release(), 0);
}

TEST(AgentHarness_EventAttribution, AnEventAfterAReleaseDoesNotAbsorbThemTwice)
{
    AutomationAttribution attribution;

    //! The pathological order - release first, event second - must not resurrect the released changes: they
    //! were recorded, and absorbing them now would suggest the event covered something it did not.
    EXPECT_TRUE(attribution.hold());
    EXPECT_EQ(attribution.release(), 1);
    EXPECT_EQ(attribution.absorb(), 0);
}

TEST(AgentHarness_EventAttribution, HoldingResumesCleanlyAfterEachTurn)
{
    AutomationAttribution attribution;

    //! Two consecutive actions, each with its own automation changes: the state must not be stuck, and the
    //! second action's first hold must schedule its own release.
    EXPECT_TRUE(attribution.hold());
    EXPECT_EQ(attribution.absorb(), 1);

    EXPECT_TRUE(attribution.hold()) << "a new turn's first hold is a new release cue";
    EXPECT_EQ(attribution.absorb(), 1);
    EXPECT_EQ(attribution.heldCount(), 0);
}
