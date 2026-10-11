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
