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

#include <functional>
#include <vector>

namespace muse::agentharness {
class FieldController;

//! What a tool gets to work with. Deliberately narrow: a tool may read the score through the field
//! and may dispatch a notation command - nothing else. There is no filesystem here, no shell, and no
//! way to reach an engraving object directly, because "the agent can only do what the tool table
//! exposes" is a structural guarantee and structure is cheaper to keep than a promise.
struct ToolContext
{
    FieldController* field = nullptr;
};

//! The result of one tool call.
//!
//! Mirrors the DSH contract (技术设计 §4.4) in the parts that matter here:
//!   - `ok == false` means the call failed (bad arguments, unknown tool, refused precondition);
//!   - a *successful* call whose outcome is uninteresting ("nothing to do") is still `ok == true`,
//!     and says so in `text`. Folding "I refused" into "it failed" would make the model retry
//!     blindly; keeping them apart lets it read why.
//!   - `meta` carries structured facts for the caller (and, later, the session log). The model sees
//!     `text`; nothing else.
struct ToolResult
{
    bool ok = true;
    QString text;
    QJsonObject meta;

    static ToolResult success(const QString& text, const QJsonObject& meta = QJsonObject())
    {
        ToolResult r;
        r.ok = true;
        r.text = text;
        r.meta = meta;
        return r;
    }

    static ToolResult failure(const QString& text, const QJsonObject& meta = QJsonObject())
    {
        ToolResult r;
        r.ok = false;
        r.text = text;
        r.meta = meta;
        return r;
    }
};

//! One entry in the tool table.
//!
//! `parameters` is the JSON Schema handed to the model. It is *not* validated here yet - the M2
//! registry will do that centrally, exactly as DSH does, so that a tool body only ever has to check
//! the constraints a schema cannot express. Until then each tool checks what it needs and says so.
struct ToolSpec
{
    QString name;
    QString description;
    QJsonObject parameters;
    std::function<ToolResult(const QJsonObject& args, const ToolContext& ctx)> execute;
};

//! The tool table. Built once; every entry is a `ToolSpec`.
//!
//! WHY A TABLE AND NOT FREE FUNCTIONS: this list is what the model is *told it can do*, so it has to
//! be enumerable in one place - for the prompt, for the panel, and for "which tools exist" to be a
//! question with one answer. Adding a capability means adding a row here and nowhere else.
const std::vector<ToolSpec>& toolTable();

//! Look up one tool by name. Null when there is no such tool - the caller reports that rather than
//! guessing, because a model that asks for a tool that does not exist should be told so plainly.
const ToolSpec* findTool(const QString& name);

//! Every tool name, for diagnostics.
QStringList toolNames();

//! ── Tool implementations ─────────────────────────────────────────────────────────────────────

//! Read: bounded summary of the score (the digest's overview size).
ToolResult toolScoreOverview(const QJsonObject& args, const ToolContext& ctx);

//! Read: one measure range of the score.
ToolResult toolScoreWindow(const QJsonObject& args, const ToolContext& ctx);

//! Read: the information field's operation timeline.
ToolResult toolFieldTimeline(const QJsonObject& args, const ToolContext& ctx);

//! Write: dispatch one notation command by its `command://notation/...` URI.
//!
//! ⛔ THE GATE MATTERS MORE THAN THE DISPATCH. A disabled command is *silently skipped* by the
//! notation controller's outer wrapper (`维护手册.md` §4.8, 第 594 条: "the command was called, the log
//! shows a dispatch, and nothing happened"). So this tool asks `ICommandsState` first and refuses
//! with a reason rather than dispatching into the void - an agent that gets "not enabled right now"
//! can do something else; an agent that gets silence will retry the same thing forever.
ToolResult toolCommandDispatch(const QJsonObject& args, const ToolContext& ctx);

//! Write: list the notation commands that are enabled right now, so the agent can choose one.
ToolResult toolCommandList(const QJsonObject& args, const ToolContext& ctx);

} // namespace muse::agentharness
