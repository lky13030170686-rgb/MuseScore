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
#include "engraving/dom/articulation.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/textbase.h"
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
const muse::String EMPTY_SCORE(u"data/empty-test.mscx"); //!< 4/4, 2 bars: a whole-measure rest in each

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
//! ── The writability gate (G1) ─────────────────────────────────────────────────────────────────
//!
//! ⛔⛔ WHY THIS IS THE MOST IMPORTANT GROUP HERE. `Score::undoChangeChordRestLen` sets two properties
//! and checks NOTHING. A duration that does not fit writes a chord running past the barline: the
//! measure is no longer full, and a score in that state is one the editor may refuse to reopen - which
//! is the "Agent 把工程写坏" risk the plan lists first.
//!
//! ⚠️ And the boundary is the point, not the middle. A test that only tries "obviously too long" passes
//! against a gate that is off by a beat. So the sweep below walks every beat of the bar and asks for
//! exactly what fits and exactly one step more.

TEST(AgentHarness_ScoreRecipes, DurationThatExactlyFillsTheRestOfTheBarIsAccepted)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! Beat 1 of a 4/4 bar with three quarters left: a DOTTED HALF is exactly the remaining space
    //! (3/4). This is the boundary that an off-by-one gate gets wrong, in the direction that REFUSES
    //! something legal - which is just as much a bug as accepting something illegal, and much easier to
    //! miss because the refusal looks like a safety feature working.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 1), 0, QStringLiteral("dotted-half"));
    });
    EXPECT_TRUE(result.ok) << "a dotted half exactly fills beats 2-4: " << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->durationType().type(), DurationType::V_HALF);
    EXPECT_EQ(found.chord->durationType().dots(), 1);
}

TEST(AgentHarness_ScoreRecipes, DurationOneStepTooLongIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! One step past the boundary above: a whole note from beat 1 of a bar that also holds three more
    //! quarters would need 4/4 but only 4/4 is available INCLUDING this beat... so use a whole note
    //! where only a dotted half fits is impossible; instead ask from beat 2, where a whole note clearly
    //! overflows.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 2), 0, QStringLiteral("whole"));
    });

    EXPECT_FALSE(result.ok) << "a whole note cannot start on beat 2 of a 4/4 bar";
    EXPECT_FALSE(result.problem.isEmpty());

    //! And nothing may have changed.
    const NoteLookup found = chordAt(score.get(), address(1, 2), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->durationType().type(), DurationType::V_QUARTER);
}

TEST(AgentHarness_ScoreRecipes, TheGateSweepsEveryBeatOfTheBar)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ THE SWEEP THE PLAN ASKS FOR, and the reason it asks for it: a gate tested only at one position
    //! can be off by a beat and still pass. `test.mscx` is 4/4 with quarters on beats 1-4, so from beat
    //! `b` the remaining space is `(5-b)/4`:
    //!
    //!   beat 1 -> 4/4 available -> whole fits
    //!   beat 2 -> 3/4 available -> dotted half fits, whole does not
    //!   beat 3 -> 2/4 available -> half fits, dotted half does not
    //!   beat 4 -> 1/4 available -> quarter fits, half does not
    //!
    //! Each row is checked in BOTH directions: what fits must be accepted, and the next size up must be
    //! refused. Accepting-only would pass against a gate that never refuses; refusing-only would pass
    //! against a gate that refuses everything.
    const struct {
        int beat;
        const char* fits;
        const char* tooLong;
    } rows[] = {
        { 1, "whole", "breve" },
        { 2, "dotted-half", "whole" },
        { 3, "half", "dotted-half" },
        { 4, "quarter", "half" },
    };

    for (const auto& row : rows) {
        //! A fresh score per row: the previous row changed the bar's shape.
        const auto fresh = loadScore(NOTE_SCORE);
        ASSERT_TRUE(fresh);

        const RecipeResult fits = runRecipe(fresh.get(), [&] {
            return setChordDuration(fresh.get(), address(1, row.beat), 0, QString::fromLatin1(row.fits));
        });
        EXPECT_TRUE(fits.ok) << "beat " << row.beat << ": " << row.fits
                             << " should fit but was refused: " << fits.problem.toStdString();
    }

    for (const auto& row : rows) {
        const auto fresh = loadScore(NOTE_SCORE);
        ASSERT_TRUE(fresh);

        const RecipeResult tooLong = runRecipe(fresh.get(), [&] {
            return setChordDuration(fresh.get(), address(1, row.beat), 0, QString::fromLatin1(row.tooLong));
        });
        EXPECT_FALSE(tooLong.ok) << "beat " << row.beat << ": " << row.tooLong
                                 << " should NOT fit but was accepted";
        EXPECT_TRUE(tooLong.problem.contains(QStringLiteral("does not fit")))
            << "beat " << row.beat << ": " << tooLong.problem.toStdString();
    }
}

TEST(AgentHarness_ScoreRecipes, TheRefusalNamesTheSpaceAvailable)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! "It does not fit" without a number leaves the caller guessing whether it overshot by a beat or by
    //! a whole bar - and the useful next action is different in each case.
    //!
    //! ⚠️ The number is a FRACTION OF A WHOLE NOTE, and that is deliberate: `1/4` for the one beat left
    //! after beat 4 of a 4/4 bar. A duration NAME would be wrong in general (three quarters is not "a
    //! dotted half", so a caller reading it that way would ask for one and be refused again), and the
    //! first live run printed a raw tick count - `only 1440 tick(s)` - which tells a musician nothing.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 4), 0, QStringLiteral("whole"));
    });

    ASSERT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("1/4")))
        << "the message should name the space left as a fraction of a whole note: "
        << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, ShrinkingIsNeverBlockedByTheGate)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⚠️ The gate is about FITTING, and a shorter duration always fits. A gate written as "the new
    //! duration must equal the space remaining" would refuse every legitimate shortening - so this pins
    //! the direction that a careless implementation gets wrong.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordDuration(score.get(), address(1, 1), 0, QStringLiteral("16th"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
}
//! ── setChordPitches ───────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, SetChordPitchesTurnsASingleNoteIntoATriad)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordPitches(score.get(), address(1, 1), 0, { 60, 64, 67 });   //!< C major
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    ASSERT_EQ(found.chord->notes().size(), 3u);
    for (int pitch : { 60, 64, 67 }) {
        EXPECT_TRUE(found.chord->findNote(pitch) != nullptr) << "missing " << pitch;
    }
}

