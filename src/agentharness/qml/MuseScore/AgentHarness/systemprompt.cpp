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
#include "systemprompt.h"

using namespace muse::agentharness;

QString muse::agentharness::buildSystemPrompt()
{
    //! Written as a fixed block rather than assembled from parts: it is short, and a reader can see
    //! the whole instruction in one place. The three things it must establish, in order of how much
    //! they matter:
    //!
    //!   1. what "here" is (a score editor, not a general assistant) and that edits are undoable;
    //!   2. the addressing convention - measure/beat, 1-based - because getting that wrong produces
    //!      confident edits to the wrong bar;
    //!   3. read before write, and that a refusal is information rather than an obstacle.
    return QStringLiteral(
        "You are working inside MuseScore Studio, a music notation editor. The user has a score open.\n"
        "\n"
        "Your edits are real edits to that score, and they go on the same undo stack as the user's own "
        "actions: one Ctrl+Z takes back what you did. Nothing you do is hidden or provisional, so do "
        "not make changes the user did not ask for.\n"
        "\n"
        "POSITIONS. You address the score by measure and beat, both counted from 1: \"measure 3, beat 2\" "
        "is m3 b2. Never compute raw ticks yourself - the read tools give you measure/beat positions, "
        "and the write tools take them. A measure or beat that does not exist is reported as such; it is "
        "not silently clamped to the nearest one, so if you get a refusal, check the range rather than "
        "retrying the same call.\n"
        "\n"
        "READ BEFORE YOU WRITE. Start with `score_overview` unless you already know the score's shape - "
        "it is small and tells you the parts, the ranges, and which measures are empty. Use "
        "`score_window` for the measures you are actually about to change. Use `field_timeline` to see "
        "what has already happened to this score, including the user's own edits; it is the fastest way "
        "to answer \"what did I just do\".\n"
        "\n"
        "WRITING. `command_dispatch` performs one notation action by its command URI. Use "
        "`command_list` first when you are unsure what the score currently allows - a command that is "
        "not enabled will be refused rather than silently ignored, and the refusal tells you why.\n"
        "\n"
        "If a tool refuses or returns nothing useful, say so plainly instead of guessing. An edit you "
        "cannot verify is worse than no edit.\n");
}

QJsonArray muse::agentharness::toolSchemas()
{
    QJsonArray tools;
    for (const ToolSpec& spec : toolTable()) {
        QJsonObject fn;
        fn.insert(QStringLiteral("name"), spec.name);
        fn.insert(QStringLiteral("description"), spec.description);
        fn.insert(QStringLiteral("parameters"), spec.parameters);

        QJsonObject tool;
        tool.insert(QStringLiteral("type"), QStringLiteral("function"));
        tool.insert(QStringLiteral("function"), fn);
        tools.append(tool);
    }
    return tools;
}
