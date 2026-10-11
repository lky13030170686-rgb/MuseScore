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
#include "llmtransport.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>
#include <QUrl>

#include "log.h"

using namespace muse::agentharness;

namespace {
//! How often to check that the stream is still alive. Coarse on purpose: this is a stuck-stream
//! detector, not a latency measurement, and a finer timer would just be more wake-ups on the GUI
//! thread.
constexpr int WATCHDOG_INTERVAL_MS = 5000;

//! How long to wait for a model list. A list is small and the user is staring at a menu that is either
//! going to fill or not, so this is a hard deadline - and a much shorter one than the stream's, which
//! exists to detect a STALLED stream rather than an unresponsive server.
constexpr int MODELS_TIMEOUT_MS = 15000;
} // namespace

LlmTransport::LlmTransport(QObject* parent)
    : QObject(parent)
{
    m_network = new QNetworkAccessManager(this);

    //! One watchdog for all calls rather than one per call: there is never more than one stream in
    //! flight (a second `start()` cancels the first), so a per-call timer would only be a lifecycle
    //! to get wrong.
    QTimer* watchdog = new QTimer(this);
    watchdog->setInterval(WATCHDOG_INTERVAL_MS);
    connect(watchdog, &QTimer::timeout, this, [this]() {
        if (!m_reply) {
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastActivityMs > m_idleTimeoutMs) {
            fail(QStringLiteral("no data for %1 ms - the stream appears to be stuck").arg(m_idleTimeoutMs));
        }
    });
    watchdog->start();
}

LlmTransport::~LlmTransport()
{
    //! Abort rather than delete: a reply still running would emit into a half-destroyed object.
    abort();
    abortModels();
}

void LlmTransport::setBaseUrl(const QString& baseUrl)
{
    m_baseUrl = baseUrl;
}

void LlmTransport::setApiKey(const QString& key)
{
    m_apiKey = key;
    m_apiKeyLoaded = true;
}

QString LlmTransport::apiKey() const
{
    if (!m_apiKeyLoaded) {
        //! Environment first, because it is the one place a key can live without being written to
        //! disk by this program. The key is NEVER recorded in the session log - a log that carries
        //! credentials is a log nobody can share.
        m_apiKey = qEnvironmentVariable("DEEPSEEK_API_KEY");
        m_apiKeyLoaded = true;
    }
    return m_apiKey;
}

void LlmTransport::abort()
{
    if (!m_reply) {
        return;
    }

    //! Disconnect before aborting so the abort's own `finished` does not re-enter the completion path
    //! and report a failure for something the caller deliberately cancelled.
    m_reply->disconnect(this);
    m_reply->abort();
    cleanup();
}

void LlmTransport::abortModels()
{
    if (!m_modelsReply) {
        return;
    }

    m_modelsReply->disconnect(this);
    m_modelsReply->abort();
    m_modelsReply->deleteLater();
    m_modelsReply = nullptr;
    m_onModels = nullptr;
}

bool LlmTransport::isOpencodeRoute() const
{
    //! ⛔ "AN OPENCODE ROUTE" IS NOT THE SAME AS "A URL CONTAINING opencode". The user's own OpenCodex
    //! proxy answers on loopback and forwards to that same upstream - measured 2026-10-10:
    //! `POST http://127.0.0.1:10100/v1/chat/completions` with a good key and the model this route serves
    //! comes back `400 Provider error 400: Request is missing x-opencode-session and cannot be routed
    //! efficiently.` Sending the header turns the same call into a 200 with a normal SSE stream
    //! (`.../v1/models` lists the ids as `opencode-go/<model>`). So loopback counts as an opencode route:
    //! a local OpenAI-compatible endpoint in this project's world IS the proxy in front of that gateway.
    //! ⚠️ A generic local server (llama.cpp, ollama) ignores the header, which is why this is safe to
    //! widen; the thing that must not happen is sending it to a NON-loopback third party.
    return m_baseUrl.contains(QStringLiteral("opencode"))
           || m_baseUrl.contains(QStringLiteral("127.0.0.1"))
           || m_baseUrl.contains(QStringLiteral("localhost"))
           || m_baseUrl.contains(QStringLiteral("[::1]"));
}

