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
#include <QSet>
#include <QJsonValue>

#include "fieldcontroller.h"
#include "scoreactiongateway.h"
#include "scoredigest.h"
#include "scorerecipes.h"
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

QJsonObject boolProperty(const QString& description)
{
    QJsonObject p;
    p.insert(QStringLiteral("type"), QStringLiteral("boolean"));
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

namespace {
//! Read the address arguments shared by the note recipes.
//!
//! `staff` and `beat` default to 1 because that is what a caller who did not think about them means,
//! and because the tools' own descriptions say "staff 1 is the top staff" - so a missing value should
//! behave like the value a reader would assume. `measure` has no sensible default and is required by
//! the schema.
bool readAddress(const QJsonObject& args, ScoreAddress& out, QString& problem)
{
    int measure = 0;
    if (!readInt(args, QStringLiteral("measure"), measure)) {
        problem = QStringLiteral("`measure` is required (1-based)");
        return false;
    }
    if (measure < 1) {
        problem = QStringLiteral("`measure` is 1-based, so %1 is not a measure").arg(measure);
        return false;
    }

    int staff = 1;
    if (args.contains(QStringLiteral("staff")) && !readInt(args, QStringLiteral("staff"), staff)) {
        problem = QStringLiteral("`staff` must be an integer");
        return false;
    }
    if (staff < 1) {
        problem = QStringLiteral("`staff` is 1-based (staff 1 is the top staff), so %1 is not a staff")
                   .arg(staff);
        return false;
    }

    int beat = 1;
    if (args.contains(QStringLiteral("beat")) && !readInt(args, QStringLiteral("beat"), beat)) {
        problem = QStringLiteral("`beat` must be an integer");
        return false;
    }
    if (beat < 1) {
        problem = QStringLiteral("`beat` is 1-based, so %1 is not a beat").arg(beat);
        return false;
    }

    //! ⛔ The address struct stores `staff` 0-BASED (addressing.h), so the conversion happens HERE, at
    //! the boundary where the model's 1-based numbers arrive. Doing it in each recipe would be four
    //! places to get it wrong, and getting it wrong edits the wrong staff.
    out.measure = measure;
    out.staff = staff - 1;
    out.beat = beat;
    return true;
}

//! Read the optional voice/note selectors.
void readVoiceAndNote(const QJsonObject& args, int& voice, int& noteIndex)
{
    voice = 0;
    noteIndex = -1;
    readInt(args, QStringLiteral("voice"), voice);
    readInt(args, QStringLiteral("note"), noteIndex);
}

QJsonObject addressProperties()
{
    return {
        { QStringLiteral("measure"), intProperty(QStringLiteral("Measure number, 1-based.")) },
        { QStringLiteral("beat"), intProperty(QStringLiteral("Beat within the measure, 1-based. Defaults to 1.")) },
        { QStringLiteral("staff"), intProperty(QStringLiteral("Staff number, 1-based; staff 1 is the top staff. Defaults to 1.")) },
        { QStringLiteral("voice"), intProperty(QStringLiteral("Voice number, 1-based. Omit to use whichever voice has a note on this beat.")) },
        { QStringLiteral("note"), intProperty(QStringLiteral("Which note of the chord, 0-based from the lowest. Required when the beat holds more than one note.")) },
    };
}
} // namespace

ToolResult muse::agentharness::toolNoteSetPitch(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    int midiPitch = 0;
    if (!readInt(args, QStringLiteral("pitch"), midiPitch)) {
        return ToolResult::failure(QStringLiteral("`pitch` is required (a MIDI note number, 60 = middle C)"));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

    //! Same fence as the command tools, for the same reason - see the note in `toolCommandDispatch` on
    //! why it is optional rather than required.
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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Set pitch"), [&](mu::engraving::Score* score) {
        return setNotePitch(score, address, voice, noteIndex, midiPitch);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolNoteTranspose(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    int semitones = 0;
    if (!readInt(args, QStringLiteral("semitones"), semitones)) {
        return ToolResult::failure(QStringLiteral("`semitones` is required (use a negative number to go down)"));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Transpose note"), [&](mu::engraving::Score* score) {
        return transposeNote(score, address, voice, noteIndex, semitones);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolNoteSetDuration(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    const QString duration = args.value(QStringLiteral("duration")).toString();
    if (duration.isEmpty()) {
        return ToolResult::failure(QStringLiteral("`duration` is required; use one of: %1")
                                   .arg(durationNames().join(QStringLiteral(", "))));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Set duration"), [&](mu::engraving::Score* score) {
        return setChordDuration(score, address, voice, duration);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolNoteRemove(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Remove note"), [&](mu::engraving::Score* score) {
        return removeNote(score, address, voice, noteIndex);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolNoteAdd(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    int midiPitch = 0;
    if (!readInt(args, QStringLiteral("pitch"), midiPitch)) {
        return ToolResult::failure(QStringLiteral("`pitch` is required (a MIDI note number, 60 = middle C)"));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Add note to chord"), [&](mu::engraving::Score* score) {
        return addNoteToChord(score, address, voice, midiPitch);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolNoteTie(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    //! The mode is validated HERE rather than defaulted, so a typo is a sentence about the argument
    //! instead of an edit the caller did not intend.
    const QString mode = args.value(QStringLiteral("mode")).toString();
    if (mode != QLatin1String("add") && mode != QLatin1String("remove")
        && mode != QLatin1String("toggle")) {
        return ToolResult::failure(QStringLiteral("`mode` must be `add`, `remove` or `toggle` (got `%1`)")
                                   .arg(mode));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Tie"), [&](mu::engraving::Score* score) {
        if (mode == QLatin1String("add")) {
            return addTie(score, address, voice, noteIndex);
        }
        if (mode == QLatin1String("remove")) {
            return removeTie(score, address, voice, noteIndex);
        }
        return toggleTie(score, address, voice, noteIndex);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolNoteToRest(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Change to rest"), [&](mu::engraving::Score* score) {
        return changeToRest(score, address, voice);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolNoteSlur(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    const QString mode = args.value(QStringLiteral("mode")).toString();
    if (mode != QLatin1String("add") && mode != QLatin1String("remove")
        && mode != QLatin1String("toggle")) {
        return ToolResult::failure(QStringLiteral("`mode` must be `add`, `remove` or `toggle` (got `%1`)")
                                   .arg(mode));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Slur"), [&](mu::engraving::Score* score) {
        if (mode == QLatin1String("add")) {
            return addSlur(score, address, voice);
        }
        if (mode == QLatin1String("remove")) {
            return removeSlur(score, address, voice);
        }
        return toggleSlur(score, address, voice);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolChordSetPitches(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    const QJsonValue pitchesValue = args.value(QStringLiteral("pitches"));
    if (!pitchesValue.isArray()) {
        return ToolResult::failure(QStringLiteral("`pitches` is required and must be an array of MIDI "
                                                  "note numbers"));
    }

    QVector<int> pitches;
    for (const QJsonValue& v : pitchesValue.toArray()) {
        if (!v.isDouble()) {
            return ToolResult::failure(QStringLiteral("every entry of `pitches` must be a MIDI note "
                                                      "number"));
        }
        pitches.append(int(v.toDouble()));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Set chord pitches"), [&](mu::engraving::Score* score) {
        return setChordPitches(score, address, voice, pitches);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolTextAdd(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    const QString style = args.value(QStringLiteral("style")).toString();
    if (style.isEmpty()) {
        return ToolResult::failure(QStringLiteral("`style` is required. Use one of: %1")
                                   .arg(textStyleNames().join(QStringLiteral(", "))));
    }

    const QString text = args.value(QStringLiteral("text")).toString();

    //! ⛔ THE ADDRESS IS REQUIRED ONLY FOR THE ATTACHED STYLES, and saying so is the whole point of
    //! asking. `addText(title)` needs no position; `addText(rehearsal-mark)` does. A tool that demanded
    //! a measure for a title would make callers invent one, and one that accepted a missing measure for a
    //! rehearsal mark would reach the null dereference inside `Score::addText`.
    bool known = false;
    const bool needsAddress = textStyleNeedsAddress(style, known);
    if (!known) {
        return ToolResult::failure(QStringLiteral("`%1` is not a text style I know. Use one of: %2")
                                   .arg(style, textStyleNames().join(QStringLiteral(", "))));
    }

    ScoreAddress address;
    if (needsAddress) {
        QString problem;
        if (!readAddress(args, address, problem)) {
            return ToolResult::failure(QStringLiteral("%1 text attaches to a position, so `measure` is "
                                                      "required. %2").arg(style, problem));
        }
    }

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Add text"), [&](mu::engraving::Score* score) {
        return addText(score, address, style, text);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolKeySignature(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int measureNumber = 0;
    if (!readInt(args, QStringLiteral("measure"), measureNumber)) {
        return ToolResult::failure(QStringLiteral("`measure` is required and must be an integer"));
    }

    int fifths = 0;
    if (!readInt(args, QStringLiteral("fifths"), fifths)) {
        return ToolResult::failure(QStringLiteral("`fifths` is required: the number of sharps (positive) "
                                                  "or flats (negative), 0 for none"));
    }

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Set key signature"), [&](mu::engraving::Score* score) {
        return setKeySignature(score, measureNumber, fifths);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolTimeSignature(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int measureNumber = 0;
    if (!readInt(args, QStringLiteral("measure"), measureNumber)) {
        return ToolResult::failure(QStringLiteral("`measure` is required and must be an integer"));
    }

    int numerator = 0;
    int denominator = 0;
    if (!readInt(args, QStringLiteral("numerator"), numerator)
        || !readInt(args, QStringLiteral("denominator"), denominator)) {
        return ToolResult::failure(QStringLiteral("`numerator` and `denominator` are required, e.g. 3 and 4 "
                                                  "for 3/4"));
    }

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

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Set time signature"), [&](mu::engraving::Score* score) {
        return setTimeSignature(score, measureNumber, numerator, denominator);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
namespace {
//! The revision fence, in one place. Every write tool does the same thing with `expectRevision`, and
//! repeating it per tool is how one of them ends up checking it slightly differently.
//! Returns an empty string when the fence passes or was not asked for.
QString revisionRefusal(const QJsonObject& args, const ToolContext& ctx)
{
    if (!args.contains(QStringLiteral("expectRevision"))) {
        return QString();
    }
    int expected = 0;
    if (!readInt(args, QStringLiteral("expectRevision"), expected)) {
        return QStringLiteral("`expectRevision` must be an integer");
    }
    const int actual = ctx.field->scoreRevision();
    if (expected == actual) {
        return QString();
    }
    return QStringLiteral("the score has changed since you read it (you expected revision %1, it is now "
                          "%2), so this edit was NOT applied. Read the score again and redo the edit "
                          "against what is there now.").arg(expected).arg(actual);
}
} // namespace

ToolResult muse::agentharness::toolDynamicAdd(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    const QString mark = args.value(QStringLiteral("mark")).toString();
    if (mark.isEmpty()) {
        return ToolResult::failure(QStringLiteral("`mark` is required, e.g. `mf`"));
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Add dynamic"), [&](mu::engraving::Score* score) {
        return addDynamic(score, address, mark);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolHairpinAdd(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress from;
    QString problem;
    if (!readAddress(args, from, problem)) {
        return ToolResult::failure(problem);
    }

    //! The end is OPTIONAL: without it the hairpin runs to the next chord rest, which is what the palette
    //! does. Naming it is how a caller says "and stop there" instead of "and stop wherever".
    ScoreAddress to = from;
    if (args.contains(QStringLiteral("toMeasure")) || args.contains(QStringLiteral("toBeat"))) {
        if (!readInt(args, QStringLiteral("toMeasure"), to.measure)
            || !readInt(args, QStringLiteral("toBeat"), to.beat)) {
            return ToolResult::failure(QStringLiteral("`toMeasure` and `toBeat` must both be given, and be "
                                                      "integers"));
        }
        to.staff = from.staff;
    }

    const QString kind = args.value(QStringLiteral("kind")).toString();
    if (kind.isEmpty()) {
        return ToolResult::failure(QStringLiteral("`kind` is required: `crescendo` or `diminuendo`"));
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Add hairpin"), [&](mu::engraving::Score* score) {
        return addHairpin(score, from, to, kind);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolMeasureInsert(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int beforeMeasure = 0;
    if (!readInt(args, QStringLiteral("beforeMeasure"), beforeMeasure)) {
        return ToolResult::failure(QStringLiteral("`beforeMeasure` is required: the 1-based number to "
                                                  "insert before. Use one past the last measure to append."));
    }

    int count = 1;
    if (args.contains(QStringLiteral("count")) && !readInt(args, QStringLiteral("count"), count)) {
        return ToolResult::failure(QStringLiteral("`count` must be an integer"));
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Insert measures"), [&](mu::engraving::Score* score) {
        return insertMeasures(score, beforeMeasure, count);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolMeasureRemove(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int first = 0;
    if (!readInt(args, QStringLiteral("firstMeasure"), first)) {
        return ToolResult::failure(QStringLiteral("`firstMeasure` is required: the 1-based number of the "
                                                  "first measure to remove"));
    }

    //! `lastMeasure` defaults to `firstMeasure`, so removing one bar is one argument rather than two that
    //! have to agree.
    int last = first;
    if (args.contains(QStringLiteral("lastMeasure")) && !readInt(args, QStringLiteral("lastMeasure"), last)) {
        return ToolResult::failure(QStringLiteral("`lastMeasure` must be an integer"));
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Remove measures"), [&](mu::engraving::Score* score) {
        return removeMeasures(score, first, last);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolNoteMove(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress from;
    QString problem;
    if (!readAddress(args, from, problem)) {
        return ToolResult::failure(problem);
    }

    //! The target is read from `toMeasure`/`toBeat`/`toStaff` rather than a nested object, because the
    //! schema language used here has no clean way to describe one - and a flat pair of names is what a
    //! caller writes without thinking.
    ScoreAddress to;
    to.staff = from.staff;
    if (!readInt(args, QStringLiteral("toMeasure"), to.measure)) {
        return ToolResult::failure(QStringLiteral("`toMeasure` is required: the 1-based measure to move the "
                                                  "note to"));
    }
    if (!readInt(args, QStringLiteral("toBeat"), to.beat)) {
        return ToolResult::failure(QStringLiteral("`toBeat` is required: the 1-based beat to move the note "
                                                  "to"));
    }
    if (args.contains(QStringLiteral("toStaff")) && !readInt(args, QStringLiteral("toStaff"), to.staff)) {
        return ToolResult::failure(QStringLiteral("`toStaff` must be an integer"));
    }
    if (args.contains(QStringLiteral("toStaff"))) {
        --to.staff;   //!< 1-based at the boundary, 0-based inside - the same rule as `staff`
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Move note"), [&](mu::engraving::Score* score) {
        return moveNote(score, from, noteIndex, to);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolNoteArticulation(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    ScoreAddress address;
    QString problem;
    if (!readAddress(args, address, problem)) {
        return ToolResult::failure(problem);
    }

    const QString name = args.value(QStringLiteral("articulation")).toString();
    if (name.isEmpty()) {
        return ToolResult::failure(QStringLiteral("`articulation` is required, e.g. `articStaccatoAbove`"));
    }

    int voice = 0;
    int noteIndex = -1;
    readVoiceAndNote(args, voice, noteIndex);

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Articulation"), [&](mu::engraving::Score* score) {
        return toggleArticulation(score, address, voice, noteIndex, name);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
namespace {
//! The part index a staff tool was given, or a failure that says what the valid range is.
//! Shared by both directions, because "there is no part 3" must read the same either way.
bool readPartIndex(const QJsonObject& args, const ToolContext& ctx, int& partIndex, ToolResult& failure)
{
    if (!readInt(args, QStringLiteral("part"), partIndex)) {
        failure = ToolResult::failure(QStringLiteral("`part` is required: the 0-based index of the part"));
        return false;
    }

    mu::engraving::Score* score = ctx.field->currentScore();
    if (!score) {
        failure = ToolResult::failure(QStringLiteral("no score is open"));
        return false;
    }

    const int count = int(score->parts().size());
    if (partIndex < 0 || partIndex >= count) {
        //! ⚠️ The range is in the message. "No such part" without it leaves the caller guessing whether
        //! parts are 0-based, 1-based, or counted per staff.
        failure = ToolResult::failure(QStringLiteral("there is no part %1; the score has %2 part(s), "
                                                     "numbered 0 to %3")
                                      .arg(partIndex).arg(count).arg(count - 1));
        return false;
    }
    return true;
}
} // namespace

ToolResult muse::agentharness::toolStaffAdd(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int partIndex = 0;
    ToolResult failure;
    if (!readPartIndex(args, ctx, partIndex, failure)) {
        return failure;
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Add staff"), [&](mu::engraving::Score* score) {
        return appendStaff(score, partIndex);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}

ToolResult muse::agentharness::toolStaffRemove(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int partIndex = 0;
    ToolResult failure;
    if (!readPartIndex(args, ctx, partIndex, failure)) {
        return failure;
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Remove staff"), [&](mu::engraving::Score* score) {
        return removeLastStaff(score, partIndex);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolMeasureFill(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int measureNumber = 0;
    if (!readInt(args, QStringLiteral("measure"), measureNumber)) {
        return ToolResult::failure(QStringLiteral("`measure` is required and must be an integer"));
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Fill measure"), [&](mu::engraving::Score* score) {
        return fillMeasureWithRests(score, measureNumber);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolTempoSet(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int measureNumber = 0;
    if (!readInt(args, QStringLiteral("measure"), measureNumber)) {
        return ToolResult::failure(QStringLiteral("`measure` is required and must be an integer"));
    }

    const QJsonValue bpmValue = args.value(QStringLiteral("bpm"));
    if (!bpmValue.isDouble()) {
        return ToolResult::failure(QStringLiteral("`bpm` is required: the tempo in beats per minute"));
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    const double bpm = bpmValue.toDouble();
    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Set tempo"), [&](mu::engraving::Score* score) {
        return setTempo(score, measureNumber, bpm);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolTransposeRange(const QJsonObject& args, const ToolContext& ctx)
{
    if (!ctx.field) {
        return ToolResult::failure(QStringLiteral("no information field available"));
    }

    int fromMeasure = 0;
    int toMeasure = 0;
    int partIndex = 0;
    int semitones = 0;
    if (!readInt(args, QStringLiteral("fromMeasure"), fromMeasure)) {
        return ToolResult::failure(QStringLiteral("`fromMeasure` is required and must be an integer"));
    }
    if (!readInt(args, QStringLiteral("toMeasure"), toMeasure)) {
        return ToolResult::failure(QStringLiteral("`toMeasure` is required and must be an integer"));
    }
    if (!readInt(args, QStringLiteral("part"), partIndex)) {
        return ToolResult::failure(QStringLiteral("`part` is required: the 0-based index of the part"));
    }
    if (!readInt(args, QStringLiteral("semitones"), semitones)) {
        return ToolResult::failure(QStringLiteral("`semitones` is required and must be an integer"));
    }

    const QString refused = revisionRefusal(args, ctx);
    if (!refused.isEmpty()) {
        return ToolResult::failure(refused);
    }

    //! Optional, and FALSE when absent: moving the key signature is a separate decision from moving the
    //! notes, and a caller that did not ask for it must not get it. `false` also keeps every existing call
    //! site's meaning unchanged.
    const bool transposeKeySignature = args.value(QStringLiteral("transposeKeySignature")).toBool(false);

    const RecipeResult result = ctx.field->runNoteRecipe(
        QStringLiteral("Transpose range"), [&](mu::engraving::Score* score) {
        return transposeRange(score, fromMeasure, toMeasure, partIndex, semitones, transposeKeySignature);
    });

    if (!result.ok) {
        return ToolResult::failure(result.problem);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("revision"), ctx.field->scoreRevision());
    return ToolResult::success(result.detail, meta);
}
ToolResult muse::agentharness::toolNoteCapabilities(const QJsonObject&, const ToolContext&)
{
    //! ⛔⛔ THE LIST IS DERIVED FROM THE TOOL TABLE, NOT TYPED OUT.
    //!
    //! The first version was a hand-written list, and it went stale the moment `note_tie` was added:
    //! the tool existed, the model could call it, and this tool - the one place that says what the
    //! vocabulary is - did not mention it. Nothing failed; the capability was simply invisible.
    //!
    //! Deriving it means a new `note_*` tool appears here automatically. The invariant is enforced by a
    //! check rather than by remembering: see the drift guard below.
    QStringList lines;
    lines.append(QStringLiteral("Operations that address a note or chord by position "
                                "(measure/beat, both 1-based; `staff` defaults to 1; `note` is 0-based "
                                "from the lowest and is required when a beat holds several notes):"));
    lines.append(QString());

    for (const ToolSpec& spec : toolTable()) {
        if (!spec.name.startsWith(QLatin1String("note_")) || spec.name == QLatin1String("note_capabilities")) {
            continue;
        }

        //! The tool's own `description` is reused verbatim, which is deliberate: it is the same text the
        //! model already receives in the request's `tools` array, so the two cannot describe the same
        //! tool differently. Restating it here would be a second description to keep in sync - the
        //! exact mistake this function was rewritten to stop making.
        QString description = spec.description;
        description.replace(QLatin1Char('\n'), QLatin1Char(' '));

        lines.append(QStringLiteral("  %1").arg(spec.name));
        lines.append(QStringLiteral("      %1").arg(description));
        lines.append(QStringLiteral("      arguments: %1")
                     .arg(spec.parameters.value(QStringLiteral("properties")).toObject().keys()
                          .join(QStringLiteral(", "))));
    }

    lines.append(QString());
    lines.append(QStringLiteral("Duration names: %1 - optionally dotted, e.g. `dotted-quarter`, `quarter.`")
                 .arg(durationNames().join(QStringLiteral(", "))));
    lines.append(QString());
    lines.append(QStringLiteral("Every one of these takes an optional `expectRevision`: pass the number "
                                "`score_revision` gave you and the edit is refused if the score changed "
                                "first, instead of landing on a score you have not seen."));
    lines.append(QString());
    lines.append(QStringLiteral("For anything else, use command_list and command_dispatch: the score's own "
                                "editing commands (insert measures, rests, dynamics, and so on) are "
                                "reached that way. The two sets are complementary - a command URI cannot "
                                "name a note, and a note recipe cannot insert a measure."));

    return ToolResult::success(lines.join(QLatin1Char('\n')));
}
ToolResult muse::agentharness::toolScoreRevision(const QJsonObject&, const ToolContext& ctx){
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
        op.tool = obj.value(QStringLiteral("tool")).toString();
        op.params = obj.value(QStringLiteral("params")).toObject();

        //! ── A `tool` OP, RESOLVED TO THE TOOL ITSELF ──────────────────────────────────────────────
        //!
        //! ⛔⛔ WHY THIS EXISTS: without it `patch_apply` could not run any of the `note_*` recipes, and a
        //! batch is exactly how "change many notes in one undo step" is done - so the tools that most
        //! needed batching were the ones that could not use it. Measured: a batch naming `note_add` was
        //! refused twice, once for the wrong key and once because a tool name is not a `command://` URI.
        //!
        //! ⚠️ The op is resolved to the TOOL FUNCTION, not to the recipe underneath it, so a batched
        //! operation and a direct call are the same code: the same argument parsing, the same revision
        //! fence, the same refusals. Re-implementing seventeen tools' parsing here would have been two
        //! implementations of one contract, drifting apart a refusal at a time.
        if (!op.tool.isEmpty()) {
            const ToolSpec* spec = findTool(op.tool);
            if (!spec) {
                return ToolResult::failure(QStringLiteral("`%1` is not a tool. Nothing was applied.")
                                           .arg(op.tool));
            }
            if (!isBatchableTool(op.tool)) {
                //! ⛔ Refused rather than ignored. A read tool inside a batch is a caller that has
                //! misunderstood what a batch is, and silently doing nothing would let it carry on
                //! believing the result of that read was used.
                return ToolResult::failure(
                    QStringLiteral("`%1` cannot take part in a batch: only the write tools can. "
                                   "`patch_apply` itself and the read tools are not batchable. Nothing was "
                                   "applied.").arg(op.tool));
            }

            const std::function<ToolResult(const QJsonObject&, const ToolContext&)> call = spec->execute;
            const QJsonObject toolArgs = op.params;
            op.recipe = [call, toolArgs](const ToolContext& context) {
                return call(toolArgs, context);
            };
            ops.append(op);
            continue;
        }

        if (op.command.isEmpty()) {
            return ToolResult::failure(QStringLiteral(
                                           "every operation needs either a `command` (a "
                                           "`command://notation/...` URI) or a `tool` (the name of a write "
                                           "tool, with its arguments in `params`); one of them had "
                                           "neither. Nothing was applied."));
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
    gateway.performBatch(ops, actionName, ctx,
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
                           "measures, say - rather than calling the tools one at a time.\n"
                           "Each operation names EITHER a `command` (a command://notation/... URI, with "
                           "`params`) OR a `tool` (the name of a write tool such as `note_set_pitch`, with "
                           "its arguments in `params`). The two kinds can be mixed in one batch.\n"
                           "All or nothing: if any operation is refused, none of them are applied."),
            schemaObject({
                { QStringLiteral("ops"), QJsonObject{
                      { QStringLiteral("type"), QStringLiteral("array") },
                      { QStringLiteral("description"), QStringLiteral("The operations, in order.") },
                      { QStringLiteral("items"), QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("object") },
                            { QStringLiteral("properties"), QJsonObject{
                                  { QStringLiteral("command"), stringProperty(QStringLiteral("The command://notation/... URI. Give this OR `tool`.")) },
                                    { QStringLiteral("tool"), stringProperty(QStringLiteral("The name of a write tool, e.g. note_set_pitch. Give this OR `command`. Its arguments go in `params`.")) },
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
        ToolSpec{
            QStringLiteral("note_set_pitch"),
            QStringLiteral("Set the pitch of one note, addressed by measure and beat. The spelling "
                           "(sharp or flat) is chosen from the key signature, so you give a pitch and "
                           "the score writes it the way a musician would. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                props.insert(QStringLiteral("pitch"), intProperty(QStringLiteral(
                                                           "MIDI note number: 60 is middle C, 61 is C#, 69 is A440.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("pitch") }),
            toolNoteSetPitch,
        },
        ToolSpec{
            QStringLiteral("note_transpose"),
            QStringLiteral("Move one note up or down by a number of semitones, keeping its spelling where "
                           "the key allows. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                props.insert(QStringLiteral("semitones"), intProperty(QStringLiteral(
                                                              "How far to move the note; negative goes down. 12 is an octave.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("semitones") }),
            toolNoteTranspose,
        },
        ToolSpec{
            QStringLiteral("note_set_duration"),
            QStringLiteral("Set how long the beat at an address lasts, by name: `whole`, `half`, "
                           "`quarter`, `eighth`, `16th` … optionally dotted (`dotted-quarter`). All the "
                           "notes on that beat change together, because a chord's notes sound for the "
                           "same length. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                props.insert(QStringLiteral("duration"), stringProperty(QStringLiteral(
                                                                   "The duration name, optionally dotted: e.g. `quarter`, `dotted-eighth`.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("duration") }),
            toolNoteSetDuration,
        },
        ToolSpec{
            QStringLiteral("note_remove"),
            QStringLiteral("Remove one note from the chord at an address, for turning a chord into a "
                           "single note. Refuses when it is the chord's only note - silencing a beat "
                           "means replacing it with a rest, which is a different operation. Undoable "
                           "with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure") }),
            toolNoteRemove,
        },
        ToolSpec{
            QStringLiteral("note_add"),
            QStringLiteral("Add a note to the chord at an address, making a single note into a chord. "
                           "Refuses a pitch the chord already has. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                props.insert(QStringLiteral("pitch"), intProperty(QStringLiteral(
                                                           "MIDI note number: 60 is middle C, 64 is E, 67 is G.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("pitch") }),
            toolNoteAdd,
        },
        ToolSpec{
            QStringLiteral("chord_set_pitches"),
            QStringLiteral("Make the chord at an address have exactly these pitches - e.g. turn a single "
                           "note into a C major triad in one call. Use this instead of repeated "
                           "note_add / note_remove when replacing a chord. Refuses an empty list (that "
                           "is note_to_rest) and a repeated pitch. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                QJsonObject pitchesProp;
                pitchesProp.insert(QStringLiteral("type"), QStringLiteral("array"));
                pitchesProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                      "The MIDI note numbers the chord should end up with, e.g. [60, 64, 67] for a C "
                                                                      "major triad. Order does not matter."));
                QJsonObject items;
                items.insert(QStringLiteral("type"), QStringLiteral("integer"));
                pitchesProp.insert(QStringLiteral("items"), items);
                props.insert(QStringLiteral("pitches"), pitchesProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("pitches") }),
            toolChordSetPitches,
        },
        ToolSpec{
            QStringLiteral("text_add"),
            QStringLiteral("Add text to the score: a title, subtitle, composer, lyricist, rehearsal "
                           "mark, system text, staff text or expression. The frame styles (title, "
                           "subtitle, composer, lyricist) need no position; the others attach to a beat, "
                           "so `measure` is required for them. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                //! The address is optional HERE even though `addressProperties()` marks measure as
                //! required - a title has no position. Which styles need it is stated in the
                //! description and enforced by the tool, which is more useful than a schema that
                //! demands a measure for a title.
                props.insert(QStringLiteral("measure"), intProperty(QStringLiteral(
                                                              "Required for the attached styles (rehearsal-mark, system, staff, expression); "
                                                              "not used by title, subtitle, composer or lyricist.")));
                QJsonObject styleProp;
                styleProp.insert(QStringLiteral("type"), QStringLiteral("string"));
                styleProp.insert(QStringLiteral("enum"), QJsonArray::fromStringList(textStyleNames()));
                styleProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                   "Which kind of text. The frame styles need no position; the rest attach to a beat."));
                props.insert(QStringLiteral("style"), styleProp);
                QJsonObject textProp;
                textProp.insert(QStringLiteral("type"), QStringLiteral("string"));
                textProp.insert(QStringLiteral("description"), QStringLiteral("The text itself."));
                props.insert(QStringLiteral("text"), textProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("style"), QStringLiteral("text") }),
            toolTextAdd,
        },
        ToolSpec{
            QStringLiteral("key_signature_set"),
            QStringLiteral("Set the key signature at a measure, as a number of sharps (positive) or flats "
                           "(negative). 0 is no accidentals. Applies from that measure on, to every "
                           "linked staff. Undoable with Ctrl+Z. (There is no command for this - use this "
                           "tool, not command_dispatch.)"),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("measure"), intProperty(QStringLiteral(
                                                              "1-based measure number where the signature starts.")));
                QJsonObject fifthsProp;
                fifthsProp.insert(QStringLiteral("type"), QStringLiteral("integer"));
                fifthsProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                     "-7 (seven flats) to 7 (seven sharps). 0 = no sharps or flats. Positive = "
                                                                     "sharps, e.g. 2 for D major / B minor."));
                props.insert(QStringLiteral("fifths"), fifthsProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("fifths") }),
            toolKeySignature,
        },
        ToolSpec{
            QStringLiteral("time_signature_set"),
            QStringLiteral("Set the time signature at a measure, e.g. 3/4. The measures from there on are "
                           "rewritten to the new signature, and if that cannot be done nothing is "
                           "changed. Undoable with Ctrl+Z. (There is no command for this - use this tool, "
                           "not command_dispatch.)"),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("measure"), intProperty(QStringLiteral(
                                                              "1-based measure number where the signature starts.")));
                props.insert(QStringLiteral("numerator"), intProperty(QStringLiteral(
                                                                   "The upper number, e.g. 3 for 3/4.")));
                props.insert(QStringLiteral("denominator"), intProperty(QStringLiteral(
                                                                     "The lower number, e.g. 4 for 3/4. Must be a power of two.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("numerator"),
                             QStringLiteral("denominator") }),
            toolTimeSignature,
        },
        ToolSpec{
            QStringLiteral("dynamic_add"),
            QStringLiteral("Add a dynamic marking (`pp`, `mf`, `ff`, `sfz`, ...) at a beat. The marking is "
                           "parsed by the score format's own parser, so anything the format accepts works. "
                           "Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                QJsonObject markProp;
                markProp.insert(QStringLiteral("type"), QStringLiteral("string"));
                markProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                   "The marking as written, e.g. `mf`. Not a number - the score stores the marking "
                                                                   "itself, not a loudness."));
                props.insert(QStringLiteral("mark"), markProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("mark") }),
            toolDynamicAdd,
        },
        ToolSpec{
            QStringLiteral("hairpin_add"),
            QStringLiteral("Add a crescendo or diminuendo hairpin starting at a beat. Without an end it "
                           "runs to the next beat; give `toMeasure`/`toBeat` to say where it stops. "
                           "Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                QJsonObject kindProp;
                kindProp.insert(QStringLiteral("type"), QStringLiteral("string"));
                kindProp.insert(QStringLiteral("enum"), QJsonArray{ QStringLiteral("crescendo"),
                                                                    QStringLiteral("diminuendo") });
                props.insert(QStringLiteral("kind"), kindProp);
                props.insert(QStringLiteral("toMeasure"), intProperty(QStringLiteral(
                                                                  "Optional. 1-based measure where the hairpin ends. Omit to run to the next beat.")));
                props.insert(QStringLiteral("toBeat"), intProperty(QStringLiteral(
                                                               "Optional. 1-based beat where the hairpin ends. Give it together with `toMeasure`.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("kind") }),
            toolHairpinAdd,
        },
        ToolSpec{
            QStringLiteral("measure_insert"),
            QStringLiteral("Insert one or more measures before a given measure. Use one past the last "
                           "measure to append at the end. Measure numbers from there on shift, so re-read "
                           "the score afterwards. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("beforeMeasure"), intProperty(QStringLiteral(
                                                                     "1-based measure to insert before. One past the last measure appends at the end.")));
                props.insert(QStringLiteral("count"), intProperty(QStringLiteral(
                                                              "Optional. How many measures to insert. Defaults to 1.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("beforeMeasure") }),
            toolMeasureInsert,
        },
        ToolSpec{
            QStringLiteral("measure_remove"),
            QStringLiteral("Remove a measure or a range of measures. Refused if it would leave the score "
                           "with no measures. Measure numbers after the range shift back, so re-read the "
                           "score afterwards. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("firstMeasure"), intProperty(QStringLiteral(
                                                                    "1-based number of the first measure to remove.")));
                props.insert(QStringLiteral("lastMeasure"), intProperty(QStringLiteral(
                                                                   "Optional. 1-based number of the last measure to remove. Defaults to "
                                                                   "`firstMeasure`, i.e. remove just that one.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("firstMeasure") }),
            toolMeasureRemove,
        },
        ToolSpec{
            QStringLiteral("note_move"),
            QStringLiteral("Move one note's pitch from one beat to another - e.g. take the C4 on beat 1 and "
                           "put it on beat 3. Refused if the destination already has that pitch, if it is "
                           "a rest, or if the source holds only that one note (use note_to_rest then add "
                           "it). Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                props.insert(QStringLiteral("toMeasure"), intProperty(QStringLiteral(
                                                                  "1-based measure to move the note to.")));
                props.insert(QStringLiteral("toBeat"), intProperty(QStringLiteral(
                                                               "1-based beat to move the note to.")));
                props.insert(QStringLiteral("toStaff"), intProperty(QStringLiteral(
                                                               "Optional. 1-based staff to move the note to. Defaults to the same staff.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("toMeasure"),
                             QStringLiteral("toBeat") }),
            toolNoteMove,
        },
        ToolSpec{
            QStringLiteral("note_articulation"),
            QStringLiteral("Add an articulation (staccato, accent, tenuto, marcato, ...) to a note, or "
                           "remove it if it is already there. The names are the score format's own SMuFL "
                           "names. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                QJsonObject articProp;
                articProp.insert(QStringLiteral("type"), QStringLiteral("string"));
                articProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                   "The SMuFL symbol name, e.g. `articStaccatoAbove`, `articAccentAbove`, "
                                                                   "`articTenutoAbove`, `articMarcatoAbove`, `articStaccatissimoAbove`. "
                                                                   "`Above` or `Below` picks the side."));
                props.insert(QStringLiteral("articulation"), articProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("articulation") }),
            toolNoteArticulation,
        },
        ToolSpec{
            QStringLiteral("staff_add"),
            QStringLiteral("Add a staff to a part - e.g. give a piano part a third staff. The new staff gets "
                           "the part's key signature, joins its brace and barline group, and gets its own "
                           "clef. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("part"), intProperty(QStringLiteral(
                                                             "0-based index of the part, in the order the parts appear in the score.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("part") }),
            toolStaffAdd,
        },
        ToolSpec{
            QStringLiteral("staff_remove"),
            QStringLiteral("Remove the last staff of a part. Refused when it is the part's only staff, "
                           "because a part cannot have none. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("part"), intProperty(QStringLiteral(
                                                             "0-based index of the part, in the order the parts appear in the score.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("part") }),
            toolStaffRemove,
        },
        ToolSpec{
            QStringLiteral("measure_fill"),
            QStringLiteral("Fill the EMPTY SPACE in a measure with rests, so the bar is as long as its "
                           "time signature says. Use it after content was removed and left a hole, or "
                           "after a beat was shortened. It fills every uncovered stretch of the bar, "
                           "wherever it is - it does not lengthen a note, so a note that is too short "
                           "stays that length and a rest is inserted after it. Refused when the bar "
                           "already holds more than it can. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("measure"), intProperty(QStringLiteral(
                                                              "1-based measure number to complete.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure") }),
            toolMeasureFill,
        },
        ToolSpec{
            QStringLiteral("tempo_set"),
            QStringLiteral("Set the tempo, in beats per minute, at a measure. NOTE: tempo is not a notation "
                           "element - it lives in the score's automation data, so this changes how the "
                           "score PLAYS without changing how it LOOKS. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("measure"), intProperty(QStringLiteral(
                                                              "1-based measure where the tempo takes effect.")));
                QJsonObject bpmProp;
                bpmProp.insert(QStringLiteral("type"), QStringLiteral("number"));
                bpmProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                  "The tempo in beats per minute, e.g. 120. Refused outside the range the "
                                                                  "score can hold."));
                props.insert(QStringLiteral("bpm"), bpmProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("bpm") }),
            toolTempoSet,
        },
        ToolSpec{
            QStringLiteral("transpose_range"),
            QStringLiteral("Transpose every note in a range of measures by a number of semitones, in one "
                           "part. By default this moves the NOTES only and leaves the key signature "
                           "alone; set `transposeKeySignature` to true to move the key with them (the key "
                           "in force after the range is written back, so measures outside the range keep "
                           "their key). A whole-octave interval never moves the key. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props;
                props.insert(QStringLiteral("fromMeasure"), intProperty(QStringLiteral(
                                                                   "1-based first measure of the range.")));
                props.insert(QStringLiteral("toMeasure"), intProperty(QStringLiteral(
                                                                 "1-based last measure of the range, inclusive.")));
                props.insert(QStringLiteral("part"), intProperty(QStringLiteral(
                                                             "0-based index of the part whose staves are transposed.")));
                props.insert(QStringLiteral("semitones"), intProperty(QStringLiteral(
                                                               "How many semitones to move, negative for down. Refused beyond four "
                                                               "octaves.")));
                props.insert(QStringLiteral("transposeKeySignature"), boolProperty(QStringLiteral(
                                                                           "Optional, default false. When true, the key signature moves by the same "
                                                                           "interval as the notes.")));
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("fromMeasure"), QStringLiteral("toMeasure"),
                             QStringLiteral("part"), QStringLiteral("semitones") }),
            toolTransposeRange,
        },
        ToolSpec{
            QStringLiteral("note_tie"),
            QStringLiteral("Tie the note at an address to the next note of the SAME PITCH, remove that "
                           "tie, or toggle it. A tie changes how the notes SOUND (one longer note); a "
                           "slur changes how they are played. Use note_slur for a slur. Undoable with "
                           "Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                QJsonObject modeProp;
                modeProp.insert(QStringLiteral("type"), QStringLiteral("string"));
                modeProp.insert(QStringLiteral("enum"), QJsonArray{ QStringLiteral("add"),
                                                                    QStringLiteral("remove"),
                                                                    QStringLiteral("toggle") });
                modeProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                   "`add` ties the note to the next one of the same pitch; `remove` takes that tie "
                                                                   "away; `toggle` does whichever is not already the case."));
                props.insert(QStringLiteral("mode"), modeProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("mode") }),
            toolNoteTie,
        },
        ToolSpec{
            QStringLiteral("note_to_rest"),
            QStringLiteral("Replace the beat at an address with a rest of the same length - i.e. silence "
                           "it. This is how you remove the last note of a chord: note_remove refuses that "
                           "case, because an empty chord is not a rest. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure") }),
            toolNoteToRest,
        },
        ToolSpec{
            QStringLiteral("note_slur"),
            QStringLiteral("Slur the note at an address to the next note, remove that slur, or toggle "
                           "it. A slur joins ANY two notes and changes how they are played (legato); a "
                           "tie joins two notes of the SAME pitch and changes how they sound. Use "
                           "note_tie for a tie. Undoable with Ctrl+Z."),
            schemaObject([&] {
                QJsonObject props = addressProperties();
                QJsonObject modeProp;
                modeProp.insert(QStringLiteral("type"), QStringLiteral("string"));
                modeProp.insert(QStringLiteral("enum"), QJsonArray{ QStringLiteral("add"),
                                                                    QStringLiteral("remove"),
                                                                    QStringLiteral("toggle") });
                modeProp.insert(QStringLiteral("description"), QStringLiteral(
                                                                   "`add` slurs to the next note; `remove` takes that slur away; `toggle` does "
                                                                   "whichever is not already the case."));
                props.insert(QStringLiteral("mode"), modeProp);
                props.insert(QStringLiteral("expectRevision"), intProperty(QStringLiteral(
                                                                       "Optional. The revision you last read; refused if the score changed since.")));
                return props;
            }(), QJsonArray{ QStringLiteral("measure"), QStringLiteral("mode") }),
            toolNoteSlur,
        },
        ToolSpec{
            QStringLiteral("note_capabilities"),
            QStringLiteral("List the operations that address a note or chord by position, with their "
                           "arguments. Call this when you need to change a specific note - the command "
                           "tools cover everything else."),
            schemaObject({}),
            toolNoteCapabilities,
        },
    };
    return table;
}

bool muse::agentharness::isBatchableTool(const QString& name)
{
    //! ⛔⛔ THE LIST IS EXPLICIT AND SHORT ON PURPOSE, and the alternative was tried and rejected: marking
    //! each `ToolSpec` with a "batchable" flag would put the answer in seventeen places, where one of them
    //! would eventually be wrong - and the wrongness would show up as a batch that silently skipped an
    //! operation.
    //!
    //! ⚠️ What is NOT here matters as much as what is:
    //!   - the READ tools (`score_overview`, `score_window`, ...) - a batch is a write, and a read inside
    //!     one is a caller that has misunderstood what a batch is;
    //!   - `command_dispatch` - it would run a command OUTSIDE the batch's merge window, so it would not
    //!     be part of the single undo step the caller asked for;
    //!   - `patch_apply` - nesting a batch inside a batch has no meaning for the undo stack;
    //!   - `command_list` / `note_capabilities` / `score_revision` - reads.
    static const QSet<QString> kBatchable = {
        //! note-level
        QStringLiteral("note_set_pitch"), QStringLiteral("note_transpose"),
        QStringLiteral("note_set_duration"), QStringLiteral("note_add"),
        QStringLiteral("note_remove"), QStringLiteral("note_to_rest"),
        QStringLiteral("note_tie"), QStringLiteral("note_slur"),
        QStringLiteral("note_move"), QStringLiteral("chord_set_pitches"),
        QStringLiteral("note_articulation"),
        //! notation elements
        QStringLiteral("text_add"), QStringLiteral("dynamic_add"),
        QStringLiteral("hairpin_add"),
        //! parts and staves
        QStringLiteral("staff_add"), QStringLiteral("staff_remove"),
        //! signatures and structure
        QStringLiteral("key_signature_set"), QStringLiteral("time_signature_set"),
        QStringLiteral("measure_insert"), QStringLiteral("measure_remove"),
        QStringLiteral("measure_fill"),
        QStringLiteral("tempo_set"), QStringLiteral("transpose_range"),
    };
    return kBatchable.contains(name);
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