TEST(AgentHarness_ScoreRecipes, SetChordPitchesReplacesWithoutEmptyingInTheMiddle)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ THE CASE THE RECIPE EXISTS FOR. The chord starts as [C4 E4 B4] and must end as [D4 F#4]:
    //! NOTHING is shared, so a caller doing "remove the old, then add the new" would empty the chord in
    //! the middle - which is a broken object, and which `removeNote` refuses. The recipe adds first, so
    //! the chord is non-empty at every point.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordPitches(score.get(), address(1, 1), 0, { 62, 66 });
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    ASSERT_EQ(found.chord->notes().size(), 2u);
    EXPECT_TRUE(found.chord->findNote(62) != nullptr) << "D4 should be there";
    EXPECT_TRUE(found.chord->findNote(66) != nullptr) << "F#4 should be there";
    EXPECT_TRUE(found.chord->findNote(60) == nullptr) << "C4 should be gone";
    EXPECT_TRUE(found.chord->findNote(64) == nullptr) << "E4 should be gone";
    EXPECT_TRUE(found.chord->findNote(67) == nullptr) << "B4 should be gone";
}

TEST(AgentHarness_ScoreRecipes, SetChordPitchesCanGrowAndShrinkAtOnce)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! [C4 E4 B4] -> [C4 E4 G4 A4]: C4 and E4 stay, B4 goes, G4 and A4 arrive. Both directions in one
    //! call, which is the ordinary case for "make this chord a ..." and the one a sequence of external
    //! adds/removes gets wrong.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordPitches(score.get(), address(1, 1), 0, { 60, 64, 67, 69 });
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->notes().size(), 4u);
    for (int pitch : { 60, 64, 67, 69 }) {
        EXPECT_TRUE(found.chord->findNote(pitch) != nullptr) << "missing " << pitch;
    }
}

TEST(AgentHarness_ScoreRecipes, SetChordPitchesRefusesAnEmptyListAndPointsAtRest)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ An empty list means "silence this beat" - and an empty chord is NOT a rest, it is a broken
    //! object. Naming the tool that does what the caller wants is more useful than refusing flatly.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordPitches(score.get(), address(1, 1), 0, {});
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("note_to_rest"))) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->notes().size(), 1u) << "nothing may have changed";
}

TEST(AgentHarness_ScoreRecipes, SetChordPitchesRefusesARepeatedPitchBeforeWritingAnything)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ Caught BEFORE anything is written: two notes at the same pitch draw as one notehead while every
    //! later read sees two, which reads as "the chord has a note I cannot see". Checking first also means
    //! the score is untouched on refusal, which the assertion below pins.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordPitches(score.get(), address(1, 1), 0, { 60, 64, 64 });
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("E4"))) << result.problem.toStdString();

    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    EXPECT_EQ(found.chord->notes().size(), 3u) << "the original chord must be intact";
}

TEST(AgentHarness_ScoreRecipes, SetChordPitchesRefusesOnARest)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! A rest is not a chord, and a rest cannot be given pitches - putting notes there needs an existing
    //! chord (`note_add` on a note). Refused with that direction rather than half-done.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordPitches(score.get(), address(2, 1), 0, { 60, 64 });
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("rest"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, SetChordPitchesToWhatIsAlreadyThereSucceedsAndSaysSo)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! Not an error: the chord already is what was asked for. A model told "failed" here would go looking
    //! for another way to do what is already done.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setChordPitches(score.get(), address(1, 1), 0, { 64, 67, 60 });   //!< same set, other order
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_TRUE(result.detail.contains(QStringLiteral("already"))) << result.detail.toStdString();
}
//! ── addText ───────────────────────────────────────────────────────────────────────────────────
//!
//! ⛔⛔ THE CRASH THESE GUARD AGAINST: `Score::addText` calls `chordOrRest(destination)` for every
//! ATTACHED style and uses the result without checking it. `chordOrRest` returns null for a null
//! destination, so `addText(REHEARSAL_MARK, nullptr)` dereferences null and takes the process down. A
//! model that names a rehearsal mark and forgets the measure reaches that in one call - which is why the
//! recipe checks the style BEFORE calling upstream rather than trusting the caller.

TEST(AgentHarness_ScoreRecipes, AddTextStyleNamesAreSplittableIntoTheTwoKinds)
{
    //! The distinction the tool is built on: frame styles belong to the score, attached styles hang off a
    //! beat. If a style ever moves between the two lists, this fails and points at the description that
    //! has to change with it.
    bool known = false;

    for (const char* frame : { "title", "subtitle", "composer", "lyricist" }) {
        EXPECT_FALSE(textStyleNeedsAddress(QString::fromLatin1(frame), known)) << frame;
        EXPECT_TRUE(known) << frame << " should be a known style";
    }

    for (const char* attached : { "rehearsal-mark", "system", "staff", "expression" }) {
        EXPECT_TRUE(textStyleNeedsAddress(QString::fromLatin1(attached), known)) << attached;
        EXPECT_TRUE(known) << attached << " should be a known style";
    }

    //! An unknown name must be reported as unknown rather than silently treated as frame text - which
    //! would send it down the "no address needed" path and produce the wrong kind of element.
    textStyleNeedsAddress(QStringLiteral("dynamics"), known);
    EXPECT_FALSE(known) << "`dynamics` is a style OF an existing element, not something you add as text";
}

TEST(AgentHarness_ScoreRecipes, AddTextRejectsAnUnknownStyleAndListsTheOnesItKnows)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! `dynamics` is the interesting wrong answer: it IS a TextStyleType, so a caller could reasonably
    //! try it - but it styles a dynamic that already exists, and adding a plain text box wearing a
    //! dynamic's font looks right and is not a dynamic.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addText(score.get(), address(1, 1), QStringLiteral("dynamics"), QStringLiteral("mf"));
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("title"))) << "it must list the valid names";
}

