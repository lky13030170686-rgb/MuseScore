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
#include "spill.h"

#include <QDateTime>
#include <QtGlobal>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "log.h"

using namespace muse::agentharness;

namespace {
//! Where spilled results go: the app's cache directory, under `agentharness/spill`.
//!
//! ⚠️ CACHE and not documents or settings: a spilled result is a COPY of something the conversation
//! already contains a preview of, so losing it costs a re-run of the tool and nothing else. Putting it
//! among the user's documents would make throwaway files look like the user's files.
//!
//! ⚠️ Falls back to the temp directory when there is no cache location, rather than giving up: a spill
//! that cannot find a home would push the whole result back into the conversation, which is the thing it
//! exists to prevent.
QString spillDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    return base + QStringLiteral("/agentharness/spill");
}

//! A file name that says which tool made it and when, and cannot collide.
//!
//! ⚠️ The tool name is SANITISED, not used as-is: a name with a `/` or a `..` in it would write outside
//! the spill directory. Tool names are ours and have neither today, but this is the kind of thing that
//! stops being true later.
QString fileNameFor(const QString& toolName)
{
    QString safe = toolName;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    if (safe.isEmpty()) {
        safe = QStringLiteral("tool");
    }
    return QStringLiteral("%1-%2.txt")
           .arg(safe, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
}
} // namespace

int muse::agentharness::spillThresholdBytes()
{
    //! ⚠️ Read on every call rather than cached, because the point of the switch is to be set before the
    //! app starts and read once the path is reached - caching it in a static would work today and break the
    //! first time someone tried to change it mid-session while debugging.
    bool ok = false;
    const int override = qEnvironmentVariableIntValue("MUSE_AGENT_SPILL_THRESHOLD", &ok);
    if (ok && override >= 0) {
        return override;
    }
    return SPILL_THRESHOLD_BYTES;
}

QString muse::agentharness::spillIfLarge(const QString& toolName, const QString& text)
{
    if (text.toUtf8().size() <= spillThresholdBytes()) {
        return text;
    }

    const QString directory = spillDirectory();
    if (!QDir().mkpath(directory)) {
        LOGW() << "[agent-spill] could not create" << directory << "- returning the result whole";
        return text;
    }

    const QString path = directory + QLatin1Char('/') + fileNameFor(toolName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        //! ⛔ A FAILED SPILL MUST NOT FAIL THE CALL. The tool did its work; the result is in hand; the only
        //! thing that went wrong is where to put it. Turning that into a failure would report a successful
        //! edit as an error - the caller would redo it and apply it twice. Returning the result whole is
        //! worse for the context and correct for the caller.
        LOGW() << "[agent-spill] could not write" << path << "- returning the result whole";
        return text;
    }

    const QByteArray payload = text.toUtf8();
    file.write(payload);
    file.close();

    const int omitted = payload.size() - 2 * SPILL_PREVIEW_BYTES;

    LOGW() << "[agent-spill]" << toolName << "result is" << payload.size()
           << "bytes -> spilled to" << path << "omitted" << omitted;

    //! ⚠️ The order is deliberate: WHAT it is, then WHERE the rest is, then HOW MUCH is missing. A caller
    //! that reads only the first line still learns the shape of the result; one that wants more has the
    //! path in front of it; and the count is what tells it whether reading the file is worth it.
    return QStringLiteral("%1\n\n[%2 of %3 bytes omitted. The whole result is in %4 - read it if you need "
                          "the middle.]\n\n%5")
           .arg(text.left(SPILL_PREVIEW_BYTES))
           .arg(omitted)
           .arg(payload.size())
           .arg(path)
           .arg(text.right(SPILL_PREVIEW_BYTES));
}
