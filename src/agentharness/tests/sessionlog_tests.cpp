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

#include <QJsonArray>
#include <QJsonObject>

#include "agentharness/qml/MuseScore/AgentHarness/sessionlog.h"

using namespace muse::agentharness;

namespace {
QJsonObject data(const std::initializer_list<QPair<QString, QJsonValue>>& pairs)
{
    QJsonObject o;
    for (const auto& p : pairs) {
        o.insert(p.first, p.second);
    }
    return o;
}

QJsonObject toolCall(const QString& id, const QString& name, const QString& args)
{
    QJsonObject fn;
    fn.insert(QStringLiteral("name"), name);
    fn.insert(QStringLiteral("arguments"), args);

    QJsonObject call;
    call.insert(QStringLiteral("id"), id);
    call.insert(QStringLiteral("type"), QStringLiteral("function"));
    call.insert(QStringLiteral("function"), fn);
    return call;
}
} // namespace

//! ── The log itself ────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SessionLog, SeqIsTheIndexAndIsAssignedNotSupplied)
{
    //! `seq === index` is the invariant every other structure relies on. It is assigned by the log so
    //! that no caller can choose it - a caller-chosen sequence number is a caller-chosen ordering bug.
    SessionLog log;
    EXPECT_EQ(log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 1 } })), 0u);
    EXPECT_EQ(log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("hi") } })), 1u);
    EXPECT_EQ(log.append(SessionEvent::TURN_END, data({ { QStringLiteral("turn"), 1 } })), 2u);

    ASSERT_EQ(log.size(), 3);
    for (int i = 0; i < log.size(); ++i) {
        EXPECT_EQ(log.events()[i].seq, quint64(i));
    }
}

TEST(AgentHarness_SessionLog, TurnAndStepAreDerivedFromTheLog)
{
    SessionLog log;
    EXPECT_EQ(log.currentTurn(), -1) << "nothing recorded yet";
    EXPECT_FALSE(log.isTurnOpen());

    log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 1 } }));
    log.append(SessionEvent::STEP_START, data({ { QStringLiteral("turn"), 1 }, { QStringLiteral("step"), 1 } }));
    EXPECT_EQ(log.currentTurn(), 1);
    EXPECT_EQ(log.currentStep(), 1);
    EXPECT_TRUE(log.isTurnOpen());

    log.append(SessionEvent::TURN_END, data({ { QStringLiteral("turn"), 1 } }));
    EXPECT_FALSE(log.isTurnOpen());

    log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 2 } }));
    EXPECT_EQ(log.currentTurn(), 2) << "the last turn/start wins";
    EXPECT_TRUE(log.isTurnOpen());
}

TEST(AgentHarness_SessionLog, AnUnclosedTurnIsVisible)
{
    //! This is the state a crash leaves behind. A reader must be able to see it, because "the log ends
    //! mid-turn" is the difference between "we were interrupted" and "we finished".
    SessionLog log;
    log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 1 } }));
    log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("hi") } }));
    EXPECT_TRUE(log.isTurnOpen());
}

//! ── The projection ────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SessionLog, MessagesAreDerivedInOrder)
{
    SessionLog log;
    log.append(SessionEvent::SYSTEM_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("SYS") } }));
    log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 1 } }));
    log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("hello") } }));
    log.append(SessionEvent::ASSISTANT_MESSAGE,
               data({ { QStringLiteral("content"), QStringLiteral("hi there") } }));

    const QVector<WireMessage> msgs = log.deriveMessages();
    ASSERT_EQ(msgs.size(), 3);
    EXPECT_EQ(msgs[0].role, QStringLiteral("system"));
    EXPECT_EQ(msgs[0].content, QStringLiteral("SYS"));
    EXPECT_EQ(msgs[1].role, QStringLiteral("user"));
    EXPECT_EQ(msgs[1].content, QStringLiteral("hello"));
    EXPECT_EQ(msgs[2].role, QStringLiteral("assistant"));
    EXPECT_EQ(msgs[2].content, QStringLiteral("hi there"));
}

TEST(AgentHarness_SessionLog, OnlyTheLastSystemMessageSurvives)
{
    //! The system prompt is ONE thing. Emitting two system messages in one request forces the model to
    //! guess which governs, so a later prompt replaces the earlier text rather than stacking on it.
    SessionLog log;
    log.append(SessionEvent::SYSTEM_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("OLD") } }));
    log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("q") } }));
    log.append(SessionEvent::SYSTEM_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("NEW") } }));

    const QVector<WireMessage> msgs = log.deriveMessages();
    ASSERT_EQ(msgs.size(), 2);
    EXPECT_EQ(msgs[0].role, QStringLiteral("system"));
    EXPECT_EQ(msgs[0].content, QStringLiteral("NEW"));
    EXPECT_EQ(msgs[1].role, QStringLiteral("user"));
}