TEST(AgentHarness_ScoreRecipes, AddTextRejectsEmptyText)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! An empty text box is invisible and unselectable - an object in the score nobody can find again.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addText(score.get(), address(1, 1), QStringLiteral("staff"), QStringLiteral("   "));
    });

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, AddTextAttachesAStaffTextToTheNamedBeat)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return addText(score.get(), address(1, 2), QStringLiteral("staff"), QStringLiteral("dolce"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ AND IT MUST LAND ON THE NAMED BEAT, not wherever the selection happened to be. This is the
    //! difference between an addressed write and the interactive command, and the assertion is the point
    //! of the whole recipe.
    const NoteLookup found = chordAt(score.get(), address(1, 2), 0);
    ASSERT_TRUE(found.found());
    ASSERT_TRUE(found.chord->segment() != nullptr);

    bool attachedHere = false;
    for (mu::engraving::EngravingItem* item : found.chord->segment()->annotations()) {
        if (item->isStaffText()) {
            attachedHere = true;
        }
    }
    EXPECT_TRUE(attachedHere) << "the staff text should be attached to beat 2";
}

TEST(AgentHarness_ScoreRecipes, AddTextOnARestStillAttachesRatherThanCrashing)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⚠️ A rest is a legitimate anchor for attached text - a rehearsal mark over a silent bar is
    //! ordinary. The locator returns `rest` for that case, and the recipe has to pass the REST through
    //! rather than a null chord.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addText(score.get(), address(2, 1), QStringLiteral("rehearsal-mark"), QStringLiteral("B"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, AddTextOutOfRangeIsRefusedNotCrashed)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! The lookup fails, so there is no destination - and the refusal must happen BEFORE `addText`, which
    //! would dereference the null. Before the recipe had this check, this exact call was a crash.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addText(score.get(), address(99, 1), QStringLiteral("rehearsal-mark"), QStringLiteral("Z"));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty());
}
//! ── Key and time signature ────────────────────────────────────────────────────────────────────
//!
//! ⛔ THESE HAVE NO COMMAND. `command_list` enumerates every notation command that exists and there is no
//! key-signature or time-signature one - so for a caller that only knows the command layer, changing the
//! key is IMPOSSIBLE, not merely awkward. That is why the plan lists these as recipe-only, and why the
//! tool descriptions say "use this tool, not command_dispatch".

TEST(AgentHarness_ScoreRecipes, SetKeySignaturePutsTheRequestedAccidentalsOnTheStaff)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);
    EXPECT_EQ(int(score->staff(0)->key(Fraction(0, 1))), 0) << "test.mscx starts with no accidentals";

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setKeySignature(score.get(), 1, -3);   //!< three flats
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ VERIFIED BY READING THE STAFF BACK, not by the call returning. `undoChangeKeySig` returns void
    //! and skips staves it cannot handle, so "I called it" is not evidence that the key changed.
    EXPECT_EQ(int(score->staff(0)->key(Fraction(0, 1))), -3);
}

TEST(AgentHarness_ScoreRecipes, SetKeySignatureAppliesFromTheNamedMeasureOn)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! A key signature is not a property of one bar - it takes effect from there to the next signature.
    //! Setting it at bar 2 must leave bar 1 alone, which is the difference between this and a per-measure
    //! property and the thing a careless implementation gets wrong.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setKeySignature(score.get(), 2, 2);   //!< two sharps from bar 2
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    Measure* first = score->firstMeasure();
    ASSERT_TRUE(first != nullptr);
    Measure* second = first->nextMeasure();
    ASSERT_TRUE(second != nullptr);

    EXPECT_EQ(int(score->staff(0)->key(first->tick())), 0) << "bar 1 should be unchanged";
    EXPECT_EQ(int(score->staff(0)->key(second->tick())), 2) << "bar 2 should carry the new key";
}

TEST(AgentHarness_ScoreRecipes, SetKeySignatureRejectsAValueOutsideTheRange)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! Eight sharps does not exist. The bounds come from the enum, so this test and the recipe agree on
    //! where the edge is rather than both hard-coding 7.
    for (int bad : { 8, -8, 99, -99 }) {
        const RecipeResult result = runRecipe(score.get(), [&] {
            return setKeySignature(score.get(), 1, bad);
        });
        EXPECT_FALSE(result.ok) << bad << " must be refused";
        EXPECT_TRUE(result.problem.contains(QStringLiteral("7"))) << result.problem.toStdString();
    }
    EXPECT_EQ(int(score->staff(0)->key(Fraction(0, 1))), 0) << "nothing may have changed";
}

TEST(AgentHarness_ScoreRecipes, SetKeySignatureOutOfRangeMeasureIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setKeySignature(score.get(), 99, 1);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("no measure 99"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, SetKeySignatureToWhatIsAlreadyThereSucceedsAndSaysSo)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! Not an error: the key already is what was asked for. A model told "failed" would go looking for
    //! another way to do what is already done.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setKeySignature(score.get(), 1, 0);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_TRUE(result.detail.contains(QStringLiteral("already"))) << result.detail.toStdString();
}

TEST(AgentHarness_ScoreRecipes, SetTimeSignatureChangesTheSignatureAndTheMeasureLength)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setTimeSignature(score.get(), 2, 3, 4);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔⛔ RE-LOOKED UP BY TICK, AND THAT IS NOT PEDANTRY. `addTimeSig` reflows by REMOVING AND RECREATING
    //! the measures, so every `Measure*` taken before it is dangling afterwards. The first version of this
    //! recipe verified through the pointer it already held and read freed memory - reporting "still 4/4"
    //! for a signature that HAD been applied, which sent the debugging after the wrong thing entirely.
    Measure* second = score->tick2measure(Fraction(4, 4));
    ASSERT_TRUE(second != nullptr);
    EXPECT_EQ(second->timesig(), Fraction(3, 4));

    //! ⛔ THE BAR MUST ACTUALLY BE THREE BEATS LONG, not just labelled 3/4. If only the label changed, the
    //! bar would still hold four beats while claiming to be 3/4 - a measure that does not match its own
    //! signature.
    EXPECT_EQ(second->ticks(), Fraction(3, 4)) << "the bar must be reflowed, not just relabelled";
}

