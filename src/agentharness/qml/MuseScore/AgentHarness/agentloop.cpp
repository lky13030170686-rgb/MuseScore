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
#include "agentloop.h"

#include <QJsonDocument>

#include "log.h"

#include "fieldcontroller.h"
#include "llmtransport.h"
#include "systemprompt.h"
#include "tools.h"

using namespace muse::agentharness;

namespace {
QJsonObject turnData(int turn)
{
    QJsonObject o;
    o.insert(QStringLiteral("turn"), turn);
    return o;
}

QJsonObject stepData(int turn, int step)
{
    QJsonObject o;
    o.insert(QStringLiteral("turn"), turn);
    o.insert(QStringLiteral("step"), step);
    return o;
}
} // namespace

AgentLoop::AgentLoop(SessionLog* session, LlmTransport* transport, FieldController* field, QObject* parent)
    : QObject(parent), m_session(session), m_transport(transport), m_field(field)
{
}

void AgentLoop::submitUserMessage(const QString& text)
{
    if (m_running) {
        //! Refused rather than queued: two concurrent turns would interleave their steps, and the log
        //! would show tool results answering calls from a different step.
        emit turnFinished(false, QStringLiteral("a turn is already running"));
        return;
    }

    if (text.trimmed().isEmpty()) {
        return;
    }

    //! The system prompt goes in as a logged event before the first user message, exactly once per
    //! conversation. It is a *surface node* in DSH's terms - part of the derived history rather than a
    //! request field - which is why it is recorded here and not passed to the transport directly.
    bool hasSystemPrompt = false;
    for (const SessionEvent& e : m_session->events()) {
        if (e.type == SessionEvent::SYSTEM_MESSAGE) {
            hasSystemPrompt = true;
            break;
        }
    }
    if (!hasSystemPrompt) {
        QJsonObject data;
        data.insert(QStringLiteral("content"), buildSystemPrompt());
        m_session->append(SessionEvent::SYSTEM_MESSAGE, data);
    }

    m_turn = m_session->currentTurn() + 1;
    m_step = 0;

    m_session->append(SessionEvent::TURN_START, turnData(m_turn));

    QJsonObject userData;
    userData.insert(QStringLiteral("content"), text);
    m_session->append(SessionEvent::USER_MESSAGE, userData);

    m_running = true;
    emit runningChanged();

    runNextStep();
}

void AgentLoop::runNextStep()
{
    ++m_step;
    m_session->append(SessionEvent::STEP_START, stepData(m_turn, m_step));

    m_text.clear();
    m_reasoning.clear();
    m_assembler.reset();
    m_promptTokens = 0;
    m_completionTokens = 0;

    //! The request is assembled from the LOG, never from a live message list. That is the whole point
    //! of the projection: what the model is sent is a function of what was recorded, so "what did the
    //! model see" is answerable after the fact.
    const QVector<WireMessage> messages = m_session->deriveMessages();

    QJsonArray wireMessages;
    for (const WireMessage& m : messages) {
        wireMessages.append(m.toWire());
    }

    //! The tool schemas travel with every request. Order is stable (see systemprompt.h), so two
    //! requests with the same history are byte-identical as far as a prefix cache is concerned.
    const QJsonObject body = buildChatRequest(m_model, wireMessages, toolSchemas(), m_thinking, m_effort);

    QJsonObject header;
    header.insert(QStringLiteral("model"), m_model);
    header.insert(QStringLiteral("thinking"), m_thinking);
    header.insert(QStringLiteral("tools"), int(toolTable().size()));
    m_session->append(SessionEvent::REQUEST_HEADER, header);

    LOGW() << "[agent-loop] turn" << m_turn << "step" << m_step
           << "messages=" << wireMessages.size()
           << "model=" << m_model;

    m_transport->start(body,
                       [this](const StreamChunk& chunk) {
        switch (chunk.type) {
        case StreamChunk::Type::TextDelta:
            m_text += chunk.text;
            emit assistantText(chunk.text);
            break;
        case StreamChunk::Type::ReasoningDelta:
            m_reasoning += chunk.text;
            emit assistantReasoning(chunk.text);
            break;
        case StreamChunk::Type::ToolCallDelta:
            m_assembler.addDelta(chunk.index, chunk.toolCallId, chunk.toolCallName, chunk.argumentsDelta);
            break;
        case StreamChunk::Type::Usage:
            m_promptTokens = chunk.promptTokens;
            m_completionTokens = chunk.completionTokens;
            break;
        default:
            break;
        }
    },
                       [this](bool ok, const QString& error) {
        onStreamFinished(ok, error);
    });
}

