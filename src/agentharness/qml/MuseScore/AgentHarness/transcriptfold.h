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

#include <QVariantList>

namespace muse::agentharness {
//! Fold runs of consecutive tool rows into one `toolGroup` row, for display.
//!
//! \b Why this exists. Measured 2026-10-11 on a real turn: the model read the score with FOUR tools, and
//! the conversation showed it as EIGHT rows (a call line and a result line each) between the question and
//! the answer - and the step ceiling is 8, so the worst case is sixteen. The answer - the only part the
//! user asked for - ends up below the fold. The web client folds a run of tool calls into one line for
//! exactly this reason.
//!
//! \b The rule, and the two things it must never do. A maximal run of consecutive rows whose `kind` is
//! `toolCall` or `toolResult` becomes ONE row of kind `toolGroup` carrying the original rows in `items`.
//! The fold is a RUN-LENGTH COMPRESSION IN PLACE:
//!   - nothing is REORDERED: a group sits exactly where its first row sat;
//!   - nothing is DROPPED: the folded rows are still there, in order, inside `items` (the view shows them
//!     again when the user expands the group).
//! A run of one is folded too - one rule, no special case, and the group line is still shorter than the
//! two rows it replaces.
//!
//! \b Why a pure function rather than logic in the delegate. What the user is SHOWN is a claim about the
//! conversation, and this project's rule is that such claims are checkable without a window: the fold is
//! text-in / text-out, so "the order is preserved" and "an error is never hidden" are unit tests
//! (`transcriptfold_tests.cpp`) instead of something somebody has to notice on a screenshot.
//!
//! The rows are the maps `FieldController::agentTranscript()` produces; unknown fields are passed through
//! untouched, so the fold cannot be the reason a field disappears.
QVariantList foldToolRows(const QVariantList& rows);

} // namespace muse::agentharness
