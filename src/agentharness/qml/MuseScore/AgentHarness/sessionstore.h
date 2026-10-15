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

#include "sessionlog.h"

namespace muse::agentharness {
//! ── More than one conversation (第 141 条) ───────────────────────────────────────────────────────
//!
//! ⛔ WHY THIS FILE EXISTS AT ALL. Until now the harness had **exactly one session per process**: the log
//! was a member of `FieldController`, and the only way to start a clean conversation was to restart the
//! program. `AgentFactsPanel.qml` and `AgentWorkbench.qml` both said so in a comment, and used it as the
//! reason NOT to build the session list the DSH client has ("a list would be furniture with nothing
//! behind it"). That reasoning was sound and its premise was checkable - so this is the round that makes
//! the premise false, exactly as 第 135 条 did for the right column's tab strip.
//!
//! ⛔ ONE LIVE LOG, N ARCHIVED ONES, AND THE LIVE ONE NEVER MOVES. `AgentLoop` holds a `SessionLog*`
//! taken in its constructor, so replacing the live `SessionLog` object would leave the loop writing into
//! freed memory - or, worse, into a log nobody reads. Switching therefore *restores into* the live object
//! (`SessionLog::fromJsonLines`) instead of pointing at another one, and this store owns only what is NOT
//! live: the JSONL of every other conversation, plus the row data the list needs.
//!
//! ⛔ THE INVARIANT, AND IT IS ONE SENTENCE: **exactly one entry has an empty payload, and that entry is
//! the live conversation.** An entry with a payload is an archived conversation; the live one has no
//! payload because the live log itself is the payload. That single rule is what makes "the list" and "the
//! conversation being appended to" impossible to disagree.
//!
//! Everything here is a plain container plus two pure free functions, so the rules that decide what a row
//! says and which conversation survives a click are unit-testable without an application, a score or a
//! network.

//! Whether a conversation has anything in it that a reader would miss if it were thrown away.
//!
//! ⛔ THE SYSTEM PROMPT DOES NOT COUNT. Every session starts with one (the loop seeds it), so counting it
//! would make every session "worth keeping" and turn the list into one row per click on "New session" -
//! the furniture 第 124 条 warned about, arrived at from the other side.
bool sessionHasContent(const SessionLog& log);

//! How much of a first message a list row shows. One constant, because the elision and the label it
//! produces have to agree: two 44s in two files is the shape that drifts (第 92 条's lesson).
constexpr int SESSION_TITLE_CHARS = 44;

//! A one-line label for a conversation, derived from its log: the FIRST user message, whitespace
//! collapsed and elided. PURE, and derived rather than stored for the usual reason - a title assigned
//! once is a second truth about a log that keeps growing.
//!
//! ⚠️ `maxChars` counts UTF-16 units, so the elision steps back one position when the cut would land
//! between the halves of a surrogate pair (an emoji in a first message is not a hypothesis).
QString sessionTitleFrom(const SessionLog& log, int maxChars = SESSION_TITLE_CHARS);

//! One row of the session list.
struct SessionEntry
{
    //! Stable for the life of the conversation. It is also the routing identity sent with the requests
    //! belonging to it (`x-opencode-session`), so a retry or a resumed turn stays inside the same
    //! conversation from the gateway's point of view as well.
    QString id;
    QString title;
    int events = 0;
    QString startedAt;
    //! Whether this conversation has ever been archived - i.e. whether it has content worth keeping.
    //!
    //! ⛔⛔ IT IS **NOT** `!payload.isEmpty()`, and getting that wrong loses conversations. The LIVE entry's
    //! payload is empty by construction, and it is empty for two very different reasons: a brand-new
    //! conversation nobody has said anything in, and a conversation that was restored from this list a
    //! moment ago. The first version of this file used the payload as the test, so switching away from a
    //! restored conversation looked exactly like abandoning an empty slot and the row was DROPPED -
    //! silently, since the list still rendered. `SwitchingKeepsTheConversationThatWasLeft` is the test that
    //! caught it, and it is the reason this flag exists rather than a cleverer predicate.
    bool archived = false;
    //! The archived JSONL. EMPTY MEANS THIS IS THE LIVE CONVERSATION (see the invariant above).
    QByteArray payload;
};

//! The conversations of one process.
//!
//! ⚠️ NOT PERSISTED, deliberately rather than by omission: conversations in this harness do not survive
//! the process today (`交接文档.md` says the same about the transcript), so a list that came back empty
//! after a restart would be a promise the rest of the subsystem does not keep. Writing sessions to disk
//! and restoring them is its own piece of work; until it exists, the UI says so instead of pretending.
class SessionStore
{
public:
    SessionStore();

    int count() const { return int(m_entries.size()); }
    int currentIndex() const { return m_current; }
    //! Out of range returns an empty entry rather than asserting: the caller's index came from a QML model
    //! that may have been built before a switch changed the list.
    SessionEntry at(int index) const;
    const SessionEntry& current() const { return m_entries[m_current]; }
    //! True while the live conversation still has nothing worth keeping - which is a question about what
    //! has happened to this entry (`archived`), never about its payload: see the note on that field.
    bool currentIsEmpty() const { return !m_entries[m_current].archived; }

    //! Keep the live conversation: its JSONL, its size and its label. The caller archives BEFORE it clears
    //! or replaces the live log; a conversation with nothing in it is simply not archived, and its slot is
    //! then reused by `startNewLive` (so two clicks on "New session" leave one row, not two).
    void archiveCurrent(const QByteArray& payload, int events, const QString& title);

    //! Begin a new live conversation, keeping the archived ones.
    void startNewLive();

    //! Make `index` live, handing back its archived JSONL. Returns false (changing nothing) for an
    //! out-of-range index or for the entry that is already live. ⛔ An outgoing entry with no payload is an
    //! empty slot and is DROPPED rather than left behind: otherwise "new session" followed by "go back"
    //! would leave a permanent row called "(new session)" with zero events.
    bool makeCurrent(int index, QByteArray* payload);

    //! Label and size of the live conversation are NOT stored here: the live row is read from the live
    //! log by the caller (`FieldController::agentSessions`), so it cannot describe a conversation that has
    //! since grown. ⚠️ The two setters that once stood here were written speculatively and removed the
    //! moment nothing called them - a setter nobody calls is a second way to say what the log already says.

private:
    QVector<SessionEntry> m_entries;
    int m_current = 0;
};
} // namespace muse::agentharness
