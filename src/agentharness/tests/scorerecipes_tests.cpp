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
#include "engraving/dom/slur.h"
#include "engraving/dom/tie.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/tests/utils/scorerw.h"

#include "agentharness/qml/MuseScore/AgentHarness/addressing.h"
#include "agentharness/qml/MuseScore/AgentHarness/notelocator.h"
#include "agentharness/qml/MuseScore/AgentHarness/scorerecipes.h"

using namespace mu::engraving;
using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS.
//!
//! The recipes are the WRITE side's entire vocabulary, and until now every one of them was verified by
//! running the program and reading the log. Round 15 showed what that leaves out: the locator had no
//! tests, and the first systematic suite found a null-pointer crash on a path no recipe happened to
//! take. The recipes are the next component with the same shape of risk - each one has error branches
//! (out of range, already in that state, no target) that a happy-path run never reaches.
//!
//! ⛔ EVERY RECIPE MUST RUN INSIDE A TRANSACTION. They push `UndoableCommand`s, and outside a
//! transaction `currentOrDummyTransaction()` hands back a dummy that DISCARDS them - so the score would
//! appear unchanged and the test would "prove" the recipe does nothing. `runRecipe` below is the only
//! way these are called, so that cannot be forgotten at a call site.

namespace {
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
    a.staff = staff - 1;
    return a;
}

//! Run one recipe inside a real transaction, which is the only way its commands survive.
RecipeResult runRecipe(Score* score, const std::function<RecipeResult()>& body)
{
    score->startCmd(muse::TranslatableString::untranslatable("agentharness test"));
    RecipeResult result = body();
    score->endCmd();
    return result;
}

int pitchAt(Score* score, int measure, int beat, int index = -1)
{
    const NoteLookup found = noteAt(score, address(measure, beat), 0, index);
    return found.ok() ? found.note->pitch() : -1;
}
} // namespace

//! ── setNotePitch ──────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, SetNotePitchChangesTheNote)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);
    EXPECT_EQ(pitchAt(score.get(), 1, 1), 60);   //!< C4

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setNotePitch(score.get(), address(1, 1), 0, -1, 67);   //!< -> G4
    });

    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_EQ(pitchAt(score.get(), 1, 1), 67) << "the score must actually change";
}

TEST(AgentHarness_ScoreRecipes, SetNotePitchToTheSamePitchSucceedsAndSaysSo)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ NOT A FAILURE. A model told "failed" here would go looking for another way to do what is
    //! already done. The detail says the note is already that pitch, which is the honest answer.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setNotePitch(score.get(), address(1, 1), 0, -1, 60);
    });

    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_FALSE(result.detail.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, SetNotePitchRejectsAPitchOutsideMidiRange)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    for (int bad : { -1, 128, 999 }) {
        const RecipeResult result = runRecipe(score.get(), [&] {
            return setNotePitch(score.get(), address(1, 1), 0, -1, bad);
        });
        EXPECT_FALSE(result.ok) << "pitch " << bad << " must be refused";
        EXPECT_FALSE(result.problem.isEmpty()) << "and the refusal must say why";
    }

    EXPECT_EQ(pitchAt(score.get(), 1, 1), 60) << "nothing may have changed";
}

TEST(AgentHarness_ScoreRecipes, SetNotePitchOnARestIsRefusedWithoutCrashing)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ THE CRASH GUARD. `chordAt` returns with only `rest` set on a rest, so a recipe that reaches
    //! for `found.chord` unconditionally dereferences null. The locator suite found that crash; this
    //! pins it from the recipe side, which is where a caller would actually hit it.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setNotePitch(score.get(), address(2, 1), 0, -1, 64);
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("rest"))) << result.problem.toStdString();
}

//! ── transposeNote ─────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, TransposeMovesBySemitones)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult up = runRecipe(score.get(), [&] {
        return transposeNote(score.get(), address(1, 1), 0, -1, 5);    //!< C4 + 5 = F4
    });
    EXPECT_TRUE(up.ok) << up.problem.toStdString();
    EXPECT_EQ(pitchAt(score.get(), 1, 1), 65);

    const RecipeResult down = runRecipe(score.get(), [&] {
        return transposeNote(score.get(), address(1, 1), 0, -1, -7);   //!< F4 - 7 = A3
    });
    EXPECT_TRUE(down.ok) << down.problem.toStdString();
    EXPECT_EQ(pitchAt(score.get(), 1, 1), 58);
}

