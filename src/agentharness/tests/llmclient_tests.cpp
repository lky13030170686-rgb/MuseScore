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

#include "agentharness/qml/MuseScore/AgentHarness/llmclient.h"

using namespace muse::agentharness;

namespace {
//! One streamed chunk, as the wire spells it. Built as JSON text (not as objects) so the tests exercise
//! the same path a real payload takes - a test that hands the parser an already-built object would not
//! catch a serialization assumption.
QByteArray chunk(const QString& deltaJson, const QString& finish = QString())
{
    const QString finishField = finish.isEmpty()
                                ? QStringLiteral("null")
                                : QStringLiteral("\"%1\"").arg(finish);
    return QStringLiteral("{\"choices\":[{\"delta\":%1,\"finish_reason\":%2}]}")
           .arg(deltaJson, finishField)
           .toUtf8();
}
} // namespace

//! ── SSE parsing ───────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_LlmClient, PlainTextDeltasComeThrough)
{
    bool done = false;
    QVector<StreamChunk> chunks = parseSseData(chunk(QStringLiteral("{\"content\":\"Hel\"}")), done);
    ASSERT_EQ(chunks.size(), 1);
    EXPECT_EQ(chunks[0].type, StreamChunk::Type::TextDelta);
    EXPECT_EQ(chunks[0].text, QStringLiteral("Hel"));

    chunks = parseSseData(chunk(QStringLiteral("{\"content\":\"lo\"}")), done);
    ASSERT_EQ(chunks.size(), 1);
    EXPECT_EQ(chunks[0].text, QStringLiteral("lo"));
}

TEST(AgentHarness_LlmClient, DoneSentinelIsRecognizedAndEmitsNothing)
{
    //! `[DONE]` carries no chunk of its own - it is the boundary. Emitting a chunk for it would put an
    //! empty entry in the transcript.
    bool done = false;
    const QVector<StreamChunk> chunks = parseSseData("[DONE]", done);
    EXPECT_TRUE(done);
    EXPECT_TRUE(chunks.isEmpty());
}

TEST(AgentHarness_LlmClient, KeepAliveFramesAreNotErrors)
{
    //! Comments and blank frames keep the connection alive. Treating them as malformed would abort a
    //! perfectly healthy stream.
    bool done = false;
    EXPECT_TRUE(parseSseData("", done).isEmpty());
    EXPECT_FALSE(done);
    EXPECT_TRUE(parseSseData("   ", done).isEmpty());
    EXPECT_FALSE(done);
}

TEST(AgentHarness_LlmClient, ReasoningDeltaIsSeparateFromText)
{
    bool done = false;
    const QVector<StreamChunk> chunks = parseSseData(chunk(QStringLiteral("{\"reasoning_content\":\"thin\"}")), done);
    ASSERT_EQ(chunks.size(), 1);
    EXPECT_EQ(chunks[0].type, StreamChunk::Type::ReasoningDelta);
    EXPECT_EQ(chunks[0].text, QStringLiteral("thin"));
}

TEST(AgentHarness_LlmClient, AnEmptyReasoningFirstChunkOpensNothing)
{
    //! ⛔ The first chunk of a thinking response carries `reasoning_content: ""` as a marker. Opening a
    //! reasoning block on it produces an empty thinking section - and in a transcript, a stray blank
    //! line. Nothing is emitted, which is the correct reading of "no content yet".
    bool done = false;
    const QVector<StreamChunk> chunks = parseSseData(chunk(QStringLiteral("{\"role\":\"assistant\",\"content\":null,\"reasoning_content\":\"\"}")), done);
    EXPECT_TRUE(chunks.isEmpty()) << "an empty marker is not content";
}

TEST(AgentHarness_LlmClient, NullContentIsNotText)
{
    bool done = false;
    EXPECT_TRUE(parseSseData(chunk(QStringLiteral("{\"content\":null}")), done).isEmpty());
}

TEST(AgentHarness_LlmClient, FinishReasonIsReported)
{
    bool done = false;
    const QVector<StreamChunk> chunks = parseSseData(chunk(QStringLiteral("{}"), QStringLiteral("tool_calls")), done);
    ASSERT_EQ(chunks.size(), 1);
    EXPECT_EQ(chunks[0].type, StreamChunk::Type::Finish);
    EXPECT_EQ(chunks[0].finishReason, QStringLiteral("tool_calls"));
}

