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
#include "sessionstore.h"

#include <QDateTime>
#include <QUuid>

using namespace muse::agentharness;

namespace {
QString freshId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString nowStamp()
{
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}

SessionEntry freshLiveEntry()
{
    SessionEntry e;
    e.id = freshId();
    e.startedAt = nowStamp();
    return e;
}
} // namespace

bool muse::agentharness::sessionHasContent(const SessionLog& log)
{
    for (const SessionEvent& e : log.events()) {
        if (e.type == SessionEvent::USER_MESSAGE
            || e.type == SessionEvent::ASSISTANT_MESSAGE
            || e.type == SessionEvent::TOOL_CALL
            || e.type == SessionEvent::TOOL_RESULT) {
            return true;
        }
    }

    return false;
}

QString muse::agentharness::sessionTitleFrom(const SessionLog& log, int maxChars)
{
    QString text;

    //! ⛔ A NAME THE USER GAVE WINS (第 144 条), and the LAST one wins over earlier ones: renaming twice
    //! means the second name is the name. Read backwards, because "the last one" is the one that counts.
    for (auto it = log.events().crbegin(); it != log.events().crend(); ++it) {
        if (it->type == SessionEvent::SESSION_TITLE) {
            text = it->data.value(QStringLiteral("content")).toString();
            break;
        }
    }

    if (text.isEmpty()) {
        for (const SessionEvent& e : log.events()) {
            if (e.type == SessionEvent::USER_MESSAGE) {
                text = e.data.value(QStringLiteral("content")).toString();
                break;
            }
        }
    }

    //! A session that exists but has said nothing yet is a real state - the list shows it as the current
    //! row while the user is typing the first message - so it gets a label rather than an empty cell.
    if (text.isEmpty()) {
        return sessionHasContent(log) ? QStringLiteral("(no message yet)")
                                      : QStringLiteral("(new session)");
    }

    return elideTitle(text, maxChars);
}

QString muse::agentharness::elideTitle(const QString& text, int maxChars)
{
    //! One line, whatever the user pasted or typed: a name containing a newline would take two rows and break
    //! the "one conversation, one row" reading the column is built on.
    QString folded = text.simplified();

    if (maxChars > 0 && folded.size() > maxChars) {
        int cut = maxChars;
        //! Do not split a surrogate pair: half an emoji renders as a replacement glyph, and a name is the one
        //! place the user sees their own words coming back.
        if (folded.at(cut - 1).isHighSurrogate()) {
            --cut;
        }
        folded = folded.left(cut) + QChar(0x2026);
    }

    return folded;
}

SessionStore::SessionStore()
{
    m_entries.append(freshLiveEntry());
    m_current = 0;
}

SessionEntry SessionStore::at(int index) const
{
    if (index < 0 || index >= m_entries.size()) {
        return SessionEntry {};
    }

    return m_entries.at(index);
}

void SessionStore::archiveCurrent(const QByteArray& payload, int events, const QString& title)
{
    SessionEntry& entry = m_entries[m_current];
    entry.archived = true;
    entry.payload = payload;
    entry.events = events;
    entry.title = title;
}

void SessionStore::startNewLive()
{
    //! ⛔ THE UNUSED SLOT IS REUSED, NOT PARKED. If the outgoing conversation was never archived there is
    //! nothing to keep, so appending a second entry would leave a row with zero events behind every click
    //! on "New session". ⚠️ The test is `archived`, NOT "the payload is empty" - the two differ for a
    //! conversation that was restored from this list (see the field's note).
    if (!m_entries[m_current].archived) {
        m_entries[m_current] = freshLiveEntry();
        return;
    }

    m_entries.append(freshLiveEntry());
    m_current = int(m_entries.size()) - 1;
}

bool SessionStore::makeCurrent(int index, QByteArray* payload)
{
    if (index < 0 || index >= m_entries.size() || index == m_current) {
        return false;
    }

    if (!m_entries[m_current].archived) {
        //! The outgoing conversation was never used: drop its slot rather than list it forever.
        m_entries.removeAt(m_current);
        if (index > m_current) {
            --index;
        }
    }

    m_current = index;

    SessionEntry& target = m_entries[m_current];
    if (payload) {
        *payload = target.payload;
    }
    //! Hand the payload over and let go of it: from here the live log IS that conversation, and a copy
    //! left behind would be a second copy that the next appended event makes wrong. ⚠️ `archived` stays
    //! true - that is what says "this conversation has content", and it is what keeps it from being
    //! dropped as an empty slot when the user switches away again.
    target.payload.clear();

    return true;
}

QByteArray SessionStore::adoptStored(const QVector<SessionEntry>& entries, int liveIndex)
{
    if (entries.isEmpty()) {
        return QByteArray();
    }

    //! ⛔ A BAD INDEX FALLS BACK TO THE LAST ENTRY, and the last entry is the NEWEST: `loadStoredSessions`
    //! returns them oldest first. A caller that asked for a session which is no longer there (the user
    //! deleted the file) gets the most recent conversation rather than an assertion.
    const int clamped = (liveIndex < 0 || liveIndex >= entries.size()) ? entries.size() - 1 : liveIndex;

    m_entries = entries;
    m_current = clamped;

    QByteArray payload = m_entries[m_current].payload;
    m_entries[m_current].payload.clear();
    //! ⛔ `archived` IS TRUE EVEN THOUGH THE PAYLOAD IS NOW EMPTY: this conversation has content (it was
    //! written to a file), and the flag is exactly what keeps `makeCurrent` from dropping it as an unused
    //! slot when the user switches away from it. Getting this wrong is the bug the store's own suite
    //! caught once already - see the note on `SessionEntry::archived`.
    m_entries[m_current].archived = true;

    return payload;
}

bool SessionStore::removeAt(int index)
{
    if (index < 0 || index >= m_entries.size() || index == m_current) {
        return false;
    }

    m_entries.removeAt(index);
    //! The live entry moved down by one when something before it was removed, and nothing else changes:
    //! which entry is live is a fact about the log, not about the list's order.
    if (index < m_current) {
        --m_current;
    }

    return true;
}

void SessionStore::dropCurrentLive()
{
    //! ⛔ IN PLACE, and that is the whole method: the entry that stood for the deleted conversation is
    //! replaced by a fresh live one, so the list cannot come back with a row for a conversation whose file
    //! is gone. The id changes with it (`freshLiveEntry`), which is what makes it a NEW conversation rather
    //! than a resurrected one.
    m_entries[m_current] = freshLiveEntry();
}

void SessionStore::setStoredTitle(int index, const QString& title)
{
    if (index < 0 || index >= m_entries.size()) {
        return;
    }

    m_entries[index].title = title;
}

void SessionStore::appendStoredLine(int index, const QByteArray& line)
{
    if (index < 0 || index >= m_entries.size() || line.isEmpty()) {
        return;
    }

    SessionEntry& entry = m_entries[index];
    entry.payload.append(line);
    //! ⛔ THE NEWLINE IS PART OF THE SAME APPEND, exactly as in `sessionpersist.cpp`: a payload that ends
    //! mid-line is the tear that `parseStoredSession` then has to repair, and manufacturing one here would
    //! make the in-memory copy differ from the file it is supposed to be.
    entry.payload.append('\n');
    ++entry.events;
}
