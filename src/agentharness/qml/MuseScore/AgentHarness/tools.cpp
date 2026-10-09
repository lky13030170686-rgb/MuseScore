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
#include "tools.h"

#include <QJsonArray>
#include <QJsonValue>

#include "fieldcontroller.h"
#include "scoreactiongateway.h"
#include "scoredigest.h"
#include "semanticderive.h"

using namespace muse::agentharness;

//! ── Schema helpers ────────────────────────────────────────────────────────────────────────────
//! Small builders so each tool's schema reads as what it is. JSON Schema, because that is what the
//! model is given and what the (M2) registry will validate against.
namespace {
QJsonObject schemaObject(const QJsonObject& properties, const QJsonArray& required = QJsonArray())
{
    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    if (!required.isEmpty()) {
        schema.insert(QStringLiteral("required"), required);
    }
    //! Closed on purpose: a model that invents a field is told so instead of having it ignored.
    schema.insert(QStringLiteral("additionalProperties"), false);
    return schema;
}

QJsonObject intProperty(const QString& description)
{
    QJsonObject p;
    p.insert(QStringLiteral("type"), QStringLiteral("integer"));
    p.insert(QStringLiteral("description"), description);
    return p;
}

QJsonObject stringProperty(const QString& description)
{
    QJsonObject p;
    p.insert(QStringLiteral("type"), QStringLiteral("string"));
    p.insert(QStringLiteral("description"), description);
    return p;
}

QJsonObject objectProperty(const QString& description)
{
    QJsonObject p;
    p.insert(QStringLiteral("type"), QStringLiteral("object"));
    p.insert(QStringLiteral("description"), description);
    return p;
}

//! Read an integer argument. JSON has no integer type in QJsonValue terms, so a model may send
//! `2` or `2.0`; both must work, and a missing key must be distinguishable from a zero.
bool readInt(const QJsonObject& args, const QString& key, int& out)
{
    if (!args.contains(key)) {
        return false;
    }
    const QJsonValue v = args.value(key);
    if (!v.isDouble()) {
        return false;
    }
    out = int(v.toDouble());
    return true;
}
} // namespace

//! ── Read tools ────────────────────────────────────────────────────────────────────────────────

ToolResult muse::agentharness::toolScoreOverview(const QJsonObject&, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    const QString text = ctx.field->digestOverview();
    if (text == QStringLiteral("(no score open)")) {
        //! A successful call with an unhelpful situation, not a failure: the tool worked, there is
        //! simply nothing to describe. Reporting it as an error would make the model retry.
        return ToolResult::success(text);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("measures"), ctx.field->measureCount());
    meta.insert(QStringLiteral("staves"), ctx.field->staffCount());
    meta.insert(QStringLiteral("revision"), ctx.field->revision());
    return ToolResult::success(text, meta);
}

ToolResult muse::agentharness::toolScoreWindow(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int first = 1;
    int last = 1;

    //! Default to measure 1 when the caller says nothing, but never invent an upper bound: a missing
    //! `last` means "the same measure", which is the smallest useful window and cannot surprise.
    if (!readInt(args, QStringLiteral("first"), first)) {
        first = 1;
    }
    if (!readInt(args, QStringLiteral("last"), last)) {
        last = first;
    }

    const QString text = ctx.field->digestWindow(first, last);
    if (text == QStringLiteral("(no score open)")) {
        return ToolResult::success(text);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("first"), first);
    meta.insert(QStringLiteral("last"), last);
    return ToolResult::success(text, meta);
}

ToolResult muse::agentharness::toolFieldTimeline(const QJsonObject&, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    const QVariantList ops = ctx.field->recentOps();
    if (ops.isEmpty()) {
        return ToolResult::success(QStringLiteral("no operations recorded yet"));
    }

    QStringList lines;
    for (const QVariant& v : ops) {
        const QVariantMap m = v.toMap();
        lines.append(m.value(QStringLiteral("line")).toString());
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("count"), int(ops.size()));
    meta.insert(QStringLiteral("total"), ctx.field->eventCount());
    return ToolResult::success(lines.join(QLatin1Char('\n')), meta);
}

//! ── Write tools ───────────────────────────────────────────────────────────────────────────────

