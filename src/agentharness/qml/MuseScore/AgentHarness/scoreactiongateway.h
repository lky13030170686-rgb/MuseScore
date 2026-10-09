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

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace muse::agentharness {
class FieldController;

//! One requested write.
struct WriteOp
{
    QString command;
    QJsonObject params;
};

//! The outcome of one requested write, in request order.
struct WriteResult
{
    QString command;
    bool ok = true;
    //! Why it was refused, or what the command layer reported.
    QString error;
};

//! The write side's single entry point.
//!
//! WHY A GATEWAY AND NOT "just dispatch from the tool": three things have to be true of every write,
//! and they have to be true in ONE place or they will eventually be true in none:
//!
//!   1. **It goes through the notation command layer.** That is what makes an agent's edit the user's
//!      edit - same undo stack, same command state, same notifications. There is no second write path
//!      to keep in sync (技术设计 §4.1).
//!   2. **The command is enabled.** A disabled command is silently skipped by the controller's outer
//!      wrapper, so dispatching one produces "it worked and nothing happened" (维护手册.md §4.8).
//!   3. **A batch is ONE undo step.** See `performBatch`.
//!
//! It owns no score access of its own: everything goes through the controller, which is where the
//! notation is resolved. That keeps "who can touch the score" answerable by reading one file.
class ScoreActionGateway
{
public:
    explicit ScoreActionGateway(FieldController* field);

    //! Perform one write. Never throws; a refusal comes back as `ok == false` with a reason.
    WriteResult perform(const WriteOp& op) const;

    //! Perform several writes as ONE undo step.
    //!
    //! ⛔ THE REASON THIS EXISTS, and it is not convenience. A model that wants to add a note to eight
    //! measures will issue eight writes; if each is its own undo step the user needs eight Ctrl+Z to
    //! take back one instruction, and the information field records eight unrelated-looking actions.
    //! Batching makes "what the agent was asked to do" and "what one Ctrl+Z undoes" the same thing.
    //!
    //! ⛔⛔ **THE WHOLE BATCH IS QUEUED AS ONE UNIT, and that is the only way it works.**
    //! `ICommandDispatcher::dispatch` is ASYNCHRONOUS - `make_promise` goes through `Async::call`,
    //! which sends the work to the thread's queue port rather than running it. So a batch that opened
    //! its transaction, dispatched, and committed synchronously would commit BEFORE any handler ran,
    //! and the handlers would each open their own transaction afterwards. The measured symptom was
    //! exactly that: three appends produced three undo steps, and the transaction reported
    //! `stateIndex 1 -> 1, committed=0` while the dispatches reported `txActive=1` - both true, because
    //! nothing had run yet.
    //!
    //! Queueing the transaction and the dispatches together puts them in one FIFO, so the handlers run
    //! while the transaction is open and `Score::undo()` pushes into it.
    //!
    //! \b All-or-nothing. If a command is refused mid-batch the transaction is ROLLED BACK, not
    //! committed with the successful prefix. A half-applied instruction is the worst outcome available:
    //! the model believes it did the whole thing, the user sees part of it, and neither can tell which
    //! part.
    //!
    //! \b Asynchronous. `done` is called with the per-operation results and whether the batch committed.
    //! It is invoked from the event loop, so callers must not assume it has run when this returns.
    void performBatch(const QVector<WriteOp>& ops, const QString& actionName,
                      std::function<void(const QVector<WriteResult>&, bool committed)> done) const;

private:
    //! The body of `perform`, as a free-standing form that takes the field explicitly.
    //!
    //! ⛔ WHY IT IS STATIC: `performBatch`'s queued callback must not hold `this`. The gateway is a
    //! stack temporary in the tool that builds it, so by the time the queue runs the callback, `this`
    //! points at a destroyed object - which SIGSEGVed the application. A static function taking the
    //! field by value has nothing to dangle.
    static WriteResult performWith(FieldController* field, const WriteOp& op);

    FieldController* m_field = nullptr;
};

} // namespace muse::agentharness
