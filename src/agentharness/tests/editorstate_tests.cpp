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

#include "engraving/dom/measure.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/score.h"
#include "engraving/dom/select.h"
#include "engraving/editing/transaction/undostack.h"
#include "engraving/tests/utils/scorerw.h"

#include "agentharness/qml/MuseScore/AgentHarness/editorstate.h"
//! The recipe that adds a staff, for the one test that needs two of them - the staff parameter cannot be
//! tested on a single-staff score, and a parameter nobody exercises is a parameter nobody knows works.
#include "agentharness/qml/MuseScore/AgentHarness/scorerecipes.h"

using namespace mu::engraving;
using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS (第 138 条).
//!
//! The toolset could address notes, measures, beats and staves - and could not address the SELECTION, which
//! is what the command layer's 400+ commands act on. Measured 2026-10-13, asked to split a measure, the
//! agent answered, correctly, that it could not guarantee which bar the command would hit.
//!
//! The tests below pin the two things that make that claim false: the range is exactly what was asked for
//! (not one bar wider, which is what extending a leftover selection would produce), and every refusal
//! happens BEFORE the selection is touched.

namespace {
const muse::String TWO_BAR_SCORE(u"data/test.mscx");        //!< 4/4, 2 bars: C4 D4 E4 F4 | whole rest
const muse::String ONE_BAR_SCORE(u"data/chord-test.mscx");  //!< 4/4, 1 bar: a three-note chord

std::shared_ptr<MasterScore> loadScore(const muse::String& name)
{
    return std::shared_ptr<MasterScore>(mu::engraving::ScoreRW::readScore(name));
}

//! Run a recipe the way the tools do: inside a transaction. A selection needs none, but going through the
//! same door as production keeps the test from proving something the real path does not do.
RecipeResult runRecipe(Score* score, const std::function<RecipeResult()>& body)
{
    score->startCmd(muse::TranslatableString::untranslatable("agentharness test"));
    RecipeResult result = body();
    score->endCmd();
    return result;
}
} // namespace

TEST(AgentHarness_EditorState, SelectMeasuresSelectsExactlyThatRange)
{
    const std::shared_ptr<MasterScore> score = loadScore(TWO_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 1, 2, 0);
    });
    ASSERT_TRUE(result.ok) << result.problem.toStdString();

    const Selection& selection = score->selection();
    EXPECT_TRUE(selection.isRange()) << "a measure selection must be a RANGE, not a list of elements";
    //! ⛔ The ticks, not just "something is selected": a range that is one bar too long is exactly the
    //! failure a caller cannot see, and `Score::selectRange` EXTENDS an existing range rather than
    //! replacing it - so a leftover selection would produce one.
    EXPECT_EQ(selection.tickStart(), score->firstMeasure()->tick());
    EXPECT_EQ(selection.tickEnd(), score->firstMeasure()->nextMeasure()->endTick());
    EXPECT_EQ(int(selection.staffStart()), 0);
    EXPECT_EQ(int(selection.staffEnd()), 1) << "staffEnd is exclusive, so every staff of a 1-staff score is 1";

    //! And the report says what it selected, so a caller that never reads the selection back still knows.
    EXPECT_TRUE(result.detail.contains(QStringLiteral("measures 1-2"))) << result.detail.toStdString();
    EXPECT_TRUE(result.detail.contains(QStringLiteral("moved"))) << result.detail.toStdString();
}

TEST(AgentHarness_EditorState, SelectMeasuresReplacesALeftoverSelectionInsteadOfGrowingIt)
{
    const std::shared_ptr<MasterScore> score = loadScore(TWO_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    //! Select the whole score first, then ask for bar 2 alone. `selectRange` on a measure EXTENDS an
    //! existing range, so without the deselect this would come back as bars 1-2 - the range the caller did
    //! NOT ask for, reported as success.
    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 1, 2, 0);
    }).ok);

    const RecipeResult second = runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 2, 2, 0);
    });
    ASSERT_TRUE(second.ok) << second.problem.toStdString();

    const Selection& selection = score->selection();
    EXPECT_EQ(selection.tickStart(), score->firstMeasure()->nextMeasure()->tick())
        << "the previous selection grew the range instead of being replaced";
    EXPECT_EQ(selection.tickEnd(), score->firstMeasure()->nextMeasure()->endTick());
    EXPECT_TRUE(second.detail.contains(QStringLiteral("measure 2"))) << second.detail.toStdString();
}

