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
#include "engraving/dom/score.h"
#include "engraving/tests/utils/scorerw.h"

#include "agentharness/qml/MuseScore/AgentHarness/scoredigest.h"

using namespace mu::engraving;
using namespace muse::agentharness;

namespace {
const muse::String DIGEST_SCORE(u"data/test.mscx");

std::shared_ptr<MasterScore> loadScore()
{
    return std::shared_ptr<MasterScore>(mu::engraving::ScoreRW::readScore(DIGEST_SCORE));
}
} // namespace

//! ── Pure helpers ──────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreDigest, PitchNamesUseScientificOctaves)
{
    //! MIDI 60 is C4, not C5 and not C3. An off-by-one octave reads perfectly plausible in a digest
    //! and would mislead every downstream decision, so it is pinned here rather than reasoned about
    //! at each call site.
    EXPECT_EQ(pitchName(60), QStringLiteral("C4"));
    EXPECT_EQ(pitchName(61), QStringLiteral("C#4"));
    EXPECT_EQ(pitchName(69), QStringLiteral("A4")) << "A4 = 440Hz is the reference";
    EXPECT_EQ(pitchName(0), QStringLiteral("C-1"));
    EXPECT_EQ(pitchName(127), QStringLiteral("G9"));

    //! Out of range must not index past the name table.
    EXPECT_EQ(pitchName(-1), QStringLiteral("?"));
    EXPECT_EQ(pitchName(128), QStringLiteral("?"));
}

//! ── Overview ──────────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreDigest, OverviewDescribesTheRealScore)
{
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildScoreOverview(score.get());

    EXPECT_TRUE(text.contains(QStringLiteral("measures: 2"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("staves: 1"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("4/4"))) << text.toStdString();
    //! The staff list is the part a reader actually uses to address the score, so its presence is
    //! part of the contract, not decoration.
    EXPECT_TRUE(text.contains(QStringLiteral("staff 1"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, OverviewIsBounded)
{
    //! The whole point of a summary is that it is safe to send without measuring first. If this
    //! grows with the score it has stopped being a summary - and the failure would be silent until
    //! someone hit a context limit.
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildScoreOverview(score.get());
    const int lines = text.count(QLatin1Char('\n')) + 1;

    EXPECT_LT(lines, 60) << "overview grew to " << lines << " lines";
}

TEST(AgentHarness_ScoreDigest, OverviewOfNoScoreSaysSo)
{
    //! "no score" and "a score with nothing in it" must not look the same: the first means the tool
    //! was called at the wrong time, the second means the score is genuinely empty.
    EXPECT_EQ(buildScoreOverview(nullptr), QStringLiteral("(no score)"));
}

TEST(AgentHarness_ScoreDigest, OverviewCountsNotesOnTheRealScore)
{
    //! The test score has real notes; a traversal that silently visited nothing would still produce
    //! a well-formed overview, which is exactly why the count is asserted rather than the shape.
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildScoreOverview(score.get());

    //! Pull the note count out of the staff line and require it to be positive.
    const int idx = text.indexOf(QStringLiteral("notes="));
    ASSERT_GE(idx, 0) << text.toStdString();
    const int value = text.mid(idx + 6).section(QLatin1Char(' '), 0, 0).toInt();
    EXPECT_GT(value, 0) << "the traversal found no notes on a score that has them";
}

//! ── Window ────────────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreDigest, WindowCoversExactlyTheRequestedMeasures)
{
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildMeasureWindow(score.get(), 1, 1);

    EXPECT_TRUE(text.contains(QStringLiteral("measures 1..1 of 2"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("m1 "))) << text.toStdString();
    //! Measure 2 must NOT be in a window that asked for measure 1 only. Off-by-one here means the
    //! model edits a bar it never asked about.
    EXPECT_FALSE(text.contains(QStringLiteral("m2 "))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowReportsOutOfRangeInsteadOfClamping)
{
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    //! ⛔ The failure this guards: clamping "measure 99" to the last measure produces a confident,
    //! well-formed answer about the wrong bar. The addressing layer refuses to do that (it returns
    //! -1); the digest must refuse too, and must say so in words.
    const QString text = buildMeasureWindow(score.get(), 99, 100);
    EXPECT_TRUE(text.contains(QStringLiteral("outside this score"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("m99"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowRejectsInvertedAndZeroRanges)
{
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    //! Measures are 1-based everywhere in this subsystem; 0 and inverted ranges are caller errors,
    //! and reporting them as such is cheaper to debug than an empty window.
    EXPECT_TRUE(buildMeasureWindow(score.get(), 0, 1).contains(QStringLiteral("outside this score")));
    EXPECT_TRUE(buildMeasureWindow(score.get(), 2, 1).contains(QStringLiteral("outside this score")));
}

TEST(AgentHarness_ScoreDigest, WindowClampsTheUpperEndToTheScore)
{
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    //! Asking for more than exists is a *range* question, not a *target* question: the start is
    //! valid, so answering with what exists is helpful rather than misleading. That is the opposite
    //! decision from the out-of-range case above, and deliberately so.
    const QString text = buildMeasureWindow(score.get(), 2, 999);
    EXPECT_TRUE(text.contains(QStringLiteral("measures 2..2 of 2"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("m2 "))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowOfNoScoreSaysSo)
{
    EXPECT_EQ(buildMeasureWindow(nullptr, 1, 1), QStringLiteral("(no score)"));
}
