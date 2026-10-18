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

#include <algorithm>

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

QString AgentLoop::stepBudgetNote(int step, int maxSteps)
{
    //! ⚠️ ONE NOTE, NOT ONE PER TOOL CALL: the caller attaches it to the LAST result of the step, so a step
    //! that ran three tools does not say the same thing three times (the model would read it as three
    //! warnings and could reasonably think it had three steps left).
    const int remaining = maxSteps - step;
    if (remaining > 1) {
        //! Room to keep working. ⚠️ `> 1` rather than `> 0`: with one step left the model is about to make
        //! the decision that ends the turn, and that is exactly the step worth telling.
        return QString();
    }

    if (remaining == 1) {
        return QStringLiteral("⏳ Harness note: this turn's step limit is %1 and ONE step remains after this "
                              "one. Use it to FINISH: do the part that matters, then answer in words with "
                              "what is done, what is left and what you would do next. Do not spend it on a "
                              "check you can do without - the user can tell you to continue.")
               .arg(maxSteps);
    }

    //! ⛔ THE LAST STEP GETS THE STRONGER TEXT, AND THE REASON IS MEASURED (2026-10-18): with the softer note
    //! ("0 step(s) remain after this one") a real model did the work and then spent that last step asking for
    //! one more tool - `score_window`, a check it did not need - so the turn ended without an answer even
    //! though everything it was asked to do had been done. The wording now names the trap.
    return QStringLiteral("⛔ Harness note: this is the LAST step of this turn (limit %1) - there is NO step "
                          "after this one. Answer now, in words: what you did, what is left, and what you "
                          "would do next. Do not start another tool call unless it is essential to leave the "
                          "score consistent: a tool call here ends the turn with no answer at all. The user "
                          "can tell you to continue.")
           .arg(maxSteps);
}

QString AgentLoop::stepLimitReason(int maxSteps)
{
    return QStringLiteral("this turn used all %1 of its steps and the model was still asking for tools. "
                          "Nothing is lost: the tools that ran are in the score (each step is one Ctrl+Z), "
                          "and the whole conversation is on record. Say \"continue\" to carry on from here, "
                          "or raise the step limit in the assistant settings.").arg(maxSteps);
}

