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

#include "transcriptfold.h"

#include <QVariantMap>

using namespace muse::agentharness;

namespace {
const QString TOOL_CALL = QStringLiteral("toolCall");
const QString TOOL_RESULT = QStringLiteral("toolResult");
const QString TOOL_GROUP = QStringLiteral("toolGroup");

bool isToolRow(const QVariantMap& row)
{
    const QString kind = row.value(QStringLiteral("kind")).toString();
    return kind == TOOL_CALL || kind == TOOL_RESULT;
}

//! The tool's NAME, out of a call row's text. The row's text is `name arguments` (that is how the
//! transcript builds it), so the name is the first token - the same rule `AgentToolInspector` uses to
//! title the newest call. ⚠️ Empty when the row carries no text, and the header then says "tool call"
//! rather than inventing a name.
QString toolNameOf(const QVariantMap& row)
{
    const QString text = row.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty()) {
        return QString();
    }

    const int space = text.indexOf(QLatin1Char(' '));
    return space < 0 ? text : text.left(space);
}

//! A stable identity for the group, so the VIEW can remember whether this group is expanded across the
//! model rebuilds that happen on every streamed token. `seq` is the log's own sequence number (never
//! reused, equal to the index in the log), which is the only identity here that is stable by
//! construction; the fallback keeps the function total for a caller that passes rows without one.
QString groupIdOf(const QVariantMap& firstRow, int ordinal)
{
    const QVariant seq = firstRow.value(QStringLiteral("seq"));
    if (seq.isValid()) {
        return QStringLiteral("tg%1").arg(seq.toULongLong());
    }
    return QStringLiteral("tg#%1").arg(ordinal);
}
} // namespace

QVariantList muse::agentharness::foldToolRows(const QVariantList& rows)
{
    QVariantList out;
    int ordinal = 0;

    int i = 0;
    while (i < rows.size()) {
        const QVariantMap row = rows[i].toMap();

        if (!isToolRow(row)) {
            out.append(rows[i]);
            ++i;
            continue;
        }

        //! The run: consecutive tool rows. ⚠️ `toolCall` and `toolResult` are BOTH tool rows and they
        //! interleave (the assistant message carries its calls, then the results arrive), so a run is
        //! "any tool row after any tool row" rather than "all the calls, then all the results".
        QVariantList items;
        QStringList names;
        int callCount = 0;
        int resultCount = 0;
        bool hasError = false;

        while (i < rows.size()) {
            const QVariantMap item = rows[i].toMap();
            if (!isToolRow(item)) {
                break;
            }

            const QString kind = item.value(QStringLiteral("kind")).toString();
            if (kind == TOOL_CALL) {
                ++callCount;
                const QString name = toolNameOf(item);
                if (!name.isEmpty()) {
                    names.append(name);
                }
            } else {
                ++resultCount;
            }

            //! ⛔ AN ERROR IS NEVER HIDDEN INSIDE A FOLDED GROUP. The view reads this flag and opens the
            //! group by default: a failed tool call is the explanation for everything that follows, and
            //! "the agent did something and it did not work" must not be one line the user has to think
            //! to click.
            if (item.value(QStringLiteral("isError")).toBool()) {
                hasError = true;
            }

            items.append(rows[i]);
            ++i;
        }

        QVariantMap group;
        group.insert(QStringLiteral("kind"), TOOL_GROUP);
        //! The role stays "tool": a renderer that only knows roles still treats the group as tool output,
        //! and the two surfaces keep one vocabulary.
        group.insert(QStringLiteral("role"), QStringLiteral("tool"));
        group.insert(QStringLiteral("groupId"), groupIdOf(row, ordinal));
        group.insert(QStringLiteral("callCount"), callCount);
        group.insert(QStringLiteral("resultCount"), resultCount);
        group.insert(QStringLiteral("hasError"), hasError);
        group.insert(QStringLiteral("names"), names.join(QStringLiteral(", ")));
        group.insert(QStringLiteral("items"), items);
        //! `text` is set as well, so a renderer that shows `text` and nothing else still shows something
        //! true about the group rather than an empty line.
        group.insert(QStringLiteral("text"), names.join(QStringLiteral(", ")));
        out.append(group);

        ++ordinal;
    }

    return out;
}
