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
#include <QStringList>
#include <QVector>

#include "addressing.h"

namespace muse::agentharness {
struct RawFieldEvent;

//! One operation as a reader - a person or a model - should see it.
//!
//! This is the **semantic layer** of the information field (技术设计 §3.4). It is *derived* from the
//! raw layer and never stored: `deriveSemanticOps()` is a pure function of the raw events, so when a
//! derivation rule turns out to be wrong the facts are still there to re-derive from. That is the
//! whole reason the raw layer exists, and it is also the reason this layer may be opinionated:
//! being wrong here costs a re-run, not the history.
//!
//! WHAT IT DELIBERATELY DOES NOT DO: it does not invent a value-level description ("C4 became D4").
//! `ScoreChanges` carries the *kind* of change and the region, not the before/after values - the old
//! value is not preserved anywhere (ChangeProperty::flip() swaps old and new in place). Producing a
//! value-level story would mean guessing, and a field that guesses is worse than one that is
//! honestly coarse.
struct SemanticOp
{
    quint64 seq = 0;          //!< Same coordinate system as the raw layer, so the two can be joined.
    QString wallClock;
    //! "user" / "agent" / "plugin" / "unknown" - who caused it. Currently always "user"; the agent's
    //! own writes will be tagged here, which is what makes "who changed this bar" answerable.
    QString source;
    //! What happened, in the undo stack's own words ("输入音符" / "Insert note"). This is the most
    //! valuable field in the struct: it is a human-written name, not a reconstruction.
    QString action;
    //! True when this operation took a previous one back. The action name still describes the
    //! operation being undone, so a reader must not treat the name alone as "what happened".
    bool isUndo = false;
    bool isRedo = false;

    //! Where, as a tick range (the only coordinate that is always correct).
    int tickFrom = -1;
    int tickTo = -1;
    int staffFrom = -1;
    int staffTo = -1;
    //! Where, in the units a reader uses. Empty when the tick range is unknown.
    QString where;

    //! How much: number of distinct engraving objects the transaction touched.
    int objectCount = 0;
    //! What kind of change, as a compact sorted list of names ("AddElement", "ChangePitch").
    //! Sorted so two runs of the same edit produce byte-identical output - which is what makes
    //! "derive the same ops from the same log" a testable claim.
    QStringList kinds;

    //! One line, the way the panel and the model both want it.
    QString toString() const;
};

//! Turn raw events into readable operations. PURE: no score, no context, no clock.
//!
//! The mapping is intentionally close to one-to-one - one raw event, one op - because the raw layer
//! is already one-event-per-transaction (`TransactionManager::commitTransaction` sends exactly once
//! per committed transaction and not at all for an empty one). Merging consecutive events would make
//! the op stream diverge from the undo stack, and the undo stack is what the user can actually act
//! on; an op stream that does not line up with Ctrl+Z is not useful for explaining or auditing.
//!
//! @param raw      events oldest-first, as `FieldController::events()` returns them
//! @param grid     measure grid, for the readable `where` field; may be empty
//! @param part     part index to stamp on the addresses (0 = the score itself)
QVector<SemanticOp> deriveSemanticOps(const QVector<RawFieldEvent>& raw, const MeasureGrid& grid, int part = 0);

//! Name for a `CommandType` value, as it appears in the enum. Unknown values render as
//! "CommandType(<n>)" rather than being dropped: an unnamed kind is still evidence that *something*
//! happened, and silently omitting it would make the op stream look cleaner than reality.
QString commandTypeName(int value);

//! True when `value` is an element-type bucket rather than a command-type one. The raw layer packs
//! both into one histogram with an offset (see FieldController), and this is the inverse of that.
bool isElementTypeBucket(int value);

//! Name for an element-type bucket value, or empty when it is not one.
QString elementTypeName(int value);

} // namespace muse::agentharness
