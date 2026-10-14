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
#pragma once

#include <QString>

#include "addressing.h"
#include "scorerecipes.h"

namespace mu::engraving {
class Score;
}

namespace muse::agentharness {
//! ── Editor state that is NOT score data ────────────────────────────────────────────────────────
//!
//! What is selected, and where the cursor is. These change with every click and cost no undo step, so
//! they are neither a read of the score nor a write to it - and they live in their own file for exactly
//! that reason: a caller must be able to tell "did this change my music?" from the name of the thing it
//! called.
//!
//! ⛔ WHY THIS EXISTS (第 138 条). Measured 2026-10-13, asked whether it could split a measure, the agent
//! answered - correctly - *"命令存在，但我不能保证作用在你说的小节上"*: the command layer's 400+ commands act
//! on the SELECTION, and nothing in the toolset could put the selection at an address. The user's own path
//! (click a bar, then use the command) was the only one, so a whole class of commands was reachable but
//! not addressable.

//! Select measures `firstMeasure`..`lastMeasure` inclusive (1-based), optionally limited to one staff.
//!
//! ⚠️ This MOVES THE USER'S SELECTION, and that is the point rather than a side effect: the commands that
//! act on a selection (`split-measure`, `join-measures`, `copy`, `delete`, ...) are the reason to call it,
//! and the result says what the selection became. The score data is untouched, so `revision` does not move.
//!
//! ⛔ WHY IT STARTS BY DESELECTING. `Score::selectRange` EXTENDS an existing range rather than replacing it
//! (`m_selection.isRange()` -> `extendRangeSelection`), so a leftover selection would silently grow the
//! range the caller asked for - and a range that is one bar too long is exactly the kind of wrong the
//! caller cannot see. Starting from "nothing selected" makes the result a function of the arguments.
//!
//! @param staff 1-based staff, or 0 for every staff (which is what a "select these bars" means by default).
RecipeResult selectMeasures(mu::engraving::Score* score, int firstMeasure, int lastMeasure, int staff);

//! Put the selection ON the note or rest at `address` - a SINGLE-element selection, which is a different
//! precondition from the measure range above.
//!
//! ⛔ WHY BOTH ARE NEEDED (measured 2026-10-13): the commands that act on a selection disagree about what
//! they want. `NotationInteraction::joinSelectedMeasures` requires `selection()->isRange()`, while
//! `splitSelectedMeasure` reads `selection()->element()` and **returns silently** when there is none - so a
//! measure range makes join work and split do nothing at all. Handing the caller one kind of selection and
//! letting it guess which commands accept it would be exactly the silent no-op this project keeps hunting.
RecipeResult selectElementAt(mu::engraving::Score* score, const ScoreAddress& address, int voice);
} // namespace muse::agentharness