TEST(AgentHarness_LlmClient, UsageIsReported)
{
    bool done = false;
    const QByteArray payload =
        "{\"choices\":[],\"usage\":{\"prompt_tokens\":28,\"completion_tokens\":6}}";
    const QVector<StreamChunk> chunks = parseSseData(payload, done);
    ASSERT_EQ(chunks.size(), 1);
    EXPECT_EQ(chunks[0].type, StreamChunk::Type::Usage);
    EXPECT_EQ(chunks[0].promptTokens, 28);
    EXPECT_EQ(chunks[0].completionTokens, 6);
}

TEST(AgentHarness_LlmClient, InBandErrorsAreNotIgnored)
{
    //! ⛔ The stream can carry `{"error": ...}` INSTEAD of a chunk. A client that only looks at
    //! `choices` ignores it completely, and the visible symptom is "the model said nothing at all".
    //! DSH's own chat-completions adapter has this gap; it is closed here on purpose.
    bool done = false;
    const QByteArray payload = "{\"error\":{\"message\":\"rate limited\",\"type\":\"rate_limit\"}}";
    const QVector<StreamChunk> chunks = parseSseData(payload, done);
    ASSERT_EQ(chunks.size(), 1);
    EXPECT_EQ(chunks[0].type, StreamChunk::Type::Malformed);
    EXPECT_TRUE(chunks[0].problem.contains(QStringLiteral("rate limited"))) << chunks[0].problem.toStdString();
}

TEST(AgentHarness_LlmClient, GarbageIsReportedNotDropped)
{
    //! A payload that cannot be parsed must produce a Malformed chunk. Dropping it would make a broken
    //! stream look like a stream that simply ended.
    bool done = false;
    const QVector<StreamChunk> chunks = parseSseData("{not json", done);
    ASSERT_EQ(chunks.size(), 1);
    EXPECT_EQ(chunks[0].type, StreamChunk::Type::Malformed);
    EXPECT_FALSE(chunks[0].problem.isEmpty());
}

//! ── Tool-call reassembly ──────────────────────────────────────────────────────────────────────

TEST(AgentHarness_LlmClient, ToolCallArgumentsAreConcatenatedAcrossDeltas)
{
    //! The captured shape from a real stream: the first delta carries id and name with empty arguments,
    //! the rest carry fragments only.
    ToolCallAssembler asm_;
    asm_.addDelta(0, QStringLiteral("call_00_x"), QStringLiteral("get_weather"), QString());
    asm_.addDelta(0, QString(), QString(), QStringLiteral("{\"city\""));
    asm_.addDelta(0, QString(), QString(), QStringLiteral(": \"Paris\"}"));

    const QVector<QJsonObject> calls = asm_.calls();
    ASSERT_EQ(calls.size(), 1);
    EXPECT_EQ(calls[0].value(QStringLiteral("id")).toString(), QStringLiteral("call_00_x"));
    EXPECT_EQ(calls[0].value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("get_weather"));
    EXPECT_EQ(calls[0].value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments")).toString(),
              QStringLiteral("{\"city\": \"Paris\"}"));
}

TEST(AgentHarness_LlmClient, EmptyRepeatsDoNotEraseTheIdentity)
{
    //! ⛔ Continuation deltas repeat `id`/`name` as `''` (and sometimes `null`) to mean "unchanged".
    //! Treating those as values replaces a good name with an empty one, and the call then fails as
    //! "unknown tool" with nothing in the log to explain it.
    ToolCallAssembler asm_;
    asm_.addDelta(0, QStringLiteral("call_1"), QStringLiteral("glob"), QString());
    asm_.addDelta(0, QString(), QString(), QStringLiteral("{}"));

    const QVector<QJsonObject> calls = asm_.calls();
    ASSERT_EQ(calls.size(), 1);
    EXPECT_EQ(calls[0].value(QStringLiteral("id")).toString(), QStringLiteral("call_1"));
    EXPECT_EQ(calls[0].value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("glob")) << "the name must survive an empty repeat";
}

TEST(AgentHarness_LlmClient, ParallelCallsKeepSeparateIdentity)
{
    ToolCallAssembler asm_;
    asm_.addDelta(0, QStringLiteral("a"), QStringLiteral("first"), QStringLiteral("{"));
    asm_.addDelta(1, QStringLiteral("b"), QStringLiteral("second"), QStringLiteral("{"));
    asm_.addDelta(0, QString(), QString(), QStringLiteral("}"));
    asm_.addDelta(1, QString(), QString(), QStringLiteral("}"));

    const QVector<QJsonObject> calls = asm_.calls();
    ASSERT_EQ(calls.size(), 2);
    EXPECT_EQ(calls[0].value(QStringLiteral("id")).toString(), QStringLiteral("a"));
    EXPECT_EQ(calls[1].value(QStringLiteral("id")).toString(), QStringLiteral("b"));
    EXPECT_EQ(calls[0].value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments")).toString(),
              QStringLiteral("{}"));
    EXPECT_EQ(calls[1].value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments")).toString(),
              QStringLiteral("{}"));
}

