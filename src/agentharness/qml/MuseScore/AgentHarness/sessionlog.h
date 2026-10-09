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

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace muse::agentharness {
//! One durable fact about a conversation, as a flat record.
//!
//! WHY THIS IS THE CENTRE OF THE DESIGN (技术设计 §5.1, and the one thing DSH is worth copying):
//! the conversation the model sees is not stored anywhere - it is *derived* from this log. There is
//! exactly one state machine (the log), and everything else (the request history, the panel's
//! transcript, "what did the agent see before it made that edit") is a projection of it. The
//! alternative - keeping a live message list and logging alongside it - is two truths that drift.
//!
//! `type` is the event name, the rest of the fields are payload. Deliberately flat and JSON-shaped:
//! it is written to disk one line per event, so it must survive a round trip through JSON without
//! losing anything the projection needs.
struct SessionEvent
{
    //! Monotonic, never reused, and equal to the index in the log (DSH keeps `seq === index`).
    //! Every other structure refers to events by this number, which is what makes "replay from the
    //! log" and "render the live stream" produce the same result.
    quint64 seq = 0;
    QString type;
    QString time;
    QJsonObject data;

    //! The event names this build produces. Kept as constants so a typo is a compile error rather
    //! than an event that silently never matches any projection rule.
    static const QString TURN_START;
    static const QString TURN_END;
    static const QString STEP_START;
    static const QString STEP_END;
    static const QString USER_MESSAGE;
    static const QString SYSTEM_MESSAGE;
    static const QString ASSISTANT_MESSAGE;
    static const QString ASSISTANT_ATTEMPT;
    static const QString TOOL_CALL;
    static const QString TOOL_RESULT;
    static const QString REQUEST_HEADER;
    static const QString REQUEST_CONTEXT;

    QJsonObject toJson() const;
    static SessionEvent fromJson(const QJsonObject& obj);
};

//! A message as the model sees it. Derived, never stored.
struct WireMessage
{
    QString role;       //!< "system" | "user" | "assistant" | "tool"
    QString content;
    //! Assistant reasoning, echoed back on tool-call turns. DeepSeek requires it in thinking mode
    //! when the turn carries tool calls; it is ignored elsewhere.
    QString reasoning;
    //! Set on an assistant turn that requested tools.
    QVector<QJsonObject> toolCalls;
    //! Set on a `role: "tool"` message: which call it answers.
    QString toolCallId;

    QJsonObject toWire() const;
};

//! The append-only log.
//!
//! It owns no persistence and no policy: it is a vector plus the projection rules, so the whole
//! thing is unit-testable without an application, a network, or a score. Persistence (JSONL) and
//! the agent loop sit on top.
class SessionLog
{
public:
    //! Append one event. `seq` is assigned here - callers never set it, because a caller-chosen
    //! sequence number is a caller-chosen ordering bug.
    quint64 append(const QString& type, const QJsonObject& data = QJsonObject());

    const QVector<SessionEvent>& events() const { return m_events; }
    int size() const { return m_events.size(); }
    bool isEmpty() const { return m_events.isEmpty(); }
    void clear();

    //! The current turn/step, derived from the log rather than tracked alongside it.
    int currentTurn() const;
    int currentStep() const;
    //! True while a turn is open (a `turn/start` without its `turn/end`).
    bool isTurnOpen() const;

    //! ── The projection ────────────────────────────────────────────────────────────────────────
    //! The model-visible history, derived from the events. PURE: same log in, same messages out.
    //!
    //! The rules, and why each one is what it is:
    //!   - `system/message` renders as the system message. Only the LAST one is used: a later system
    //!     message replaces the earlier text rather than appending, because "the system prompt" is a
    //!     single thing and two of them in one request is a contradiction the model has to resolve.
    //!   - `user/message` renders verbatim.
    //!   - `assistant/message` renders as an assistant turn, carrying its tool calls and reasoning.
    //!   - `tool/result` renders as a `role: "tool"` message. A tool result with no preceding call is
    //!     DROPPED: the wire format pairs them by id and an orphan makes the whole request invalid.
    //!   - everything else (turn/step markers, attempts, request headers) is log-only.
    QVector<WireMessage> deriveMessages() const;

    //! JSONL: one flat JSON object per line, in order. This is the durable form.
    QByteArray toJsonLines() const;
    //! Parse JSONL back. Unparseable lines are REPORTED rather than skipped silently - a log that
    //! quietly drops its own lines is worse than one that refuses to load.
    static bool fromJsonLines(const QByteArray& lines, SessionLog& out, QString* error = nullptr);

private:
    QVector<SessionEvent> m_events;
};

} // namespace muse::agentharness
