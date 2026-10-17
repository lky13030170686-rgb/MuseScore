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

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "agentharness/qml/MuseScore/AgentHarness/sessionlog.h"
#include "agentharness/qml/MuseScore/AgentHarness/sessionpersist.h"

using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS (第 142 条). This is the only code in the harness that writes the user's own words
//! to disk, and its failure modes are quiet:
//!   - an id with a `/` in it writes a conversation OUTSIDE the directory it belongs in;
//!   - a file that cannot be parsed disappears from the list (the user's conversation is "gone" with no
//!     error anywhere);
//!   - a crash mid-write leaves an unterminated line, and refusing the whole file because of it throws away
//!     everything the user said in that conversation.
//! So the rules are pinned here, in a temporary directory, with no application and no settings: what an id
//! may be, what a torn tail means, and what survives a real round trip through real files.

namespace {
const QString UUID_A = QStringLiteral("11111111-2222-3333-4444-555555555555");
const QString UUID_B = QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");

SessionLog logWith(const QString& firstUserMessage, int extraEvents = 0)
{
    SessionLog log;
    QJsonObject system;
    system.insert(QStringLiteral("content"), QStringLiteral("you are a notation assistant"));
    log.append(SessionEvent::SYSTEM_MESSAGE, system);

    QJsonObject user;
    user.insert(QStringLiteral("content"), firstUserMessage);
    log.append(SessionEvent::USER_MESSAGE, user);

    for (int i = 0; i < extraEvents; ++i) {
        QJsonObject data;
        data.insert(QStringLiteral("content"), QStringLiteral("answer %1").arg(i));
        log.append(SessionEvent::ASSISTANT_MESSAGE, data);
    }

    return log;
}

//! A directory of our own, deleted when the test ends - the same shape 第 137 条 used for the test score.
class TempSessions
{
public:
    TempSessions()
    {
        EXPECT_TRUE(m_dir.isValid());
        m_path = m_dir.path() + QStringLiteral("/agentharness-sessions");
    }

    QString path() const { return m_path; }

private:
    QTemporaryDir m_dir;
    QString m_path;
};
} // namespace

TEST(AgentHarness_SessionPersist, DirectoryIsNamedAfterTheSubsystem)
{
    EXPECT_EQ(sessionsDirectory(QStringLiteral("C:/data")), QStringLiteral("C:/data/agentharness-sessions"));
    //! No data path means nowhere to write. The caller reports that and writes nothing - it must NOT fall
    //! back to a relative path, which would drop conversation files next to the executable.
    EXPECT_TRUE(sessionsDirectory(QString()).isEmpty());
}