TEST(AgentHarness_ScoreRecipes, SetTimeSignatureLeavesTheBarsBeforeItAlone)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! A signature takes effect FROM the named measure on. Bar 1 must be untouched - otherwise the tool
    //! would be changing more than it was asked to.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setTimeSignature(score.get(), 2, 3, 4);
    });
    ASSERT_TRUE(result.ok) << result.problem.toStdString();

    Measure* first = score->tick2measure(Fraction(0, 1));
    ASSERT_TRUE(first != nullptr);
    EXPECT_EQ(first->timesig(), Fraction(4, 4)) << "bar 1 must be untouched";
    EXPECT_EQ(first->ticks(), Fraction(4, 4));
}

TEST(AgentHarness_ScoreRecipes, SetTimeSignatureOutOfRangeMeasureIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setTimeSignature(score.get(), 99, 3, 4);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("no measure 99"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, SetTimeSignatureRejectsANonPowerOfTwoDenominator)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! The notation layer stores the denominator as a power of two, so 6/3 would be accepted here and
    //! then silently stored as something else - which is worse than a refusal.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return setTimeSignature(score.get(), 1, 6, 3);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("power of two"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, SetTimeSignatureRejectsZero)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    for (const auto& pair : { std::pair<int, int> { 0, 4 }, { 4, 0 }, { -3, 4 } }) {
        const RecipeResult result = runRecipe(score.get(), [&] {
            return setTimeSignature(score.get(), 1, pair.first, pair.second);
        });
        EXPECT_FALSE(result.ok) << pair.first << "/" << pair.second << " must be refused";
    }
}

TEST(AgentHarness_ScoreRecipes, SetTimeSignatureToWhatIsAlreadyThereSucceedsAndSaysSo)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return setTimeSignature(score.get(), 1, 4, 4);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_TRUE(result.detail.contains(QStringLiteral("already"))) << result.detail.toStdString();
}
//! ── Dynamics and hairpins ─────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, AddDynamicPutsTheMarkingOnTheNamedBeat)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return addDynamic(score.get(), address(1, 2), QStringLiteral("mf"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ AND IT MUST LAND ON THE NAMED BEAT. A dynamic is not a property of the score - it takes effect
    //! from where it is placed, so putting it on the wrong beat changes how the music sounds from the
    //! wrong place onward.
    const NoteLookup found = chordAt(score.get(), address(1, 2), 0);
    ASSERT_TRUE(found.found());
    ASSERT_TRUE(found.chord->segment() != nullptr);

    bool foundDynamic = false;
    for (mu::engraving::EngravingItem* item : found.chord->segment()->annotations()) {
        if (item->isDynamic()) {
            foundDynamic = true;
            EXPECT_EQ(toDynamic(item)->dynamicType(), DynamicType::MF);
        }
    }
    EXPECT_TRUE(foundDynamic) << "the dynamic should be attached to beat 2";
}

TEST(AgentHarness_ScoreRecipes, AddDynamicAcceptsTheMarkingsTheFormatAccepts)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! The parser is the notation layer's own, so this is checking that the recipe REUSES it rather than
    //! keeping a list that would drift from what the file format accepts.
    for (const char* mark : { "pp", "p", "mp", "mf", "f", "ff", "sfz" }) {
        const auto fresh = loadScore(NOTE_SCORE);
        ASSERT_TRUE(fresh);
        const RecipeResult result = runRecipe(fresh.get(), [&] {
            return addDynamic(fresh.get(), address(1, 1), QString::fromLatin1(mark));
        });
        EXPECT_TRUE(result.ok) << mark << ": " << result.problem.toStdString();
    }
}

TEST(AgentHarness_ScoreRecipes, AddDynamicRejectsSomethingThatIsNotAMarking)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ THE PARSER FALLS BACK TO `DynamicType::OTHER` RATHER THAN FAILING, so without a check the caller
    //! would be told "added `12345`" for a marking stored as an uninterpreted blob. That is the
    //! "looks like it worked" failure this project keeps meeting.
    //!
    //! ⚠️ The first version of this test used `xyzzy`, which the parser ACCEPTS: its pattern is
    //! `[fmnprsz]+`, and `xyzzy` is made only of those letters. That is not a bug in the parser - a
    //! dynamic marking really can be spelled from that set - so the test data was wrong, not the code.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addDynamic(score.get(), address(1, 1), QStringLiteral("12345"));
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("12345"))) << result.problem.toStdString();

    //! And nothing may have been added.
    //!
    //! ⚠️ COUNTED, not asserted-absent: `test.mscx` already carries a `pp` on beat 1, so "there is no
    //! dynamic here" fails on the pre-existing one. Comparing the count before and after is what actually
    //! tests "this call added nothing".
    const NoteLookup found = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(found.found());
    int dynamicsHere = 0;
    for (mu::engraving::EngravingItem* item : found.chord->segment()->annotations()) {
        if (item->isDynamic()) {
            ++dynamicsHere;
        }
    }
    EXPECT_EQ(dynamicsHere, 1) << "the score ships with one dynamic on beat 1; the refused call must not "
                               << "have added a second";
}