ToolResult muse::agentharness::toolCommandList(const QJsonObject&, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    const QStringList names = ctx.field->enabledCommandNames();
    if (names.isEmpty()) {
        return ToolResult::success(QStringLiteral("no notation commands are enabled right now"));
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("count"), int(names.size()));

    QStringList lines;
    lines.append(QStringLiteral("%1 enabled notation commands:").arg(names.size()));
    for (const QString& n : names) {
        lines.append(QStringLiteral("  %1").arg(n));
    }
    return ToolResult::success(lines.join(QLatin1Char('\n')), meta);
}

ToolResult muse::agentharness::toolCommandDispatch(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    const QString command = args.value(QStringLiteral("command")).toString();
    if (command.isEmpty()) {
        return ToolResult::failure(QStringLiteral("`command` is required (a command://notation/... URI)"));
    }

    //! ── The fence ─────────────────────────────────────────────────────────────────────────────
    //! `expectRevision` is OPTIONAL, and that is a deliberate compromise. Requiring it would make every
    //! write fail for a model that has not read the score yet, and the first thing it would do is
    //! learn to pass a number it did not check - which is worse than not having the fence, because it
    //! would look like one. Optional means a model that read the score can be safe, and a model that
    //! did not is no worse off than before.
    if (args.contains(QStringLiteral("expectRevision"))) {
        int expected = 0;
        if (!readInt(args, QStringLiteral("expectRevision"), expected)) {
            return ToolResult::failure(QStringLiteral("`expectRevision` must be an integer"));
        }

        const int actual = ctx.field->scoreRevision();
        if (expected != actual) {
            return ToolResult::failure(
                QStringLiteral("the score has changed since you read it (you expected revision %1, it is "
                               "now %2), so this edit was NOT applied. Read the score again and redo the "
                               "edit against what is there now.").arg(expected).arg(actual));
        }
    }

    //! Forward the arguments. This is not a convenience: `append-measures` with no `count` opens a
    //! dialog instead of doing anything, so a forwarder that dropped `params` would report success
    //! while the score stayed untouched.
    const QJsonObject params = args.value(QStringLiteral("params")).toObject();
    const QString error = ctx.field->dispatchCommand(command, params);
    if (!error.isEmpty()) {
        return ToolResult::failure(QStringLiteral("dispatching `%1` failed: %2").arg(command, error));
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("command"), command);
    meta.insert(QStringLiteral("revision"), ctx.field->revision());
    return ToolResult::success(QStringLiteral("dispatched %1").arg(command), meta);
}

ToolResult muse::agentharness::toolScoreRevision(const QJsonObject&, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    const int revision = ctx.field->scoreRevision();

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), revision);

    //! The text says what the number is FOR. A bare "3" invites the model to treat it as a count of
    //! something it can compute; naming it as the value to pass back is what makes the fence usable.
    return ToolResult::success(
        QStringLiteral("score revision: %1\nPass this as `expectRevision` to a write so it is refused "
                       "if the score changes first.").arg(revision),
        meta);
}