TEST(AgentHarness_EditorState, SelectMeasuresCanLimitItselfToOneStaff)
{
    const std::shared_ptr<MasterScore> score = loadScore(ONE_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    //! ⚠️ A second staff first: the staff parameter is untestable on a single-staff score, and the
    //! difference between "staff 1" and "every staff" is invisible there.
    const RecipeResult added = runRecipe(score.get(), [&] {
        return appendStaff(score.get(), 0);
    });
    ASSERT_TRUE(added.ok) << added.problem.toStdString();
    ASSERT_EQ(score->nstaves(), 2u);

    const RecipeResult onSecond = runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 1, 1, 2);
    });
    ASSERT_TRUE(onSecond.ok) << onSecond.problem.toStdString();
    EXPECT_EQ(int(score->selection().staffStart()), 1);
    EXPECT_EQ(int(score->selection().staffEnd()), 2);
    EXPECT_TRUE(onSecond.detail.contains(QStringLiteral("staff 2"))) << onSecond.detail.toStdString();

    //! And "no staff given" means every staff, which is what a bare "select these bars" means.
    const RecipeResult onAll = runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 1, 1, 0);
    });
    ASSERT_TRUE(onAll.ok) << onAll.problem.toStdString();
    EXPECT_EQ(int(score->selection().staffStart()), 0);
    EXPECT_EQ(int(score->selection().staffEnd()), 2);
}

TEST(AgentHarness_EditorState, SelectMeasuresRefusesBeforeItTouchesTheSelection)
{
    const std::shared_ptr<MasterScore> score = loadScore(TWO_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    //! A selection to lose: if a refusal threw it away, the caller would be left with nothing and no edit.
    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 1, 1, 0);
    }).ok);
    const Fraction before = score->selection().tickStart();

    const RecipeResult range = runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 1, 99, 0);
    });
    EXPECT_FALSE(range.ok);
    //! ⚠️ The count is part of the answer: "there is no measure 99" without it leaves the caller guessing.
    EXPECT_TRUE(range.problem.contains(QStringLiteral("2"))) << range.problem.toStdString();

    const RecipeResult inverted = runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 2, 1, 0);
    });
    EXPECT_FALSE(inverted.ok);

    const RecipeResult staff = runRecipe(score.get(), [&] {
        return selectMeasures(score.get(), 1, 1, 7);
    });
    EXPECT_FALSE(staff.ok);
    EXPECT_TRUE(staff.problem.contains(QStringLiteral("staff"))) << staff.problem.toStdString();

    //! The selection is where it was before all three refusals.
    EXPECT_TRUE(score->selection().isRange());
    EXPECT_EQ(score->selection().tickStart(), before);
}

