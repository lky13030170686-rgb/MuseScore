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

    //! ⛔ The gate, and the whole reason this tool is more than a one-line forwarder: a disabled
    //! command is silently skipped by the notation controller's outer wrapper, so dispatching
    //! blindly produces "the call succeeded and nothing happened" - the exact failure this project
    //! has already paid for twice (维护手册.md §4.8, 第 594 条).
    if (!ctx.field->isCommandEnabled(command)) {
        return ToolResult::failure(
            QStringLiteral("command `%1` is not enabled right now, so dispatching it would be silently "
                           "ignored. Use command_list to see what is available.").arg(command));
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
            }, QJsonArray{ QStringLiteral("command") }),
            toolCommandDispatch,
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
