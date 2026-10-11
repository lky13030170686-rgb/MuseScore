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

#include <QHash>
#include <QString>
#include <QStringList>

namespace muse::agentharness {
//! One gateway's model catalog, as it is remembered on disk.
//!
//! \b Why the catalog is stored PER GATEWAY. A model id is the gateway's vocabulary (第 129 条): the list
//! that `api.deepseek.com` serves says nothing about what a local OpenCodex proxy serves, and mixing them
//! offers the user names that do not exist where they are about to be used. Keying the catalog by the base
//! URL means switching gateways back and forth keeps each list, which is what a user who tries two
//! gateways once expects - and the reason this is a MAP and not a single list.
struct ModelCatalog
{
    //! Model ids, in the order the gateway listed them (the order is information: a gateway may rank).
    QStringList ids;
    //! When the list was fetched, ISO-8601 in UTC. Empty means "no list from this gateway yet".
    QString fetchedAt;

    bool isEmpty() const { return ids.isEmpty(); }
};

//! Read the stored catalogs out of the settings string.
//!
//! PURE, and separated from the settings for the same reason `parseModelIds()` is separated from the
//! socket: this is the part that decides what the user is offered, and a wrong answer is invisible (an
//! empty catalog looks exactly like "never fetched").
//!
//! The shape is `{"<base url>": {"ids": ["..."], "fetchedAt": "..."}}`. ⚠️ Anything unreadable yields an
//! EMPTY map plus a `problem` - never a partial one: a half-parsed catalog would show a gateway's models
//! as if they were complete, which is the failure this project keeps refusing.
QHash<QString, ModelCatalog> parseModelCatalogs(const QString& json, QString* problem = nullptr);

//! The inverse. Kept here so the two directions cannot drift apart.
QString serializeModelCatalogs(const QHash<QString, ModelCatalog>& catalogs);

//! One gateway's catalog, or an empty one when this gateway has none. Case-insensitive on the key, because
//! a user who types `HTTP://127.0.0.1:10100/V1` means the same endpoint.
ModelCatalog catalogFor(const QHash<QString, ModelCatalog>& catalogs, const QString& baseUrl);

//! Store (or replace) one gateway's catalog and return the updated map. Insertion order is not preserved
//! by `QHash` and does not need to be: the settings string is a map, and the UI shows ONE gateway at a time.
QHash<QString, ModelCatalog> withCatalog(const QHash<QString, ModelCatalog>& catalogs, const QString& baseUrl,
                                         const ModelCatalog& catalog);

} // namespace muse::agentharness
