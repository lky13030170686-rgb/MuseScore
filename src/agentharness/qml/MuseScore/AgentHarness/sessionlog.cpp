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
#include "sessionlog.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

using namespace muse::agentharness;

//! Event names. Same vocabulary as DSH (技术设计 §5.1) so that reading one teaches you the other.
const QString SessionEvent::TURN_START = QStringLiteral("turn/start");
const QString SessionEvent::TURN_END = QStringLiteral("turn/end");
const QString SessionEvent::STEP_START = QStringLiteral("step/start");
const QString SessionEvent::STEP_END = QStringLiteral("step/end");
const QString SessionEvent::USER_MESSAGE = QStringLiteral("user/message");
const QString SessionEvent::SYSTEM_MESSAGE = QStringLiteral("system/message");
const QString SessionEvent::ASSISTANT_MESSAGE = QStringLiteral("assistant/message");
const QString SessionEvent::ASSISTANT_ATTEMPT = QStringLiteral("assistant/attempt");
const QString SessionEvent::TOOL_CALL = QStringLiteral("tool/call");
const QString SessionEvent::TOOL_RESULT = QStringLiteral("tool/result");
const QString SessionEvent::REQUEST_HEADER = QStringLiteral("request/header");
const QString SessionEvent::REQUEST_CONTEXT = QStringLiteral("request/context");

QJsonObject SessionEvent::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("seq"), QJsonValue(double(seq)));
    obj.insert(QStringLiteral("type"), type);
    obj.insert(QStringLiteral("time"), time);
    obj.insert(QStringLiteral("data"), data);
    return obj;
}

SessionEvent SessionEvent::fromJson(const QJsonObject& obj)
{
    SessionEvent e;
    e.seq = quint64(obj.value(QStringLiteral("seq")).toDouble());
    e.type = obj.value(QStringLiteral("type")).toString();
    e.time = obj.value(QStringLiteral("time")).toString();
    e.data = obj.value(QStringLiteral("data")).toObject();
    return e;
}

QJsonObject WireMessage::toWire() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("role"), role);

    if (role == QLatin1String("assistant")) {
        //! `content` is NEVER null on the wire (技术设计 §5.3): some gateways reject null, and a
        //! tool-call-only turn is exactly the case where it would otherwise be null. The empty
        //! string is the agreed spelling of "no text this turn".
        obj.insert(QStringLiteral("content"), content);
        if (!reasoning.isEmpty()) {
            obj.insert(QStringLiteral("reasoning_content"), reasoning);
        }
        if (!toolCalls.isEmpty()) {
            QJsonArray calls;
            for (const QJsonObject& call : toolCalls) {
                calls.append(call);
            }
            obj.insert(QStringLiteral("tool_calls"), calls);
        }
        return obj;
    }

    if (role == QLatin1String("tool")) {
        obj.insert(QStringLiteral("tool_call_id"), toolCallId);
        obj.insert(QStringLiteral("content"), content);
        return obj;
    }

    obj.insert(QStringLiteral("content"), content);
    return obj;
}

quint64 SessionLog::append(const QString& type, const QJsonObject& data)
{
    SessionEvent e;
    //! `seq` is the index, by construction. DSH keeps the same invariant, and it is what lets a
    //! consumer address an event by number without a lookup table.
    e.seq = quint64(m_events.size());
    e.type = type;
    e.time = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    e.data = data;
    m_events.append(e);
    return e.seq;
}

void SessionLog::clear()
{
    m_events.clear();
}

int SessionLog::currentTurn() const
{
    //! The LAST turn/start wins. Scanning backwards rather than tracking a counter alongside the log:
    //! a counter is a second truth, and this one is cheap to derive.
    for (auto it = m_events.crbegin(); it != m_events.crend(); ++it) {
        if (it->type == SessionEvent::TURN_START) {
            return it->data.value(QStringLiteral("turn")).toInt();
        }
    }
    return -1;
}

int SessionLog::currentStep() const
{
    for (auto it = m_events.crbegin(); it != m_events.crend(); ++it) {
        if (it->type == SessionEvent::STEP_START) {
            return it->data.value(QStringLiteral("step")).toInt();
        }
    }
    return -1;
}