TEST(AgentHarness_ScoreRecipes, AddDynamicOnARestStillWorks)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! A dynamic over a rest is ordinary notation. The locator returns `rest` for that case and the recipe
    //! has to pass it through rather than a null chord.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addDynamic(score.get(), address(2, 1), QStringLiteral("p"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, AddHairpinFromOneBeatToAnother)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return addHairpin(score.get(), address(1, 1), address(1, 3), QStringLiteral("crescendo"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ THE END MUST BE WHERE IT WAS ASKED FOR. A hairpin that exists but spans the wrong beats draws
    //! correctly and sounds wrong - the crescendo happens over the wrong notes.
    const NoteLookup start = chordAt(score.get(), address(1, 1), 0);
    const NoteLookup end = chordAt(score.get(), address(1, 3), 0);
    ASSERT_TRUE(start.found());
    ASSERT_TRUE(end.found());

    //! ⛔ A HAIRPIN IS A SPANNER, NOT AN ANNOTATION. Looking for it in the segment's annotations finds
    //! nothing even when it exists - the first version of this test did exactly that and reported "the
    //! hairpin should start at beat 1" for a hairpin that was there. Spanners live in the score's spanner
    //! map, which is the same place `ChordRest::slur()` looks.
    //! ⛔⛔ TWO THINGS ABOUT THIS ASSERTION, both learned the hard way.
    //!
    //! ⚠️ ONE: `test.mscx` SHIPS WITH A HAIRPIN over bar 1, so searching the spanner map finds more than
    //! the one just created - asserting on every hairpin in range fails on the pre-existing one. Counting
    //! hairpins that START at the requested tick is what actually tests "this call added one".
    //!
    //! ⛔ TWO: THE END IS `cr2->endTick()`, NOT `cr2->tick()`. Upstream sets the hairpin's end to the END
    //! of the chord rest it was given, so a hairpin told to end "at beat 3" covers beats 1-3 INCLUSIVE and
    //! its `tick2` is beat 4. The first version of this test expected `tick2 == end.chord->tick()` and
    //! failed against correct behaviour. A musician means the same thing by it - "crescendo through beat
    //! 3" - so the recipe is right and the expectation was wrong.
    //! ⚠️ AND THE PRE-EXISTING HAIRPIN IS SHORTER THAN THE REQUESTED ONE, which is what makes the span
    //! itself the discriminator. `test.mscx`'s own hairpin starts at beat 1 but STOPS at beat 2 - upstream
    //! truncates a hairpin at the next dynamic, and there is a `pp` on beat 2. So "starts at beat 1" alone
    //! matches both, and only "starts at beat 1 AND ends at beat 4" identifies the one this call made.
    int hairpinsWithTheRequestedSpan = 0;
    for (auto& pair : score->spannerMap().findOverlapping(start.chord->tick().ticks(),
                                                          end.chord->endTick().ticks())) {
        mu::engraving::Spanner* spanner = pair.value;
        if (!spanner->isHairpin()) {
            continue;
        }
        if (spanner->tick() == start.chord->tick() && spanner->tick2() == end.chord->endTick()) {
            ++hairpinsWithTheRequestedSpan;
        }
    }
    EXPECT_EQ(hairpinsWithTheRequestedSpan, 1)
        << "exactly one hairpin should span beat 1 to the end of beat 3";
}

TEST(AgentHarness_ScoreRecipes, AddHairpinWithoutAnEndRunsToTheNextBeat)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! The palette behaviour: no end given means "from here to the next thing". Passing the same address
    //! as the end is how a caller says that, and it must NOT be confused with "the end is this beat".
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addHairpin(score.get(), address(1, 1), address(1, 1), QStringLiteral("diminuendo"));
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, AddHairpinRejectsAnUnknownKind)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return addHairpin(score.get(), address(1, 1), address(1, 1), QStringLiteral("swell"));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("crescendo"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, AddHairpinWithAnEndThatDoesNotExistIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ THE CASE THE EXPLICIT END RESOLUTION EXISTS FOR. A null `cr2` makes upstream run to the next
    //! chord rest, so "I named bar 99" and "I named nothing" would otherwise produce the SAME hairpin -
    //! and the caller would have no way to tell that its address was ignored.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return addHairpin(score.get(), address(1, 1), address(99, 1), QStringLiteral("crescendo"));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("99"))) << result.problem.toStdString();
}
//! ── Measures ──────────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, InsertMeasuresAddsThemAndShiftsWhatFollows)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);
    ASSERT_EQ(score->firstMeasure()->nextMeasure()->nextMeasure(), nullptr) << "test.mscx has 2 bars";

    const RecipeResult result = runRecipe(score.get(), [&] {
        return insertMeasures(score.get(), 1, 2);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ COUNTED BY WALKING, not by trusting the call. `insertMeasure` returns a pointer, and "it
    //! returned non-null" is not evidence that the score grew by the right amount.
    int count = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++count;
    }
    EXPECT_EQ(count, 4);
}

TEST(AgentHarness_ScoreRecipes, InsertOnePastTheEndAppends)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⚠️ The commonest case: adding a bar at the end. Refusing "3" for a two-bar score would leave the
    //! caller with no way to do it, and clamping would put the bar somewhere it did not ask for.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return insertMeasures(score.get(), 3, 1);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    int count = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++count;
    }
    EXPECT_EQ(count, 3);
}

TEST(AgentHarness_ScoreRecipes, InsertBeyondOnePastTheEndIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return insertMeasures(score.get(), 99, 1);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("99"))) << result.problem.toStdString();
    //! And the message must say what IS allowed, or the caller is left guessing at the range.
    EXPECT_TRUE(result.problem.contains(QStringLiteral("3"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, InsertWithACountBelowOneIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return insertMeasures(score.get(), 1, 0);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty());
}

TEST(AgentHarness_ScoreRecipes, RemoveMeasuresTakesThemOut)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return removeMeasures(score.get(), 1, 1);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    int count = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++count;
    }
    EXPECT_EQ(count, 1);
}

TEST(AgentHarness_ScoreRecipes, RemoveRefusesToEmptyTheScore)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ A score with no measures is not a short score, it is a broken one. This is the caller getting the
    //! arithmetic wrong, and the message has to say how many it MAY remove or it is not actionable.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return removeMeasures(score.get(), 1, 2);
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("at least one measure"))) << result.problem.toStdString();
    EXPECT_TRUE(result.problem.contains(QStringLiteral("1"))) << "it must say how many may go";

    int count = 0;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        ++count;
    }
    EXPECT_EQ(count, 2) << "nothing may have been removed";
}

