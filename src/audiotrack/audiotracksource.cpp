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
#include "audiotracksource.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audiotrack;

AudioTrackSource::AudioTrackSource()
{
}

AudioTrackSource::~AudioTrackSource()
{
    unload();
}

bool AudioTrackSource::load(const muse::String& path)
{
    unload();

    if (!m_reader.open(path)) {
        return false;
    }

    m_readPos = 0.0;
    m_cacheStartFrame = 0;
    m_readerFramePos = 0;
    m_cacheFrames = 0;
    m_cachedChannels = 0;

    {
        std::lock_guard<std::mutex> lock(m_seekMutex);
        m_pendingSeekSeconds = 0.0;
        m_hasPendingSeek = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_posMutex);
        m_publishedSeconds = 0.0;
    }

    // Default to the file's own rate/channels until the engine tells us otherwise.
    m_outSampleRate = m_reader.info().sampleRate;
    m_outChannels = m_reader.info().channels;
    m_ratio = 1.0;

    return true;
}

void AudioTrackSource::unload()
{
    m_reader.close();
    m_cache.clear();
    m_lastFrame.clear();
    m_cacheFrames = 0;
    m_cachedChannels = 0;
    m_cacheStartFrame = 0;
    m_readerFramePos = 0;
    m_readPos = 0.0;

    {
        std::lock_guard<std::mutex> lock(m_seekMutex);
        m_pendingSeekSeconds = 0.0;
        m_hasPendingSeek = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_posMutex);
        m_publishedSeconds = 0.0;
    }
}

bool AudioTrackSource::isValid() const
{
    return m_reader.isOpen();
}

const AudioFileInfo& AudioTrackSource::info() const
{
    return m_reader.info();
}

unsigned int AudioTrackSource::audioChannelsCount() const
{
    // Channel count is fixed by the file: we never change it at runtime, so the
    // corresponding channel-change notification is never fired.
    const int ch = m_reader.isOpen() ? m_reader.info().channels : 2;
    return static_cast<unsigned int>(ch);
}

void AudioTrackSource::setPositionSeconds(double seconds)
{
    // Called from the engine thread. Do NOT touch playback state here: the audio thread
    // owns it. Publish the target and let process() apply it.
    std::lock_guard<std::mutex> lock(m_seekMutex);
    m_pendingSeekSeconds = seconds;
    m_hasPendingSeek = true;
}

bool AudioTrackSource::applyPendingSeek()
{
    // Audio thread only.
    double seconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(m_seekMutex);
        if (!m_hasPendingSeek) {
            return false;
        }
        seconds = m_pendingSeekSeconds;
        m_hasPendingSeek = false;
    }

    if (!m_reader.isOpen()) {
        return false;
    }

    const double fileRate = static_cast<double>(m_reader.info().sampleRate);
    int64_t frame = static_cast<int64_t>(std::llround(seconds * fileRate));
    frame = std::max<int64_t>(0, std::min<int64_t>(frame, m_reader.info().frames));

    m_readPos = static_cast<double>(frame);
    m_cacheStartFrame = frame;
    m_readerFramePos = frame;
    m_cacheFrames = 0;      // force a refill on the next process()
    // Drop the cross-chunk lookahead frame: it belongs to the audio we just jumped away
    // from, and keeping it would splice one stale frame into the new position.
    m_lastFrame.clear();

    m_reader.seekToFrame(frame);

    {
        std::lock_guard<std::mutex> lock(m_posMutex);
        m_publishedSeconds = static_cast<double>(frame) / fileRate;
    }

    return true;
}

double AudioTrackSource::positionSeconds() const
{
    std::lock_guard<std::mutex> lock(m_posMutex);
    return m_publishedSeconds;
}

void AudioTrackSource::seekTo(const TimePosition& position)
{
    setPositionSeconds(position.time());
}

TimePosition AudioTrackSource::position() const
{
    if (!m_reader.isOpen()) {
        return TimePosition();
    }

    return TimePosition::fromTime(secs_t(positionSeconds()),
                                  static_cast<sample_rate_t>(m_reader.info().sampleRate));
}

TimePosition AudioTrackSource::duration() const
{
    if (!m_reader.isOpen()) {
        return TimePosition();
    }

    return TimePosition::fromTime(secs_t(m_reader.info().duration),
                                  static_cast<sample_rate_t>(m_reader.info().sampleRate));
}

bool AudioTrackSource::refill()
{
    if (!m_reader.isOpen()) {
        return false;
    }

    const int outCh = m_outChannels;

    // Make the reader's own file cursor agree with m_readerFramePos before reading.
    // Without this the two drift apart: process() legitimately rewinds m_readerFramePos
    // to floor(readPos) when it needs a chunk covering an earlier frame, while the
    // libsndfile cursor keeps advancing sequentially. Trusting the implicit cursor
    // would then label the returned data with the wrong start frame.
    if (!m_reader.seekToFrame(m_readerFramePos)) {
        m_cacheFrames = 0;
        return false;
    }

    // The cache starts one frame before the read position, duplicating the last frame of
    // the previous chunk. That gives linear interpolation a left neighbour for the
    // chunk's first sample, so interpolation never straddles a chunk boundary.
    const bool haveLookahead = !m_lastFrame.empty() && m_readerFramePos > 0;
    m_cacheStartFrame = haveLookahead ? (m_readerFramePos - 1) : m_readerFramePos;

    float* dst = m_cache.data() + (haveLookahead ? static_cast<size_t>(outCh) : 0);

    const int64_t got = m_reader.readFrames(dst, CACHE_FRAMES, outCh);
    if (got <= 0) {
        m_cacheFrames = 0;
        return false;
    }

    if (haveLookahead) {
        std::copy(m_lastFrame.begin(), m_lastFrame.end(), m_cache.begin());
        m_cacheFrames = got + 1;
    } else {
        m_cacheFrames = got;
    }

    m_lastFrame.assign(m_cache.begin() + static_cast<size_t>(m_cacheFrames - 1) * outCh,
                       m_cache.begin() + static_cast<size_t>(m_cacheFrames) * outCh);

    // Next sequential read starts after this chunk (which began at m_cacheStartFrame).
    m_readerFramePos = m_cacheStartFrame + got;
    return true;
}

