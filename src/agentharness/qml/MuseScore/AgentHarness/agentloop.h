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

#include <QJsonArray>
#include <QObject>
#include <QString>

#include "llmclient.h"
#include "sessionlog.h"

namespace muse::agentharness {
class LlmTransport;
class FieldController;

//! The agent loop: one turn is "the user said something"; a step is one model request plus the tool
//! calls it asked for (技术设计 §5.2).
//!
//! WHAT IT OWNS: the sequencing. Everything else is already somewhere else - the log holds the facts,
//! the transport holds the socket, the tool table holds the capabilities. This class decides *when*
//! each happens, and that is deliberately all it does, because sequencing is the part that has to be
//! readable in one place.
//!
//! WHY THE LOOP IS NOT RECURSIVE: a step that asks for tools is followed by another step. Writing that
//! as recursion would grow the stack with the number of tool rounds; a model that loops on tools would
//! eventually take the process down. It is driven by `runNextStep()`, which returns to the event loop
//! between steps.
class AgentLoop : public QObject
{
    Q_OBJECT

public:
    AgentLoop(SessionLog* session, LlmTransport* transport, FieldController* field, QObject* parent = nullptr);

    //! Accept a user message and run the turn. Ignored while a turn is already running - a second
    //! turn would interleave its steps with the first one's.
    void submitUserMessage(const QString& text);

    //! Cancel the in-flight turn. The turn is closed with `turn/end{aborted}` so the log never ends
    //! mid-turn, which would make the next load look like a crash.
    void abort();

    bool isRunning() const { return m_running; }

    //! What the assistant has produced so far in the running step. For the panel's live rendering -
    //! NOT a second record of the conversation: the durable form is the log, and this is cleared at
    //! the start of every step.
    QString streamingText() const { return m_text; }
    QString streamingReasoning() const { return m_reasoning; }

    //! Model id and thinking settings. Defaults are the DeepSeek ones; the caller may override.
    void setModel(const QString& model) { m_model = model; }
    QString model() const { return m_model; }
    //! The model this loop ships with. Exposed so "reset the preferences" restores THE default instead of
    //! a copy of it that can drift (第 130 条).
    static QString defaultModel() { return QStringLiteral("deepseek-chat"); }
    void setThinking(bool thinking) { m_thinking = thinking; }
    void setReasoningEffort(const QString& effort) { m_effort = effort; }

    //! How many tool rounds one turn may take before the loop stops asking. A model that keeps calling
    //! tools without converging would otherwise run until the user notices; a bounded loop ends with a
    //! clear reason instead.
    //!
    //! ⛔ CLAMPED, because this number became USER-EDITABLE (Agent 页的 设置 → 助手, 第 130 条). The guard
    //! exists to bound runaway cost, and "the value came from a settings field" is not a reason to remove
    //! the bound: `0` would stop every turn before the first tool ran, and `100000` is the unbounded loop
    //! the guard was written against. The bounds live HERE, next to the check that enforces them
    //! (`agentloop.cpp`), so the clamp and the limit cannot disagree.
    static constexpr int MIN_STEPS_PER_TURN = 1;
    static constexpr int MAX_STEPS_PER_TURN = 100;
    //! What a turn gets when nobody says otherwise. ⚠️ `FieldController` does NOT repeat this number: it
    //! asks the loop for the value it actually holds (`agentMaxSteps()`), and its reset uses THIS constant
    //! - so there is one default, not two.
    static constexpr int DEFAULT_STEPS_PER_TURN = 8;

    void setMaxStepsPerTurn(int steps);
    int maxStepsPerTurn() const { return m_maxStepsPerTurn; }

signals:
    //! Live stream, for the panel. These are *not* the durable record - the log is.
    void assistantText(const QString& text);
    void assistantReasoning(const QString& text);
    void toolStarted(const QString& name, const QString& args);
    void toolFinished(const QString& name, bool ok, const QString& text);
    void turnFinished(bool ok, const QString& error);
    void runningChanged();

private:
    void runNextStep();
    void onStreamFinished(bool ok, const QString& error);
    void executeToolCalls(const QVector<QJsonObject>& calls);
    void closeTurn(bool ok, const QString& reason);

    SessionLog* m_session = nullptr;
    LlmTransport* m_transport = nullptr;
    FieldController* m_field = nullptr;

    bool m_running = false;
    int m_turn = 0;
    int m_step = 0;
    int m_maxStepsPerTurn = DEFAULT_STEPS_PER_TURN;

    QString m_model = defaultModel();
    bool m_thinking = false;
    QString m_effort = QStringLiteral("high");

    //! The step's accumulated stream. Cleared at the start of each step.
    QString m_text;
    QString m_reasoning;
    ToolCallAssembler m_assembler;
    int m_promptTokens = 0;
    int m_completionTokens = 0;
};

} // namespace muse::agentharness
