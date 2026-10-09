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
#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include "agentharness/qml/MuseScore/AgentHarness/systemprompt.h"

using namespace muse::agentharness;

TEST(AgentHarness_SystemPrompt, PromptEstablishesTheThreeEssentials)
{
    const QString prompt = buildSystemPrompt();

    //! 1. Where it is, and that edits are real and undoable. An assistant that thinks it is drafting
    //!    something provisional will make changes the user did not ask for.
    EXPECT_TRUE(prompt.contains(QStringLiteral("MuseScore"))) << prompt.toStdString();
    EXPECT_TRUE(prompt.contains(QStringLiteral("Ctrl+Z"))) << "it must know the edits are undoable";

    //! 2. The addressing convention. Getting this wrong produces confident edits to the wrong bar.
    EXPECT_TRUE(prompt.contains(QStringLiteral("measure and beat"))) << prompt.toStdString();
    EXPECT_TRUE(prompt.contains(QStringLiteral("from 1"))) << "1-based must be stated explicitly";
    EXPECT_TRUE(prompt.contains(QStringLiteral("ticks"))) << "and it must be told not to compute ticks";

    //! 3. Read before write, and that a refusal is information.
    EXPECT_TRUE(prompt.contains(QStringLiteral("score_overview"))) << prompt.toStdString();
    EXPECT_TRUE(prompt.contains(QStringLiteral("refus"))) << "a refusal must not read as an obstacle";
}

TEST(AgentHarness_SystemPrompt, PromptDoesNotDuplicateToolDescriptions)
{
    //! Tool descriptions live in the request's `tools` array - the channel the model actually reads for
    //! capabilities. Restating them in prose creates two descriptions that can disagree, and the prose
    //! is the one nobody updates.
    const QString prompt = buildSystemPrompt();
    for (const ToolSpec& spec : toolTable()) {
        //! Naming a tool is fine (the prompt tells the model which to start with); restating its
        //! description is not.
        EXPECT_FALSE(prompt.contains(spec.description))
            << "the prompt restates the description of " << spec.name.toStdString();
    }
}

TEST(AgentHarness_SystemPrompt, ToolSchemasMirrorTheTable)
{
    const QJsonArray schemas = toolSchemas();
    const std::vector<ToolSpec>& table = toolTable();

    ASSERT_EQ(int(schemas.size()), int(table.size()));

    //! ⚠️ Order is part of the request envelope: a request that reorders its tools for no reason is a
    //! different request as far as any prefix cache is concerned. So the schema array must follow the
    //! table's order exactly, not just contain the same names.
    for (int i = 0; i < schemas.size(); ++i) {
        const QJsonObject tool = schemas[i].toObject();
        EXPECT_EQ(tool.value(QStringLiteral("type")).toString(), QStringLiteral("function"));

        const QJsonObject fn = tool.value(QStringLiteral("function")).toObject();
        EXPECT_EQ(fn.value(QStringLiteral("name")).toString(), table[size_t(i)].name);
        EXPECT_EQ(fn.value(QStringLiteral("description")).toString(), table[size_t(i)].description);
        EXPECT_TRUE(fn.value(QStringLiteral("parameters")).isObject());
    }
}

TEST(AgentHarness_SystemPrompt, EveryToolHasANameAndADescription)
{
    //! A tool with no description is one the model cannot choose correctly; a tool with no name cannot
    //! be called at all. Both are silent failures at the request level.
    for (const ToolSpec& spec : toolTable()) {
        EXPECT_FALSE(spec.name.isEmpty());
        EXPECT_FALSE(spec.description.isEmpty()) << spec.name.toStdString();
        EXPECT_TRUE(spec.parameters.value(QStringLiteral("type")).toString() == QStringLiteral("object"))
            << spec.name.toStdString();
        EXPECT_TRUE(spec.execute != nullptr) << spec.name.toStdString();
    }
}

TEST(AgentHarness_SystemPrompt, LookupByNameWorksAndUnknownNamesAreRejected)
{
    for (const ToolSpec& spec : toolTable()) {
        const ToolSpec* found = findTool(spec.name);
        ASSERT_TRUE(found != nullptr) << spec.name.toStdString();
        EXPECT_EQ(found->name, spec.name);
    }

    //! An unknown tool must be reported, not silently mapped to something else - a model that asks for
    //! a tool that does not exist should be told so plainly.
    EXPECT_TRUE(findTool(QStringLiteral("no_such_tool")) == nullptr);
}
