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

#include "agentharness/qml/MuseScore/AgentHarness/sessionstore.h"

using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS (第 141 条). The list is the first thing in this harness that can lose a
//! conversation, and every way it can go wrong looks like a working list:
//!   - a session that was never archived is gone, and the list still renders;
//!   - a row whose label describes a conversation that has since grown is plausible and wrong;
//!   - an empty slot left behind by "new session" is a row nobody can explain, and it renders too.
//! The store is a plain container, so none of this needs an application - and the FIELD's half (the live
//! log being restored into, never replaced) is judged on the real program, because that part is about the
//! loop's pointer and cannot be reached from here.

namespace {
SessionLog logWithUserMessage(const QString& text)
{
    SessionLog log;
    QJsonObject data;
    data.insert(QStringLiteral("content"), text);
    log.append(SessionEvent::USER_MESSAGE, data);
    return log;
}

SessionLog logWithSystemPromptOnly()
{
    SessionLog log;
    QJsonObject data;
    data.insert(QStringLiteral("content"), QStringLiteral("you are a notation assistant"));
    log.append(SessionEvent::SYSTEM_MESSAGE, data);
    return log;
}

bool hasLoneSurrogate(const QString& text)
{
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.isHighSurrogate()) {
            if (i + 1 >= text.size() || !text.at(i + 1).isLowSurrogate()) {
                return true;
            }
            ++i;
        } else if (c.isLowSurrogate()) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST(AgentHarness_SessionStore, SystemPromptAloneIsNotAConversation)
{
    //! ⛔ THE RULE THAT KEEPS THE LIST HONEST. Every session is seeded with the system prompt, so counting
    //! it as content would give every click on "new session" a permanent row of its own.
    EXPECT_FALSE(sessionHasContent(logWithSystemPromptOnly()));
    EXPECT_TRUE(sessionHasContent(logWithUserMessage(QStringLiteral("hello"))));
}

TEST(AgentHarness_SessionStore, TitleComesFromTheFirstUserMessage)
{
    SessionLog log = logWithUserMessage(QStringLiteral("how many measures?"));
    log.append(SessionEvent::ASSISTANT_MESSAGE, QJsonObject());

    EXPECT_EQ(sessionTitleFrom(log), QStringLiteral("how many measures?"));
}

TEST(AgentHarness_SessionStore, TitleIgnoresASystemPromptAndSaysSoWhenEmpty)
{
    EXPECT_EQ(sessionTitleFrom(logWithSystemPromptOnly()), QStringLiteral("(new session)"));
    EXPECT_EQ(sessionTitleFrom(SessionLog()), QStringLiteral("(new session)"));
}

TEST(AgentHarness_SessionStore, TitleIsOneLineWhateverWasPasted)
{
    const QString pasted = QStringLiteral("first line\n\n   second line\twith tabs\r\nthird");
    const QString title = sessionTitleFrom(logWithUserMessage(pasted));

    EXPECT_FALSE(title.contains(QLatin1Char('\n')));
    EXPECT_FALSE(title.contains(QLatin1Char('\t')));
    EXPECT_EQ(title, QStringLiteral("first line second line with tabs third"));
}

TEST(AgentHarness_SessionStore, TitleIsElidedToTheLimit)
{
    const QString long_(QStringLiteral("abcdefghij").repeated(10));
    const QString title = sessionTitleFrom(logWithUserMessage(long_), 20);

    //! 20 characters plus the ellipsis, and the ellipsis is what says "there was more".
    EXPECT_EQ(title.size(), 21);
    EXPECT_TRUE(title.endsWith(QChar(0x2026)));
    EXPECT_EQ(title.left(20), long_.left(20));
}

TEST(AgentHarness_SessionStore, TitleDoesNotSplitSurrogatePairs)
{
    //! An emoji in the first message is not a hypothesis, and half of one renders as a replacement glyph -
    //! in the one place a user sees their own words coming back.
    const QString emoji = QString::fromUtf8("\xF0\x9F\x8E\xB5");   // U+1F3B5, a surrogate pair
    const QString title = sessionTitleFrom(logWithUserMessage(emoji.repeated(6)), 5);

    EXPECT_FALSE(hasLoneSurrogate(title));
    EXPECT_TRUE(title.endsWith(QChar(0x2026)));
    //! The cut stepped back one position rather than emitting half a pair: four whole symbols, then the
    //! ellipsis.
    EXPECT_EQ(title.size(), 5);
}

TEST(AgentHarness_SessionStore, NewSessionReusesAnEmptySlot)
{
    SessionStore store;
    ASSERT_EQ(store.count(), 1);
    const QString firstId = store.at(0).id;

    //! Nothing was archived from the first conversation (it only held the system prompt), so clicking "new
    //! session" must not add a row.
    store.startNewLive();

    EXPECT_EQ(store.count(), 1);
    EXPECT_EQ(store.currentIndex(), 0);
    EXPECT_TRUE(store.currentIsEmpty());
    //! A new conversation is a new identity, even when it is the same slot.
    EXPECT_NE(store.at(0).id, firstId);
}

TEST(AgentHarness_SessionStore, NewSessionAfterContentAddsARow)
{
    SessionStore store;
    const QString firstId = store.at(0).id;
    store.archiveCurrent(QByteArray("{\"type\":\"user/message\"}\n"), 1, QStringLiteral("hello"));

    EXPECT_FALSE(store.currentIsEmpty());
    store.startNewLive();

    EXPECT_EQ(store.count(), 2);
    EXPECT_EQ(store.currentIndex(), 1);
    EXPECT_EQ(store.at(0).id, firstId);
    EXPECT_EQ(store.at(0).title, QStringLiteral("hello"));
    EXPECT_EQ(store.at(0).events, 1);
    EXPECT_FALSE(store.at(0).payload.isEmpty());
}

TEST(AgentHarness_SessionStore, SwitchingDropsTheEmptyOutgoingSlot)
{
    SessionStore store;
    store.archiveCurrent(QByteArray("A\n"), 1, QStringLiteral("first"));
    store.startNewLive();          // the second conversation, still empty

    QByteArray payload;
    ASSERT_TRUE(store.makeCurrent(0, &payload));

    //! The empty one is gone rather than left as a row called "(new session)" with zero events.
    EXPECT_EQ(store.count(), 1);
    EXPECT_EQ(store.currentIndex(), 0);
    EXPECT_EQ(payload, QByteArray("A\n"));
    //! ⛔ And it is NOT "empty" even though it is live and carries no payload: it is a conversation with
    //! content that happens to be in front of the user right now. Reading emptiness off the payload is
    //! exactly the mistake that lost conversations - see `SwitchingKeepsTheConversationThatWasLeft`.
    EXPECT_FALSE(store.currentIsEmpty());
    EXPECT_TRUE(store.at(0).payload.isEmpty());   // the caller holds the payload; live entries do not
}

TEST(AgentHarness_SessionStore, SwitchingKeepsTheConversationThatWasLeft)
{
    //! ⛔⛔ THIS IS THE REGRESSION GUARD FOR A REAL BUG, caught by this very test the first time it ran.
    //! The store's first version decided "is this an unused slot?" from `payload.isEmpty()` - and the LIVE
    //! entry's payload is empty by construction, both for a conversation nobody has said anything in AND
    //! for one that was restored from the list a moment ago. So switching BACK to a conversation and then
    //! away from it again dropped the row: the count came back 1 where the test expected 2, and the
    //! conversation was gone with no error anywhere. The list would have kept rendering.
    SessionStore store;
    store.archiveCurrent(QByteArray("A\n"), 1, QStringLiteral("first"));
    store.startNewLive();
    store.archiveCurrent(QByteArray("B\n"), 2, QStringLiteral("second"));
    store.startNewLive();

    QByteArray payload;
    ASSERT_TRUE(store.makeCurrent(0, &payload));
    EXPECT_EQ(store.count(), 2);
    EXPECT_EQ(store.at(1).payload, QByteArray("B\n"));   // the one just left is still there

    //! What the FIELD does next, and the reason the row survives: the conversation is live now, so its
    //! payload comes back the moment it is archived again - there is only ever one copy of a conversation,
    //! and while it is live that copy is the live log itself.
    store.archiveCurrent(QByteArray("A\n"), 1, QStringLiteral("first"));

    //! And going back the other way hands over what was left behind - and keeps BOTH rows.
    ASSERT_TRUE(store.makeCurrent(1, &payload));
    EXPECT_EQ(payload, QByteArray("B\n"));
    EXPECT_EQ(store.count(), 2);
    EXPECT_EQ(store.at(0).payload, QByteArray("A\n"));
}

TEST(AgentHarness_SessionStore, MakeCurrentRefusesTheLiveOneAndOutOfRange)
{
    SessionStore store;
    store.archiveCurrent(QByteArray("A\n"), 1, QStringLiteral("first"));
    store.startNewLive();

    QByteArray payload;
    EXPECT_FALSE(store.makeCurrent(1, &payload));    // already live
    EXPECT_FALSE(store.makeCurrent(-1, &payload));
    EXPECT_FALSE(store.makeCurrent(99, &payload));

    //! Refusals change nothing.
    EXPECT_EQ(store.count(), 2);
    EXPECT_EQ(store.currentIndex(), 1);
    EXPECT_TRUE(store.currentIsEmpty());
}

TEST(AgentHarness_SessionStore, OutOfRangeRowsAreEmptyRatherThanACrash)
{
    SessionStore store;
    const SessionEntry missing = store.at(7);

    EXPECT_TRUE(missing.id.isEmpty());
    EXPECT_TRUE(missing.title.isEmpty());
    EXPECT_TRUE(missing.payload.isEmpty());
    EXPECT_EQ(missing.events, 0);
}

TEST(AgentHarness_SessionStore, ConversationsSurviveARoundTripThroughTheStore)
{
    //! The claim the whole file exists for, at the level it can be checked without a window: two real logs
    //! go into the store, come back out, and project to what they projected to before.
    SessionLog first = logWithUserMessage(QStringLiteral("first question"));
    first.append(SessionEvent::ASSISTANT_MESSAGE, QJsonObject());
    SessionLog second = logWithUserMessage(QStringLiteral("second question"));
    second.append(SessionEvent::ASSISTANT_MESSAGE, QJsonObject());
    second.append(SessionEvent::USER_MESSAGE, QJsonObject());

    SessionStore store;
    store.archiveCurrent(first.toJsonLines(), first.size(), sessionTitleFrom(first));
    store.startNewLive();
    store.archiveCurrent(second.toJsonLines(), second.size(), sessionTitleFrom(second));
    store.startNewLive();

    QByteArray payload;
    ASSERT_TRUE(store.makeCurrent(0, &payload));
    SessionLog restoredFirst;
    QString problem;
    ASSERT_TRUE(SessionLog::fromJsonLines(payload, restoredFirst, &problem)) << problem.toStdString();
    EXPECT_EQ(restoredFirst.size(), first.size());
    EXPECT_EQ(restoredFirst.deriveMessages().size(), first.deriveMessages().size());
    EXPECT_EQ(sessionTitleFrom(restoredFirst), QStringLiteral("first question"));

    ASSERT_TRUE(store.makeCurrent(1, &payload));
    SessionLog restoredSecond;
    ASSERT_TRUE(SessionLog::fromJsonLines(payload, restoredSecond, &problem)) << problem.toStdString();
    EXPECT_EQ(restoredSecond.size(), second.size());
    EXPECT_EQ(sessionTitleFrom(restoredSecond), QStringLiteral("second question"));
}

//! ── Renaming and deleting (第 144 条) ─────────────────────────────────────────────────────────────

TEST(AgentHarness_SessionStore, ANameTheUserGaveBeatsTheFirstMessage)
{
    SessionLog log = logWithUserMessage(QStringLiteral("how many measures?"));
    QJsonObject name;
    name.insert(QStringLiteral("content"), QStringLiteral("Bar counting"));
    log.append(SessionEvent::SESSION_TITLE, name);

    EXPECT_EQ(sessionTitleFrom(log), QStringLiteral("Bar counting"));
}

TEST(AgentHarness_SessionStore, TheLastRenameWins)
{
    //! Renaming twice means the second name is the name - the alternative (the first rename sticking) would
    //! make the second attempt look like it silently failed.
    SessionLog log = logWithUserMessage(QStringLiteral("how many measures?"));
    for (const QString& name : { QStringLiteral("First name"), QStringLiteral("Second name") }) {
        QJsonObject data;
        data.insert(QStringLiteral("content"), name);
        log.append(SessionEvent::SESSION_TITLE, data);
    }

    EXPECT_EQ(sessionTitleFrom(log), QStringLiteral("Second name"));
}

TEST(AgentHarness_SessionStore, TheRenameBoxGetsTheUnelidedName)
{
    //! ⛔ `maxChars = 0` MEANS "DO NOT CUT IT", and that is what the rename box is prefilled with (第 144 条):
    //! handing an edit box a name that was already elided would truncate the real name the moment somebody
    //! pressed Save - and the user would have no way to tell that it had happened.
    const QString long_(QStringLiteral("abcdefghij").repeated(10));
    SessionLog log = logWithUserMessage(long_);
    QJsonObject name;
    name.insert(QStringLiteral("content"), long_);
    log.append(SessionEvent::SESSION_TITLE, name);

    EXPECT_EQ(sessionTitleFrom(log), elideTitle(long_));      //!< the row's label
    EXPECT_EQ(sessionTitleFrom(log, 0), long_);               //!< what the edit box gets
    EXPECT_GT(sessionTitleFrom(log, 0).size(), sessionTitleFrom(log).size());
}

TEST(AgentHarness_SessionStore, RemovingARowKeepsTheLiveEntryLive)
{
    SessionStore store;
    store.archiveCurrent(QByteArray("A\n"), 1, QStringLiteral("first"));
    store.startNewLive();
    store.archiveCurrent(QByteArray("B\n"), 2, QStringLiteral("second"));
    store.startNewLive();
    const QString liveId = store.current().id;

    //! Remove the OLDEST, which sits before the live entry: the live index has to move down with it.
    ASSERT_TRUE(store.removeAt(0));

    EXPECT_EQ(store.count(), 2);
    EXPECT_EQ(store.current().id, liveId);
    EXPECT_TRUE(store.currentIsEmpty());          //!< still the live one
    EXPECT_EQ(store.at(0).title, QStringLiteral("second"));

    //! ⛔ AND THE LIVE ENTRY ITSELF IS NOT REMOVABLE HERE: "delete the conversation I am in" is a decision
    //! about the live log, which this class does not own (`dropCurrentLive` is the caller's tool for it).
    EXPECT_FALSE(store.removeAt(store.currentIndex()));
    EXPECT_FALSE(store.removeAt(99));
    EXPECT_EQ(store.count(), 2);
}

TEST(AgentHarness_SessionStore, DroppingTheLiveConversationLeavesNoRowForIt)
{
    //! ⛔ THE DIFFERENCE FROM `startNewLive()` IS THE WHOLE POINT: that one keeps an archived entry as a row
    //! (right for "start another conversation"), while a DELETE must leave nothing behind - otherwise the
    //! conversation the user just deleted stays in the list for ever, and its file is gone.
    SessionStore store;
    store.archiveCurrent(QByteArray("A\n"), 1, QStringLiteral("first"));
    const QString deletedId = store.current().id;

    store.dropCurrentLive();

    EXPECT_EQ(store.count(), 1);
    EXPECT_EQ(store.currentIndex(), 0);
    EXPECT_NE(store.current().id, deletedId);
    EXPECT_TRUE(store.currentIsEmpty());
}

TEST(AgentHarness_SessionStore, AppendingToAStoredConversationCountsItAndKeepsTheBytesWhole)
{
    SessionStore store;
    store.archiveCurrent(QByteArray("A\n"), 1, QStringLiteral("first"));
    store.startNewLive();

    store.appendStoredLine(0, QByteArray("B"));
    store.setStoredTitle(0, QStringLiteral("Renamed"));

    EXPECT_EQ(store.at(0).events, 2);
    EXPECT_EQ(store.at(0).payload, QByteArray("A\nB\n"));
    EXPECT_EQ(store.at(0).title, QStringLiteral("Renamed"));

    //! ⛔ The newline is part of the append: a payload ending mid-line is the tear `parseStoredSession` has to
    //! repair, and manufacturing one here would make the in-memory copy differ from the file it mirrors.
    EXPECT_TRUE(store.at(0).payload.endsWith('\n'));
}
