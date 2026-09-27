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
#include "audiowaveformservice.h"

#include <chrono>

#include "audiofilereader.h"
#include "log.h"

using namespace muse;
using namespace muse::audiotrack;

AudioWaveformService::AudioWaveformService(std::shared_ptr<AudioWaveformProvider> provider)
    : m_provider(std::move(provider))
{
}

AudioWaveformService::~AudioWaveformService()
{
    // Let any in-flight decode finish before the provider goes away, otherwise the worker
    // could touch a half-destroyed service. The generation bump makes the result a no-op.
    m_generation.fetch_add(1);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.valid()) {
        m_pending.wait();
    }
}

std::shared_ptr<WaveformCache> AudioWaveformService::buildCache(const muse::io::path_t& path)
{
    AudioFileReader reader;
    if (!reader.open(path.toString())) {
        LOGE() << "audiotrack: cannot open audio file for waveform: " << path
               << ", error: " << reader.errorString();
        return nullptr;
    }

    auto cache = std::make_shared<WaveformCache>();

    // Channel 0 only: a backing track's picture is conventionally a single lane, and
    // keeping one channel halves the peak-extraction cost. Multi-lane display would need a
    // per-channel cache and is deliberately out of scope for now.
    const auto started = std::chrono::steady_clock::now();
    if (!cache->generate(reader, 0)) {
        LOGE() << "audiotrack: failed to build waveform cache for: " << path;
        return nullptr;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    LOGI() << "audiotrack: waveform ready for " << path
           << " (" << cache->duration() << " s, "
           << cache->frames() << " frames, took " << elapsedMs << " ms)";

    return cache;
}

void AudioWaveformService::loadWaveform(const muse::io::path_t& path)
{
    if (!m_provider) {
        LOGW() << "audiotrack: no waveform provider registered, skipping waveform load";
        return;
    }

    if (path.empty()) {
        clearWaveform();
        return;
    }

    const uint64_t myGeneration = m_generation.fetch_add(1) + 1;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Wait for the previous decode to finish before starting another. Decoding is
    // CPU-bound and reads the disk; stacking them up would not finish any sooner, and
    // this keeps at most one worker alive.
    if (m_pending.valid()) {
        m_pending.wait();
    }

    m_pending = std::async(std::launch::async, [this, path, myGeneration]() {
        std::shared_ptr<WaveformCache> cache = buildCache(path);

        // Superseded by a newer request: drop this result rather than fighting over the
        // provider. This is what makes rapid file switching safe.
        if (m_generation.load() != myGeneration) {
            LOGI() << "audiotrack: waveform load superseded, discarding: " << path;
            return;
        }

        m_provider->setCache(cache);   // nullptr on failure -> hasWaveform() goes false
        m_waveformChanged.notify();
    });
}

void AudioWaveformService::clearWaveform()
{
    // Bump first so an in-flight decode cannot install its result afterwards.
    m_generation.fetch_add(1);

    if (m_provider) {
        m_provider->clearCache();
    }
    m_waveformChanged.notify();
}

bool AudioWaveformService::hasWaveform() const
{
    return m_provider && m_provider->hasWaveform();
}

async::Notification AudioWaveformService::waveformChanged() const
{
    return m_waveformChanged;
}