TEST(AgentHarness_ScoreRecipes, TransposeByZeroIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ A caller that asks to transpose by nothing has misunderstood something, and a silent success
    //! would let it carry on believing that.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return transposeNote(score.get(), address(1, 1), 0, -1, 0);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, TransposePastTheEndOfTheRangeIsRefusedWithBothNumbers)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! The message must name where it would have landed, so the caller can see how far it overshot
    //! instead of guessing.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return transposeNote(score.get(), address(1, 1), 0, -1, 120);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("127"))) << result.problem.toStdString();
    EXPECT_EQ(pitchAt(score.get(), 1, 1), 60) << "nothing may have changed";
}

//! ── setChordDuration ──────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, SetDurationAcceptsPlainAndDottedNames)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const struct {
        const char* name;
        DurationType type;
        int dots;
    } cases[] = {
        { "half", DurationType::V_HALF, 0 },
        { "eighth", DurationType::V_EIGHTH, 0 },
        { "dotted-quarter", DurationType::V_QUARTER, 1 },
        { "quarter.", DurationType::V_QUARTER, 1 },
        { "double-dotted-half", DurationType::V_HALF, 2 },
    };

    for (const auto& c : cases) {
        const RecipeResult result = runRecipe(score.get(), [&] {
            return setChordDuration(score.get(), address(1, 1), 0, QString::fromLatin1(c.name));
        });
        ASSERT_TRUE(result.ok) << c.name << ": " << result.problem.toStdString();

        const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
        ASSERT_TRUE(found.found());
        EXPECT_EQ(found.chord->durationType().type(), c.type) << c.name;
        EXPECT_EQ(found.chord->durationType().dots(), c.dots) << c.name;
    }
}

TEST(AgentHarness_ScoreRecipes, SetDurationRejectsANameItDoesNotKnowAndListsTheOnesItDoes)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! `quaver` is the British name for an eighth. A model may well try it, and the useful answer is
    //! "here are the names I accept" rather than "no".
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 1), 0, QStringLiteral("quaver"));
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("quaver"))) << result.problem.toStdString();
    EXPECT_TRUE(result.problem.contains(QStringLiteral("quarter"))) << "it must list the valid names";
}

TEST(AgentHarness_ScoreRecipes, SetDurationRejectsAMeasureWithDots)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! A measure-long chord is complete by definition. Accepting a dot silently would suggest it did
    //! something.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 1), 0, QStringLiteral("dotted-measure"));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, SetDurationRejectsMoreThanThreeDots)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 1), 0, QStringLiteral("quarter...."));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, SetDurationOnTheSameDurationSucceedsAndSaysSo)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 1), 0, QStringLiteral("quarter"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_FALSE(result.detail.isEmpty());
}

//! ── addNoteToChord / removeNote ───────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, AddNoteMakesASingleNoteIntoAChord)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return addNoteToChord(score.get(), address(1, 1), 0, 67);   //!< add G4
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->notes().size(), 2u);
}

TEST(AgentHarness_ScoreRecipes, AddNoteRefusesAPitchTheChordAlreadyHas)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ A duplicate pitch is not a chord - it is the same note twice. Upstream will create it without
    //! complaint, and the result draws as ONE notehead while every later read sees two notes at the
    //! same pitch, which reads as "the chord has a note I cannot see".
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addNoteToChord(score.get(), address(1, 1), 0, 64);   //!< E4 is already there
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("E4"))) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->notes().size(), 3u) << "nothing may have been added";
}

TEST(AgentHarness_ScoreRecipes, RemoveNoteTakesOneNoteOutOfAChord)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return removeNote(score.get(), address(1, 1), 0, 1);   //!< remove E4
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->notes().size(), 2u);
}

TEST(AgentHarness_ScoreRecipes, RemoveNoteRefusesToEmptyTheChord)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ An empty chord is not a rest - it draws as nothing and is a broken object. The refusal has to
    //! point at the tool that does what the caller actually wants.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return removeNote(score.get(), address(1, 1), 0, -1);
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("note_to_rest"))) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->notes().size(), 1u) << "nothing may have been removed";
}

