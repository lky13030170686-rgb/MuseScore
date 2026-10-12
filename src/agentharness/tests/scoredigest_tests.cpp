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
//! The WRITE side, on purpose: the point of these tests is the loop the agent actually runs - write with
//! a recipe, then read the measure back through the digest. Testing the digest against hand-built
//! engraving objects would test my idea of what the score looks like instead of what the tools create.
#include "agentharness/qml/MuseScore/AgentHarness/scorerecipes.h"

using namespace mu::engraving;
using namespace muse::agentharness;

namespace {
const muse::String DIGEST_SCORE(u"data/test.mscx");
//! ⛔ A SECOND score, because clefs, repeats and endings cannot be made to appear in `test.mscx` without
//! changing what every other test in this file reads (第 101 条: three of that round's test failures came
//! from elements the test score already had). Measured on 2026-10-11, the layout score is:
//!   m1 `|:` C4 D4 E4 F4 · m2 `:|` ×3 with the 1st ending, C4 D4 E4 F4 ·
//!   m3 the 2nd ending, a BASS CLEF at its start, C3 D3 E3 F3 ·
//!   m4 two bars in bass clef, a TREBLE CLEF change at t960, two bars in treble clef, and a double bar.
//! ⚠️ Every fact in it was put there by hand-editing the XML, so the tests below are also the check that
//! the FILE READER and this digest agree about what the file says - a clef change that never reached the
//! engraving layer's clef list would read as "no clef change" here rather than as an error.
//! ⛔ And each volta in it must keep BOTH halves (`<next>` at its start, `<prev>` where it ends): a file
//! with only the first half loads cleanly and has no volta at all (see `LayoutTestScoreReallyContainsThe
//! Endings` for the mechanism - it cost an hour of looking at the digest).
const muse::String LAYOUT_SCORE(u"data/layout-test.mscx");

std::shared_ptr<MasterScore> loadScore()
{
    return std::shared_ptr<MasterScore>(mu::engraving::ScoreRW::readScore(DIGEST_SCORE));
}

std::shared_ptr<MasterScore> loadLayoutScore()
{
    return std::shared_ptr<MasterScore>(mu::engraving::ScoreRW::readScore(LAYOUT_SCORE));
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

TEST(AgentHarness_ScoreDigest, OverviewAnswersWhatKeyItIsIn)
{
    //! ⛔ The gap this closes was reported BY THE MODEL, twice, in its own words: asked "what key is it
    //! in?", it said the overview lists time signatures but no key signature, so it would not claim one.
    //! A digest that cannot answer that is missing the second thing a musician asks about a score.
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildScoreOverview(score.get());

    //! The test score has no key signature at all, and THAT is the answer - not a missing line.
    EXPECT_TRUE(text.contains(QStringLiteral("key signatures (staff 1): m1 no sharps or flats")))
        << text.toStdString();
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

//! ── Window: can the reader see what the writer just did? (第 133 条) ───────────────────────────
//!
//! ⛔ WHY THESE EXIST. Measured 2026-10-11: asked to add a staccato and then read the measure back, the
//! agent could not confirm it - the window reported pitches and durations only, while the toolset could
//! create articulations, ties, slurs and dynamics. **A read tool that cannot see its own toolset's writes
//! is half a tool**: the agent cannot verify its work, and cannot see the marks that were already there
//! before it decides what to change. Each test below therefore WRITES with the same recipe the tool calls
//! and then READS through the digest - the exact loop the agent runs.

namespace {
//! A fresh score per test: the recipes write into it, and one test's articulation must not appear in
//! another's read-back.
std::shared_ptr<MasterScore> freshScore()
{
    return loadScore();
}

ScoreAddress firstBeat()
{
    ScoreAddress address;
    address.measure = 1;
    address.beat = 1;
    return address;
}
} // namespace

TEST(AgentHarness_ScoreDigest, WindowShowsAnArticulationThatWasJustAdded)
{
    const std::shared_ptr<MasterScore> score = freshScore();
    ASSERT_TRUE(score != nullptr);

    const RecipeResult written = toggleArticulation(score.get(), firstBeat(), 1, 0,
                                                    QStringLiteral("articStaccatoAbove"));
    ASSERT_TRUE(written.ok) << written.problem.toStdString();

    const QString text = buildMeasureWindow(score.get(), 1, 1);

    //! ⚠️ The SAME SMuFL name the writer took: `SymNames::nameForSymId` is the inverse of the
    //! `symIdByName` the write path validates with, so "add articStaccatoAbove" and "read it back" speak
    //! one vocabulary. A digest that printed an enum number would make the round trip unverifiable.
    //! ⛔ And the assertion is against what the SCORE holds, not against the name that was asked for: a
    //! real turn caught upstream placing the dot BELOW while the tool reported "added …Above" (第 133 条),
    //! so this reads the symbol out of the digest and requires the tool's message to agree with it. The
    //! assertion cannot be wrong about the input - it compares two observations, not an expectation.
    const int idx = text.indexOf(QStringLiteral("art="));
    ASSERT_GE(idx, 0) << text.toStdString();
    const QString onTheChord = text.mid(idx + 4).section(QLatin1Char(' '), 0, 0);

    EXPECT_TRUE(written.detail.contains(onTheChord))
        << "the tool said: " << written.detail.toStdString()
        << " / the score holds: " << onTheChord.toStdString();

    //! ⚠️ And for the placement-paired symbols the message must NOT imply the side is settled: the flip is
    //! decided at LAYOUT time, i.e. after the recipe returned, so what this call can honestly say is where
    //! to look. (Without layout in a unit test the symbol does not flip - which is exactly why the message,
    //! not the test, has to carry the caveat.)
    EXPECT_TRUE(written.detail.contains(QStringLiteral("read the measure back"))) << written.detail.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowShowsADynamicThatWasJustAdded)
{
    const std::shared_ptr<MasterScore> score = freshScore();
    ASSERT_TRUE(score != nullptr);

    const RecipeResult written = addDynamic(score.get(), firstBeat(), QStringLiteral("mf"));
    ASSERT_TRUE(written.ok) << written.problem.toStdString();

    const QString text = buildMeasureWindow(score.get(), 1, 1);

    //! `mf`, not a number: `Dynamic::dynamicText` is the inverse of the `setDynamicType(String)` the write
    //! path uses, so the marking comes back as the string that went in.
    EXPECT_TRUE(text.contains(QStringLiteral("dyn=mf"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowShowsATieBetweenTwoNotes)
{
    const std::shared_ptr<MasterScore> score = freshScore();
    ASSERT_TRUE(score != nullptr);

    //! The test score's measure 1 is C4 D4 E4 F4, and a tie needs the SAME pitch twice - so beat 2 is
    //! turned into another C4 first. ⚠️ Two writes rather than a hand-built tie: the point is the loop the
    //! agent runs, and the agent would have to do exactly this.
    ScoreAddress second;
    second.measure = 1;
    second.beat = 2;
    const RecipeResult retuned = setNotePitch(score.get(), second, 1, 0, 60);
    ASSERT_TRUE(retuned.ok) << retuned.problem.toStdString();

    const RecipeResult written = addTie(score.get(), firstBeat(), 1, 0);
    ASSERT_TRUE(written.ok) << written.problem.toStdString();

    const QString text = buildMeasureWindow(score.get(), 1, 1);

    EXPECT_TRUE(text.contains(QStringLiteral(" tie"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowShowsWhereASlurStartsAndEnds)
{
    const std::shared_ptr<MasterScore> score = freshScore();
    ASSERT_TRUE(score != nullptr);

    const RecipeResult written = addSlur(score.get(), firstBeat(), 1);
    ASSERT_TRUE(written.ok) << written.problem.toStdString();

    const QString text = buildMeasureWindow(score.get(), 1, 1);

    //! ⛔ Both ends, because "a slur exists somewhere" is not actionable: the reader has to know WHICH
    //! notes it covers. ⚠️ These come from the score's spanner map - if `Score::addSlur` ever stops
    //! registering there, this test goes red rather than the digest quietly reporting nothing.
    EXPECT_TRUE(text.contains(QStringLiteral("slur→"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("slur←"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowShowsTheKeyAndWhereItChanges)
{
    const std::shared_ptr<MasterScore> score = freshScore();
    ASSERT_TRUE(score != nullptr);

    //! Two flats from measure 1. The window must state it, because a reader who asks for measures 1..1
    //! alone has no other way to learn what key that bar is in (第 132 条 added it to the overview; a
    //! window that starts mid-score needs it just as much).
    const RecipeResult written = setKeySignature(score.get(), 1, -2);
    ASSERT_TRUE(written.ok) << written.problem.toStdString();

    const QString text = buildMeasureWindow(score.get(), 1, 1);

    EXPECT_TRUE(text.contains(QStringLiteral("key=2 flats"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowReportsAKeyChangeOnlyWhereItChanges)
{
    const std::shared_ptr<MasterScore> score = freshScore();
    ASSERT_TRUE(score != nullptr);

    //! m1 keeps the score's own key (none), m2 gets two flats. A key printed on EVERY line would be noise
    //! that grows with the window; printed only where it changes (plus the first line), the reader sees
    //! the modulation and nothing else.
    const RecipeResult written = setKeySignature(score.get(), 2, -2);
    ASSERT_TRUE(written.ok) << written.problem.toStdString();

    const QString text = buildMeasureWindow(score.get(), 1, 2);
    const QStringList lines = text.split(QLatin1Char('\n'));
    ASSERT_GE(lines.size(), 3) << text.toStdString();

    //! Line 0 is the "measures 1..2 of 2" header; the per-measure lines follow.
    EXPECT_TRUE(lines[1].contains(QStringLiteral("key=no sharps or flats"))) << text.toStdString();
    EXPECT_TRUE(lines[2].contains(QStringLiteral("key=2 flats"))) << text.toStdString();
}

//! ── Window: the layout facts - clefs, repeats, endings (第 134 条) ─────────────────────────────
//!
//! ⛔ WHY THESE EXIST. The window could describe pitch, duration, key, time signature, articulations,
//! dynamics, ties and slurs - and said NOTHING about the clef (which decides what every printed pitch
//! MEANS) or the repeat structure (which decides how many times a passage is played at all). Both halves
//! were reachable and invisible on 2026-10-11: the toolset itself can add a clef
//! (`command://notation/add-clef-violin` / `add-clef-bass` are in the list the agent is handed), and the
//! repeat signs come from the user's own hands. A read tool that cannot see either is describing a
//! different score from the one on the screen.
//!
//! ⚠️ The assertions read the digest of a file whose contents were fixed by hand, so a clef that the
//! engraving layer never learned about shows up here as a missing token rather than as a silent success.

namespace {
//! The per-measure line for `m<N>` out of a window, so a test can say WHICH measure it is talking about
//! instead of "the window contains this string somewhere" (which would pass even if every token were on
//! the wrong line).
QString lineForMeasure(const QString& window, int measure)
{
    const QStringList lines = window.split(QLatin1Char('\n'));
    const QString prefix = QStringLiteral("m%1 ").arg(measure);
    for (const QString& line : lines) {
        if (line.startsWith(prefix)) {
            return line;
        }
    }
    return QString();
}
} // namespace

TEST(AgentHarness_ScoreDigest, WindowShowsTheClefAndOnlyWhereItChanges)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildMeasureWindow(score.get(), 1, 4);

    //! The clef is stated on the window's first measure - a reader who asks for measures 5..8 alone has
    //! no other way to learn it - and afterwards only where it changes, exactly like `key=`.
    EXPECT_TRUE(lineForMeasure(text, 1).contains(QStringLiteral("clef=G"))) << text.toStdString();
    EXPECT_TRUE(lineForMeasure(text, 3).contains(QStringLiteral("clef=F"))) << text.toStdString();
    //! ⛔ Measure 2 is in the SAME clef as measure 1, so it must say nothing: a clef repeated on every
    //! bar is the noise this rule exists to avoid.
    EXPECT_FALSE(lineForMeasure(text, 2).contains(QStringLiteral("clef="))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowStatesAClefEvenWhenTheWindowStartsMidScore)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    //! ⛔ A window that starts at measure 3 must not answer "no clef mentioned" - the clef in force is a
    //! fact about that bar whether or not the window saw the change happen.
    const QString text = buildMeasureWindow(score.get(), 3, 3);
    EXPECT_TRUE(lineForMeasure(text, 3).contains(QStringLiteral("clef=F"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowReportsAClefChangeInsideAMeasureAtItsOwnTick)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    //! ⛔ A clef change is NOT measure-aligned: this one is at t960 of the LAST measure. An
    //! implementation that sampled the clef at measure starts would report measure 4 as bass clef and
    //! never mention the change at all - so this assertion is what pins the position-accurate walk.
    const QString text = buildMeasureWindow(score.get(), 4, 4);
    const QString line = lineForMeasure(text, 4);
    ASSERT_FALSE(line.isEmpty()) << text.toStdString();

    EXPECT_TRUE(line.contains(QStringLiteral("t960 s1 clef=G"))) << text.toStdString();
    //! And measure 4 itself starts in the OLD clef (bass), so the prefix must say F: a mid-measure change
    //! reported as if it were the measure's clef would be wrong about the first two beats.
    EXPECT_TRUE(line.contains(QStringLiteral("clef=F"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowPutsAClefChangeInFrontOfTheNotesItAppliesTo)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    const QString line = lineForMeasure(buildMeasureWindow(score.get(), 4, 4), 4);
    ASSERT_FALSE(line.isEmpty());

    //! ⛔ Order is meaning here: a clef changes what the notes AFTER it are. Printing the token after them
    //! would describe the same objects while telling the reader the opposite thing.
    const int clefAt = line.indexOf(QStringLiteral("clef=G"));
    const int noteAfter = line.indexOf(QStringLiteral("[G4]"));
    ASSERT_GE(clefAt, 0) << line.toStdString();
    ASSERT_GE(noteAfter, 0) << line.toStdString();
    EXPECT_LT(clefAt, noteAfter) << line.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowReportsRepeatsAndHowManyTimesTheyPlay)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildMeasureWindow(score.get(), 1, 2);

    EXPECT_TRUE(lineForMeasure(text, 1).contains(QStringLiteral("start-repeat"))) << text.toStdString();
    //! ⛔ The COUNT, not just "there is a repeat": `:|` with a 3 over it is played three times, and a
    //! reader that only learns "there is a repeat here" will count the bars wrong.
    EXPECT_TRUE(lineForMeasure(text, 2).contains(QStringLiteral("end-repeat×3"))) << text.toStdString();
    //! ⚠️ And the score's own file says `<endRepeat>3</endRepeat>`, so this also checks that the repeat
    //! count survived the file reader - not just that the token is spelled the way the writer meant.
}

TEST(AgentHarness_ScoreDigest, LayoutTestScoreReallyContainsTheEndings)
{
    //! ⛔ THE INPUT, PINNED FIRST. Everything below reads the endings out of this file, and on 2026-10-11
    //! the file was WRONG in a way that looks exactly like the digest being wrong: a spanner is written in
    //! TWO halves (`<Spanner><Volta>…<next><location>` where it starts, `<Spanner><prev>…` where it ends),
    //! and `ReadContext::addConnectorInfo` only adds a chain to the score once `finished()` holds - so a
    //! file carrying the `<next>` half alone loads without a single warning and simply has no volta. The
    //! digest then reported no endings, which is the right answer about the score it was handed and the
    //! wrong answer about the score I meant to write (第 100 条的形状: 三件各自为真的事拼出一个合理但全错的
    //! 故事 - there, a stale `Measure*`; here, a half-written spanner).
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    std::vector<Fraction> starts;
    for (const auto& entry : score->spannerMap().map()) {
        const Spanner* spanner = entry.second;
        if (spanner && spanner->isVolta()) {
            starts.push_back(spanner->tick());
        }
    }

    ASSERT_EQ(starts.size(), 2u) << "the test score no longer carries its two voltas - check that each one "
                                    "still has BOTH halves (`<next>` at its start, `<prev>` at its end)";
    //! m2 (tick 1920) and m3 (tick 3840) in 4/4 - the 1st and 2nd endings the window test below names.
    //! ⚠️ Compared as TICKS: `Fraction` normalises (`1920/1920` is `1/1`), so asserting `Fraction(1920, 1)`
    //! compares against a whole note and fails for a reason that has nothing to do with the score.
    ASSERT_EQ(starts[0].ticks(), 1920);
    ASSERT_EQ(starts[1].ticks(), 3840);
}

TEST(AgentHarness_ScoreDigest, WindowNamesTheEndingsAndTheMeasuresTheyCover)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildMeasureWindow(score.get(), 1, 4);

    //! ⛔ Both ends of each ending, because "there is a volta here" is not actionable: the reader needs
    //! to know WHICH bars are the first time through and which are the second.
    EXPECT_TRUE(text.contains(QStringLiteral("endings (voltas): 1st ending m2-m2, 2nd ending m3-m3")))
        << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowLeavesOutEndingsThatDoNotTouchIt)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    //! ⛔ The failure this guards: a per-score line pasted into every window. A reader asking about
    //! measure 1 would be told about endings in measures 2-3 and could go looking for them there.
    const QString text = buildMeasureWindow(score.get(), 1, 1);
    EXPECT_FALSE(text.contains(QStringLiteral("endings"))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, WindowReportsANonNormalBarline)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildMeasureWindow(score.get(), 1, 4);
    EXPECT_TRUE(lineForMeasure(text, 4).contains(QStringLiteral("barline=double"))) << text.toStdString();
    //! ⚠️ An ordinary barline is not reported - otherwise every measure of every score carries a token
    //! that says nothing.
    EXPECT_FALSE(lineForMeasure(text, 1).contains(QStringLiteral("barline="))) << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, OverviewReportsTheRepeatStructure)
{
    const std::shared_ptr<MasterScore> score = loadLayoutScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildScoreOverview(score.get());

    EXPECT_TRUE(text.contains(QStringLiteral("repeats: m1-m2 ×3"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("endings (voltas): 1st ending m2-m2, 2nd ending m3-m3")))
        << text.toStdString();
}

TEST(AgentHarness_ScoreDigest, OverviewSaysSoWhenThereAreNoRepeats)
{
    const std::shared_ptr<MasterScore> score = loadScore();
    ASSERT_TRUE(score != nullptr);

    const QString text = buildScoreOverview(score.get());

    //! ⛔ An absent line and "there are none" are different answers, and only one of them can be acted
    //! on: a reader that finds no `repeats:` line cannot tell whether the score has no repeats or
    //! whether the tool forgot to look. Same reason the overview says `empty measures: none`.
    EXPECT_TRUE(text.contains(QStringLiteral("repeats: none"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("endings"))) << text.toStdString();
}
