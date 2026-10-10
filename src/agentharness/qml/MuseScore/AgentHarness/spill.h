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
//! Above this many bytes, a tool result is written to disk and the model sees a preview instead.
//!
//! ⚠️ The number is the plan's DSH default. It is not a model limit - it is the point past which a
//! result stops being readable in a conversation and starts being a wall: `score_window` over a long
//! score, or `command_list` over all 446 commands, are both perfectly legitimate results that a caller
//! cannot use as prose.
constexpr int SPILL_THRESHOLD_BYTES = 50000;

//! How much of each end the preview keeps.
//!
//! ⛔ BOTH ENDS, not just the head. The head says what the result IS; the tail is where a list ends up
//! saying how many there were ("50 measures", a closing bracket, a final error). Keeping only the head
//! would hide exactly the part a caller checks to decide whether the result is complete.
constexpr int SPILL_PREVIEW_BYTES = 2000;

//! The threshold actually in force, which `MUSE_AGENT_SPILL_THRESHOLD` can lower.
//!
//! ⛔ WHY A SWITCH, AND WHY THIS ONE MATTERS MORE THAN USUAL. The whole spill path is invisible in normal
//! use: it fires above 50 KB, and the largest real result this harness produces (`command_list` over all
//! 457 commands) is about 20 KB. So the end-to-end path - "the preview reaches the conversation, the file
//! is on disk, the file holds the whole result" - CANNOT be reached by any ordinary call, and a bug in it
//! would only be found by the one user who eventually asks for something huge.
//!
//! This is the repository's established pattern for exactly that situation (维护手册.md, `MUSE_MIDI_*`):
//! a switch that makes the rare path reachable WITHOUT A REBUILD. Lowering the threshold to 200 bytes
//! exercises the same code as 50 KB - there is nothing size-dependent in it except the comparison.
int spillThresholdBytes();

//! Write `text` to a file and return what the model should see instead.
//!
//! The returned text is a head/tail preview, the path, and how much was left out - in that order, because
//! the preview is what a caller reads first and the path is what it reaches for second.
//!
//! @param toolName used in the file name, so a directory listing says which tool produced what
//! @return the preview. When the text is under the threshold, or cannot be written, the text is returned
//!         UNCHANGED - see the note in the implementation on why a failed spill must not fail the call.
QString spillIfLarge(const QString& toolName, const QString& text);
} // namespace muse::agentharness