//! ── changeToRest ──────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, ChangeToRestKeepsTheDuration)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup before = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(before.found());
    const Fraction ticksBefore = before.chord->ticks();

    const RecipeResult result = runRecipe(score.get(), [&] {
        return changeToRest(score.get(), address(1, 1), 0);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup after = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(after.found());
    EXPECT_TRUE(after.isRest()) << "it must BE a rest now";
    //! ⛔ THE LENGTH IS THE POINT. "Make this beat a rest" means the beat keeps its length; a rest of a
    //! different length is a different edit, and the caller would have no way to tell which went wrong.
    EXPECT_EQ(after.rest->ticks(), ticksBefore);
}

TEST(AgentHarness_ScoreRecipes, ChangeToRestOnARestSucceedsAndSaysSo)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! The round-14 regression, seen from the recipe side: this used to answer "there is no note at
    //! measure 2 beat 1" because the locator could not see a whole-measure rest at all.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return changeToRest(score.get(), address(2, 1), 0);
    });

    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_TRUE(result.detail.contains(QStringLiteral("already"))) << result.detail.toStdString();
}

//! ── Ties ──────────────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, AddTieConnectsTheNextNoteOfTheSamePitch)
{
    const auto score = loadScore(TIE_SCORE);
    ASSERT_TRUE(score);

    const NoteLookup first = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.note->tieFor() == nullptr) << "the test data starts untied";

    const RecipeResult result = runRecipe(score.get(), [&] {
        return addTie(score.get(), address(1, 1), 0, -1);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup after = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(after.ok());
    ASSERT_TRUE(after.note->tieFor() != nullptr) << "the tie must exist";

    //! ⛔ AND IT MUST REACH THE RIGHT NOTE. A tie that exists but points at the wrong note draws as a
    //! tie and reads as one, so the destination is the part worth asserting.
    const NoteLookup second = noteAt(score.get(), address(2, 1), 0, -1);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(after.note->tieFor()->endNote(), second.note);
}

TEST(AgentHarness_ScoreRecipes, AddTieRefusesANoteThatIsAlreadyTied)
{
    const auto score = loadScore(TIE_SCORE);
    ASSERT_TRUE(score);

    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return addTie(score.get(), address(1, 1), 0, -1);
    }).ok);

    //! ⛔ Refused rather than replaced: silently redirecting an existing tie is an edit the caller
    //! cannot see in the result, and the caller may be asking precisely because it wants to know
    //! whether the earlier call worked.
    const RecipeResult again = runRecipe(score.get(), [&] {
        return addTie(score.get(), address(1, 1), 0, -1);
    });
    EXPECT_FALSE(again.ok);
    EXPECT_TRUE(again.problem.contains(QStringLiteral("already tied"))) << again.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, AddTieWithNoSamePitchLaterIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! test.mscx is C4 D4 E4 F4 - no later note shares C4's pitch. Tying different pitches is a SLUR,
    //! not a tie, and upstream would draw it as one.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addTie(score.get(), address(1, 1), 0, -1);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, RemoveTieTakesItAwayAndRefusesWhenThereIsNone)
{
    const auto score = loadScore(TIE_SCORE);
    ASSERT_TRUE(score);

    //! Refused when there is nothing to remove: "there was nothing to remove" and "I removed it" are
    //! different answers, and a caller that gets the second when the first is true believes it changed
    //! something.
    const RecipeResult nothing = runRecipe(score.get(), [&] {
        return removeTie(score.get(), address(1, 1), 0, -1);
    });
    EXPECT_FALSE(nothing.ok);
    EXPECT_FALSE(nothing.problem.isEmpty());

    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return addTie(score.get(), address(1, 1), 0, -1);
    }).ok);

    const RecipeResult removed = runRecipe(score.get(), [&] {
        return removeTie(score.get(), address(1, 1), 0, -1);
    });
    EXPECT_TRUE(removed.ok) << removed.problem.toStdString();

    const NoteLookup after = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(after.note->tieFor() == nullptr) << "the tie must be gone";
}