TEST(AgentHarness_ScoreRecipes, RemoveWithABadRangeIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! `last` before `first` is an empty or backwards range, and both spellings are the caller's mistake.
    for (const auto& pair : { std::pair<int, int> { 2, 1 }, { 0, 1 } }) {
        const RecipeResult result = runRecipe(score.get(), [&] {
            return removeMeasures(score.get(), pair.first, pair.second);
        });
        EXPECT_FALSE(result.ok) << pair.first << "-" << pair.second << " must be refused";
    }
}

TEST(AgentHarness_ScoreRecipes, RemovePastTheEndIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return removeMeasures(score.get(), 99, 99);
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("no measure 99"))) << result.problem.toStdString();
}
//! ── moveNote ──────────────────────────────────────────────────────────────────────────────────
//!
//! ⛔⛔ A MOVE IS A DELETE PLUS AN ADD, and that shape is what these tests are about. Done naively - remove
//! then add - a failure in the second half leaves the note GONE, so the score changed AND the caller was
//! told the operation failed. The recipe validates everything first and performs the halves in the order
//! that keeps the note alive, and the refusal tests below are the ones that pin it: each is a case where a
//! naive implementation would have destroyed the note before discovering the problem.

TEST(AgentHarness_ScoreRecipes, MoveNoteCarriesThePitchToTheTargetAndOffTheSource)
{
    const auto notes = loadScore(NOTE_SCORE);
    ASSERT_TRUE(notes);

    //! ⛔ THE SOURCE HAS TO BE ABLE TO SPARE THE NOTE, so beat 2 is first made into a two-note chord
    //! [C4 D4]. Without that this move is REFUSED - `test.mscx` beats hold one note each, and moving the
    //! only one would leave an empty chord. (The first version of this test asserted success on the
    //! single-note case and failed, correctly: the refusal rule is the point, not an obstacle to it.)
    ASSERT_TRUE(runRecipe(notes.get(), [&] {
        return addNoteToChord(notes.get(), address(1, 2), 0, 60);
    }).ok);

    const RecipeResult result = runRecipe(notes.get(), [&] {
        return moveNote(notes.get(), address(1, 2), 1, address(1, 1));   //!< move the D4 to beat 1
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ THE PITCH MUST BE AT THE TARGET *AND* GONE FROM THE SOURCE. Checking only one of the two would
    //! pass against a recipe that copied instead of moving, or that deleted without adding.
    const NoteLookup target = chordAt(notes.get(), address(1, 1), 0);
    ASSERT_TRUE(target.found());
    EXPECT_TRUE(target.chord->findNote(62) != nullptr) << "D4 should now be on beat 1";

    const NoteLookup source = chordAt(notes.get(), address(1, 2), 0);
    ASSERT_TRUE(source.found());
    EXPECT_TRUE(source.chord->findNote(62) == nullptr) << "D4 should be gone from beat 2";
    EXPECT_TRUE(source.chord->findNote(60) != nullptr) << "but the C4 on beat 2 must have stayed";
}

TEST(AgentHarness_ScoreRecipes, MoveNoteRefusesToEmptyTheSourceChord)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔⛔ THE CASE THAT PINS THE ORDER. `test.mscx` beat 2 holds ONE note (D4), so moving it would leave
    //! an empty chord - which is not a rest. A naive implementation would remove it first and only then
    //! discover it cannot, leaving the beat broken. The recipe refuses BEFORE writing anything.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return moveNote(score.get(), address(1, 2), -1, address(1, 1));
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("note_to_rest"))) << result.problem.toStdString();

    //! ⛔ AND NOTHING MAY HAVE CHANGED - which is the whole point. The note must still be there.
    const NoteLookup still = noteAt(score.get(), address(1, 2), 0, -1);
    ASSERT_TRUE(still.ok()) << "the note must NOT have been removed";
    EXPECT_EQ(still.note->pitch(), 62);

    const NoteLookup target = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(target.found());
    EXPECT_EQ(target.chord->notes().size(), 1u) << "and the target must not have gained it either";
}

TEST(AgentHarness_ScoreRecipes, MoveNoteRefusesAPitchTheTargetAlreadyHas)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! `test.mscx` has C4 on beat 1 and D4 on beat 2, and beat 2 holds only that note - so this is refused
    //! for TWO reasons. To test the duplicate rule alone, move from a beat that can spare the note: use
    //! `chord-test.mscx`, whose single beat holds [C4 E4 B4], and move its C4 onto... itself, which is the
    //! same-beat rule. So this uses the other direction: `test.mscx` beat 1 is a single note too.
    //!
    //! ⚠️ The honest note: on these small test scores almost every move trips more than one rule at once,
    //! so this asserts the refusal happens and that the score is untouched, rather than which message won.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return moveNote(score.get(), address(1, 1), -1, address(1, 2));
    });

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty()) << "a refusal must say why";

    const NoteLookup still = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(still.ok()) << "the note must still be at the source";
    EXPECT_EQ(still.note->pitch(), 60);
}

TEST(AgentHarness_ScoreRecipes, MoveNoteRefusesTheSameBeat)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return moveNote(score.get(), address(1, 1), -1, address(1, 1));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("nowhere to move"))) << result.problem.toStdString();
}

TEST(AgentHarness_ScoreRecipes, MoveNoteRefusesARestTarget)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! A rest is not something a note can be moved INTO - the note has to go onto a beat that already has
    //! one. `chord-test.mscx` is a single bar, so the target has to be a rest in another score; use
    //! `test.mscx` bar 2, which is a whole-measure rest, and a source that can spare its note.
    const auto notes = loadScore(NOTE_SCORE);
    ASSERT_TRUE(notes);

    const RecipeResult result = runRecipe(notes.get(), [&] {
        return moveNote(notes.get(), address(1, 2), -1, address(2, 1));
    });

    //! Refused either for the rest target or for emptying the source - both are correct refusals, and the
    //! point of the test is that NOTHING WAS WRITTEN.
    EXPECT_FALSE(result.ok);

    const NoteLookup still = noteAt(notes.get(), address(1, 2), 0, -1);
    ASSERT_TRUE(still.ok()) << "the note must still be at the source";
}