void LlmTransport::applyAuthHeaders(QNetworkRequest& request) const
{
    request.setRawHeader("authorization", QByteArray("Bearer ") + apiKey().toUtf8());

    //! ⛔⛔ THE OPENCODE ROUTES REQUIRE A PER-CONVERSATION SESSION HEADER, AND WITHOUT IT *EVERY* REQUEST
    //! IS REFUSED - with a message that does not name the header.
    //!
    //! Measured against `https://opencode.ai/zen/go/v1`: a well-formed request with a valid key and a
    //! model id that route really serves comes back
    //! `400 {"type":"MissingSessionID","message":"Request is missing x-opencode-session and cannot be
    //! routed efficiently."}`. ⚠️ And an earlier probe that also omitted the field produced
    //! `{"type":"ModelError","message":"Model  is not supported"}` - **with an empty name where the model
    //! should be** - which reads like a bad model id and sends a caller off to try other names. That is
    //! how this cost time: the real problem was a missing HEADER, and the error pointed at the BODY.
    //!
    //! ⚠️ It is sent ONLY to opencode routes. Adding it unconditionally would put a header on DeepSeek's
    //! own endpoint that means nothing there - harmless today, and exactly the kind of "harmless" that
    //! becomes a 400 the day the other gateway starts validating unknown headers.
    //!
    //! ⚠️ The value is the harness's own session id when one was supplied, so retries and resumed turns
    //! keep the same routing identity; otherwise a fresh UUID, because an EMPTY value is not the same as
    //! an absent one and would fail the same check.
    if (isOpencodeRoute()) {
        const QString session = m_sessionId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                                      : m_sessionId;
        request.setRawHeader("x-opencode-session", session.toUtf8());
    }
}

void LlmTransport::fetchModels(std::function<void(bool, const QStringList&, const QString&)> done)
{
    //! One at a time: two live fetches would race to fill the same menu, and the older answer could win.
    abortModels();

    if (apiKey().isEmpty()) {
        //! Same split as `start()`: "not configured" and "the server refused you" are different problems
        //! and must not look the same.
        if (done) {
            done(false, QStringList(), QStringLiteral(
                     "no API key configured. Set the DEEPSEEK_API_KEY environment variable."));
        }
        return;
    }

    if (m_baseUrl.isEmpty()) {
        if (done) {
            done(false, QStringList(), QStringLiteral("no API base URL configured"));
        }
        return;
    }

    const QUrl url(m_baseUrl + QStringLiteral("/models"));

    QNetworkRequest request(url);
    request.setRawHeader("accept", "application/json");
    applyAuthHeaders(request);

    m_onModels = std::move(done);
    QNetworkReply* reply = m_network->get(request);
    m_modelsReply = reply;

    //! A hard deadline, and it must only fire for THIS reply: the pointer is captured and compared, so a
    //! later fetch is not killed by an earlier fetch's timer.
    QTimer::singleShot(MODELS_TIMEOUT_MS, this, [this, reply]() {
        if (m_modelsReply == reply) {
            finishModels(false, QStringList(),
                         QStringLiteral("no answer within %1 ms").arg(MODELS_TIMEOUT_MS));
        }
    });

    connect(reply, &QNetworkReply::finished, this, &LlmTransport::onModelsFinished);

    LOGW() << "[agent-llm] GET" << url.toString();
}

void LlmTransport::onModelsFinished()
{
    if (!m_modelsReply) {
        return;
    }

    QNetworkReply* reply = m_modelsReply;
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();

    if (error != QNetworkReply::NoError) {
        //! ⛔ The body is carried up, not replaced by "connection failed": the useful part of a 401 or a
        //! 404 here is the server's own sentence, and the caller shows it to the user.
        QString detail = QString::fromUtf8(body).left(400);
        if (detail.isEmpty()) {
            detail = reply->errorString();
        }
        finishModels(false, QStringList(), QStringLiteral("HTTP %1: %2").arg(status).arg(detail));
        return;
    }

    QString problem;
    const QStringList ids = parseModelIds(body, &problem);
    if (!problem.isEmpty()) {
        finishModels(false, QStringList(), problem);
        return;
    }

    finishModels(true, ids, QString());
}

