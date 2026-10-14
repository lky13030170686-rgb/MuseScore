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

namespace muse::agentharness {
//! ── Which ACTION does a raw notification belong to? ─────────────────────────────────────────────
//!
//! ⛔⛔ WHY THIS EXISTS (第 139 条). The field has two change channels - the score's own
//! (`changesChannel`, once per non-empty transaction) and the automation data's (`AutomationData::changed`,
//! `automationdata.h:49`). Both were recorded as raw events, and for most edits that is right: a typed note
//! changes only the score. But a STRUCTURAL edit makes the automation data change **as a consequence** -
//! splitting a measure forces the tempo/repeat timeline to be rebuilt - and the rebuild notifies. Measured
//! 2026-10-14, one user action each:
//!
//! | action | raw events before | of which automation |
//! |---|---|---|
//! | `split-measure` | 4 | **3** |
//! | `join-measures` | 3 | **2** |
//! | `repeat_set` + `volta_add` (one batch) | 2 | **1** |
//! | `measure_input_notes` x4 | 4 | 0 |
//! | `measure_insert` (append) | 1 | 0 |
//! | `add-clef-bass` | 1 | 0 |
//!
//! Three rows in the timeline for one keystroke is a lie about how much happened - and it inflated
//! `revision` too, so a model that read `revision 1` had to guess `4` after a split.
//!
//! ── The rule: HOLD, THEN LET THE SCORE EVENT ABSORB ─────────────────────────────────────────────
//! An automation change is held for the rest of the current event-loop turn. If the score's own event
//! arrives in that turn, it is the ACTION those changes belong to, and it absorbs them - one action, one
//! event. If no event arrives, they are recorded as their own events when the turn ends. Nothing is lost
//! either way; only the attribution changes.
//!
//! ⛔⛔ AND THE OBVIOUS-LOOKING TEST IS THE WRONG ONE - measured, not guessed. The first version of this
//! class held a change only while `hasActiveTransaction()` was true. On the real application that caught
//! two of the three changes of a `split-measure` and MISSED the third (`Automation reset`), because
//! `TransactionManager::endTransaction` runs `updateAutomation` **after** it has already ended the
//! transaction and cleared `m_currentTransaction`:
//!
//!     stack->endTransaction(isCurrentTransactionEmpty);
//!     m_currentTransaction.reset();
//!     if (!isCurrentTransactionEmpty && !rollback) {
//!         m_masterScore->updateAutomation(changes);   // <- "no transaction in flight" from here on
//!         m_masterScore->changesChannel().send(changes);
//!     }
//!
//! So "is a transaction open" answers a question about the STACK, while the automation rebuild of a commit
//! belongs to the EVENT that is about to be sent. The ordering is what carries the answer, and the ordering
//! is reliable: a same-thread `Channel::send` calls its receivers synchronously (`channelimpl.h`,
//! `SendMode::Auto` -> `receiversCall`), so the score event lands in the same turn as the changes it owns.
class AutomationAttribution
{
public:
    //! An automation change arrived and is being held. Returns true when this is the FIRST one held, which
    //! is the caller's cue to schedule the end-of-turn release.
    bool hold();

    //! The score's own event arrived. Returns how many held changes it absorbs - they are part of this
    //! action, so they are NOT recorded separately.
    int absorb();

    //! The turn ended. Returns how many held changes were not covered by any event and must now be recorded
    //! as their own events.
    int release();

    //! How many changes are held right now (for the trace log and the tests).
    int heldCount() const { return m_held; }

private:
    int m_held = 0;
};
} // namespace muse::agentharness
