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
    for (const SessionEvent& e : log.events()) {
        if (e.type == SessionEvent::USER_MESSAGE) {
            text = e.data.value(QStringLiteral("content")).toString();
            break;
        }
    }

    //! A session that exists but has said nothing yet is a real state - the list shows it as the current
    //! row while the user is typing the first message - so it gets a label rather than an empty cell.
    if (text.isEmpty()) {
        return sessionHasContent(log) ? QStringLiteral("(no message yet)")
                                      : QStringLiteral("(new session)");
    }

    //! One line, whatever the user pasted: a title containing a newline would take two rows and break the
    //! "one conversation, one row" reading the column is built on.
    text = text.simplified();

    if (maxChars > 0 && text.size() > maxChars) {
        int cut = maxChars;
        //! Do not split a surrogate pair: half an emoji renders as a replacement glyph, and a title is the
        //! one place the user sees their own words coming back.
        if (text.at(cut - 1).isHighSurrogate()) {
            --cut;
        }
        text = text.left(cut) + QChar(0x2026);
    }

    return text;
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