void AgentLoop::onStreamFinished(bool ok, const QString& error)
{
    if (!ok) {
        //! ⛔ A failed attempt is recorded as `assistant/attempt`, NOT as an assistant message. Writing
        //! it as a message would put text the model never produced into the history, and every later
        //! request would carry it - the conversation would be permanently poisoned by a transient
        //! network error.
        QJsonObject attemptData = stepData(m_turn, m_step);
        attemptData.insert(QStringLiteral("problem"), error);
        m_session->append(SessionEvent::ASSISTANT_ATTEMPT, attemptData);

        closeTurn(false, error);
        return;
    }

    const QVector<QJsonObject> calls = m_assembler.calls();

    QJsonObject messageData = stepData(m_turn, m_step);
    messageData.insert(QStringLiteral("content"), m_text);
    if (!m_reasoning.isEmpty()) {
        messageData.insert(QStringLiteral("reasoning"), m_reasoning);
    }
    if (!calls.isEmpty()) {
        QJsonArray callArray;
        for (const QJsonObject& call : calls) {
            callArray.append(call);
        }
        messageData.insert(QStringLiteral("toolCalls"), callArray);
    }

    QJsonObject usage;
    usage.insert(QStringLiteral("promptTokens"), m_promptTokens);
    usage.insert(QStringLiteral("completionTokens"), m_completionTokens);
    messageData.insert(QStringLiteral("usage"), usage);

    m_session->append(SessionEvent::ASSISTANT_MESSAGE, messageData);

    if (calls.isEmpty()) {
        //! No tool calls: the model answered. The turn is over.
        closeTurn(true, QString());
        return;
    }

    if (m_step >= m_maxStepsPerTurn) {
        //! Bounded on purpose: a model that keeps asking for tools without converging would otherwise
        //! run until someone notices. Ending with a stated reason is better than an unbounded loop.
        executeToolCalls(calls);
        closeTurn(false, QStringLiteral("stopped after %1 steps without the model finishing")
                  .arg(m_maxStepsPerTurn));
        return;
    }

    executeToolCalls(calls);

    //! ⛔ Back to the event loop before the next step, not straight into it. Calling `runNextStep()`
    //! from here would recurse once per tool round and grow the stack with the conversation; it would
    //! also mean the panel never gets a chance to paint what just happened.
    QMetaObject::invokeMethod(this, [this]() { runNextStep(); }, Qt::QueuedConnection);
}

void AgentLoop::executeToolCalls(const QVector<QJsonObject>& calls)
{
    //! Serial, in model order. DSH allows execution to overlap but commits results strictly in the
    //! order the model asked for them; here they are not even overlapped, because every tool in the
    //! table touches the same `Score` and two writers on one score is not a concurrency problem worth
    //! having.
    for (const QJsonObject& call : calls) {
        const QString id = call.value(QStringLiteral("id")).toString();
        const QJsonObject fn = call.value(QStringLiteral("function")).toObject();
        const QString name = fn.value(QStringLiteral("name")).toString();
        const QString args = fn.value(QStringLiteral("arguments")).toString();

        QJsonObject callData = stepData(m_turn, m_step);
        callData.insert(QStringLiteral("callId"), id);
        callData.insert(QStringLiteral("name"), name);
        callData.insert(QStringLiteral("arguments"), args);
        m_session->append(SessionEvent::TOOL_CALL, callData);

        emit toolStarted(name, args);

        QString resultText;
        bool ok = true;

        if (!m_field) {
            ok = false;
            resultText = QStringLiteral("no score context available");
        } else {
            resultText = m_field->runTool(name, args);
            ok = m_field->lastToolOk();
        }

        QJsonObject resultData = stepData(m_turn, m_step);
        resultData.insert(QStringLiteral("callId"), id);
        resultData.insert(QStringLiteral("content"), resultText);
        resultData.insert(QStringLiteral("isError"), !ok);
        m_session->append(SessionEvent::TOOL_RESULT, resultData);

        emit toolFinished(name, ok, resultText);
    }
}

void AgentLoop::closeTurn(bool ok, const QString& reason)
{
    QJsonObject endData = turnData(m_turn);
    endData.insert(QStringLiteral("ok"), ok);
    if (!reason.isEmpty()) {
        endData.insert(QStringLiteral("reason"), reason);
    }
    m_session->append(SessionEvent::TURN_END, endData);

    m_running = false;
    emit runningChanged();
    emit turnFinished(ok, reason);
}

void AgentLoop::abort()
{
    if (!m_running) {
        return;
    }

    m_transport->abort();

    //! The turn is CLOSED rather than left hanging. A log that ends mid-turn is indistinguishable from
    //! a crash on the next load, and "we were interrupted" is a fact worth recording.
    closeTurn(false, QStringLiteral("aborted"));
}