void AgentLoop::setMaxStepsPerTurn(int steps)
{
    //! Clamped, not rejected: a settings field or an environment variable that asks for 0 or for 100000
    //! still gets a working agent, and the value it gets is the value the UI will read back
    //! (`maxStepsPerTurn()`), so nothing has to guess what was actually applied (第 130 条).
    m_maxStepsPerTurn = std::clamp(steps, MIN_STEPS_PER_TURN, MAX_STEPS_PER_TURN);
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
    //! ⛔ RESET PER TURN (第 147 条): the answer-only step belongs to the turn that ran out of steps. A flag
    //! left set would strip the tool schemas from a normal request, and the model would spend the rest of the
    //! conversation saying it has no tools.
    m_finalAnswerStep = false;
    m_finalAnswerReason.clear();

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
    //! ⛔ A CLOSED TURN MUST NOT START ANOTHER STEP, and this guard is not theoretical: the step ceiling
    //! ends the turn from INSIDE the tool-call path (`executeToolCalls` runs the tools the model already
    //! asked for, then `closeTurn(false, "stopped after N steps...")`), and those tools' own continuations
    //! call back into here when they finish. Measured 2026-10-11 with the ceiling set to 2: the log showed
    //! `turn finished FAILED error="stopped after 2 steps"` and, in the same millisecond,
    //! `turn0 step3 messages=7` - a THIRD request that the ceiling was supposed to prevent, and that the
    //! user pays for. The check lives here rather than at each caller because "the turn is over" is the
    //! loop's own state and every continuation has to respect it.
    if (!m_running) {
        return;
    }

    ++m_step;
    m_session->append(SessionEvent::STEP_START, stepData(m_turn, m_step));

    m_text.clear();
    m_reasoning.clear();
    m_streamRecorded = false;
    m_assembler.reset();
    m_promptTokens = 0;
    m_completionTokens = 0;

    //! The request is assembled from the LOG, never from a live message list. That is the whole point
    //! of the projection: what the model is sent is a function of what was recorded, so "what did the
    //! model see" is answerable after the fact.
    //! ⚠️ `m_thinking` is passed in because the wire shape depends on it in one place: in thinking mode an
    //! assistant turn that carries tool calls must send `reasoning_content` even when it is empty
    //! (第 133 条 - the upstream says the field "must be passed back", and a step whose model did not think
    //! was the one shape we sent without it).
    const QVector<WireMessage> messages = m_session->deriveMessages(m_thinking);

    QJsonArray wireMessages;
    for (const WireMessage& m : messages) {
        wireMessages.append(m.toWire());
    }

    //! The tool schemas travel with every request. Order is stable (see systemprompt.h), so two
    //! requests with the same history are byte-identical as far as a prefix cache is concerned.
    //!
    //! ⛔⛔ ON THE FINAL STEP THEY ARE NOT SENT AT ALL (第 147 条), and that is the fix that worked where
    //! persuasion did not: measured twice on a real model (2026-10-18), a note saying "this is the LAST step,
    //! do not start another tool call - a tool call here ends the turn with no answer at all" was delivered
    //! (the log and the projection both prove it) and the model called another tool anyway, twice. With the
    //! tool list EMPTY the question does not arise: the model has nothing to call, so the turn ends with
    //! words - which is the whole point of a landing.
    const QJsonArray schemas = m_finalAnswerStep ? QJsonArray() : toolSchemas();
    const QJsonObject body = buildChatRequest(m_model, wireMessages, schemas, m_thinking, m_effort);

    QJsonObject header;
    header.insert(QStringLiteral("model"), m_model);
    header.insert(QStringLiteral("thinking"), m_thinking);
    header.insert(QStringLiteral("tools"), int(schemas.size()));
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
    //! ⛔ THE TEXT IS NOW THE RECORD, so the live stream has done its job (第 132 条). Measured on a real
    //! turn: the stream was cleared only at the START of each step, so when a turn ended the last step's
    //! text and reasoning were still there - the answer appeared once in the transcript and again below
    //! it, under a `Thinking… (1073)` button that made a finished turn look like a running one.
    //! ⚠️ The reasoning is NOT dropped here but in `closeTurn`: while tools run, the reasoning of the step
    //! that asked for them is the only explanation of what is happening, and it is still that step's turn.
    m_streamRecorded = true;

    if (calls.isEmpty()) {
        //! No tool calls: the model answered. The turn is over.
        //! ⛔ ON THE FINAL STEP THIS IS THE LANDING (第 147 条): no tools were offered, so what came back is
        //! the answer we asked for - and the turn still ends as `false` with the limit as its reason, because
        //! "the model said something" and "the request was completed" are different claims and the user is
        //! owed both.
        if (m_finalAnswerStep) {
            closeTurn(false, m_finalAnswerReason);
            return;
        }

        closeTurn(true, QString());
        return;
    }

    if (m_step >= m_maxStepsPerTurn) {
        //! Bounded on purpose: a model that keeps asking for tools without converging would otherwise
        //! run until someone notices. Ending with a stated reason is better than an unbounded loop.
        //! The calls still run - the model asked for them and refusing silently would be worse - but
        //! no further step follows.
        //! ⚠️ `stepLimitReason` says what the old bare sentence did not (第 147 条): the limit, that the work
        //! so far is real, and how to carry on. Measured 2026-10-18: eight turns ended here, four of them in a
        //! row for one user who kept retrying - a dead end reads as "it broke", not as "it paused".
        if (m_finalAnswerStep) {
            //! ⛔ THE MODEL ASKED FOR A TOOL IT WAS NOT OFFERED: the last request carries NO tool schemas, so a
            //! call here is one it invented. Running it would honour something that could not have been asked;
            //! dropping it silently would leave a call with no result in the log. So each one is recorded as
            //! refused - and the turn ends, which is what keeps this terminating.
            for (const QJsonObject& call : calls) {
                QJsonObject refusal = stepData(m_turn, m_step);
                refusal.insert(QStringLiteral("callId"), call.value(QStringLiteral("id")).toString());
                refusal.insert(QStringLiteral("content"),
                               QStringLiteral("not run: this step was answer-only (the step limit was already "
                                              "spent), so no tool was offered to the model"));
                refusal.insert(QStringLiteral("isError"), true);
                m_session->append(SessionEvent::TOOL_RESULT, refusal);
            }

            closeTurn(false, m_finalAnswerReason);
            return;
        }

        //! ⛔ THE FLAGS GO UP BEFORE THE TOOLS RUN, because `executeToolCalls` continues the loop itself once
        //! the last call has reported (`QMetaObject::invokeMethod(… runNextStep …)`) - and THAT continuation
        //! is the request we want to be answer-only. ⚠️ Measured 2026-10-18: the first version of this fix
        //! queued a continuation here as well, and the final step went out TWICE in the same millisecond
        //! (two byte-identical 6206-byte requests in the log, one answer).
        m_finalAnswerStep = true;
        m_finalAnswerReason = stepLimitReason(m_maxStepsPerTurn);
        executeToolCalls(calls);
        return;
    }

    //! The tool calls run, and the NEXT STEP is started by the last one to finish - not from here.
    //! A tool may complete later (`patch_apply` queues its work), so continuing here would send the
    //! next request before the tool results existed - the model would be asked to reason about
    //! results that are not in the history yet.
    executeToolCalls(calls);
}

