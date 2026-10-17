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
#ifndef MU_AGENTHARNESS_SCORESAVE_H
#define MU_AGENTHARNESS_SCORESAVE_H

#include <QDateTime>
#include <QString>

namespace muse::agentharness {
//! ── Saving the score to the file it came from (第 146 条) ─────────────────────────────────────────
//!
//! ⛔ WHY THIS IS NOT A RECIPE. Every other write in this subsystem goes through the notation undo stack,
//! and Ctrl+Z takes it back. Saving does not: it overwrites a file on the user's disk, and nothing in the
//! editor can undo that. So it lives in its own file - `scorerecipes` is "edits to the score", and this is
//! not an edit - and it is the one operation whose description has to SAY that it is not undoable.
//!
//! ⛔ AND WHY THE AGENT MAY DO IT AT ALL. It is the same act as the user's own Ctrl+S, on the same file,
//! through the app's own writer: the format handling, the "create a backup before saving" preference and
//! the modified-marker bookkeeping all stay the app's business. ⛔ What the agent may NOT do is choose a
//! filename - see `saveRefusal`.

//! Why the score cannot be saved where it already lives, or an empty string when it can.
//!
//! ⛔ THE THREE CASES ARE DIFFERENT, AND EACH GETS ITS OWN REASON:
//!   - NOTHING OPEN: there is no file to save to.
//!   - NO PATH: a score created in this session. Choosing a name and a folder is the user's decision, and
//!     the app's Save As would ask them for it.
//!   - `newlyCreated` WITH a path: ⛔⛔ THIS IS NOT "NEVER SAVED", which is what the first version of this
//!     function said - and the real program proved it wrong the first time it ran (measured 2026-10-17).
//!     `NotationProject::load` sets `isNewlyCreated` for a file whose MSC version is older than the current
//!     format (and for a backup): "this was imported, so do not write it in place". The app's own save asks
//!     the user WHERE for exactly those files (`ProjectActionsController::saveProject`:
//!     `isExistingSave = !isNewlyCreated()`), and a tool must not do what the app deliberately does not -
//!     overwriting somebody's old-format original with a converted one.
//!
//! ⛔ AND A TOOL MUST NOT OPEN A DIALOG IT CANNOT WAIT FOR: it would report "saved" while the user is still
//! looking at a file picker - a success report about something that has not happened, which is the failure
//! this subsystem keeps refusing. So every case above ends with the same instruction: the user saves it once
//! by hand, and this tool keeps that file up to date from then on.
QString saveRefusal(bool hasProject, bool newlyCreated, const QString& path);

//! Whether the file at `path` was written since `before` (its modification time) and `sizeBefore`.
//!
//! ⛔ THIS IS THE SECOND JUDGE, AND IT IS ABOUT THE FILE RATHER THAN ABOUT THE APP'S WORD. `save()`
//! returning success says the app thinks it wrote; this says the file on disk changed. They disagree when
//! the save went somewhere else than the caller believes - the failure that would otherwise be reported as
//! a successful save of the wrong file. ⚠️ Either a new modification time OR a different size counts: a save
//! that rewrote the file with the same number of bytes still moved the clock.
//!
//! ⚠️ WHAT IT CANNOT SEE, said plainly: a rewrite with the SAME size inside the same clock tick reads as
//! "not rewritten". That is a false negative rather than a false positive - it can only make the tool
//! report a failure it did not have, never a success it did not have - which is the direction to err in.
bool fileWasRewritten(const QString& path, const QDateTime& before, qint64 sizeBefore);
} // namespace muse::agentharness

#endif // MU_AGENTHARNESS_SCORESAVE_H
