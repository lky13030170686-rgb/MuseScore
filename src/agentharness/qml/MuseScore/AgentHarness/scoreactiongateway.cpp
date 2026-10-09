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
#include "scoreactiongateway.h"

#include "async/async.h"

#include "log.h"

#include "fieldcontroller.h"

using namespace muse::agentharness;

namespace {
//! Everything the batch chain needs, on the heap because the chain outlives this call.
struct BatchState {
    QVector<WriteResult> results;
    bool allOk = true;
    bool opened = false;
    QString failure;
    //! The transaction index the merge starts from, captured before the first dispatch.
    size_t startIndex = 0;
};
} // namespace

ScoreActionGateway::ScoreActionGateway(FieldController* field)
    : m_field(field)
{
}

WriteResult ScoreActionGateway::perform(const WriteOp& op) const
{
    return performWith(m_field, op);
}

WriteResult ScoreActionGateway::performWith(FieldController* field, const WriteOp& op)
{
    WriteResult result;
    result.command = op.command;

    if (!field) {
        result.ok = false;
        result.error = QStringLiteral("no score context available");
        return result;
    }

    if (op.command.isEmpty()) {
        result.ok = false;
        result.error = QStringLiteral("empty command");
        return result;
    }

    //! ⛔ Ask first. A disabled notation command is silently skipped by the controller's outer
    //! wrapper, so dispatching it yields "success" and no change at all (维护手册.md §4.8, 第 594 条 -
    //! this project has paid for that twice). The refusal has to happen HERE, before the dispatch,
    //! because after it there is nothing left to detect.
    if (!field->isCommandEnabled(op.command)) {
        result.ok = false;
        result.error = QStringLiteral("`%1` is not enabled right now, so dispatching it would be "
                                      "silently ignored").arg(op.command);
        return result;
    }

    result.error = field->dispatchCommand(op.command, op.params);
    result.ok = result.error.isEmpty();
    return result;
}

