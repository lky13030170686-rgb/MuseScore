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
#include "modelcatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

using namespace muse::agentharness;

QHash<QString, ModelCatalog> muse::agentharness::parseModelCatalogs(const QString& json, QString* problem)
{
    const auto fail = [problem](const QString& why) {
        if (problem) {
            *problem = why;
        }
        return QHash<QString, ModelCatalog>();
    };

    //! An empty string is NOT a problem: it is what a fresh installation has, and reporting it as an error
    //! would put a failure message in front of every user who has simply never fetched a list.
    if (json.trimmed().isEmpty()) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(QStringLiteral("the stored model catalogs are not readable: %1").arg(parseError.errorString()));
    }

    if (!doc.isObject()) {
        return fail(QStringLiteral("the stored model catalogs are not an object"));
    }

    QHash<QString, ModelCatalog> out;
    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject entry = it.value().toObject();

        ModelCatalog catalog;
        const QJsonArray ids = entry.value(QStringLiteral("ids")).toArray();
        for (const QJsonValue& id : ids) {
            const QString text = id.toString();
            if (!text.isEmpty() && !catalog.ids.contains(text)) {
                catalog.ids.append(text);
            }
        }
        catalog.fetchedAt = entry.value(QStringLiteral("fetchedAt")).toString();

        //! ⚠️ An entry with no ids is DROPPED rather than kept as an empty catalog: "this gateway serves
        //! nothing" and "we have no list for this gateway" are different facts (the UI says different
        //! things), and only one of them is worth storing.
        if (!catalog.isEmpty()) {
            out.insert(it.key(), catalog);
        }
    }

    return out;
}

QString muse::agentharness::serializeModelCatalogs(const QHash<QString, ModelCatalog>& catalogs)
{
    QJsonObject root;
    for (auto it = catalogs.begin(); it != catalogs.end(); ++it) {
        if (it.value().isEmpty()) {
            continue;
        }

        QJsonArray ids;
        for (const QString& id : it.value().ids) {
            ids.append(id);
        }

        QJsonObject entry;
        entry.insert(QStringLiteral("ids"), ids);
        entry.insert(QStringLiteral("fetchedAt"), it.value().fetchedAt);
        root.insert(it.key(), entry);
    }

    //! Compact, like the session log's JSONL: this string lives in one settings value and is written on
    //! every fetch, so its size is a real cost and its readability is zero (nobody edits it by hand).
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

ModelCatalog muse::agentharness::catalogFor(const QHash<QString, ModelCatalog>& catalogs, const QString& baseUrl)
{
    const QString wanted = baseUrl.trimmed();

    //! Exact match first, then a case-insensitive one: the stored key is whatever the gateway field held at
    //! fetch time, and a user who retypes the same address with different capitalisation means the same
    //! endpoint - throwing their list away over that would be the kind of quiet loss this file exists to
    //! avoid.
    auto exact = catalogs.constFind(wanted);
    if (exact != catalogs.constEnd()) {
        return *exact;
    }

    for (auto it = catalogs.constBegin(); it != catalogs.constEnd(); ++it) {
        if (it.key().compare(wanted, Qt::CaseInsensitive) == 0) {
            return *it;
        }
    }

    return {};
}

QHash<QString, ModelCatalog> muse::agentharness::withCatalog(const QHash<QString, ModelCatalog>& catalogs,
                                                            const QString& baseUrl, const ModelCatalog& catalog)
{
    QHash<QString, ModelCatalog> out = catalogs;
    const QString key = baseUrl.trimmed();
    if (key.isEmpty()) {
        return out;
    }

    if (catalog.isEmpty()) {
        //! Removing the last id removes the entry: an empty list is not a list, and leaving `{"gw": {"ids": []}}`
        //! behind would make the stored value claim a gateway serves nothing.
        out.remove(key);
        return out;
    }

    //! ⚠️ Replace any case-variant of the same address instead of keeping both: two entries for one endpoint
    //! would be two answers to "what does this gateway serve".
    for (auto it = out.begin(); it != out.end();) {
        if (it.key() != key && it.key().compare(key, Qt::CaseInsensitive) == 0) {
            it = out.erase(it);
        } else {
            ++it;
        }
    }

    out.insert(key, catalog);
    return out;
}
