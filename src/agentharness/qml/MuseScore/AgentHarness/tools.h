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
struct ToolResult;

//! What a tool gets to work with. Deliberately narrow: a tool may read the score through the field
//! and may dispatch a notation command - nothing else. There is no filesystem here, no shell, and no
//! way to reach an engraving object directly, because "the agent can only do what the tool table
//! exposes" is a structural guarantee and structure is cheaper to keep than a promise.
struct ToolContext
{
    FieldController* field = nullptr;

    //! Call this instead of RETURNING, when the tool's work finishes after it returns.
    //!
    //! WHY A TOOL CAN BE ASYNCHRONOUS: `patch_apply` has to queue its transaction and its dispatches
    //! into the same FIFO to make a batch one undo step (see scoreactiongateway.h), and the dispatcher
    //! runs command handlers later, not at the point of the call. A tool that cannot finish
    //! synchronously must therefore be able to say so, rather than reporting an outcome it has not
    //! seen yet.
    //!
    //! The contract: a tool either returns a `ToolResult`, or it captures this and calls it later -
    //! never both, and exactly one of the two. `AgentLoop` passes a callback that records the tool
    //! result in the session log, so an async tool's result lands in the conversation in the same shape
    //! as a synchronous one.
    std::function<void(const ToolResult&)> complete;
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

//! Read: the score's revision number, for use as the `expectRevision` fence.
ToolResult toolScoreRevision(const QJsonObject& args, const ToolContext& ctx);

//! Write: perform several commands as ONE undo step, fenced by `expectRevision`.
//!
//! WHY BOTH IN ONE TOOL: the fence and the batch are the same idea seen twice. A batch says "these
//! writes are one instruction"; the fence says "and it was composed against the score as it is now".
//! Separating them would allow a batch composed against a stale read - which is precisely the case the
//! fence exists to catch.
ToolResult toolPatchApply(const QJsonObject& args, const ToolContext& ctx);

//! Write: set one note's pitch, addressed as measure/beat rather than by command URI.
//!
//! WHY THIS IS NOT A COMMAND: the command layer is the right shape when the action is "what the user
//! does" and the target is implied by the selection ("insert a measure"). It is the wrong shape when
//! the action NAMES a note - there is no command URI for "set the note at m3 b2 to C#5", and minting
//! one per recipe would be a table of near-duplicates. This goes through a recipe instead, which is
//! still an `UndoableCommand` on the same stack, so Ctrl+Z behaves identically.
ToolResult toolNoteSetPitch(const QJsonObject& args, const ToolContext& ctx);

//! Write: move one note by a number of semitones.
ToolResult toolNoteTranspose(const QJsonObject& args, const ToolContext& ctx);

//! Write: set the duration of the chord at an address. Takes a musician's name (`quarter`,
//! `dotted-eighth`), not a tick count - see scorerecipes.h for why.
ToolResult toolNoteSetDuration(const QJsonObject& args, const ToolContext& ctx);

//! Write: remove one note from the chord at an address. Refuses to empty a chord.
ToolResult toolNoteRemove(const QJsonObject& args, const ToolContext& ctx);

//! Write: add a note to the chord at an address.
ToolResult toolNoteAdd(const QJsonObject& args, const ToolContext& ctx);

//! Write: make the chord at an address have exactly a given set of pitches.
ToolResult toolChordSetPitches(const QJsonObject& args, const ToolContext& ctx);

//! Write: tie / untie / toggle the tie on the note at an address.
//!
//! WHY ONE TOOL WITH A MODE RATHER THAN THREE: the three are the same question asked three ways
//! ("make it tied", "make it untied", "flip it"), and a model that has to pick between three tool names
//! for one keystroke's worth of editing will sometimes pick the wrong one and then have to undo. The
//! mode is explicit and validated, so a wrong mode is a sentence rather than a wrong edit.
ToolResult toolNoteTie(const QJsonObject& args, const ToolContext& ctx);

//! Write: replace the chord at an address with a rest of the same duration - i.e. silence this beat.
ToolResult toolNoteToRest(const QJsonObject& args, const ToolContext& ctx);

//! Write: slur / unslur / toggle the slur starting at the note at an address.
//!
//! ⛔ A SLUR IS NOT A TIE, and the two tools have to say so from both sides. A tie joins two notes of
//! the SAME PITCH and changes how they sound; a slur joins ANY two notes and changes how they are
//! played. A model that conflates them produces a score that reads correctly and sounds wrong.
ToolResult toolNoteSlur(const QJsonObject& args, const ToolContext& ctx);

//! Read: list the operations that address a NOTE directly, rather than a command URI.
//!
//! WHY THIS TOOL HAS TO EXIST: the command tools are discoverable - `command_list` enumerates them, and
//! their URIs appear in `command_dispatch`'s description. The note recipes are not: a model reading the
//! tool descriptions would learn that it can set a pitch and a duration only if it worked that out from
//! the names. This is the one place that says what the recipe vocabulary is, so the model does not have
//! to infer it - and so a recipe added later is reachable without changing the prompt.
ToolResult toolNoteCapabilities(const QJsonObject& args, const ToolContext& ctx);

} // namespace muse::agentharness
