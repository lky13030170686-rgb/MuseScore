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

#include "agentharness/qml/MuseScore/AgentHarness/agentloop.h"
#include "agentharness/qml/MuseScore/AgentHarness/sessionlog.h"

using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS. The per-turn step guard became a USER-EDITABLE setting (Agent 页 的设置 → 助手,
//! 第 130 条), which means its value now arrives from a text field and from an environment variable instead
//! of from a programmer. The clamp is what keeps the guard a guard: `0` would end every turn before its
//! first tool ran, and a five-digit number is the unbounded loop the guard was written against. None of
//! that is visible from the outside - the turn simply stops, or does not - so it is pinned here.

TEST(AgentHarness_AgentLoop, StepsPerTurnIsClampedNotRejected)
{
    SessionLog session;
    AgentLoop loop(&session, nullptr, nullptr);

    //! A sane value is taken as-is.
    loop.setMaxStepsPerTurn(24);
    EXPECT_EQ(loop.maxStepsPerTurn(), 24);

    //! ⛔ Below the floor and above the ceiling: the value is CLAMPED, not refused. Refusing would leave
    //! whatever was there before - and a settings field that silently keeps the old value is the failure
    //! this project keeps refusing (the user reads back a number they did not choose).
    loop.setMaxStepsPerTurn(0);
    EXPECT_EQ(loop.maxStepsPerTurn(), AgentLoop::MIN_STEPS_PER_TURN);

    loop.setMaxStepsPerTurn(-5);
    EXPECT_EQ(loop.maxStepsPerTurn(), AgentLoop::MIN_STEPS_PER_TURN);

    loop.setMaxStepsPerTurn(100000);
    EXPECT_EQ(loop.maxStepsPerTurn(), AgentLoop::MAX_STEPS_PER_TURN);

    //! And the bounds are ordered, so "clamped into the range" cannot mean an empty range.
    EXPECT_LT(AgentLoop::MIN_STEPS_PER_TURN, AgentLoop::DEFAULT_STEPS_PER_TURN);
    EXPECT_LT(AgentLoop::DEFAULT_STEPS_PER_TURN, AgentLoop::MAX_STEPS_PER_TURN);
}

//! ── The soft landing (第 147 条) ──────────────────────────────────────────────────────────────────
//!
//! ⛔ THE MEASUREMENT BEHIND THESE TWO RULES (2026-10-18, 197 log files, 139 turns): successful turns need
//! 1-8 steps, and EVERY turn that hit the limit needed 9-12. So the cap cuts the tail rather than the
//! runaway, and the model was never told it was about to run out - it asked for one more tool, the turn
//! failed, and the user saw a bare "stopped after 8 steps". What is pinned here is the wording that turns
//! that dead end into a landing, and the arithmetic that decides WHEN it is said.

TEST(AgentHarness_AgentLoop, TheBudgetNoteStaysQuietWhileThereIsRoom)
{
    //! ⚠️ A note on every step would be noise, and noise about a limit is worse than none: a model told
    //! "you have 8 steps left" eight times learns to ignore the sentence.
    EXPECT_TRUE(AgentLoop::stepBudgetNote(1, 8).isEmpty());
    EXPECT_TRUE(AgentLoop::stepBudgetNote(5, 8).isEmpty());
    EXPECT_TRUE(AgentLoop::stepBudgetNote(6, 8).isEmpty());

    //! With ONE step left it speaks, and it says both numbers: the limit, and what remains.
    //! ⚠️ "ONE", spelled out, is what the note says - the test asserts the sentence rather than the digit,
    //! because the digit was never the point (and asserting it made a wording improvement look like a bug).
    const QString note = AgentLoop::stepBudgetNote(7, 8);
    EXPECT_FALSE(note.isEmpty());
    EXPECT_TRUE(note.contains(QStringLiteral("step limit is 8"))) << note.toStdString();
    EXPECT_TRUE(note.contains(QStringLiteral("ONE step remains"))) << note.toStdString();

    //! At the last step it still speaks - that is the step whose decision ends the turn - and it says
    //! something STRONGER, because that is where a real model got it wrong (2026-10-18): with the softer
    //! wording it did the work and then spent its last step asking for one more tool (`score_window`, a
    //! check it did not need), so the turn ended with no answer. The last-step text names the trap.
    //! ⚠️ The assertion is about the number, not about the character `-`: the first version of this test
    //! asserted "contains no hyphen", and the note legitimately has one in its prose ("… do next - the user
    //! can tell you to continue …"). A proxy that the correct output fails is a wrong ruler, not a finding.
    const QString last = AgentLoop::stepBudgetNote(8, 8);
    EXPECT_FALSE(last.isEmpty());
    EXPECT_FALSE(last.contains(QStringLiteral("-1"))) << last.toStdString();
    EXPECT_TRUE(last.contains(QStringLiteral("LAST step"))) << last.toStdString();
    EXPECT_TRUE(last.contains(QStringLiteral("NO step after this one"))) << last.toStdString();
    EXPECT_NE(last, note) << "the last step must say something the one-before-last does not";
}

TEST(AgentHarness_AgentLoop, TheBudgetNoteSaysWhatToDoInsteadOfJustStopping)
{
    //! ⛔ THE THREE THINGS THAT MAKE IT A LANDING RATHER THAN A DEAD END: finish the part that matters, say
    //! what is left, and know that the user can ask for more. The first version of this turn's ending said
    //! none of them ("stopped after 8 steps without the model finishing"), and one user hit it four times
    //! in a row - which is what a dead end looks like from the outside.
    const QString note = AgentLoop::stepBudgetNote(7, 8);
    EXPECT_TRUE(note.contains(QStringLiteral("what is done"), Qt::CaseInsensitive)) << note.toStdString();
    EXPECT_TRUE(note.contains(QStringLiteral("what is left"), Qt::CaseInsensitive)) << note.toStdString();
    EXPECT_TRUE(note.contains(QStringLiteral("continue"))) << note.toStdString();

    //! And the reason the turn ends with says the same three things, because it is read by a USER, not by a
    //! model: the limit, that the work so far is real, and both ways to carry on.
    const QString reason = AgentLoop::stepLimitReason(8);
    EXPECT_TRUE(reason.contains(QStringLiteral("8"))) << reason.toStdString();
    EXPECT_TRUE(reason.contains(QStringLiteral("Ctrl+Z"))) << reason.toStdString();
    EXPECT_TRUE(reason.contains(QStringLiteral("continue"))) << reason.toStdString();
    EXPECT_TRUE(reason.contains(QStringLiteral("settings"))) << reason.toStdString();
}

TEST(AgentHarness_AgentLoop, TheDefaultIsWhatAResetRestores)
{
    //! `FieldController::resetAgentPreferences()` writes `DEFAULT_STEPS_PER_TURN` back. This asserts that
    //! the constant IS the value a fresh loop holds - otherwise "reset" would restore a number the loop
    //! never used, which reads as a working reset and is not one.
    SessionLog session;
    AgentLoop loop(&session, nullptr, nullptr);
    EXPECT_EQ(loop.maxStepsPerTurn(), AgentLoop::DEFAULT_STEPS_PER_TURN);

    loop.setMaxStepsPerTurn(31);
    loop.setMaxStepsPerTurn(AgentLoop::DEFAULT_STEPS_PER_TURN);
    EXPECT_EQ(loop.maxStepsPerTurn(), AgentLoop::DEFAULT_STEPS_PER_TURN);
}

TEST(AgentHarness_AgentLoop, TheModelDefaultIsAlsoAskingTheLoop)
{
    //! Same argument for the model: the reset restores `AgentLoop::defaultModel()`, so that string has to
    //! be the one a fresh loop actually carries.
    SessionLog session;
    AgentLoop loop(&session, nullptr, nullptr);
    EXPECT_EQ(loop.model(), AgentLoop::defaultModel());
}