TEST(AgentHarness_EditorState, SelectElementAtPutsTheSelectionOnThatChordRest)
{
    const std::shared_ptr<MasterScore> score = loadScore(TWO_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    ScoreAddress at;
    at.measure = 1;
    at.beat = 3;
    at.staff = 0;

    const RecipeResult result = runRecipe(score.get(), [&] {
        return selectElementAt(score.get(), at, 0);
    });
    ASSERT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ A SINGLE-element selection, not a range, AND it must be a NOTE - measured: the first version
    //! selected the ChordRest, and `split-measure` did nothing at all because its handler accepts only a
    //! note or a rest (`[split-probe] element=Chord isNote=0`, 第 138 条).
    const Selection& selection = score->selection();
    EXPECT_FALSE(selection.isRange()) << "split-measure needs a single selected element, not a range";
    ASSERT_TRUE(selection.element() != nullptr);
    EXPECT_TRUE(selection.element()->isNote()) << "a Chord is not accepted by split-measure; a Note is";
    //! ⚠️ Compared as TICKS: `Fraction` normalises (960 ticks is `1/2`, not `960/1`), and a `Fraction`
    //! literal that means "960 whole notes" is a comparison that can never be true (第 134 条 lost a test
    //! to exactly this).
    EXPECT_EQ(selection.element()->tick().ticks(), 960) << "beat 3 of a 4/4 bar is tick 960";

    EXPECT_TRUE(result.detail.contains(QStringLiteral("measure 1 beat 3"))) << result.detail.toStdString();
}

TEST(AgentHarness_EditorState, SelectElementAtReportsWhereTheSelectionLanded)
{
    const std::shared_ptr<MasterScore> score = loadScore(TWO_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    //! Bar 2 is a whole-measure rest, so this also pins that a REST is a legal target (it is what
    //! `split-measure` accepts besides a note).
    ScoreAddress at;
    at.measure = 2;
    at.beat = 1;
    at.staff = 0;

    const RecipeResult result = runRecipe(score.get(), [&] {
        return selectElementAt(score.get(), at, 0);
    });
    ASSERT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_TRUE(result.detail.contains(QStringLiteral("rest"))) << result.detail.toStdString();
    ASSERT_TRUE(score->selection().element() != nullptr);
    EXPECT_TRUE(score->selection().element()->isRest());
}

TEST(AgentHarness_EditorState, SelectElementAtRefusesAnAddressWithNothingThere)
{
    const std::shared_ptr<MasterScore> score = loadScore(TWO_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    //! A beat that does not exist in the bar. The refusal must come from the address layer with its own
    //! reason, and the previous selection must survive it (nothing was touched).
    ScoreAddress good;
    good.measure = 1;
    good.beat = 1;
    good.staff = 0;
    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return selectElementAt(score.get(), good, 0);
    }).ok);
    mu::engraving::EngravingItem* before = score->selection().element();
    ASSERT_TRUE(before != nullptr);

    ScoreAddress bad;
    bad.measure = 1;
    bad.beat = 99;
    bad.staff = 0;
    const RecipeResult result = runRecipe(score.get(), [&] {
        return selectElementAt(score.get(), bad, 0);
    });

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty()) << "a refusal without a reason is not actionable";
    EXPECT_EQ(score->selection().element(), before) << "a refused selection must not move the selection";
}

TEST(AgentHarness_EditorState, SelectingMeasuresPushesNoUndoCommands)
{
    const std::shared_ptr<MasterScore> score = loadScore(TWO_BAR_SCORE);
    ASSERT_TRUE(score != nullptr);

    //! ⛔ THE CLAIM THE TOOL'S DESCRIPTION MAKES: it moves the selection and touches no music. The test
    //! looks at the TRANSACTION, because that is the fact the harness depends on - and it also pins what
    //! upstream does with an empty one: measured here, `TransactionManager::endTransaction` rolls an empty
    //! transaction back itself (`stack->endTransaction(activeTransaction()->empty())`), so no undo step is
    //! appended and no change event is sent. That is why a state-only tool needs no special handling, and
    //! it is the fact a comment in `FieldController::runNoteRecipe` had wrong until 第 138 条 checked it.
    const size_t before = score->undoStack()->size();

    score->startCmd(muse::TranslatableString::untranslatable("agentharness test"));
    const RecipeResult result = selectMeasures(score.get(), 1, 2, 0);
    UndoableTransaction* transaction = score->undoStack()->activeTransaction();
    ASSERT_TRUE(transaction != nullptr) << "no transaction is open, so this test would prove nothing";
    const bool pushedNothing = transaction->empty();
    score->endCmd();

    ASSERT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_TRUE(pushedNothing) << "selecting must not push an undo command";
    EXPECT_EQ(score->undoStack()->size(), before)
        << "an EMPTY transaction must leave no undo step behind (upstream rolls it back)";
}