bool SessionLog::isTurnOpen() const
{
    //! Walk back to whichever came last. An unmatched `turn/start` means work is in flight - which is
    //! the state a crash leaves behind, and the state a reader must be able to see.
    for (auto it = m_events.crbegin(); it != m_events.crend(); ++it) {
        if (it->type == SessionEvent::TURN_START) {
            return true;
        }
        if (it->type == SessionEvent::TURN_END) {
            return false;
        }
    }
    return false;
}

QVector<WireMessage> SessionLog::deriveMessages() const
{
    QVector<WireMessage> messages;

    //! The system prompt is the LAST `system/message`, not all of them concatenated: it is one thing,
    //! and a request carrying two system messages forces the model to guess which governs. Collecting
    //! it first also puts it at the head of the list, which is where every provider expects it.
    QString systemText;
    for (const SessionEvent& e : m_events) {
        if (e.type == SessionEvent::SYSTEM_MESSAGE) {
            systemText = e.data.value(QStringLiteral("content")).toString();
        }
    }

    if (!systemText.isEmpty()) {
        WireMessage sys;
        sys.role = QStringLiteral("system");
        sys.content = systemText;
        messages.append(sys);
    }

    //! Which tool calls have been answered. Used to drop orphan results (see the header): the wire
    //! format pairs a result to a call by id, and a result whose call is not in the history makes the
    //! whole request invalid - so it must not be emitted, however it got into the log.
    QSet<QString> knownCallIds;

    for (const SessionEvent& e : m_events) {
        if (e.type == SessionEvent::USER_MESSAGE) {
            WireMessage m;
            m.role = QStringLiteral("user");
            m.content = e.data.value(QStringLiteral("content")).toString();
            messages.append(m);
            continue;
        }

        if (e.type == SessionEvent::ASSISTANT_MESSAGE) {
            WireMessage m;
            m.role = QStringLiteral("assistant");
            m.content = e.data.value(QStringLiteral("content")).toString();
            m.reasoning = e.data.value(QStringLiteral("reasoning")).toString();

            const QJsonArray calls = e.data.value(QStringLiteral("toolCalls")).toArray();
            for (const QJsonValue& v : calls) {
                const QJsonObject call = v.toObject();
                m.toolCalls.append(call);
                knownCallIds.insert(call.value(QStringLiteral("id")).toString());
            }

            //! An assistant turn with neither text nor tool calls carries nothing the model needs to
            //! see. Emitting it would add an empty turn to the history for no reason.
            if (m.content.isEmpty() && m.toolCalls.isEmpty()) {
                continue;
            }

            messages.append(m);
            continue;
        }

        if (e.type == SessionEvent::TOOL_RESULT) {
            const QString callId = e.data.value(QStringLiteral("callId")).toString();
            if (!knownCallIds.contains(callId)) {
                continue;
            }

            WireMessage m;
            m.role = QStringLiteral("tool");
            m.toolCallId = callId;
            m.content = e.data.value(QStringLiteral("content")).toString();
            messages.append(m);
            continue;
        }
    }

    return messages;
}

QByteArray SessionLog::toJsonLines() const
{
    QByteArray out;
    for (const SessionEvent& e : m_events) {
        //! Compact, one line each. The format is "JSONL": a newline inside an event would split it
        //! into two lines and destroy the log, so compact serialization is not a size optimisation
        //! here, it is correctness.
        out.append(QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact));
        out.append('\n');
    }
    return out;
}

bool SessionLog::fromJsonLines(const QByteArray& lines, SessionLog& out, QString* error)
{
    out.clear();

    const QList<QByteArray> split = lines.split('\n');
    int lineNumber = 0;

    for (const QByteArray& raw : split) {
        ++lineNumber;

        const QByteArray line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError parseError {};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            if (error) {
                *error = QStringLiteral("line %1: %2").arg(lineNumber).arg(parseError.errorString());
            }
            //! Refuse the whole file rather than loading a partial log. A log that silently drops its
            //! own lines would make the derived history differ from what the model actually saw, and
            //! nothing downstream could tell.
            out.clear();
            return false;
        }

        out.m_events.append(SessionEvent::fromJson(doc.object()));
    }

    return true;
}
