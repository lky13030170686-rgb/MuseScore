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

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace muse::agentharness {
//! One incremental piece of a streamed model response.
//!
//! This is the *neutral* vocabulary - the same shape DSH uses between its adapter and its loop, and
//! the reason the SSE parser below can be tested without a network: the wire format stops at
//! `parseSseData()`, everything above it speaks in these terms.
struct StreamChunk
{
    enum class Type {
        TextDelta,        //!< more visible text
        ReasoningDelta,   //!< more thinking text
        ToolCallDelta,    //!< more of one tool call's name/arguments
        Usage,            //!< token accounting (arrives near the end)
        Finish,           //!< the model stopped; `finishReason` says why
        Malformed,        //!< the payload could not be understood - reported, never dropped
    };

    Type type = Type::TextDelta;
    QString text;
    //! Tool calls are disambiguated by this, stable across a call's deltas.
    int index = -1;
    QString toolCallId;
    QString toolCallName;
    QString argumentsDelta;
    QString finishReason;
    int promptTokens = 0;
    int completionTokens = 0;
    //! Set on `Malformed`: what went wrong, for the log.
    QString problem;
};

//! Parse one SSE `data:` payload from a chat-completions stream.
//!
//! PURE, and separated from the transport on purpose. The parts of an SSE client that actually break
//! are the reassembly rules - fragments that carry the id once, empty-string repeats, a reasoning
//! block that must not open on an empty first chunk - and those are exactly the parts that need no
//! socket to test. The transport below is then thin enough to review by reading.
//!
//! @param data  the payload after `data: `, i.e. one `chat.completion.chunk` object, or `[DONE]`
//! @param done  set to true when the payload was the `[DONE]` sentinel
QVector<StreamChunk> parseSseData(const QByteArray& data, bool& done);

//! Reassemble streamed tool-call fragments into complete calls.
//!
//! WHY THIS IS A CLASS AND NOT A FEW LINES IN THE LOOP: the wire sends a call's identity (`id`,
//! `name`) only on its FIRST delta, and continuation deltas may repeat them as `''` or `null` -
//! both meaning "unchanged". A reassembler that treats those as real values produces a call with an
//! empty name, which then fails as "unknown tool" for no visible reason. Keeping this in one place
//! with its own tests is cheaper than debugging that from a log.
class ToolCallAssembler
{
public:
    void reset();

    //! Feed one delta. Index addresses the call; `id`/`name` are only adopted when non-empty.
    void addDelta(int index, const QString& id, const QString& name, const QString& argumentsDelta);

    //! The assembled calls, in index order, as wire `tool_calls` entries.
    QVector<QJsonObject> calls() const;
    bool isEmpty() const { return m_calls.isEmpty(); }

private:
    struct Call
    {
        int index = -1;
        QString id;
        QString name;
        QString arguments;
    };

    QVector<Call> m_calls;
};

//! Build the request body for one chat-completions call.
//!
//! Kept as a free function so the exact shape can be asserted in a unit test: this body is a wire
//! contract, and a field that quietly disappears (or a `stream_options` that stops being sent, so
//! usage never arrives) is the kind of bug that only shows up as "the token counter is always zero".
//!
//! @param model        model id
//! @param messages     already-serialized wire messages
//! @param tools        tool schemas, or empty for a tool-free call
//! @param thinking     enable thinking mode
//! @param effort       "low" | "high" | "max"; ignored when `thinking` is false
QJsonObject buildChatRequest(const QString& model, const QJsonArray& messages, const QJsonArray& tools,
                             bool thinking, const QString& effort);

} // namespace muse::agentharness