ToolResult muse::agentharness::toolPatchApply(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    const QJsonValue opsValue = args.value(QStringLiteral("ops"));
    if (!opsValue.isArray()) {
        return ToolResult::failure(QStringLiteral("`ops` is required and must be an array"));
    }

    const QJsonArray opsArray = opsValue.toArray();
    if (opsArray.isEmpty()) {
        //! A successful call with nothing to do, not a failure: the tool worked, the caller asked for
        //! no changes. Reporting it as an error would make the model retry.
        return ToolResult::success(QStringLiteral("no operations requested"));
    }

    //! ⛔ TEMPORARY GUARD, and it must stay until the sequencing is fixed.
    //!
    //! The multi-operation path does not work yet: the transaction opens and commits as ONE undo step
    //! (that part is verified), but only the FIRST operation is applied - the chain that should start
    //! each dispatch after the previous handler completes stalls after one. A single-operation batch is
    //! correct end to end (verified: `append-measures {count:2}` inside `patch_apply` gives
    //! `ticks=0..5760` and one undo step).
    //!
    //! Refusing is the right call while that is true. A tool that silently applies one of eight
    //! requested edits is the worst thing in this table: the model believes the score changed, the
    //! user sees a partial result, and the per-operation report would even look plausible. Refusing
    //! with the reason is recoverable; a silent under-application is not.
    if (opsArray.size() > 1) {
        return ToolResult::failure(
            QStringLiteral("patch_apply currently accepts only ONE operation per call (this build): the "
                           "sequencing that makes several operations land as one undo step is not "
                           "finished, and applying only the first of %1 would silently do part of what "
                           "you asked. Use separate command_dispatch calls for now, and say so if you "
                           "need them grouped.").arg(opsArray.size()));
    }

    //! Same fence as `command_dispatch`, for the same reason - see the note there on why it is
    //! optional.
    if (args.contains(QStringLiteral("expectRevision"))) {
        int expected = 0;
        if (!readInt(args, QStringLiteral("expectRevision"), expected)) {
            return ToolResult::failure(QStringLiteral("`expectRevision` must be an integer"));
        }

        const int actual = ctx.field->scoreRevision();
        if (expected != actual) {
            return ToolResult::failure(
                QStringLiteral("the score has changed since you read it (you expected revision %1, it is "
                               "now %2), so NONE of these %3 operations were applied. Read the score "
                               "again and redo them against what is there now.")
                .arg(expected).arg(actual).arg(opsArray.size()));
        }
    }

    QVector<WriteOp> ops;
    for (const QJsonValue& v : opsArray) {
        const QJsonObject obj = v.toObject();
        WriteOp op;
        op.command = obj.value(QStringLiteral("command")).toString();
        op.params = obj.value(QStringLiteral("params")).toObject();

        if (op.command.isEmpty()) {
            return ToolResult::failure(QStringLiteral(
                                           "every operation needs a `command`; one of them had none. Nothing was applied."));
        }
        ops.append(op);
    }

    const QString actionName = args.value(QStringLiteral("actionName")).toString(
        QStringLiteral("Agent edit (%1 operation(s))").arg(ops.size()));

    if (!ctx.complete) {
        return ToolResult::failure(QStringLiteral("this tool needs an asynchronous result channel and "
                                                  "was called without one"));
    }

    //! Asynchronous, and it has to be: see the note in scoreactiongateway.h. The result reaches the
    //! conversation through `ctx.complete`, so this tool returns nothing.
    //! ⛔ `complete` is captured BY VALUE: the callback runs from a queued invocation, after this
    //! function has returned.
    ScoreActionGateway gateway(ctx.field);
    gateway.performBatch(ops, actionName,
                         [complete = ctx.complete](const QVector<WriteResult>& results, bool committed) {
        int failed = 0;
        QStringList lines;
        for (const WriteResult& r : results) {
            if (!r.ok) {
                ++failed;
                lines.append(QStringLiteral("  FAILED %1: %2").arg(r.command, r.error));
            } else {
                lines.append(QStringLiteral("  ok     %1").arg(r.command));
            }
        }

        QJsonObject meta;
        meta.insert(QStringLiteral("applied"), int(results.size()) - failed);
        meta.insert(QStringLiteral("failed"), failed);
        meta.insert(QStringLiteral("committed"), committed);

        if (failed > 0 || !committed) {
            //! ⛔ The text must say "nothing was applied" in as many words. A model reading a per-line
            //! report of ok/FAILED would reasonably conclude the successful lines landed - and they did
            //! not, because the batch rolled back. Leaving that implicit is how a model ends up
            //! believing it edited eight measures when the score is untouched.
            complete(ToolResult::failure(
                         QStringLiteral("the batch was rolled back, so NOTHING was applied (%1 of %2 "
                                        "operations failed). The score is unchanged.\n%3")
                         .arg(failed).arg(results.size()).arg(lines.join(QLatin1Char('\n'))), meta));
            return;
        }

        complete(ToolResult::success(
                     QStringLiteral("applied %1 operation(s) as one undo step (one Ctrl+Z takes them all "
                                    "back)\n%2").arg(results.size()).arg(lines.join(QLatin1Char('\n'))),
                     meta));
    });

    return ToolResult::success(QString());
}

//! ── The table ─────────────────────────────────────────────────────────────────────────────────

