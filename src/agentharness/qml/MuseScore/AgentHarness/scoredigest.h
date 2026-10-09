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

namespace mu::engraving {
class Score;
}

namespace muse::agentharness {
//! The snapshot layer: "what the score is right now", projected for a reader.
//!
//! WHY THIS IS NOT A FULL SCORE DUMP. A real orchestral score flattens to tens of thousands of
//! notes; handing that to a model is both unaffordable and useless, because the question is almost
//! never "everything". So the digest comes in two sizes, mirroring the read tool that will consume
//! it (技术设计 §3.5):
//!
//!   - `buildScoreOverview()`  - bounded (a few dozen lines), always safe to send;
//!   - `buildMeasureWindow()`  - one measure range, on request.
//!
//! WHY IT IS COMPUTED, NOT STORED. Nothing here is cached. The digest is a pure projection of the
//! score at the moment it is asked for, which means it cannot go stale - and staleness here would
//! mean the model is editing based on a score that no longer exists. The cost is one traversal of
//! the measures, paid only when a reader actually asks.

//! Bounded summary: metadata, part/staff list, measure counts, per-staff note counts and pitch
//! ranges, and where the time signature changes. Plain text, because it is going straight into a
//! prompt and text is what a model reads most reliably.
QString buildScoreOverview(const mu::engraving::Score* score);

//! One measure range, as compact per-measure lines. `firstMeasure`/`lastMeasure` are 1-based and
//! inclusive, matching how the reader addresses the score everywhere else.
//! Returns a message rather than an empty string when the range is out of bounds - "nothing" and
//! "you asked for something that does not exist" must not look the same.
QString buildMeasureWindow(const mu::engraving::Score* score, int firstMeasure, int lastMeasure);

//! ── Pure helpers (unit-testable without a score) ─────────────────────────────────────────────

//! "C4" / "F#3" / "Bb5" for a MIDI pitch. Spelling is by pitch class only (sharps), because the
//! digest is describing *sounding* pitch; the score's own spelling is a notation concern that the
//! window view reads from the note itself.
QString pitchName(int midiPitch);

} // namespace muse::agentharness