TEST(AgentHarness_ScoreRecipes, MoveNoteFromAMultiNoteChordLeavesTheRestOfTheChord)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ THE POSITIVE CASE THAT NEEDS A MULTI-NOTE SOURCE. `chord-test.mscx` has one bar with [C4 E4 B4],
    //! so a move within it is a same-beat move - which means the honest way to test "the source keeps its
    //! other notes" is to move within a score that has two beats. `test.mscx` beats are single notes, so
    //! this test builds the situation the recipe is FOR by moving a note ONTO a beat, which is the
    //! combination the other tests already cover - and asserts the source chord kept what it should.
    const auto notes = loadScore(NOTE_SCORE);
    ASSERT_TRUE(notes);

    //! Make beat 1 a two-note chord first, so the source has something to keep.
    ASSERT_TRUE(runRecipe(notes.get(), [&] {
        return addNoteToChord(notes.get(), address(1, 2), 0, 60);
    }).ok);

    const RecipeResult result = runRecipe(notes.get(), [&] {
        return moveNote(notes.get(), address(1, 2), 1, address(1, 3));   //!< move the D4 (index 1) to beat 3
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! Beat 2 must still hold the C4 that was not moved.
    const NoteLookup source = chordAt(notes.get(), address(1, 2), 0);
    ASSERT_TRUE(source.found());
    EXPECT_TRUE(source.chord->findNote(60) != nullptr) << "the C4 must have stayed on beat 2";
    EXPECT_TRUE(source.chord->findNote(62) == nullptr) << "the D4 must have gone";
}
//! ── Articulations ─────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, ArticulationIsAddedThenToggledOff)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    const RecipeResult added = runRecipe(score.get(), [&] {
        return toggleArticulation(score.get(), address(1, 1), 0, -1, QStringLiteral("articStaccatoAbove"));
    });
    EXPECT_TRUE(added.ok) << added.problem.toStdString();
    EXPECT_TRUE(added.detail.contains(QStringLiteral("added"))) << added.detail.toStdString();

    const NoteLookup afterAdd = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(afterAdd.ok());
    ASSERT_FALSE(afterAdd.chord->articulations().empty()) << "the articulation must exist";

    //! ⛔ TOGGLE, NOT ADD. Asking twice must take it away rather than stack a second dot on the same
    //! notehead - and the RESULT has to say which way it went, or a caller that asked to add and got a
    //! removal has no way to know.
    const RecipeResult removed = runRecipe(score.get(), [&] {
        return toggleArticulation(score.get(), address(1, 1), 0, -1, QStringLiteral("articStaccatoAbove"));
    });
    EXPECT_TRUE(removed.ok) << removed.problem.toStdString();
    EXPECT_TRUE(removed.detail.contains(QStringLiteral("removed"))) << removed.detail.toStdString();

    const NoteLookup afterRemove = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(afterRemove.ok());
    EXPECT_TRUE(afterRemove.chord->articulations().empty()) << "the articulation must be gone";
}

TEST(AgentHarness_ScoreRecipes, ArticulationRejectsANameItDoesNotKnow)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔⛔ `SymNames::symIdByName` RETURNS A DEFAULT RATHER THAN FAILING - `SymId::noSym`. Without the
    //! check, an unknown name would create an articulation with no symbol: an object in the score that
    //! draws as nothing and cannot be selected. This is the same "the lookup does not report failure"
    //! shape as the dynamic marking's `OTHER` fallback.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return toggleArticulation(score.get(), address(1, 1), 0, -1, QStringLiteral("staccato"));
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("articStaccatoAbove")))
        << "it must show a valid name: " << result.problem.toStdString();

    const NoteLookup after = noteAt(score.get(), address(1, 1), 0, -1);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(after.chord->articulations().empty()) << "nothing may have been added";
}

TEST(AgentHarness_ScoreRecipes, ArticulationAcceptsTheCommonNames)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    for (const char* name : { "articStaccatoAbove", "articAccentAbove", "articTenutoAbove",
                              "articMarcatoAbove", "articStaccatissimoAbove", "articStaccatoBelow" }) {
        const auto fresh = loadScore(NOTE_SCORE);
        ASSERT_TRUE(fresh);
        const RecipeResult result = runRecipe(fresh.get(), [&] {
            return toggleArticulation(fresh.get(), address(1, 1), 0, -1, QString::fromLatin1(name));
        });
        EXPECT_TRUE(result.ok) << name << ": " << result.problem.toStdString();
    }
}

TEST(AgentHarness_ScoreRecipes, ArticulationOnARestIsRefused)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⚠️ An articulation hangs off a NOTE, unlike a dynamic or a rehearsal mark which hang off the
    //! segment - so bar 2's rest has nothing to attach one to, and the refusal comes from the note
    //! lookup rather than from a null dereference.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return toggleArticulation(score.get(), address(2, 1), 0, -1, QStringLiteral("articStaccatoAbove"));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.problem.isEmpty()) << "a refusal must say why";
}

TEST(AgentHarness_ScoreRecipes, ArticulationOnAMultiNoteChordNeedsAnIndex)
{
    const auto score = loadScore(CHORD_SCORE);
    ASSERT_TRUE(score);

    //! A chord of three notes and no index: the locator refuses, and the refusal must survive the
    //! articulation path rather than being turned into "added" by the toggle.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return toggleArticulation(score.get(), address(1, 1), 0, -1, QStringLiteral("articStaccatoAbove"));
    });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("3 notes"))) << result.problem.toStdString();
}
//! ── The score-level consistency check (G3) ────────────────────────────────────────────────────
//!
//! ⛔⛔ WHAT THIS IS FOR, AND WHAT IT IS NOT. Every recipe validates its own inputs, but a BATCH composes
//! them, and composition is where invariants that hold one at a time stop holding. `MasterScore::
//! sanityCheck()` is upstream's own whole-document check and the only thing that sees the score as a
//! whole. These tests pin the PREDICATE - that it accepts a sound score and reports a broken one - so the
//! gate's silence in normal use means something.

