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

    //! ⛔⛔ THE TRANSACTION IS OPENED AND CLOSED INSIDE THE PROMISE CHAIN, and that is the whole
    //! design. `dispatch()` only QUEUES the command handler (`make_promise` goes through
    //! `Async::call`), so a transaction opened and closed around the dispatch calls commits BEFORE
    //! any handler runs. The measured symptom was three commands producing three undo steps while the
    //! transaction reported `committed=0` and the dispatches reported `txActive=1` - both true,
    //! because nothing had run yet. Opening the transaction before the first dispatch and closing it
    //! in the last dispatch's continuation is what puts the handlers inside it.
    auto chain = std::make_shared<std::function<void(int)>>();

    *chain = [field, ops, state, chain, actionName, done](int index) {
        if (index == 0) {
            //! Refusals are checked BEFORE the transaction opens: a disabled command would be silently
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

            field->beginUndoTransaction(actionName);
            state->opened = true;
        }

        if (index >= ops.size()) {
            //! Every dispatch has completed. The handlers ran while the transaction was open, so
            //! `Score::undo()` pushed into it and one Ctrl+Z takes the whole batch back.
            for (const WriteResult& r : state->results) {
                if (!r.ok && state->failure.isEmpty()) {
                    state->failure = QStringLiteral("%1: %2").arg(r.command, r.error);
                }
            }

            if (!state->allOk) {
                //! ⛔ ROLLED BACK, and the results say so. A half-applied instruction is the worst
                //! available outcome - the model believes it did the whole thing, the user sees part
                //! of it, and neither can tell which part.
                //!
                //! ⚠️ Only the SUCCESSFUL entries are relabelled. The failing one keeps its own
                //! reason: it is the cause, and overwriting it with "rolled back with the rest" hides
                //! the only line that says what to fix.
                LOGW() << "[agent-write] batch rolled back:" << state->failure;
                for (WriteResult& r : state->results) {
                    if (r.ok) {
                        r.ok = false;
                        r.error = QStringLiteral("rolled back with the rest of the batch (%1)")
                                  .arg(state->failure);
                    }
                }
                field->endUndoTransaction(false);
                state->opened = false;
                done(state->results, false);
                return;
            }

            const bool committed = field->endUndoTransaction(true);
            state->opened = false;

            LOGW() << "[agent-write] batch done: ops=" << state->results.size()
                   << "committed=" << committed
                   << "revision=" << field->scoreRevision();

            if (!committed) {
                //! Every command was accepted but the transaction did not advance, which means the
                //! framework rolled it back (a read-only score, or an error a command set). Saying so
                //! is the difference between "the edit happened" and "the edit silently did not".
                for (WriteResult& r : state->results) {
                    r.ok = false;
                    r.error = QStringLiteral("the transaction was rolled back by the framework");
                }
            }

            done(state->results, committed);
            return;
        }

        const WriteOp op = ops[index];

        //! Serial, in model order: the next dispatch waits for the previous handler to finish. Every
        //! tool in the table touches the same `Score`, and two writers at once is not a concurrency
        //! problem worth having.
        field->dispatchCommandPromise(op.command, op.params)
        .onResolve(field, [state, index](const muse::rcommand::Response& res) {
            if (!res.ret) {
                state->results[index].ok = false;
                state->results[index].error = QString::fromStdString(res.ret.toString());
                state->allOk = false;
            }
        })
        .onResolve(field, [chain, index](const muse::rcommand::Response&) {
            (*chain)(index + 1);
        });
    };

    (*chain)(0);
}