TEST(AgentHarness_SessionPersist, AnIdBecomesAPathComponentAndIsCheckedLikeOne)
{
    EXPECT_TRUE(isSessionId(UUID_A));
    EXPECT_EQ(sessionFileName(UUID_A), UUID_A + QStringLiteral(".jsonl"));
    EXPECT_EQ(sessionIdFromFileName(UUID_A + QStringLiteral(".jsonl")), UUID_A);

    //! ⛔ THE SECURITY RULE, and the reason the check is character-by-character rather than "looks like an
    //! id": this value is joined onto a directory. A permissive test would let a traversal through.
    EXPECT_FALSE(isSessionId(QStringLiteral("../../etc/passwd")));
    EXPECT_FALSE(isSessionId(QStringLiteral("aaaaaaaa/bbbb-cccc-dddd-eeeeeeeeeeee")));
    EXPECT_FALSE(isSessionId(QStringLiteral("aaaaaaaa\\bbbb-cccc-dddd-eeeeeeeeeeee")));
    EXPECT_FALSE(isSessionId(QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/../x")));
    EXPECT_TRUE(sessionFileName(QStringLiteral("../escape")).isEmpty());

    //! Anything else in the directory is not ours.
    EXPECT_TRUE(sessionIdFromFileName(QStringLiteral("notes.txt")).isEmpty());
    EXPECT_TRUE(sessionIdFromFileName(QStringLiteral("abc.jsonl")).isEmpty());
    EXPECT_TRUE(sessionIdFromFileName(UUID_A + QStringLiteral(".jsonl.bak")).isEmpty());
}

TEST(AgentHarness_SessionPersist, AStoredConversationDescribesItself)
{
    const SessionLog log = logWith(QStringLiteral("how many measures?"), 2);
    StoredSession session;
    ASSERT_TRUE(parseStoredSession(UUID_A, log.toJsonLines(), session));

    EXPECT_EQ(session.id, UUID_A);
    EXPECT_EQ(session.title, QStringLiteral("how many measures?"));
    EXPECT_EQ(session.events, log.size());
    EXPECT_TRUE(session.hasContent);
    //! The FIRST event's own timestamp, not the file's: a copied file keeps saying when it happened.
    EXPECT_EQ(session.startedAt, log.events().first().time);
    EXPECT_EQ(session.payload, log.toJsonLines());
}

TEST(AgentHarness_SessionPersist, ASystemPromptAloneIsNotContent)
{
    SessionLog log;
    QJsonObject system;
    system.insert(QStringLiteral("content"), QStringLiteral("you are a notation assistant"));
    log.append(SessionEvent::SYSTEM_MESSAGE, system);

    StoredSession session;
    ASSERT_TRUE(parseStoredSession(UUID_A, log.toJsonLines(), session));

    EXPECT_EQ(session.events, 1);
    EXPECT_FALSE(session.hasContent);
}

TEST(AgentHarness_SessionPersist, AnEmptyFileIsAnEmptyConversationNotAnError)
{
    StoredSession session;
    ASSERT_TRUE(parseStoredSession(UUID_A, QByteArray(), session));

    EXPECT_EQ(session.events, 0);
    EXPECT_FALSE(session.hasContent);
    EXPECT_TRUE(session.startedAt.isEmpty());
}

TEST(AgentHarness_SessionPersist, ATornLastLineIsDroppedRatherThanCostingTheConversation)
{
    //! ⛔ THE ONE DAMAGE AN APPEND-ONLY FILE CAN TAKE. Everything is written as whole lines, so a file that
    //! does not end in a newline was cut mid-write by a crash - and only its LAST line can be incomplete.
    //! Dropping it loses the event being written; refusing the file would lose everything the user said.
    const SessionLog log = logWith(QStringLiteral("first question"), 2);
    QByteArray jsonl = log.toJsonLines();
    jsonl.chop(12);   // the last line is now half-written, and unterminated

    StoredSession session;
    QString problem;
    bool torn = false;
    ASSERT_TRUE(parseStoredSession(UUID_A, jsonl, session, &problem, &torn));

    EXPECT_TRUE(torn);
    EXPECT_EQ(session.events, log.size() - 1);
    EXPECT_EQ(session.title, QStringLiteral("first question"));   // the conversation is still here
    //! And it says so: nobody should have to guess why a conversation is one event shorter than remembered.
    EXPECT_FALSE(problem.isEmpty());
}

TEST(AgentHarness_SessionPersist, ABadLineInTheMiddleIsRefused)
{
    //! The torn-tail repair is for the END of the file only. Damage anywhere else means this is not a log
    //! this build wrote, and `sessionlog.h`'s rule stands: refuse the whole file rather than load a
    //! partial history that would silently differ from what the model saw.
    const SessionLog log = logWith(QStringLiteral("first question"), 2);
    QByteArray jsonl = log.toJsonLines();
    const int secondLine = jsonl.indexOf('\n') + 1;
    jsonl.insert(secondLine, "{not json at all}\n");

    StoredSession session;
    QString problem;
    bool torn = false;
    EXPECT_FALSE(parseStoredSession(UUID_A, jsonl, session, &problem, &torn));
    EXPECT_FALSE(problem.isEmpty());
}

TEST(AgentHarness_SessionPersist, RoundTripThroughRealFiles)
{
    TempSessions temp;

    const SessionLog first = logWith(QStringLiteral("first question"), 1);
    const SessionLog second = logWith(QStringLiteral("second question"), 3);

    //! Written the way the live writer writes: one line per event, as they happen.
    for (const SessionEvent& e : first.events()) {
        QString problem;
        ASSERT_TRUE(appendSessionLine(temp.path(), UUID_A, jsonLine(e), &problem)) << problem.toStdString();
    }
    for (const SessionEvent& e : second.events()) {
        QString problem;
        ASSERT_TRUE(appendSessionLine(temp.path(), UUID_B, jsonLine(e), &problem)) << problem.toStdString();
    }

    QVector<QString> problems;
    const QVector<StoredSession> loaded = loadStoredSessions(temp.path(), &problems);
    EXPECT_TRUE(problems.isEmpty());

    ASSERT_EQ(loaded.size(), 2);
    //! OLDEST FIRST, which is the order the list draws them in.
    EXPECT_EQ(loaded[0].id, UUID_A);
    EXPECT_EQ(loaded[0].title, QStringLiteral("first question"));
    EXPECT_EQ(loaded[0].events, first.size());
    EXPECT_EQ(loaded[1].id, UUID_B);
    EXPECT_EQ(loaded[1].events, second.size());

    //! And what came off disk projects to what went in: this is the claim the whole feature rests on.
    SessionLog restored;
    QString problem;
    ASSERT_TRUE(SessionLog::fromJsonLines(loaded[0].payload, restored, &problem)) << problem.toStdString();
    EXPECT_EQ(restored.size(), first.size());
    EXPECT_EQ(restored.deriveMessages().size(), first.deriveMessages().size());
}

TEST(AgentHarness_SessionPersist, AMissingDirectoryIsAFirstRunNotAProblem)
{
    QVector<QString> problems;
    const QVector<StoredSession> loaded = loadStoredSessions(
        QDir::temp().filePath(QStringLiteral("agentharness-does-not-exist-12345")), &problems);

    EXPECT_TRUE(loaded.isEmpty());
    EXPECT_TRUE(problems.isEmpty());
}

TEST(AgentHarness_SessionPersist, ForeignFilesAreReportedNotSilentlySkipped)
{
    TempSessions temp;
    ASSERT_TRUE(QDir().mkpath(temp.path()));

    //! A user's own note in the directory: NOT ours, and not a problem to report - the listing only ever
    //! looks at `*.jsonl`, which is a boundary rather than a filter that happens to be here.
    QFile foreign(temp.path() + QStringLiteral("/notes.txt"));
    ASSERT_TRUE(foreign.open(QIODevice::WriteOnly));
    foreign.write("hello");
    foreign.close();

    //! This one, on the other hand, LOOKS like a conversation and is not one: it is named and reported.
    QFile badName(temp.path() + QStringLiteral("/abc.jsonl"));
    ASSERT_TRUE(badName.open(QIODevice::WriteOnly));
    badName.write("{\"type\":\"user/message\"}\n");
    badName.close();

    QVector<QString> problems;
    const QVector<StoredSession> loaded = loadStoredSessions(temp.path(), &problems);

    EXPECT_TRUE(loaded.isEmpty());
    ASSERT_EQ(problems.size(), 1);
    EXPECT_TRUE(problems.first().contains(QStringLiteral("abc.jsonl")));
}

TEST(AgentHarness_SessionPersist, AppendingRefusesAnIdThatWouldEscapeTheDirectory)
{
    TempSessions temp;
    QString problem;
    EXPECT_FALSE(appendSessionLine(temp.path(), QStringLiteral("../escape"), QByteArray("x"), &problem));
    EXPECT_FALSE(problem.isEmpty());
    EXPECT_FALSE(QFile::exists(temp.path() + QStringLiteral("/../escape.jsonl")));
}

TEST(AgentHarness_SessionPersist, TheWriterCreatesTheDirectoryOnFirstUse)
{
    TempSessions temp;
    EXPECT_FALSE(QDir(temp.path()).exists());

    QString problem;
    const SessionLog log = logWith(QStringLiteral("first question"));
    for (const SessionEvent& e : log.events()) {
        ASSERT_TRUE(appendSessionLine(temp.path(), UUID_A, jsonLine(e), &problem)) << problem.toStdString();
    }

    EXPECT_TRUE(QDir(temp.path()).exists());
    EXPECT_TRUE(QFile::exists(temp.path() + QStringLiteral("/") + UUID_A + QStringLiteral(".jsonl")));
}

//! ── Deleting (第 144 条) ──────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SessionPersist, DeletingRemovesTheFileAndTheRow)
{
    TempSessions temp;
    QString problem;

    //! ⚠️ NAMED LOCALS, not `logWith(...).events()` inline: the log would be a TEMPORARY and `events()`
    //! returns a reference into it, so the loop would iterate freed memory - measured 2026-10-16, that is a
    //! crash with no assertion message at all.
    const SessionLog first = logWith(QStringLiteral("first question"));
    const SessionLog second = logWith(QStringLiteral("second question"));

    for (const SessionEvent& e : first.events()) {
        ASSERT_TRUE(appendSessionLine(temp.path(), UUID_A, jsonLine(e), &problem)) << problem.toStdString();
    }
    for (const SessionEvent& e : second.events()) {
        ASSERT_TRUE(appendSessionLine(temp.path(), UUID_B, jsonLine(e), &problem)) << problem.toStdString();
    }
    ASSERT_EQ(loadStoredSessions(temp.path()).size(), 2);

    ASSERT_TRUE(removeStoredSession(temp.path(), UUID_A, &problem)) << problem.toStdString();

    EXPECT_FALSE(QFile::exists(temp.path() + QStringLiteral("/") + UUID_A + QStringLiteral(".jsonl")));
    const QVector<StoredSession> left = loadStoredSessions(temp.path());
    ASSERT_EQ(left.size(), 1);
    EXPECT_EQ(left.first().id, UUID_B);
}

TEST(AgentHarness_SessionPersist, DeletingSomethingThatIsAlreadyGoneIsSuccess)
{
    //! ⚠️ The caller asked for a STATE ("this conversation does not exist"), and it holds. Refusing here would
    //! leave a row in the list that can never be removed - which is what a user hits after deleting a file by
    //! hand while the app is running.
    TempSessions temp;
    QString problem;
    EXPECT_TRUE(removeStoredSession(temp.path(), UUID_A, &problem)) << problem.toStdString();
    EXPECT_TRUE(problem.isEmpty());
}

TEST(AgentHarness_SessionPersist, DeletingRefusesAnIdThatWouldEscapeTheDirectory)
{
    //! ⛔ THE SAME PATH RULE AS WRITING, and here it is a DELETE: an id with a `/` or `..` in it would remove a
    //! file outside the directory this code owns.
    TempSessions temp;
    ASSERT_TRUE(QDir().mkpath(temp.path()));
    const QString bystander = temp.path() + QStringLiteral("/../agentharness-bystander.jsonl");
    QFile file(bystander);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("do not delete me\n");
    file.close();

    QString problem;
    EXPECT_FALSE(removeStoredSession(temp.path(), QStringLiteral("../agentharness-bystander"), &problem));
    EXPECT_FALSE(problem.isEmpty());
    EXPECT_TRUE(QFile::exists(bystander));

    QFile::remove(bystander);
}