TEST(AgentHarness_ScoreRecipes, SanityCheckAcceptsTheTestScores)
{
    //! ⛔ THE MOST IMPORTANT CASE IS THE QUIET ONE. A gate that fires on healthy input is worse than no
    //! gate: it turns every batch into a refusal, and the refusal blames the tool. So every fixture the
    //! suite uses is run through it, because those are exactly the scores the tools are exercised on.
    for (const muse::String& name : { NOTE_SCORE, CHORD_SCORE, TIE_SCORE, EMPTY_SCORE }) {
        const auto score = loadScore(name);
        ASSERT_TRUE(score) << "fixture missing";
        const muse::Ret result = score->sanityCheck();
        EXPECT_TRUE(result) << "the fixture should be sound, but: " << result.text();
    }
}

TEST(AgentHarness_ScoreRecipes, SanityCheckReportsAMeasureThatDoesNotAddUp)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⛔ AND THE DETECTION IS WORTH PINNING TOO, or the gate would pass its tests by never firing.
    //! `undoChangeChordRestLen` is used deliberately here INSTEAD of `setChordDuration`: the recipe has a
    //! gate that refuses a duration which does not fit, and this test needs to actually produce the broken
    //! state to prove `sanityCheck` sees it. Using the raw upstream call is the only way to make a score
    //! that is wrong on purpose.
    score->startCmd(muse::TranslatableString::untranslatable("agentharness test"));
    const NoteLookup found = chordAt(score.get(), address(1, 4), 0);
    ASSERT_TRUE(found.found());
    //! A whole note starting on beat 4 of a 4/4 bar: one beat of room, four beats of content.
    mu::engraving::TDuration tooLong;
    tooLong.setType(mu::engraving::DurationType::V_WHOLE);
    score->undoChangeChordRestLen(found.chord, tooLong);
    score->endCmd();

    const muse::Ret result = score->sanityCheck();
    EXPECT_FALSE(result) << "a bar holding more beats than its signature must be reported";
    EXPECT_FALSE(result.text().empty()) << "and the report must say something";
}
//! ── Parts and staves ──────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_ScoreRecipes, AppendStaffAddsAStaffToThePart)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);
    ASSERT_EQ(score->parts().size(), 1u) << "test.mscx has one part";
    ASSERT_EQ(score->parts()[0]->nstaves(), 1u);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return appendStaff(score.get(), 0);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();

    //! ⛔ COUNTED, not trusted to the return value - the same shape as the measure insert. `appendStaff`
    //! returns a pointer, and a non-null pointer is not evidence that the part grew.
    EXPECT_EQ(score->parts()[0]->nstaves(), 2u);

    //! ⛔ AND THE SCORE MUST KNOW ABOUT IT TOO, not just the part. A staff that the part counts but the
    //! score does not is a staff with no measures - it would draw as an empty line, or not at all.
    EXPECT_EQ(score->nstaves(), 2u);
}

TEST(AgentHarness_ScoreRecipes, AppendStaffGivesTheNewStaffThePartsKeySignature)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! Put the part in a key first, so "the new staff inherited it" is a fact rather than a coincidence of
    //! both being C major.
    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return setKeySignature(score.get(), 1, -3);
    }).ok);

    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return appendStaff(score.get(), 0);
    }).ok);

    //! ⛔ THIS IS THE PART OF `EditPart::appendStaff` THAT IS EASY TO LOSE. Wiring a `Staff` in by hand
    //! (which `MasterScore` does internally) skips `adjustKeySigs`, and the result is a staff that reads in
    //! a DIFFERENT KEY from the one above it - visible only by looking at the accidentals.
    const NoteLookup at = chordAt(score.get(), address(1, 1), 0);
    ASSERT_TRUE(at.found());
    const mu::engraving::Staff* second = score->staff(1);
    ASSERT_TRUE(second != nullptr);
    EXPECT_EQ(int(second->key(at.chord->tick())), -3)
        << "the new staff must inherit the part's key signature";
}

TEST(AgentHarness_ScoreRecipes, RemoveLastStaffTakesItAway)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    ASSERT_TRUE(runRecipe(score.get(), [&] {
        return appendStaff(score.get(), 0);
    }).ok);
    ASSERT_EQ(score->parts()[0]->nstaves(), 2u);

    const RecipeResult result = runRecipe(score.get(), [&] {
        return removeLastStaff(score.get(), 0);
    });
    EXPECT_TRUE(result.ok) << result.problem.toStdString();
    EXPECT_EQ(score->parts()[0]->nstaves(), 1u);
    EXPECT_EQ(score->nstaves(), 1u);
}

TEST(AgentHarness_ScoreRecipes, RemoveLastStaffRefusesToLeaveThePartEmpty)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);
    ASSERT_EQ(score->parts()[0]->nstaves(), 1u);

    //! ⛔ A PART WITH NO STAVES IS NOT A SMALL PART, IT IS A BROKEN ONE - the same reasoning as "a score
    //! needs at least one measure". The part's instrument is still there claiming a staff that does not
    //! exist. Removing it is a request to remove the PART, which this tool deliberately does not do - and
    //! the message says so rather than just refusing.
    const RecipeResult result = runRecipe(score.get(), [&] {
        return removeLastStaff(score.get(), 0);
    });

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.problem.contains(QStringLiteral("only one staff"))) << result.problem.toStdString();
    EXPECT_EQ(score->parts()[0]->nstaves(), 1u) << "nothing may have been removed";
}

TEST(AgentHarness_ScoreRecipes, StaffToolsRefuseAPartThatDoesNotExist)
{
    const auto score = loadScore(NOTE_SCORE);
    ASSERT_TRUE(score);

    //! ⚠️ The range is in the message. "No such part" without it leaves the caller guessing whether parts
    //! are 0-based, 1-based, or counted per staff.
    for (int bad : { -1, 1, 99 }) {
        const RecipeResult result = runRecipe(score.get(), [&] {
            return appendStaff(score.get(), bad);
        });
        EXPECT_FALSE(result.ok) << bad << " must be refused";
        EXPECT_TRUE(result.problem.contains(QStringLiteral("0 to 0"))) << result.problem.toStdString();
    }
}