void LlmTransport::finishModels(bool ok, const QStringList& ids, const QString& error)
{
    //! Take the callback OUT before invoking it: the caller may start another fetch from inside, and a
    //! callback still stored here would then be cleared by the cleanup below - after it was replaced.
    auto onModels = m_onModels;
    m_onModels = nullptr;

    if (m_modelsReply) {
        m_modelsReply->disconnect(this);
        m_modelsReply->deleteLater();
        m_modelsReply = nullptr;
    }

    LOGW() << "[agent-llm] models" << (ok ? "OK" : "FAILED") << "count=" << ids.size() << "error=" << error;

    if (onModels) {
        onModels(ok, ids, error);
    }
}

void LlmTransport::cleanup()
{
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_buffer.clear();
    m_onChunk = nullptr;
    m_onFinished = nullptr;
}

void LlmTransport::fail(const QString& error)
{
    auto onFinished = m_onFinished;

    LOGW() << "[agent-llm] failed:" << error;
    cleanup();

    if (onFinished) {
        onFinished(false, error);
    }
    emit failed(error);
    emit finished();
}

void LlmTransport::start(const QJsonObject& requestBody,
                         std::function<void(const StreamChunk&)> onChunk,
                         std::function<void(bool, const QString&)> onFinished)
{
    //! One stream at a time. Two live streams would interleave their chunks into a single transcript
    //! and there is no way to tell afterwards which text came from which call.
    if (m_reply) {
        abort();
    }

    const QString key = apiKey();
    if (key.isEmpty()) {
        //! Report it here rather than firing a request that comes back 401: "not configured" and
        //! "the server rejected you" are different problems and should not look the same.
        if (onFinished) {
            onFinished(false, QStringLiteral(
                            "no API key configured. Set the DEEPSEEK_API_KEY environment variable."));
        }
        emit finished();
        return;
    }

    if (m_baseUrl.isEmpty()) {
        if (onFinished) {
            onFinished(false, QStringLiteral("no API base URL configured"));
        }
        emit finished();
        return;
    }

    m_onChunk = std::move(onChunk);
    m_onFinished = std::move(onFinished);

    const QUrl url(m_baseUrl + QStringLiteral("/chat/completions"));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    //! ⛔ `Accept: text/event-stream` is not decoration: without it some gateways buffer the response
    //! and the stream arrives as one lump at the end, which looks exactly like "the model is slow".
    request.setRawHeader("accept", "text/event-stream");

    //! Authorization and, on the opencode routes, the session header. Shared with `fetchModels()` so the
    //! two requests cannot drift apart - the rule, and why it is what it is, live in `applyAuthHeaders`.
    applyAuthHeaders(request);

    const QByteArray body = QJsonDocument(requestBody).toJson(QJsonDocument::Compact);

    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    m_reply = m_network->post(request, body);

    connect(m_reply, &QNetworkReply::readyRead, this, &LlmTransport::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &LlmTransport::onReplyFinished);

    //! ⚠️ The model is named HERE, even though the body carries it, because "which model answered" is the
    //! first question asked of any surprising answer - and the body is not logged (it carries the whole
    //! conversation). One field is enough to correlate a turn with a model.
    LOGW() << "[agent-llm] POST" << url.toString()
           << "model=" << requestBody.value(QStringLiteral("model")).toString()
           << "bytes=" << body.size();
}