void AgentLoop::executeToolCalls(const QVector<QJsonObject>& calls)
{
    //! ⛔ Serial, in model order, and ASYNCHRONOUS. Every tool in the table touches the same `Score`,
    //! so two writers at once is not a concurrency problem worth having; and a tool may finish later
    //! (`patch_apply` queues its work - see scoreactiongateway.h), so the next one cannot start until
    //! the previous one has reported.
    //!
    //! The recursion is bounded by `calls.size()` and does NOT grow with the conversation - the outer
    //! step loop is what handles "the model asked for more tools", and that one goes back through the
    //! event loop.
    auto index = std::make_shared<int>(0);
    auto runNext = std::make_shared<std::function<void()>>();

    *runNext = [this, calls, index, runNext]() {
        if (*index >= calls.size()) {
            //! Every result is recorded. Back to the event loop before the next step - not straight
            //! into it - so the panel can paint what just happened.
            QMetaObject::invokeMethod(this, [this]() { runNextStep(); }, Qt::QueuedConnection);
            return;
        }

        const QJsonObject call = calls[*index];
        ++(*index);

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

        auto finish = [this, id, name, runNext, index, calls](const ToolResult& result) {
            QJsonObject resultData = stepData(m_turn, m_step);
            resultData.insert(QStringLiteral("callId"), id);
            resultData.insert(QStringLiteral("content"), result.text);
            resultData.insert(QStringLiteral("isError"), !result.ok);

            //! ⛔ THE SOFT LANDING (第 147 条): on the step before the wall, the model is TOLD how many steps
            //! it has left. ⚠️ On the LAST result of the step only - a step that ran three tools would
            //! otherwise repeat the same warning three times, and the model could read that as three steps
            //! remaining.
            //! ⚠️ It goes in its OWN field rather than being appended to `content`: the log then says which
            //! words came from the tool and which came from the harness, and the two readers take what
            //! belongs to them (the wire gets both, the transcript shows the tool's own text).
            if (*index >= calls.size()) {
                const QString note = stepBudgetNote(m_step, m_maxStepsPerTurn);
                if (!note.isEmpty()) {
                    resultData.insert(QStringLiteral("budget"), note);
                }
            }

            m_session->append(SessionEvent::TOOL_RESULT, resultData);

            emit toolFinished(name, result.ok, result.text);

            //! The next call is started from HERE, after this one has reported - which is what makes
            //! the sequence serial even when a tool finishes later.
            (*runNext)();
        };

        if (!m_field) {
            finish(ToolResult::failure(QStringLiteral("no score context available")));
            return;
        }

        m_field->runToolAsync(name, args, finish);
    };

    (*runNext)();
}

void AgentLoop::closeTurn(bool ok, const QString& reason)
{
    QJsonObject endData = turnData(m_turn);
    endData.insert(QStringLiteral("ok"), ok);
    if (!reason.isEmpty()) {
        endData.insert(QStringLiteral("reason"), reason);
    }
    m_session->append(SessionEvent::TURN_END, endData);

    //! ⛔ THE LIVE STREAM IS DROPPED ONCE IT IS IN THE LOG, AND ONLY THEN (第 132 条). The transcript is
    //! derived from the log, so a stream that survives the turn that produced it shows the same answer
    //! twice - and the reasoning counter under it makes a finished turn look like a running one.
    //! ⚠️ When the step FAILED or was aborted the text never reached the log (`onStreamFinished` records a
    //! failure as `assistant/attempt`, never as a message, so a transient error cannot poison the
    //! history). The stream is then the ONLY copy of what the model had said, and clearing it would delete
    //! the half-finished answer the user was reading.
    if (m_streamRecorded) {
        m_text.clear();
        m_reasoning.clear();
        m_streamRecorded = false;
    }

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
