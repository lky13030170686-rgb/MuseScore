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

#include "agentharness/qml/MuseScore/AgentHarness/modelcatalog.h"

using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS. The catalog is what the user is OFFERED, and it now survives a restart - so a
//! wrong answer here outlives the session that produced it. Two failure modes are invisible from the
//! outside: a catalog that is stored under the wrong gateway (the list of one endpoint shown for another),
//! and a catalog that silently loses entries on a round trip. Both are pinned below.

namespace {
QHash<QString, ModelCatalog> oneGateway(const QString& url, const QStringList& ids, const QString& at)
{
    ModelCatalog catalog;
    catalog.ids = ids;
    catalog.fetchedAt = at;
    return withCatalog({}, url, catalog);
}
} // namespace

TEST(AgentHarness_ModelCatalog, RoundTripsThroughTheStoredString)
{
    //! The two directions must agree, or a restart quietly changes the list.
    const QHash<QString, ModelCatalog> before = oneGateway(QStringLiteral("http://127.0.0.1:10100/v1"),
                                                           { QStringLiteral("b"), QStringLiteral("a") },
                                                           QStringLiteral("2026-10-11T14:00:00Z"));

    QString problem;
    const QHash<QString, ModelCatalog> after = parseModelCatalogs(serializeModelCatalogs(before), &problem);

    EXPECT_TRUE(problem.isEmpty()) << problem.toStdString();
    const ModelCatalog catalog = catalogFor(after, QStringLiteral("http://127.0.0.1:10100/v1"));
    ASSERT_EQ(catalog.ids.size(), 2);
    //! ⚠️ Order is kept: a gateway may rank its list, and re-sorting it would hide that.
    EXPECT_EQ(catalog.ids[0], QStringLiteral("b"));
    EXPECT_EQ(catalog.ids[1], QStringLiteral("a"));
    EXPECT_EQ(catalog.fetchedAt, QStringLiteral("2026-10-11T14:00:00Z"));
}

TEST(AgentHarness_ModelCatalog, AnEmptyStringIsNotAProblem)
{
    //! A fresh installation has no catalogs, and that must not read as a failure - otherwise every new user
    //! sees an error line about a file they never wrote.
    QString problem;
    EXPECT_TRUE(parseModelCatalogs(QString(), &problem).isEmpty());
    EXPECT_TRUE(problem.isEmpty());
    EXPECT_TRUE(parseModelCatalogs(QStringLiteral("   "), &problem).isEmpty());
    EXPECT_TRUE(problem.isEmpty());
}

TEST(AgentHarness_ModelCatalog, UnreadableStorageIsReportedNotGuessed)
{
    //! Half-parsed storage would show one gateway's models as if the list were complete.
    QString problem;
    EXPECT_TRUE(parseModelCatalogs(QStringLiteral("{not json"), &problem).isEmpty());
    EXPECT_FALSE(problem.isEmpty());

    problem.clear();
    EXPECT_TRUE(parseModelCatalogs(QStringLiteral("[1,2,3]"), &problem).isEmpty());
    EXPECT_FALSE(problem.isEmpty()) << "an array is not a map of gateways";
}

TEST(AgentHarness_ModelCatalog, AGatewayWithoutIdsIsNotStored)
{
    //! "This gateway serves nothing" and "we have no list for this gateway" are different facts, and only
    //! one of them is a catalog.
    const QHash<QString, ModelCatalog> catalogs = oneGateway(QStringLiteral("http://gw/v1"), {}, QString());
    EXPECT_TRUE(catalogs.isEmpty());

    QString problem;
    const QHash<QString, ModelCatalog> parsed = parseModelCatalogs(
        QStringLiteral(R"({"http://gw/v1":{"ids":[],"fetchedAt":"x"}})"), &problem);
    EXPECT_TRUE(problem.isEmpty()) << problem.toStdString();
    EXPECT_TRUE(parsed.isEmpty());
}

TEST(AgentHarness_ModelCatalog, EachGatewayKeepsItsOwnList)
{
    //! The whole reason this is a map: a model id is the GATEWAY's vocabulary, and the list of one endpoint
    //! says nothing about another.
    QHash<QString, ModelCatalog> catalogs = oneGateway(QStringLiteral("https://api.deepseek.com"), { QStringLiteral("deepseek-chat") }, QStringLiteral("t1"));
    catalogs = withCatalog(catalogs, QStringLiteral("http://127.0.0.1:10100/v1"), { { QStringLiteral("opencode-go/x") }, QStringLiteral("t2") });

    EXPECT_EQ(catalogFor(catalogs, QStringLiteral("https://api.deepseek.com")).ids.size(), 1);
    EXPECT_EQ(catalogFor(catalogs, QStringLiteral("http://127.0.0.1:10100/v1")).ids.size(), 1);
    //! A gateway nobody fetched from answers with nothing - not with somebody else's list.
    EXPECT_TRUE(catalogFor(catalogs, QStringLiteral("http://other/v1")).isEmpty());
}

TEST(AgentHarness_ModelCatalog, TheSameEndpointInDifferentCaseIsOneEntry)
{
    //! A user who retypes the same address with different capitalisation means the same endpoint; keeping
    //! two entries would be two answers to "what does this gateway serve".
    QHash<QString, ModelCatalog> catalogs = oneGateway(QStringLiteral("HTTP://127.0.0.1:10100/V1"), { QStringLiteral("old") }, QStringLiteral("t1"));
    catalogs = withCatalog(catalogs, QStringLiteral("http://127.0.0.1:10100/v1"), { { QStringLiteral("new") }, QStringLiteral("t2") });

    EXPECT_EQ(catalogs.size(), 1) << "one endpoint, one catalog";
    EXPECT_EQ(catalogFor(catalogs, QStringLiteral("http://127.0.0.1:10100/v1")).ids[0], QStringLiteral("new"));
    //! And the lookup itself is case-insensitive in both directions.
    EXPECT_EQ(catalogFor(catalogs, QStringLiteral("HTTP://127.0.0.1:10100/v1")).ids[0], QStringLiteral("new"));
}

TEST(AgentHarness_ModelCatalog, DuplicateIdsAreCollapsedAndEmptiesDropped)
{
    QString problem;
    const QHash<QString, ModelCatalog> catalogs = parseModelCatalogs(
        QStringLiteral(R"({"http://gw/v1":{"ids":["a","","a","b"],"fetchedAt":"t"}})"), &problem);

    EXPECT_TRUE(problem.isEmpty()) << problem.toStdString();
    const ModelCatalog catalog = catalogFor(catalogs, QStringLiteral("http://gw/v1"));
    ASSERT_EQ(catalog.ids.size(), 2) << "an empty id is not a model, and a repeated id is not a second model";
    EXPECT_EQ(catalog.ids[0], QStringLiteral("a"));
    EXPECT_EQ(catalog.ids[1], QStringLiteral("b"));
}