void LlmTransport::onReadyRead()
{
    if (!m_reply) {
        return;
    }

    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    m_buffer.append(m_reply->readAll());

    //! Split on newlines and keep the remainder. SSE frames can split anywhere - including in the
    //! middle of a UTF-8 character - so the buffer is BYTES and decoding happens per complete line.
    //! Decoding the buffer eagerly would corrupt any multi-byte character that straddles two reads,
    //! and the corruption would show up as mojibake in the transcript with no obvious cause.
    int start = 0;
    while (true) {
        const int nl = m_buffer.indexOf('\n', start);
        if (nl < 0) {
            break;
        }

        QByteArray line = m_buffer.mid(start, nl - start);
        start = nl + 1;
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        handleSseLine(line);
    }

    m_buffer.remove(0, start);

    //! A single line longer than this is not a chunk, it is a runaway. Dropping the buffer keeps the
    //! transport alive rather than letting a malformed stream consume memory without bound.
    constexpr int MAX_BUFFER = 8 * 1024 * 1024;
    if (m_buffer.size() > MAX_BUFFER) {
        fail(QStringLiteral("SSE line exceeded %1 bytes without a newline").arg(MAX_BUFFER));
    }
}

void LlmTransport::handleSseLine(const QByteArray& line)
{
    if (!m_reply) {
        return;
    }

    const QByteArray trimmed = line.trimmed();

    //! Comments (`: keep-alive`) and blank frames are the protocol's no-ops. Passing them down would
    //! produce Malformed chunks for a perfectly healthy stream.
    if (trimmed.isEmpty() || trimmed.startsWith(':')) {
        return;
    }

    //! Only `data:` carries payload; `event:`/`id:`/`retry:` are not used by this endpoint.
    constexpr char PREFIX[] = "data:";
    if (!trimmed.startsWith(PREFIX)) {
        return;
    }

    QByteArray payload = trimmed.mid(int(sizeof(PREFIX) - 1));
    if (payload.startsWith(' ')) {
        payload.remove(0, 1);
    }

    bool done = false;
    const QVector<StreamChunk> chunks = parseSseData(payload, done);

    for (const StreamChunk& chunk : chunks) {
        switch (chunk.type) {
        case StreamChunk::Type::Malformed:
            //! ⛔ A malformed payload or an in-band `{"error":...}` ENDS the call. Continuing would
            //! leave the caller waiting for a completion that will never come, and the transcript
            //! would silently stop mid-sentence.
            fail(chunk.problem);
            return;
        case StreamChunk::Type::TextDelta:
            if (m_onChunk) {
                m_onChunk(chunk);
            }
            emit textDelta(chunk.text);
            break;
        case StreamChunk::Type::ReasoningDelta:
            if (m_onChunk) {
                m_onChunk(chunk);
            }
            emit reasoningDelta(chunk.text);
            break;
        default:
            if (m_onChunk) {
                m_onChunk(chunk);
            }
            break;
        }
    }

    if (done) {
        //! `[DONE]` is the protocol's end marker. Waiting for `finished` instead would be waiting for
        //! the socket to close, which some gateways keep open.
        auto onFinished = m_onFinished;
        LOGW() << "[agent-llm] stream done";
        cleanup();
        if (onFinished) {
            onFinished(true, QString());
        }
        emit finished();
    }
}

void LlmTransport::onReplyFinished()
{
    if (!m_reply) {
        return;
    }

    const QNetworkReply::NetworkError error = m_reply->error();
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (error != QNetworkReply::NoError) {
        //! ⛔ Distinguish "we never got a stream" from "the stream was cut short". A body carrying a
        //! non-2xx status usually has the real reason in it, and dropping it would leave the user
        //! with "connection closed" for what is actually "your key is wrong".
        const QByteArray body = m_reply->readAll();
        QString detail = QString::fromUtf8(body).left(400);
        if (detail.isEmpty()) {
            detail = m_reply->errorString();
        }
        fail(QStringLiteral("HTTP %1: %2").arg(status).arg(detail));
        return;
    }

    //! Finished without `[DONE]`: the server closed the stream early. Reported as a failure rather
    //! than treated as a complete answer, because a truncated reply that looks complete is the worst
    //! of both worlds - the caller cannot tell it is missing text.
    fail(QStringLiteral("the stream closed before [DONE] (truncated response)"));
}
