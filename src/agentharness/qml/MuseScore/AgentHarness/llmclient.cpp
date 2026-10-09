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
#include "llmclient.h"

#include <QJsonDocument>
#include <QJsonParseError>

using namespace muse::agentharness;

namespace {
//! Read a string that may be absent, `null`, or empty - all three mean "no text here". The wire uses
//! all three spellings in practice, and distinguishing them would be inventing a difference the
//! protocol does not have.
QString optionalString(const QJsonValue& v)
{
    if (v.isString()) {
        return v.toString();
    }
    return QString();
}
} // namespace

QVector<StreamChunk> muse::agentharness::parseSseData(const QByteArray& data, bool& done)
{
    done = false;
    QVector<StreamChunk> chunks;

    const QByteArray trimmed = data.trimmed();
    if (trimmed.isEmpty()) {
        //! A keep-alive or comment frame. Nothing to report, and emphatically not an error.
        return chunks;
    }

    if (trimmed == "[DONE]") {
        //! ⛔ Everything that arrives before `[DONE]` is a *delta*; the terminal accounting and the
        //! finish reason are only settled here. That is why `Finish` and `Usage` are emitted at this
        //! point rather than when their fields appear - a client that acts on `finish_reason` as it
        //! arrives can start the next step while the usage chunk is still in flight.
        done = true;
        return chunks;
    }

    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        StreamChunk bad;
        bad.type = StreamChunk::Type::Malformed;
        bad.problem = parseError.error != QJsonParseError::NoError
                      ? parseError.errorString()
                      : QStringLiteral("payload is not a JSON object");
        chunks.append(bad);
        return chunks;
    }

    const QJsonObject obj = doc.object();

    //! ⛔ In-band errors. The chat-completions stream can carry `{"error": {...}}` as a data payload
    //! instead of a chunk, and a client that only looks at `choices` will ignore it completely - the
    //! visible symptom is "the model said nothing at all", with no clue why. DSH's own
    //! chat-completions adapter has this gap; it is closed here deliberately.
    if (obj.contains(QStringLiteral("error"))) {
        const QJsonObject err = obj.value(QStringLiteral("error")).toObject();
        StreamChunk bad;
        bad.type = StreamChunk::Type::Malformed;
        bad.problem = QStringLiteral("server error in stream: %1")
                      .arg(err.value(QStringLiteral("message")).toString(QStringLiteral("(no message)")));
        chunks.append(bad);
        return chunks;
    }

    if (obj.contains(QStringLiteral("usage")) && !obj.value(QStringLiteral("usage")).isNull()) {
        const QJsonObject usage = obj.value(QStringLiteral("usage")).toObject();
        StreamChunk u;
        u.type = StreamChunk::Type::Usage;
        u.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
        u.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();
        chunks.append(u);
    }

    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    for (const QJsonValue& choiceValue : choices) {
        const QJsonObject choice = choiceValue.toObject();
        const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

        //! ⛔ `reasoning_content` may arrive as an EMPTY STRING on the first chunk of a thinking
        //! response. That is a marker, not content: opening a reasoning block on it produces an
        //! empty thinking section, and in some clients a stray blank line in the transcript.
        const QString reasoning = optionalString(delta.value(QStringLiteral("reasoning_content")));
        if (!reasoning.isEmpty()) {
            StreamChunk c;
            c.type = StreamChunk::Type::ReasoningDelta;
            c.text = reasoning;
            chunks.append(c);
        }

        const QString content = optionalString(delta.value(QStringLiteral("content")));
        if (!content.isEmpty()) {
            StreamChunk c;
            c.type = StreamChunk::Type::TextDelta;
            c.text = content;
            chunks.append(c);
        }

        const QJsonArray toolCalls = delta.value(QStringLiteral("tool_calls")).toArray();
        for (const QJsonValue& callValue : toolCalls) {
            const QJsonObject call = callValue.toObject();
            StreamChunk c;
            c.type = StreamChunk::Type::ToolCallDelta;
            c.index = call.value(QStringLiteral("index")).toInt(-1);
            //! `id`/`name` are only meaningful when non-empty; the assembler applies that rule, and
            //! the chunk carries them through verbatim so the rule lives in exactly one place.
            c.toolCallId = optionalString(call.value(QStringLiteral("id")));
            c.toolCallName = optionalString(call.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")));
            c.argumentsDelta = optionalString(call.value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments")));
            chunks.append(c);
        }

        const QString finish = optionalString(choice.value(QStringLiteral("finish_reason")));
        if (!finish.isEmpty()) {
            StreamChunk c;
            c.type = StreamChunk::Type::Finish;
            c.finishReason = finish;
            chunks.append(c);
        }
    }

    return chunks;
}

void ToolCallAssembler::reset()
{
    m_calls.clear();
}

void ToolCallAssembler::addDelta(int index, const QString& id, const QString& name, const QString& argumentsDelta)
{
    if (index < 0) {
        return;
    }

    Call* call = nullptr;
    for (Call& c : m_calls) {
        if (c.index == index) {
            call = &c;
            break;
        }
    }

    if (!call) {
        Call fresh;
        fresh.index = index;
        m_calls.append(fresh);
        call = &m_calls.last();
    }

    //! ⛔ Only adopt identity when it is actually present. The wire repeats `id`/`name` on
    //! continuation deltas as `''` or `null` to mean "unchanged"; treating those as values replaces a
    //! good name with an empty one, and the call then fails as "unknown tool" with nothing in the
    //! log to explain it.
    if (!id.isEmpty()) {
        call->id = id;
    }
    if (!name.isEmpty()) {
        call->name = name;
    }

    call->arguments += argumentsDelta;
}

QVector<QJsonObject> ToolCallAssembler::calls() const
{
    //! Sorted by index: the model's ordering is the index order, and the *submission* order of the
    //! results has to match it (DSH commits tool results in model order even when execution
    //! overlaps). Sorting here means callers never have to think about arrival order.
    QVector<Call> sorted = m_calls;
    std::sort(sorted.begin(), sorted.end(), [](const Call& a, const Call& b) {
        return a.index < b.index;
    });

    QVector<QJsonObject> out;
    for (const Call& c : sorted) {
        QJsonObject fn;
        fn.insert(QStringLiteral("name"), c.name);
        fn.insert(QStringLiteral("arguments"), c.arguments);

        QJsonObject obj;
        obj.insert(QStringLiteral("id"), c.id);
        obj.insert(QStringLiteral("type"), QStringLiteral("function"));
        obj.insert(QStringLiteral("function"), fn);
        out.append(obj);
    }
    return out;
}

QJsonObject muse::agentharness::buildChatRequest(const QString& model, const QJsonArray& messages, const QJsonArray& tools,
                                                 bool thinking, const QString& effort)
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), true);

    //! ⛔ Without this the stream never carries a usage chunk, and token accounting silently stays at
    //! zero forever - which reads as "the model is free" rather than "we stopped asking".
    QJsonObject streamOptions;
    streamOptions.insert(QStringLiteral("include_usage"), true);
    body.insert(QStringLiteral("stream_options"), streamOptions);

    //! Top level, NOT nested inside an `extra_body`: the DeepSeek wire puts the thinking switch here.
    QJsonObject thinkingObj;
    thinkingObj.insert(QStringLiteral("type"), thinking ? QStringLiteral("enabled") : QStringLiteral("disabled"));
    body.insert(QStringLiteral("thinking"), thinkingObj);

    if (thinking && !effort.isEmpty()) {
        body.insert(QStringLiteral("reasoning_effort"), effort);
    }

    if (!tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), tools);
    }

    return body;
}