TEST(AgentHarness_LlmClient, CallsAreReturnedInIndexOrder)
{
    //! The model's ordering IS the index order, and tool results must be committed in model order even
    //! when execution overlaps. Sorting here means no caller has to think about arrival order.
    ToolCallAssembler asm_;
    asm_.addDelta(2, QStringLiteral("third"), QStringLiteral("c"), QStringLiteral("{}"));
    asm_.addDelta(0, QStringLiteral("first"), QStringLiteral("a"), QStringLiteral("{}"));
    asm_.addDelta(1, QStringLiteral("second"), QStringLiteral("b"), QStringLiteral("{}"));

    const QVector<QJsonObject> calls = asm_.calls();
    ASSERT_EQ(calls.size(), 3);
    EXPECT_EQ(calls[0].value(QStringLiteral("id")).toString(), QStringLiteral("first"));
    EXPECT_EQ(calls[1].value(QStringLiteral("id")).toString(), QStringLiteral("second"));
    EXPECT_EQ(calls[2].value(QStringLiteral("id")).toString(), QStringLiteral("third"));
}

TEST(AgentHarness_LlmClient, ANegativeIndexIsIgnored)
{
    //! Defensive: a delta without an index cannot be attributed to any call, and inventing index 0
    //! would silently merge it into the first call.
    ToolCallAssembler asm_;
    asm_.addDelta(-1, QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("{}"));
    EXPECT_TRUE(asm_.isEmpty());
}

TEST(AgentHarness_LlmClient, ResetClearsAssembledCalls)
{
    ToolCallAssembler asm_;
    asm_.addDelta(0, QStringLiteral("a"), QStringLiteral("t"), QStringLiteral("{}"));
    ASSERT_FALSE(asm_.isEmpty());
    asm_.reset();
    EXPECT_TRUE(asm_.isEmpty());
}

//! ── Request body ──────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_LlmClient, RequestBodyCarriesTheWireContract)
{
    //! This body IS a contract. A field that quietly disappears is the kind of bug that only shows up
    //! as "the token counter is always zero" or "thinking never turns on", months later.
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMsg.insert(QStringLiteral("content"), QStringLiteral("hi"));
    messages.append(userMsg);

    QJsonArray tools;
    QJsonObject tool;
    tool.insert(QStringLiteral("type"), QStringLiteral("function"));
    tools.append(tool);

    const QJsonObject body = buildChatRequest(QStringLiteral("deepseek-chat"), messages, tools, true, QStringLiteral("high"));

    EXPECT_EQ(body.value(QStringLiteral("model")).toString(), QStringLiteral("deepseek-chat"));
    EXPECT_TRUE(body.value(QStringLiteral("stream")).toBool());
    EXPECT_TRUE(body.value(QStringLiteral("stream_options")).toObject()
                .value(QStringLiteral("include_usage")).toBool())
        << "without include_usage the stream never reports tokens";
    EXPECT_EQ(body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
              QStringLiteral("enabled")) << "the thinking switch is TOP LEVEL, not inside extra_body";
    EXPECT_EQ(body.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));
    EXPECT_TRUE(body.contains(QStringLiteral("tools")));
}

TEST(AgentHarness_LlmClient, EffortIsOmittedWhenThinkingIsOff)
{
    //! Sending an effort level with thinking disabled is a contradictory request; the wire should not
    //! carry a field that means nothing.
    const QJsonObject body = buildChatRequest(QStringLiteral("m"), QJsonArray(), QJsonArray(), false, QStringLiteral("high"));
    EXPECT_EQ(body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
              QStringLiteral("disabled"));
    EXPECT_FALSE(body.contains(QStringLiteral("reasoning_effort")));
}

TEST(AgentHarness_LlmClient, ToolsAreOmittedWhenThereAreNone)
{
    //! An empty `tools: []` is not the same as no `tools` field: some gateways treat the empty array as
    //! "tool use is enabled with zero tools" and reject the request.
    const QJsonObject body = buildChatRequest(QStringLiteral("m"), QJsonArray(), QJsonArray(), false, QString());
    EXPECT_FALSE(body.contains(QStringLiteral("tools")));
}
