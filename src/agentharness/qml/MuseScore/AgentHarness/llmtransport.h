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

#include <QObject>
#include <QString>

#include <functional>

#include "llmclient.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace muse::agentharness {
//! The HTTP+SSE transport for one chat-completions call.
//!
//! WHY IT IS ASYNCHRONOUS AND NEVER BLOCKS (技术设计 §2.1): the score can only be written from the GUI
//! thread, so the whole harness lives on it - which means a blocking read here would freeze the
//! editor for the length of a model response. Everything below is therefore driven by Qt's own
//! signals, and the caller returns to the event loop immediately.
//!
//! WHY IT USES `QNetworkAccessManager` DIRECTLY rather than the project's `muse::network`: that
//! wrapper has no streaming hook (its `readyRead` goes into a caller-supplied `QIODevice` and it
//! exposes no response headers at all), and SSE is nothing but a streaming read. It also runs on the
//! main thread with a 1 ms ticker, so a long-lived stream through it would contend with the GUI. The
//! direct route is the smaller, more predictable one here. NOTE: `Qt::Network` is already linked into
//! the application (the cloud module uses it), so this adds no new dependency.
class LlmTransport : public QObject
{
    Q_OBJECT

public:
    explicit LlmTransport(QObject* parent = nullptr);
    ~LlmTransport() override;

    //! Where to send. `baseUrl` is the API root, e.g. "https://api.deepseek.com".
    void setBaseUrl(const QString& baseUrl);
    QString baseUrl() const { return m_baseUrl; }

    //! The bearer token. Read from the environment on first use (see `apiKey()`), never written to
    //! the session log.
    void setApiKey(const QString& key);
    QString apiKey() const;

    //! True when a key is available, so the caller can say "not configured" instead of firing a
    //! request that will come back 401.
    bool isConfigured() const { return !apiKey().isEmpty(); }

    //! Start one streaming call. The callbacks are invoked on the GUI thread.
    //!   - `onChunk`   one parsed piece of the response
    //!   - `onFinished` the stream ended (`ok` false means it failed; `error` says why)
    //! A second `start()` while one is in flight cancels the first: two live streams would interleave
    //! their chunks into one transcript.
    void start(const QJsonObject& requestBody,
               std::function<void(const StreamChunk&)> onChunk,
               std::function<void(bool ok, const QString& error)> onFinished);

    //! Abort the in-flight call, if any. Safe to call when idle.
    void abort();

    bool isRunning() const { return m_reply != nullptr; }

    //! How long to wait with no bytes before giving up. A stream that has silently died looks exactly
    //! like a model that is thinking, and without this the harness would wait forever.
    void setIdleTimeoutMs(int ms) { m_idleTimeoutMs = ms; }

signals:
    //! Emitted for every visible piece of the stream so the panel can render live without the caller
    //! having to thread a callback through.
    void textDelta(const QString& text);
    void reasoningDelta(const QString& text);
    void failed(const QString& error);
    void finished();

private:
    void onReadyRead();
    void onReplyFinished();
    void handleSseLine(const QByteArray& line);
    void fail(const QString& error);
    void cleanup();

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    //! Bytes received but not yet terminated by a newline. SSE frames can split anywhere, including
    //! mid-UTF-8-character, so the buffer must be bytes and the decode must happen per complete line.
    QByteArray m_buffer;

    QString m_baseUrl;
    //! `mutable` because `apiKey()` is const but resolves the environment lazily on first ask. The
    //! alternative - resolving in the constructor - would read the environment before a caller can
    //! override it, and would make the class untestable without touching the process environment.
    mutable QString m_apiKey;
    mutable bool m_apiKeyLoaded = false;

    std::function<void(const StreamChunk&)> m_onChunk;
    std::function<void(bool, const QString&)> m_onFinished;

    int m_idleTimeoutMs = 300000;
    qint64 m_lastActivityMs = 0;
};

} // namespace muse::agentharness