const std::vector<ToolSpec>& muse::agentharness::toolTable()
{
    static const std::vector<ToolSpec> table = {
        ToolSpec{
            QStringLiteral("score_overview"),
            QStringLiteral("Summarise the open score: title, measures, parts/staves, time signatures, "
                           "per-staff note counts and pitch ranges, and which measures are empty. "
                           "Bounded in size, so it is always safe to call first."),
            schemaObject({}),
            toolScoreOverview,
        },
        ToolSpec{
            QStringLiteral("score_window"),
            QStringLiteral("Read one measure range of the open score as compact per-measure lines "
                           "(pitch names, staff, voice, duration in ticks, and rests). "
                           "Measures are 1-based and inclusive."),
            schemaObject({
                { QStringLiteral("first"), intProperty(QStringLiteral("First measure (1-based).")) },
                { QStringLiteral("last"), intProperty(QStringLiteral("Last measure (1-based, inclusive). Defaults to `first`.")) },
            }),
            toolScoreWindow,
        },
        ToolSpec{
            QStringLiteral("field_timeline"),
            QStringLiteral("Read the information field: what has happened to this score, newest first, "
                           "as `#seq [undo] <action> @<measure><beat>`. This is the record of the user's "
                           "own edits, so it answers \"what did I just do\" without re-reading the score."),
            schemaObject({}),
            toolFieldTimeline,
        },
        ToolSpec{
            QStringLiteral("command_list"),
            QStringLiteral("List the notation commands that are enabled right now. Use this before "
                           "command_dispatch when unsure what the score currently allows."),
            schemaObject({}),
            toolCommandList,
        },
        ToolSpec{
            QStringLiteral("command_dispatch"),
            QStringLiteral("Perform one notation action by dispatching its command URI, e.g. "
                           "`command://notation/append-measures`. This is how the user's own editing "
                           "actions are performed, so it is undoable with Ctrl+Z exactly like a manual "
                           "edit. Refuses commands that are not currently enabled."),
            schemaObject({
                { QStringLiteral("command"), stringProperty(QStringLiteral("The command://notation/... URI to dispatch.")) },
                { QStringLiteral("params"), objectProperty(QStringLiteral("Optional command parameters (e.g. {\"count\": 2} for append-measures).")) },
                { QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                   "Optional. The revision you last read. The write is refused if the score has "
                                                                   "changed since - which includes edits the user made with the mouse.")) },
            }, QJsonArray{ QStringLiteral("command") }),
            toolCommandDispatch,
        },
        ToolSpec{
            QStringLiteral("score_revision"),
            QStringLiteral("The score's current revision number. Pass it as `expectRevision` to a write "
                           "so the write is refused if the score changes in between."),
            schemaObject({}),
            toolScoreRevision,
        },
        ToolSpec{
            QStringLiteral("patch_apply"),
            QStringLiteral("Perform several notation actions as ONE undo step: one Ctrl+Z takes them all "
                           "back, and the timeline records them as a single action. Use this whenever a "
                           "request needs more than one write - adding a note to each of several "
                           "measures, say - rather than calling command_dispatch repeatedly.\n"
                           "All or nothing: if any operation is refused, none of them are applied."),
            schemaObject({
                { QStringLiteral("ops"), QJsonObject{
                      { QStringLiteral("type"), QStringLiteral("array") },
                      { QStringLiteral("description"), QStringLiteral("The operations, in order.") },
                      { QStringLiteral("items"), QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("object") },
                            { QStringLiteral("properties"), QJsonObject{
                                  { QStringLiteral("command"), stringProperty(QStringLiteral("The command://notation/... URI.")) },
                                  { QStringLiteral("params"), objectProperty(QStringLiteral("Optional command parameters.")) },
                              } },
                            { QStringLiteral("required"), QJsonArray{ QStringLiteral("command") } },
                            { QStringLiteral("additionalProperties"), false },
                        } },
                  } },
                { QStringLiteral("actionName"), stringProperty(QStringLiteral(
                                                                "Optional. What the undo stack should call this, e.g. \"Add a note to measures 3-6\". "
                                                                "The user sees this name, so make it describe the intent rather than the mechanism.")) },
                { QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                   "Optional. The revision you last read; the whole batch is refused if the "
                                                                   "score has changed since.")) },
            }, QJsonArray{ QStringLiteral("ops") }),
            toolPatchApply,
        },
    };
    return table;
}

const ToolSpec* muse::agentharness::findTool(const QString& name)
{
    for (const ToolSpec& spec : toolTable()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

QStringList muse::agentharness::toolNames()
{
    QStringList names;
    for (const ToolSpec& spec : toolTable()) {
        names.append(spec.name);
    }
    return names;
}