void ScoreActionGateway::performBatch(const QVector<WriteOp>& ops, const QString& actionName,
                                     std::function<void(const QVector<WriteResult>&, bool)> done) const
{
    if (!m_field) {
        QVector<WriteResult> results;
        WriteResult r;
        r.ok = false;
        r.error = QStringLiteral("no score context available");
        results.append(r);
        done(results, false);
        return;
    }

    //! ⛔ THE STATE IS ON THE HEAP AND THE FIELD IS CAPTURED BY VALUE, because every part of this runs
    //! from a promise continuation - after this function, and after the tool that built this gateway,
    //! have returned. A lambda holding `this` points at a destroyed object; a reference into this
    //! frame is a write into freed memory. Three versions crashed on exactly that before this one.
    auto state = std::make_shared<BatchState>();
    FieldController* field = m_field;

    //! ⛔⛔ HOW A BATCH BECOMES ONE UNDO STEP, and it is not by opening a transaction.
    //!
    //! The obvious approach - open a transaction, dispatch, commit - does not work here, and the
    //! reason is worth stating because two earlier versions got it wrong in different ways:
    //!
    //!   1. `dispatch()` is ASYNCHRONOUS (`make_promise` goes through `Async::call`), so a transaction
    //!      opened and closed around the dispatch calls commits BEFORE any handler runs.
    //!   2. Even with the transaction held open across the completions, `append-measures` does not use
    //!      it: that command reaches `Score::startCmd`, which calls `beginTransaction` DIRECTLY on the
    //!      transaction manager, bypassing the notation undo stack. Measured: three appends produced
    //!      `stateIndex 1→2→3→4` with the batch's transaction open the whole time, and the batch's
    //!      `commitChanges` then found no active transaction at all.
    //!
    //! So the transaction cannot be IMPOSED from outside for these commands; it has to be
    //! RECONSTRUCTED afterwards with `mergeTransactions`, which folds a range of transactions into the
    //! first one - keeping its name, which is why the batch's action name is applied to the FIRST
    //! command. That is what the rest of the application does, and it is the only lever that works for
    //! commands that open their own.
    //!
    //! The batch's own transaction is still opened, because for commands that DO use `prepareChanges`
    //! it is what groups them - and merging is then a no-op.
    auto chain = std::make_shared<std::function<void(int)>>();

    *chain = [field, ops, state, chain, actionName, done](int index) {
        if (index == 0) {
            //! Refusals are checked BEFORE anything is dispatched: a disabled command would be silently
            //! skipped by the controller's outer wrapper, so catching it here is the only chance
            //! (维护手册.md §4.8, 第 594 条). Nothing has been opened, so nothing needs rolling back.
            for (const WriteOp& op : ops) {
                WriteResult r;
                r.command = op.command;
                if (!field->isCommandEnabled(op.command)) {
                    r.ok = false;
                    r.error = QStringLiteral("`%1` is not enabled right now, so dispatching it would be "
                                             "silently ignored").arg(op.command);
                    state->results.append(r);
                    state->failure = QStringLiteral("%1: %2").arg(op.command, r.error);
                    state->allOk = false;
                    done(state->results, false);
                    return;
                }
                state->results.append(r);
            }

            //! Where the merge will start. Read BEFORE the first dispatch, so it is the index of the
            //! transaction the first command is about to create.
            state->startIndex = size_t(field->scoreRevision());
            field->beginUndoTransaction(actionName);
            state->opened = true;
        }

        if (index >= ops.size()) {
            //! Every dispatch has completed. Close the batch's own transaction first: for commands
            //! that DO use `prepareChanges` it is what grouped them, and for the ones that open their
            //! own it is a no-op (measured: `stateIndex 4 -> 4, committed=0`).
            field->endUndoTransaction(true);
            state->opened = false;

            for (const WriteResult& r : state->results) {
                if (!r.ok && state->failure.isEmpty()) {
                    state->failure = QStringLiteral("%1: %2").arg(r.command, r.error);
                }
            }

            if (state->allOk) {
                //! Fold the per-command transactions into one, so the batch is a single undo step.
                if (ops.size() > 1) {
                    field->mergeTransactionsFrom(state->startIndex);
                }
            } else {
                //! ⛔ UNDO BACK, do not merely "not merge". The commands that ran committed themselves
                //! before the batch could decide, so by now the earlier ones are already on the undo
                //! stack - closing the batch's transaction with `rollback` would roll back nothing.
                //! Measured: a three-operation batch with a bad third operation left the first two
                //! applied while the report said "rolled back". Undoing to the recorded revision is
                //! the only way to make "NOTHING was applied" true after the fact.
                LOGW() << "[agent-write] batch rolled back:" << state->failure;
                field->undoToRevision(int(state->startIndex));

                //! ⚠️ Only the SUCCESSFUL entries are relabelled. The failing one keeps its own
                //! reason: it is the cause, and overwriting it with "rolled back with the rest" hides
                //! the only line that says what to fix.
                for (WriteResult& r : state->results) {
                    if (r.ok) {
                        r.ok = false;
                        r.error = QStringLiteral("rolled back with the rest of the batch (%1)")
                                  .arg(state->failure);
                    }
                }
            }

            const int revision = field->scoreRevision();
            LOGW() << "[agent-write] batch done: ops=" << state->results.size()
                   << "ok=" << state->allOk
                   << "revision=" << revision;

            done(state->results, state->allOk);
            return;
        }

        const WriteOp op = ops[index];

        //! ⛔⛔ ONE `onResolve` PER PROMISE, and the reason is a trap worth naming: the callback
        //! registry is keyed by RECEIVER, and `onResolve` registers with `Mode::SetOnce` - which
        //! REPLACES an existing callback from the same receiver rather than adding a second one
        //! (`channelimpl.h`: `if (mode == Asyncable::Mode::SetOnce) { return needIncrement; }`).
        //!
        //! The first version registered twice on the same promise with the same receiver: once to read
        //! the response, once to advance the chain. The second call SILENTLY REPLACED the first, so
        //! the error was never recorded - and the chain still looked like it worked, because the
        //! surviving callback was the one that advanced. That is why the batch reported success while
        //! only the first operation had been applied: the results were never filled in, so nothing
        //! could fail.
        //!
        //! Serial, in model order: the next dispatch starts from this one's completion. Every tool in
        //! the table touches the same `Score`, and two writers at once is not a concurrency problem
        //! worth having.
        field->dispatchCommandPromise(op.command, op.params)
        .onResolve(field, [state, chain, index](const muse::rcommand::Response& res) {
            if (!res.ret) {
                state->results[index].ok = false;
                state->results[index].error = QString::fromStdString(res.ret.toString());
                state->allOk = false;
            }

            (*chain)(index + 1);
        });
    };

    (*chain)(0);
}