samples_t AudioTrackSource::process(float* buffer, samples_t samplesPerChannel)
{
    if (!buffer || samplesPerChannel == 0) {
        return samplesPerChannel;
    }

    // Determine the output layout first: needed both for the silence path below and for
    // the mixing loop. m_outChannels may still be the file's default on the first call.
    m_outSampleRate = m_outputSpec.isValid() ? static_cast<int>(m_outputSpec.sampleRate)
                      : m_reader.info().sampleRate;
    m_outChannels = m_outputSpec.audioChannelCount > 0
                    ? static_cast<int>(m_outputSpec.audioChannelCount)
                    : (m_reader.isOpen() ? m_reader.info().channels : 2);

    const int outCh = m_outChannels;
    const samples_t total = samplesPerChannel * static_cast<samples_t>(outCh);

    if (!m_reader.isOpen()) {
        std::fill(buffer, buffer + total, 0.f);
        return samplesPerChannel;
    }

    // Apply any seek requested from the engine thread. This is the ONLY place playback
    // state is repositioned, which is what makes the source thread-safe: the audio thread
    // is the sole writer of m_readPos/m_cache/m_lastFrame/the reader cursor.
    applyPendingSeek();

    // The engine resamples nothing for us (see the class comment), so this ratio is what
    // keeps us in step with the score.
    m_ratio = static_cast<double>(m_reader.info().sampleRate) / static_cast<double>(m_outSampleRate);

    // If the engine's channel count changed (device switch, export at a different layout),
    // everything cached is mis-strided. m_lastFrame in particular would be copied into a
    // buffer sized for the new width — an out-of-bounds write inside the vector.
    if (m_cachedChannels != outCh) {
        m_cachedChannels = outCh;
        m_cache.clear();
        m_cacheFrames = 0;
        m_lastFrame.clear();
        m_cacheStartFrame = static_cast<int64_t>(m_readPos);
        m_readerFramePos = m_cacheStartFrame;
        // Re-seat the reader too: m_readPos is where playback should continue from.
        m_reader.seekToFrame(m_cacheStartFrame);
    }

    if (m_cache.empty() || static_cast<int>(m_cache.size()) < (CACHE_FRAMES + 1) * outCh) {
        m_cache.assign(static_cast<size_t>(CACHE_FRAMES + 1) * outCh, 0.f);
    }

    const int64_t fileFrames = m_reader.info().frames;

    // Single source of truth for the read cursor: m_readPos is the fractional file-frame
    // position of the sample being produced right now. Every other counter is derived
    // from it, so a refill mid-buffer cannot double-count.
    double readPos = m_readPos;
    int64_t startPos = m_cacheStartFrame;   // file frame the current cache starts at

    for (samples_t i = 0; i < samplesPerChannel; ++i) {
        float* out = buffer + i * outCh;

        if (readPos >= static_cast<double>(fileFrames)) {
            std::fill(out, out + outCh, 0.f);   // past EOF: silence, not an error
            continue;
        }

        // Ensure the cache covers floor(readPos) and the frame after it.
        int64_t rel = static_cast<int64_t>(readPos) - startPos;
        if (rel < 0 || rel + 1 >= m_cacheFrames) {
            m_readerFramePos = static_cast<int64_t>(readPos);
            if (!refill()) {
                std::fill(out, out + outCh, 0.f);
                continue;
            }
            startPos = m_cacheStartFrame;
            rel = static_cast<int64_t>(readPos) - startPos;
        }

        const double frac = readPos - std::floor(readPos);

        if (rel >= 0 && rel + 1 < m_cacheFrames) {
            const float* a = m_cache.data() + static_cast<size_t>(rel) * outCh;
            const float* b = a + outCh;
            const float f = static_cast<float>(frac);
            for (int c = 0; c < outCh; ++c) {
                out[c] = a[c] + (b[c] - a[c]) * f;
            }
        } else if (rel >= 0 && rel < m_cacheFrames) {
            const float* a = m_cache.data() + static_cast<size_t>(rel) * outCh;
            for (int c = 0; c < outCh; ++c) {
                out[c] = a[c];
            }
        } else {
            std::fill(out, out + outCh, 0.f);
        }

        readPos += m_ratio;
    }

    m_readPos = readPos;
    m_cacheStartFrame = startPos;

    // Publish the advanced position for cross-thread readers (position()/positionSeconds()).
    {
        std::lock_guard<std::mutex> lock(m_posMutex);
        m_publishedSeconds = m_readPos / static_cast<double>(m_reader.info().sampleRate);
    }

    return samplesPerChannel;
}