TEST(AgentHarness_ScoreRecipes, ToggleTieAddsThenRemoves)
{
    const auto score = loadScore(TIE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult added = runRecipe(score.get(), [&] {
        return toggleTie(score.get(), address(1, 1), 0, -1);
    });
    EXPECT_TRUE(added.ok) << added.problem.toStdString();
    EXPECT_TRUE(noteAt(score.get(), address(1, 1), 0, -1).note->tieFor() != nullptr);

    const RecipeResult removed = runRecipe(score.get(), [&] {
        return toggleTie(score.get(), address(1, 1), 0, -1);
    });
    EXPECT_TRUE(removed.ok) << removed.problem.toStdString();
    EXPECT_TRUE(noteAt(score.get(), address(1, 1), 0, -1).note->tieFor() == nullptr);
}

//! ── Slurs ─────────────────────────────────────────────────────────────────────────────────────
//!
//! ⛔ A SLUR IS NOT A TIE, and these tests are where that stops being a comment. A tie joins two notes
//! of the SAME PITCH; a slur joins any two. So the case `AddTieWithNoSamePitchLaterIsRefused` REJECTS is
//! the case a slur must ACCEPT - if both operations behaved the same way on it, one of them would be
//! wrong.

TEST(AgentHarness_ScoreRecipes, AddSlurConnectsTwoNotesOfDifferentPitch)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! test.mscx is C4 D4 E4 F4 - every neighbouring pair differs in pitch, which is exactly what a tie
    //! cannot do and a slur must.
    const NoteLookup before = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(before.found());
    EXPECT_TRUE(before.chord->slur() == nullptr) << "the test data starts unslurred";

    const RecipeResult result = runRecipe(score.get(), [&] {
        return addSlur(score.get(), address(1, 1), 0);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup after = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(after.found());
    ASSERT_TRUE(after.chord->slur() != nullptr) << "the slur must exist";

    //! ⛔ AND IT MUST REACH THE RIGHT NOTE. A slur that exists but points at the wrong note draws as a
    //! slur and reads as one, so the destination is the part worth asserting.
    const NoteLookup second = chordAt(score.get(), address(1, 2), 0);
    ASSERT_TRUE(second.found());
    EXPECT_EQ(after.chord->slur()->endElement(), static_cast<EngravingItem*>(second.chord));
}

TEST(AgentHarness_ScoreRecipes, AddSlurRefusesANoteThatIsAlreadySlurred)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return addSlur(score.get(), address(1, 1), 0);
    }).ok);

    //! ⛔ Refused, not stacked: two slurs over the same pair are not a thicker slur - they are two slurs
    //! drawn on top of each other, and the caller cannot see that from the result.
    const RecipeResult again = runRecipe(score.get(), [&] {
        return addSlur(score.get(), address(1, 1), 0);
    });
    EXPECT_FALSE(again.ok);
    EXPECT_TRUE(again.problem.contains(QStringLiteral("already slurred"))) << again.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, AddSlurOnARestIsRefusedWithoutCrashing)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! A slur needs something to start on. "Slur the silence" is not a thing, and the refusal must say
    //! that rather than reaching for a chord that is not there.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addSlur(score.get(), address(2, 1), 0);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("rest"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, AddSlurFromTheLastNoteIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! The last note of the score has nothing after it. `Score::addSlur` returns null in that case, and
    //! a success the caller cannot reconcile with the score would be worse than the refusal.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addSlur(score.get(), address(1, 4), 0);   //!< F4, the last note of bar 1... of the score
    });

    //! NOTE bar 2 is a rest, so there IS no later chord: this must be refused, not slur to the rest.
    EXPECT_FALSE(result.ok) << "there is no later note to slur to";
    EXPECT_FALSE(result.problem.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, RemoveSlurTakesItAwayAndRefusesWhenThereIsNone)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult nothing = runRecipe(score.get(), [&] {
        return removeSlur(score.get(), address(1, 1), 0);
    });
    EXPECT_FALSE(nothing.ok);
    EXPECT_FALSE(nothing.problem.isEmpty());

    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return addSlur(score.get(), address(1, 1), 0);
    }).ok);

    const RecipeResult removed = runRecipe(score.get(), [&] {
        return removeSlur(score.get(), address(1, 1), 0);
    });
    EXPECT_TRUE(removed.ok) << removed.problem.toStdString();

    const NoteLookup after = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(after.found());
    EXPECT_TRUE(after.chord->slur() == nullptr) << "the slur must be gone";
}

TEST(AgentHarness_ScoreRecipes, ToggleSlurAddsThenRemoves)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult added = runRecipe(score.get(), [&] {
        return toggleSlur(score.get(), address(1, 1), 0);
    });
    EXPECT_TRUE(added.ok) << added.problem.toStdString();
    EXPECT_TRUE(chordAt(score.get(), address(1, 1), 0).chord->slur() != nullptr);

    const RecipeResult removed = runRecipe(score.get(), [&] {
        return toggleSlur(score.get(), address(1, 1), 0);
    });
    EXPECT_TRUE(removed.ok) << removed.problem.toStdString();
    EXPECT_TRUE(chordAt(score.get(), address(1, 1), 0).chord->slur() == nullptr);
}