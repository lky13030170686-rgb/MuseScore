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

#include "engraving/dom/chord.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"
#include "engraving/tests/utils/scorerw.h"

#include "agentharness/qml/MuseScore/AgentHarness/addressing.h"
#include "agentharness/qml/MuseScore/AgentHarness/notelocator.h"

using namespace mu::engraving;
using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS AT ALL.
//!
//! The locator is where every addressed write begins, and it had NO tests - the recipes were verified
//! by running the program, which only exercises the paths a test case happens to reach. Round 14 found
//! the cost of that: a reverse check on "that bar is already a rest" answered "there is no note at
//! measure 2 beat 1", because the locator could not see a whole-measure rest at all. The positive test
//! (turning a note into a rest) passed throughout.
//!
//! So this suite is deliberately about the SHAPE of the search space - what is found, what is not, and
//! what the caller is told about it - rather than about any one recipe.

namespace {
//! ⚠️ The paths are RELATIVE TO THE DATA ROOT that `environment.cpp` sets via
//! `ScoreRW::setRootPath`, including the `data/` segment. Passing a bare filename makes
//! `ScoreRW::readScore` return null, and every assertion then fails on "score is null" rather than on
//! anything to do with the locator - which is exactly what happened the first time this suite ran.
const muse::String NOTE_SCORE(u"data/test.mscx");        //!< 4/4, 2 bars: C4 D4 E4 F4 | whole rest
const muse::String CHORD_SCORE(u"data/chord-test.mscx"); //!< 4/4, 1 bar: a three-note chord [C4 E4 B4]
const muse::String TIE_SCORE(u"data/tie-test.mscx");     //!< 4/4, 2 bars: whole C4 | whole C4

std::shared_ptr<MasterScore> loadScore(const muse::String& name)
{
    return std::shared_ptr<MasterScore>(mu::engraving::ScoreRW::readScore(name));
}

ScoreAddress address(int measure, int beat, int staff = 1)
{
    ScoreAddress a;
    a.measure = measure;
    a.beat = beat;
    a.staff = staff - 1;   //!< 0-based inside, 1-based at the boundary - see addressing.h
    return a;
}
} // namespace

//! ── Positions that do not exist ───────────────────────────────────────────────────────────────

TEST(AgentHarness_NoteLocator, MeasureOutOfRangeIsReportedWithTheCount)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup found = chordAt(score.get(), address(99, 1), 0);
    EXPECT_FALSE(found.found());
    //! The message names the number AND how many there are: a caller told only "no such measure" has
    //! to guess whether it asked for bar 3 of 2 or bar 30 of 200.
    EXPECT_TRUE(found.problem.contains(QStringLiteral("no measure 99"))) << found.problem.toStdString();
    EXPECT_TRUE(found.problem.contains(QStringLiteral("2"))) << found.problem.toStdString();
}

TEST(AgentHarness_NoteLocator, MeasureZeroIsReportedRatherThanClamped)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ CLAMPING WOULD BE WORSE THAN FAILING. A caller that asked for measure 0 and got measure 1
    //! would edit a bar it did not name, and nothing in the result would say so.
    const NoteLookup found = chordAt(score.get(), address(0, 1), 0);
    EXPECT_FALSE(found.found());
    EXPECT_FALSE(found.problem.isEmpty());
}

TEST(AgentHarness_NoteLocator, StaffOutOfRangeIsReported)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup found = chordAt(score.get(), address(1, 1, 5), 0);
    EXPECT_FALSE(found.found());
    EXPECT_TRUE(found.problem.contains(QStringLiteral("no staff 5"))) << found.problem.toStdString();
}

TEST(AgentHarness_NoteLocator, BeatOutOfRangeIsReported)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup found = chordAt(score.get(), address(1, 99), 0);
    EXPECT_FALSE(found.found());
    EXPECT_TRUE(found.problem.contains(QStringLiteral("beat 99"))) << found.problem.toStdString();
}

//! ── Finding notes ─────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_NoteLocator, FindsTheNoteOnEachBeat)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const int expected[4] = { 60, 62, 64, 65 };   //!< C4 D4 E4 F4
    for (int beat = 1; beat <= 4; ++beat) {
        const NoteLookup found = noteAt(score.get(), address(1, beat), 0, -1);
        ASSERT_TRUE(found.ok()) << "beat " << beat << ": " << found.problem.toStdString();
        EXPECT_EQ(found.note->pitch(), expected[beat - 1]) << "beat " << beat;
        EXPECT_TRUE(found.chord != nullptr);
        EXPECT_FALSE(found.isRest());
    }
}

TEST(AgentHarness_NoteLocator, VoiceZeroTakesWhicheverVoiceHasSomething)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! A caller that did not think about voices means "whatever is here", and the test data has one
    //! voice - so asking for voice 0 and voice 1 must give the same note.
    const NoteLookup anyVoice = noteAt(score.get(), address(1, 1), 0, -1);
    const NoteLookup voiceOne = noteAt(score.get(), address(1, 1), 1, -1);
    ASSERT_TRUE(anyVoice.ok());
    ASSERT_TRUE(voiceOne.ok());
    EXPECT_EQ(anyVoice.note->pitch(), voiceOne.note->pitch());
}

