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
#include "sessionpersist.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "sessionlog.h"
#include "sessionstore.h"

using namespace muse::agentharness;

namespace {
const QString SESSIONS_DIR_NAME = QStringLiteral("agentharness-sessions");
const QString SESSION_SUFFIX = QStringLiteral(".jsonl");
} // namespace

QString muse::agentharness::sessionsDirectory(const QString& dataPath)
{
    if (dataPath.isEmpty()) {
        return QString();
    }

    return QDir(dataPath).filePath(SESSIONS_DIR_NAME);
}

bool muse::agentharness::isSessionId(const QString& id)
{
    //! A bare UUID, checked character by character rather than by pattern: this value becomes a PATH
    //! COMPONENT, and the two characters that matter here (`/` and `\`) are exactly the ones a permissive
    //! "looks like an id" test would let through.
    if (id.size() != 36) {
        return false;
    }

    for (const QChar c : id) {
        const bool ok = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                        || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                        || (c >= QLatin1Char('A') && c <= QLatin1Char('F'))
                        || c == QLatin1Char('-');
        if (!ok) {
            return false;
        }
    }

    return true;
}

QString muse::agentharness::sessionFileName(const QString& id)
{
    if (!isSessionId(id)) {
        return QString();
    }

    return id + SESSION_SUFFIX;
}

QString muse::agentharness::sessionIdFromFileName(const QString& fileName)
{
    if (!fileName.endsWith(SESSION_SUFFIX)) {
        return QString();
    }

    const QString id = fileName.left(fileName.size() - SESSION_SUFFIX.size());
    return isSessionId(id) ? id : QString();
}

bool muse::agentharness::parseStoredSession(const QString& id, const QByteArray& jsonl,
                                            StoredSession& out, QString* problem, bool* tornTail)
{
    if (tornTail) {
        *tornTail = false;
    }

    QByteArray readable = jsonl;

    //! ⛔ THE ONE DAMAGE AN APPEND-ONLY FILE CAN TAKE. Everything is written as whole lines, so a file that
    //! does not end in a newline was cut mid-write by a crash - and only its last line can be incomplete.
    //! Dropping it loses the event that was being written; refusing the file would lose the conversation.
    if (!readable.isEmpty() && !readable.endsWith('\n')) {
        const int cut = readable.lastIndexOf('\n');
        readable = cut < 0 ? QByteArray() : readable.left(cut + 1);
        if (tornTail) {
            *tornTail = true;
        }
    }

    SessionLog log;
    QString parseProblem;
    if (!SessionLog::fromJsonLines(readable, log, &parseProblem)) {
        if (problem) {
            *problem = parseProblem;
        }
        return false;
    }

    out.id = id;
    out.title = sessionTitleFrom(log);
    out.events = log.size();
    out.hasContent = sessionHasContent(log);
    out.payload = readable;
    //! The conversation's own first timestamp, not the file's. An EMPTY file has none, and says so.
    out.startedAt = log.isEmpty() ? QString() : log.events().first().time;

    if (tornTail && *tornTail) {
        //! Reported through `problem` as well: the caller decides whether to log it, but nobody should have
        //! to guess why a conversation is one event shorter than the user remembers.
        if (problem) {
            *problem = QStringLiteral("the last line was incomplete and was dropped");
        }
    }

    return true;
}

QVector<StoredSession> muse::agentharness::loadStoredSessions(const QString& dir, QVector<QString>* problems)
{
    QVector<StoredSession> out;

    if (dir.isEmpty()) {
        return out;
    }

    const QDir directory(dir);
    //! A first run has no directory. That is not a problem to report - it is the normal empty case.
    if (!directory.exists()) {
        return out;
    }

    //! ⛔ ONLY `*.jsonl` IS CONSIDERED, and that is a deliberate boundary rather than a filter that happens
    //! to be here: anything else in the directory is not ours, and a user who drops a note in it should not
    //! be told their file is a problem. A `.jsonl` that is not `<uuid>.jsonl`, on the other hand, IS
    //! reported - that one looks like a conversation and is not one.
    const QStringList names = directory.entryList({ QStringLiteral("*") + SESSION_SUFFIX }, QDir::Files, QDir::Name);
    for (const QString& name : names) {
        const QString id = sessionIdFromFileName(name);
        if (id.isEmpty()) {
            if (problems) {
                problems->append(QStringLiteral("%1: not a session file (expected <uuid>.jsonl)").arg(name));
            }
            continue;
        }

        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            if (problems) {
                problems->append(QStringLiteral("%1: cannot be read (%2)").arg(name, file.errorString()));
            }
            continue;
        }

        const QByteArray bytes = file.readAll();
        file.close();

        StoredSession session;
        QString problem;
        bool torn = false;
        if (!parseStoredSession(id, bytes, session, &problem, &torn)) {
            if (problems) {
                problems->append(QStringLiteral("%1: %2").arg(name, problem));
            }
            continue;
        }

        if (torn && problems) {
            problems->append(QStringLiteral("%1: %2").arg(name, problem));
        }

        out.append(session);
    }

    //! Oldest first, which is the order the list draws them in. Ties (two sessions started in the same
    //! millisecond) are broken by id so the order is stable across runs - an unstable list is one that
    //! reorders itself for no reason the user can see.
    std::sort(out.begin(), out.end(), [](const StoredSession& a, const StoredSession& b) {
        if (a.startedAt != b.startedAt) {
            return a.startedAt < b.startedAt;
        }
        return a.id < b.id;
    });

    return out;
}

bool muse::agentharness::appendSessionLine(const QString& dir, const QString& id, const QByteArray& line,
                                           QString* problem)
{
    const QString name = sessionFileName(id);
    if (dir.isEmpty() || name.isEmpty()) {
        if (problem) {
            *problem = QStringLiteral("not a session id: %1").arg(id);
        }
        return false;
    }

    QDir directory(dir);
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        if (problem) {
            *problem = QStringLiteral("cannot create %1").arg(dir);
        }
        return false;
    }

    QFile file(directory.filePath(name));
    //! Append, never truncate: the file IS the log, and a writer that rewrote it would have to hold the
    //! whole conversation in memory to do so - which is the manifest problem again, one file down.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (problem) {
            *problem = QStringLiteral("cannot open %1 (%2)").arg(file.fileName(), file.errorString());
        }
        return false;
    }

    //! ⛔ ONE WRITE PER EVENT, newline included, and the newline is part of the SAME write: two writes would
    //! leave a window in which the file ends mid-line - the very tear `parseStoredSession` has to repair.
    QByteArray record = line;
    record.append('\n');
    const qint64 written = file.write(record);
    file.close();

    if (written != record.size()) {
        if (problem) {
            *problem = QStringLiteral("short write to %1").arg(file.fileName());
        }
        return false;
    }

    return true;
}
