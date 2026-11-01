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

namespace muse::agentharness {

//! What the user has selected RIGHT NOW, flattened into plain numbers so the sentence about it can be a
//! pure function (第 173 条).
//!
//! WHY THIS IS ITS OWN FILE. The selection is the one kind of fact nothing else in the harness reports:
//! the score digest describes the score, the timeline describes what happened, and neither knows what
//! the user is pointing at. It is also mostly a *range arithmetic* problem - `endTick()` is exclusive, a
//! selection can start and end mid-measure, and "the last measure" is where the off-by-one hides - so
//! the part worth trusting is exactly the part that can be tested without a score.
struct SelectionFacts
{
    enum class Kind {
        None,
        //! One element (a measure, a note, a dynamic…): the user clicked one thing.
        Single,
        //! A range, selected with the mouse or by Ctrl+A: from a position to a position.
        Range,
    };

    Kind kind = Kind::None;

    //! 1-based, the way the rest of the harness addresses the score. `0` means "not known".
    int firstMeasure = 0;
    int firstBeat = 0;
    int lastMeasure = 0;
    int lastBeat = 0;

    //! 1-based staff numbers (the UI shows "staff 1" for index 0). 0 = not known.
    int firstStaff = 0;
    int lastStaff = 0;

    int noteCount = 0;

    //! The element's XML tag from `TConv::toXml(ElementType)` - "Note", "Rest", "Measure", "Dynamic"…
    //! ⛔ Not `subtypeUserName()` / `translatedSubtypeUserName()`: those are TRANSLATABLE, so the same
    //! selection would read differently depending on the UI language. A fact must not depend on the locale
    //! (the rule `scoredigest.cpp` already follows for clefs and key signatures).
    QString elementType;

    //! Pitch of the selected note ("C4"), when the single selected element is a note.
    QString pitch;
};

//! One compact line, no trailing newline. **Empty for `Kind::None`**: the caller decides how to say
//! "nothing is selected" (`score_overview` prints `selection: none`, and the panel draws a dash), so the
//! word for it lives at the boundary rather than in here.
QString describeSelection(const SelectionFacts& facts);

//! ── The two conversions with an off-by-one in them (第 173 条) ─────────────────────────────────────────
//! The notation API describes a range with **half-open** bounds: `endTick()` and `endStaffIndex()` are
//! EXCLUSIVE (`INotationSelectionRange::containsItem` is `staffIdx < endStaffIndex()`). Everything a reader
//! or a tool sees is **inclusive** ("m3b2..m5b4", "staves 1..2"). Both conversions therefore subtract one,
//! both did it wrong once, and both are pure, so they are pinned by tests instead of by a screenshot.
//!
//! WHY IT MATTERS: a selection that ends exactly on a barline is the ordinary case (Ctrl+A, drag to the
//! next barline). Reporting the closed bound as the end claims one extra measure - a page of the wrong music
//! for an agent that acts on it - and every line it produces still looks perfectly ordinary.

//! Last tick that is inside the selection, given its half-open tick range. Returns `startTick` for an empty
//! or reversed range rather than a negative one.
int rangeLastTick(int startTick, int endTickExclusive);

//! 1-based, inclusive staff span, given the half-open index range. An unknown/empty range yields 0..0.
struct StaffSpan
{
    int first = 0;
    int last = 0;
};

StaffSpan staffSpanForDisplay(int startStaffIndex, int endStaffIndexExclusive);
}