TEST(AgentHarness_SessionLog, ToolResultsPairWithTheirCalls)
{
    SessionLog log;
    log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("do it") } }));

    QJsonArray calls;
    calls.append(toolCall(QStringLiteral("call_1"), QStringLiteral("score_overview"), QStringLiteral("{}")));
    log.append(SessionEvent::ASSISTANT_MESSAGE,
               data({ { QStringLiteral("content"), QString() }, { QStringLiteral("toolCalls"), calls } }));
    log.append(SessionEvent::TOOL_CALL,
               data({ { QStringLiteral("callId"), QStringLiteral("call_1") },
                      { QStringLiteral("name"), QStringLiteral("score_overview") } }));
    log.append(SessionEvent::TOOL_RESULT,
               data({ { QStringLiteral("callId"), QStringLiteral("call_1") },
                      { QStringLiteral("content"), QStringLiteral("2 measures") } }));

    const QVector<WireMessage> msgs = log.deriveMessages();
    ASSERT_EQ(msgs.size(), 3);
    EXPECT_EQ(msgs[1].role, QStringLiteral("assistant"));
    EXPECT_EQ(msgs[1].toolCalls.size(), 1);
    EXPECT_EQ(msgs[2].role, QStringLiteral("tool"));
    EXPECT_EQ(msgs[2].toolCallId, QStringLiteral("call_1"));
    EXPECT_EQ(msgs[2].content, QStringLiteral("2 measures"));
}

TEST(AgentHarness_SessionLog, OrphanToolResultsAreDropped)
{
    //! ⛔ The wire format pairs a result to a call by id. A result whose call is not in the history
    //! makes the WHOLE request invalid - so it must not be emitted, however it got into the log.
    //! This is the projection's job: refuse to produce a request that cannot be sent.
    SessionLog log;
    log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("q") } }));
    log.append(SessionEvent::TOOL_RESULT,
               data({ { QStringLiteral("callId"), QStringLiteral("ghost") },
                      { QStringLiteral("content"), QStringLiteral("orphan") } }));

    const QVector<WireMessage> msgs = log.deriveMessages();
    ASSERT_EQ(msgs.size(), 1);
    EXPECT_EQ(msgs[0].role, QStringLiteral("user")) << "the orphan must not appear";
}

TEST(AgentHarness_SessionLog, AnEmptyAssistantTurnIsNotEmitted)
{
    //! Neither text nor tool calls: nothing the model needs to see, and emitting it adds an empty turn
    //! to the history for no reason.
    SessionLog log;
    log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("q") } }));
    log.append(SessionEvent::ASSISTANT_MESSAGE, data({ { QStringLiteral("content"), QString() } }));

    const QVector<WireMessage> msgs = log.deriveMessages();
    ASSERT_EQ(msgs.size(), 1);
}

TEST(AgentHarness_SessionLog, LogOnlyEventsDoNotReachTheModel)
{
    //! turn/step markers, attempts and request headers are facts about the *run*, not things the model
    //! said or was told. If any of them leaked into the history the request would differ from what the
    //! model actually saw, and replay would stop reproducing the conversation.
    SessionLog log;
    log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 1 } }));
    log.append(SessionEvent::STEP_START, data({ { QStringLiteral("turn"), 1 }, { QStringLiteral("step"), 1 } }));
    log.append(SessionEvent::REQUEST_HEADER, data({ { QStringLiteral("model"), QStringLiteral("m") } }));
    log.append(SessionEvent::REQUEST_CONTEXT, data({ { QStringLiteral("route"), QStringLiteral("r") } }));
    log.append(SessionEvent::ASSISTANT_ATTEMPT, data({ { QStringLiteral("problem"), QStringLiteral("timeout") } }));
    log.append(SessionEvent::STEP_END, data({ { QStringLiteral("turn"), 1 }, { QStringLiteral("step"), 1 } }));
    log.append(SessionEvent::TURN_END, data({ { QStringLiteral("turn"), 1 } }));

    EXPECT_TRUE(log.deriveMessages().isEmpty()) << "none of these are model-visible";
}