TEST(AgentHarness_NoteLocator, AChordWithSeveralNotesRefusesAnUnspecifiedIndex)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ PICKING notes[0] WOULD EDIT A NOTE THE CALLER DID NOT NAME, and the caller would have no way
    //! to tell - the edit would simply land on the wrong pitch. The message has to say how to
    //! disambiguate, or the caller is stuck.
    const NoteLookup found = noteAt(score.get(), address(1, 1), 0, -1);
    EXPECT_FALSE(found.ok());
    EXPECT_TRUE(found.problem.contains(QStringLiteral("3 notes"))) << found.problem.toStdString();
    EXPECT_TRUE(found.problem.contains(QStringLiteral("note"))) << found.problem.toStdString();

    //! The CHORD is still findable, which is what recipes that act on the whole beat need.
    const NoteLookup chord = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(chord.found());
    ASSERT_TRUE(chord.chord != nullptr);
    EXPECT_EQ(chord.chord->notes().size(), 3u);
}

TEST(AgentHarness_NoteLocator, AnExplicitIndexSelectsOneNoteOfTheChord)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    const int expected[3] = { 60, 64, 67 };   //!< C4 E4 B4, lowest first
    for (int i = 0; i < 3; ++i) {
        const NoteLookup found = noteAt(score.get(), address(1, 1), 0, i);
        ASSERT_TRUE(found.ok()) << "index " << i << ": " << found.problem.toStdString();
        EXPECT_EQ(found.note->pitch(), expected[i]) << "index " << i;
    }
}

TEST(AgentHarness_NoteLocator, AnIndexPastTheEndOfTheChordIsReported)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup found = noteAt(score.get(), address(1, 1), 0, 7);
    EXPECT_FALSE(found.ok());
    EXPECT_TRUE(found.problem.contains(QStringLiteral("3 note"))) << found.problem.toStdString();
}

//! ── Rests: the blind spot that cost round 14 ──────────────────────────────────────────────────

TEST(AgentHarness_NoteLocator, AWholeMeasureRestIsFoundAndReportedAsARest)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔⛔ THIS IS THE REGRESSION GUARD FOR THE ROUND-14 BLIND SPOT. A whole-measure rest does NOT live
    //! on a `SegmentType::ChordRest` segment - it lives on the measure's own segment - so a walk
    //! restricted to ChordRest segments cannot see it, and the caller was told "there is no note at
    //! measure 2 beat 1" about a bar that is entirely rest.
    const NoteLookup found = chordAt(score.get(), address(2, 1), 0);
    EXPECT_TRUE(found.found()) << "a whole-measure rest must be findable: " << found.problem.toStdString();
    EXPECT_TRUE(found.isRest());
    EXPECT_FALSE(found.ok()) << "there is no NOTE there, and ok() asks about notes";
    EXPECT_TRUE(found.problem.isEmpty()) << "the lookup succeeded, so there is nothing to report";
}

TEST(AgentHarness_NoteLocator, AskingForANoteOnARestSaysThereIsNoNote)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup found = noteAt(score.get(), address(2, 1), 0, -1);
    EXPECT_FALSE(found.ok());
    EXPECT_FALSE(found.problem.isEmpty()) << "a failure must say why";
}

TEST(AgentHarness_NoteLocator, ARestIsFoundEvenForAVoiceThatHasNoElement)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⚠️ A whole-measure rest is stored ONCE, on the measure - not once per voice. So asking about
    //! voice 2 of an empty bar has to answer "that is a rest" rather than "there is nothing here",
    //! and the locator's fallback to any chord-like element on the segment is what makes that true.
    for (int voice = 0; voice <= 4; ++voice) {
        const NoteLookup found = chordAt(score.get(), address(2, 1), voice);
        EXPECT_TRUE(found.found()) << "voice " << voice << ": " << found.problem.toStdString();
        EXPECT_TRUE(found.isRest()) << "voice " << voice;
    }
}

//! ── Walking forward ───────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_NoteLocator, NextNoteSkipsRestsAndMeasures)
{
    const auto score = loadScore(TIE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup first = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(first.ok()) << first.problem.toStdString();

    //! The next note is in the NEXT MEASURE, not on the next beat. That is exactly why the walk exists
    //! rather than the caller computing "the next beat and looking it up" - the next beat is a rest.
    const NoteLookup next = nextNote(score.get(), first.note);
    ASSERT_TRUE(next.ok()) << next.problem.toStdString();
    EXPECT_EQ(next.note->pitch(), first.note->pitch());
    EXPECT_NE(next.note->chord()->tick(), first.note->chord()->tick());
}

TEST(AgentHarness_NoteLocator, NextNoteFromTheLastNoteSaysSo)
{
    const auto score = loadScore(TIE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup second = noteAt(score.get(), address(2, 1), 0, -1);
    ASSERT_TRUE(second.ok()) << second.problem.toStdString();

    const NoteLookup next = nextNote(score.get(), second.note);
    EXPECT_FALSE(next.ok());
    //! ⛔ A FAILURE MUST SAY WHY. An empty `problem` is indistinguishable from "the lookup never ran",
    //! which is what made the round-13 bug expensive to find.
    EXPECT_FALSE(next.problem.isEmpty()) << "no next note is a reason, not a silence";
}

TEST(AgentHarness_NoteLocator, NextNoteWithNoSamePitchLaterIsReported)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! test.mscx is C4 D4 E4 F4, so no later note shares C4's pitch. Tying would connect two different
    //! pitches - which is a SLUR, not a tie - so there is nothing to tie to and the caller is told.
    const NoteLookup first = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(first.ok());

    const NoteLookup next = nextNote(score.get(), first.note);
    EXPECT_FALSE(next.ok());
    EXPECT_FALSE(next.problem.isEmpty());
}

TEST(AgentHarness_NoteLocator, NextNoteWithNoNoteAtAllIsReported)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    EXPECT_FALSE(nextNote(score.get(), nullptr).ok());
}
