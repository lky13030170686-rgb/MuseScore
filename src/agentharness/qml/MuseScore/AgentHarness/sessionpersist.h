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

#include <QByteArray>
#include <QString>
#include <QVector>

namespace muse::agentharness {
//! ── Conversations on disk (第 142 条) ─────────────────────────────────────────────────────────────
//!
//! ⛔⛔ **THERE IS NO MANIFEST, AND THAT IS THE DESIGN.** One file per conversation, `<id>.jsonl`, and
//! everything the list shows - the title, the event count, when it started - is DERIVED by reading that
//! file. A manifest would be a second record of the same conversations, and its first symptom would be a
//! list that disagrees with what is in the files (a session listed but not there, or the reverse), with
//! no way to tell which one is right. It also removes the corruption class entirely: a crash while
//! rewriting an index costs the index; a crash while APPENDING to a log costs at most the last line.
//!
//! ⛔ AND THE APPEND-ONLY PROPERTY IS USED, not just admired: because a file only ever grows by whole
//! lines, an unterminated LAST line can only be a write that a crash cut in half. That one line is dropped
//! and reported; any other unreadable line means the file is not ours and the whole session is refused
//! (`sessionlog.h` refuses a log it cannot fully read, and that rule is kept here).
//!
//! Everything in this file is either a pure function or a thin file operation on an explicit directory,
//! so the whole round trip is unit-testable in a temporary directory - no application, no settings, no
//! score.

//! Where sessions live, given the application's data path. One directory, named after the subsystem, so
//! a user who goes looking finds conversations together and can delete them by hand.
QString sessionsDirectory(const QString& dataPath);

//! Whether `id` is one this code could have written (a bare UUID). ⛔ The id arrives from a FILE NAME or
//! from the settings - i.e. from outside - and an id containing `/` or `..` would write outside the
//! directory. `spill.cpp` learned the same lesson about result file names (第 110 条).
bool isSessionId(const QString& id);

//! `<id>.jsonl`, or empty when `id` is not a session id (see above).
QString sessionFileName(const QString& id);

//! The id a file name encodes, or empty for anything else that happens to be in the directory.
QString sessionIdFromFileName(const QString& fileName);

//! One stored conversation, as its own bytes describe it.
struct StoredSession
{
    QString id;
    QString title;
    int events = 0;
    //! Whether anybody ever SAID anything in it (`sessionHasContent`), which is not the same as "the file
    //! has lines": the system prompt alone is a line. See `restoreStoredSessions` for what happens to a
    //! stored conversation that has none.
    bool hasContent = false;
    //! The FIRST event's own timestamp - not the file's mtime. A file copied or restored keeps saying when
    //! the conversation happened, and the list does not reorder itself because something touched a file.
    QString startedAt;
    //! The whole file, ready to be handed to `SessionLog::fromJsonLines`.
    QByteArray payload;
};

//! Parse one stored conversation.
//!
//! Returns false and fills `problem` when the file is not a conversation this build can read. `tornTail`
//! (optional) is set when the last line was unterminated and was dropped - the only damage an append-only
//! file can suffer from a crash, and the reason it is dropped rather than refused.
bool parseStoredSession(const QString& id, const QByteArray& jsonl, StoredSession& out,
                        QString* problem = nullptr, bool* tornTail = nullptr);

//! Every conversation in `dir`, OLDEST FIRST (the order the list draws them in). A missing directory is
//! simply empty - a first run is not an error. Files that cannot be read are REPORTED by name and reason
//! rather than skipped: a session that silently vanishes is worse than one that says why.
QVector<StoredSession> loadStoredSessions(const QString& dir, QVector<QString>* problems = nullptr);

//! Append one line (without its newline) to `<dir>/<id>.jsonl`, creating directory and file as needed.
bool appendSessionLine(const QString& dir, const QString& id, const QByteArray& line, QString* problem = nullptr);

//! Delete one stored conversation (第 144 条). ⛔ The id is validated BEFORE the filesystem is touched: it
//! becomes a path, and `isSessionId` is what keeps a delete inside the directory.
//!
//! ⚠️ A file that is already gone counts as SUCCESS: the caller asked for a state ("this conversation does
//! not exist"), and refusing because somebody got there first would leave a row in the list that can never
//! be removed - which is exactly the case a user hits after deleting a file by hand while the app runs.
bool removeStoredSession(const QString& dir, const QString& id, QString* problem = nullptr);
} // namespace muse::agentharness