TEST(AgentHarness_SessionLog, AssistantContentIsNeverNullOnTheWire)
{
    //! A tool-call-only turn has no text. Some gateways reject `content: null`, so the agreed spelling
    //! is the empty string - and this is exactly the turn where it would otherwise be null.
    WireMessage m;
    m.role = QStringLiteral("assistant");
    m.content = QString();
    m.toolCalls.append(toolCall(QStringLiteral("c"), QStringLiteral("t"), QStringLiteral("{}")));

    const QJsonObject wire = m.toWire();
    ASSERT_TRUE(wire.contains(QStringLiteral("content")));
    EXPECT_TRUE(wire.value(QStringLiteral("content")).isString()) << "must be a string, not null";
    EXPECT_TRUE(wire.value(QStringLiteral("tool_calls")).isArray());
}

TEST(AgentHarness_SessionLog, ReasoningIsCarriedBackOnAssistantTurns)
{
    //! DeepSeek requires `reasoning_content` to be echoed on tool-call turns in thinking mode; it is
    //! ignored elsewhere. Dropping it makes those turns fail on the next request.
    WireMessage m;
    m.role = QStringLiteral("assistant");
    m.content = QStringLiteral("ok");
    m.reasoning = QStringLiteral("thinking...");

    const QJsonObject wire = m.toWire();
    EXPECT_EQ(wire.value(QStringLiteral("reasoning_content")).toString(), QStringLiteral("thinking..."));
}

//! ── Persistence ───────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SessionLog, JsonLinesRoundTrip)
{
    SessionLog log;
    log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 1 } }));
    log.append(SessionEvent::USER_MESSAGE, data({ { QStringLiteral("content"), QStringLiteral("a \"quoted\" line\nwith a newline") } }));
    log.append(SessionEvent::TURN_END, data({ { QStringLiteral("turn"), 1 } }));

    const QByteArray lines = log.toJsonLines();

    //! ⛔ ONE event per line, and no event may contain a newline of its own. Compact serialization is
    //! therefore correctness, not a size optimisation: a pretty-printed event would split into several
    //! lines and the log would no longer be parseable as JSONL at all.
    const QList<QByteArray> split = lines.split('\n');
    int nonEmpty = 0;
    for (const QByteArray& l : split) {
        if (!l.trimmed().isEmpty()) {
            ++nonEmpty;
        }
    }
    EXPECT_EQ(nonEmpty, 3) << "each event must occupy exactly one line";

    SessionLog reloaded;
    QString error;
    ASSERT_TRUE(SessionLog::fromJsonLines(lines, reloaded, &error)) << error.toStdString();
    ASSERT_EQ(reloaded.size(), 3);
    EXPECT_EQ(reloaded.events()[1].data.value(QStringLiteral("content")).toString(),
              QStringLiteral("a \"quoted\" line\nwith a newline")) << "the newline must survive inside the field";

    //! And the projection must be identical after a round trip - that is what makes the log the single
    //! source of truth rather than a cache of one.
    EXPECT_EQ(reloaded.deriveMessages().size(), log.deriveMessages().size());
}

TEST(AgentHarness_SessionLog, AMalformedLineFailsTheWholeLoad)
{
    //! ⛔ Refuse the file rather than loading a partial log. A log that silently drops its own lines
    //! makes the derived history differ from what the model actually saw, and nothing downstream can
    //! detect that - so it is better to fail loudly and keep the file intact.
    const QByteArray bad = "{\"seq\":0,\"type\":\"turn/start\",\"data\":{}}\nNOT JSON\n";
    SessionLog log;
    QString error;
    EXPECT_FALSE(SessionLog::fromJsonLines(bad, log, &error));
    EXPECT_FALSE(error.isEmpty()) << "the reason must be reported, not swallowed";
    EXPECT_TRUE(log.isEmpty()) << "nothing may be kept from a rejected file";
}

TEST(AgentHarness_SessionLog, BlankLinesAreTolerated)
{
    //! A trailing newline is normal in a text file; it must not be treated as a malformed event.
    const QByteArray ok = "{\"seq\":0,\"type\":\"turn/start\",\"data\":{}}\n\n";
    SessionLog log;
    EXPECT_TRUE(SessionLog::fromJsonLines(ok, log, nullptr));
    EXPECT_EQ(log.size(), 1);
}

TEST(AgentHarness_SessionLog, ClearEmptiesTheLog)
{
    SessionLog log;
    log.append(SessionEvent::TURN_START, data({ { QStringLiteral("turn"), 1 } }));
    ASSERT_EQ(log.size(), 1);
    log.clear();
    EXPECT_TRUE(log.isEmpty());
    EXPECT_EQ(log.currentTurn(), -1);
}
