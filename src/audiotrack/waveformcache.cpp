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
#include "waveformcache.h"

#include <algorithm>
#include <cmath>

#include "log.h"

using namespace muse::audiotrack;

bool WaveformCache::generate(AudioFileReader& reader, int channel)
{
    clear();

    if (!reader.isOpen()) {
        return false;
    }

    const AudioFileInfo& info = reader.info();
    if (channel < 0 || channel >= info.channels) {
        channel = 0;
    }

    m_frames = info.frames;
    m_sampleRate = info.sampleRate;
    m_channels = info.channels;
    m_channel = channel;

    const int64_t totalPixels = (m_frames + BASE_SAMPLES_PER_PIXEL - 1) / BASE_SAMPLES_PER_PIXEL;
    if (totalPixels <= 0) {
        return false;
    }

    // ── Level 0: one streaming pass over the file ──────────────────────────────
    // Streaming, not readAll: 5 min stereo float32 would be ~106 MB in one buffer.
    m_levels[0].resize(static_cast<size_t>(totalPixels));

    constexpr int64_t CHUNK = 16384;
    std::vector<float> buf(static_cast<size_t>(CHUNK) * info.channels, 0.f);

    if (!reader.seekToFrame(0)) {
        clear();
        return false;
    }

    int64_t framesDone = 0;
    int64_t pixel = 0;

    // Running min/max for the pixel currently being accumulated.
    float curMin = 0.f;
    float curMax = 0.f;
    int64_t curCount = 0;

    while (framesDone < m_frames) {
        const int64_t want = std::min<int64_t>(CHUNK, m_frames - framesDone);
        const int64_t got = reader.readFrames(buf.data(), want, info.channels);
        if (got <= 0) {
            break;
        }

        for (int64_t f = 0; f < got; ++f) {
            const float v = buf[static_cast<size_t>(f) * info.channels + channel];

            if (curCount == 0) {
                curMin = v;
                curMax = v;
            } else {
                curMin = std::min(curMin, v);
                curMax = std::max(curMax, v);
            }
            ++curCount;

            if (curCount == BASE_SAMPLES_PER_PIXEL) {
                if (pixel < totalPixels) {
                    m_levels[0][static_cast<size_t>(pixel)] = PeakPair { curMin, curMax };
                }
                ++pixel;
                curCount = 0;
            }
        }

        framesDone += got;
    }

    // Flush the trailing partial pixel so the tail of the file is not dropped.
    if (curCount > 0 && pixel < totalPixels) {
        m_levels[0][static_cast<size_t>(pixel)] = PeakPair { curMin, curMax };
        ++pixel;
    }

    if (pixel == 0) {
        clear();
        return false;
    }

    // Trim to what we actually produced (a short read at EOF is possible).
    m_levels[0].resize(static_cast<size_t>(pixel));

    // ── Coarser levels: reduce from the previous one, never from the raw file ──
    m_levelCount = 1;
    for (size_t level = 1; level < MAX_LEVELS; ++level) {
        const std::vector<PeakPair>& prev = m_levels[level - 1];
        if (prev.size() <= 1) {
            break;
        }

        const size_t count = (prev.size() + LEVEL_FACTOR - 1) / LEVEL_FACTOR;
        m_levels[level].resize(count);

        for (size_t i = 0; i < count; ++i) {
            const size_t begin = i * LEVEL_FACTOR;
            const size_t end = std::min(begin + LEVEL_FACTOR, prev.size());

            float mn = prev[begin].min;
            float mx = prev[begin].max;
            for (size_t j = begin + 1; j < end; ++j) {
                mn = std::min(mn, prev[j].min);
                mx = std::max(mx, prev[j].max);
            }
            m_levels[level][i] = PeakPair { mn, mx };
        }

        m_levelCount = static_cast<int>(level) + 1;
    }

    LOGI() << "waveform cache generated: frames " << m_frames
           << ", channel " << m_channel << "/" << m_channels
           << ", levels " << m_levelCount
           << ", L0 peaks " << m_levels[0].size();

    return true;
}

void WaveformCache::clear()
{
    for (size_t i = 0; i < MAX_LEVELS; ++i) {
        m_levels[i].clear();
    }
    m_levelCount = 0;
    m_frames = 0;
    m_sampleRate = 0;
    m_channels = 0;
    m_channel = 0;
}

double WaveformCache::duration() const
{
    if (m_sampleRate <= 0) {
        return 0.0;
    }
    return static_cast<double>(m_frames) / static_cast<double>(m_sampleRate);
}

int64_t WaveformCache::samplesPerPixelAt(int level) const
{
    int64_t spp = BASE_SAMPLES_PER_PIXEL;
    for (int i = 0; i < level; ++i) {
        spp *= LEVEL_FACTOR;
    }
    return spp;
}

int WaveformCache::levelForSamplesPerPixel(int64_t spp) const
{
    // Walk up while the level's density is still finer than or equal to the request.
    int level = 0;
    while (level + 1 < m_levelCount && samplesPerPixelAt(level + 1) <= spp) {
        ++level;
    }
    return level;
}

void WaveformCache::peaksAt(int level, int64_t startPixel, int64_t count, std::vector<PeakPair>& out) const
{
    out.assign(static_cast<size_t>(std::max<int64_t>(0, count)), PeakPair { 0.f, 0.f });

    if (level < 0 || level >= m_levelCount || count <= 0) {
        return;
    }

    const std::vector<PeakPair>& lvl = m_levels[level];
    const int64_t n = static_cast<int64_t>(lvl.size());

    for (int64_t i = 0; i < count; ++i) {
        const int64_t p = startPixel + i;
        if (p >= 0 && p < n) {
            out[static_cast<size_t>(i)] = lvl[static_cast<size_t>(p)];
        }
    }
}
