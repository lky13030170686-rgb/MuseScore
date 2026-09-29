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
#include "audiowaveformprovider.h"

#include <algorithm>
#include <cmath>

using namespace muse;
using namespace muse::audiotrack;
using namespace mu::engraving;

void AudioWaveformProvider::setCache(std::shared_ptr<const WaveformCache> cache)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache = std::move(cache);
}

void AudioWaveformProvider::clearCache()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.reset();
}

bool AudioWaveformProvider::hasWaveform() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache && !m_cache->isEmpty();
}

double AudioWaveformProvider::waveformDuration() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache ? m_cache->duration() : 0.0;
}

void AudioWaveformProvider::setScoreOffsetSeconds(double seconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_scoreOffsetSeconds = seconds;
}

double AudioWaveformProvider::scoreOffsetSeconds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_scoreOffsetSeconds;
}

void AudioWaveformProvider::waveformPeaks(double scoreFromSeconds, double scoreToSeconds, int64_t count,
                                          std::vector<AudioWaveformPeak>& out) const
{
    out.assign(static_cast<size_t>(std::max<int64_t>(0, count)), AudioWaveformPeak {});

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cache || m_cache->isEmpty() || count <= 0) {
        return;
    }

    // Callers ask in SCORE seconds, because that is what the engraver knows. The cache holds
    // the file, whose own beginning sounds at the alignment offset. Without this conversion
    // a track that had been moved would still be drawn where it used to be -- so lining the
    // audio up would have to be done blind.
    const double fromSeconds = scoreFromSeconds - m_scoreOffsetSeconds;
    const double toSeconds = scoreToSeconds - m_scoreOffsetSeconds;

    const WaveformCache& cache = *m_cache;

    const double span = toSeconds - fromSeconds;
    if (!(span > 0.0)) {
        return;
    }

    const double secondsPerColumn = span / static_cast<double>(count);

    // Choose the cache level whose density is closest to (but not finer than) what one
    // column needs, then reduce across every stored peak the column covers.
    const double framesPerColumn = secondsPerColumn * static_cast<double>(cache.sampleRate());
    const int level = cache.levelForSamplesPerPixel(
        static_cast<int64_t>(std::max(1.0, framesPerColumn)));
    const int64_t spp = cache.samplesPerPixelAt(level);
    const int64_t totalPeaks = static_cast<int64_t>(cache.peakCountAt(level));
    if (totalPeaks <= 0) {
        return;
    }

    std::vector<PeakPair> peaks;

    for (int64_t col = 0; col < count; ++col) {
        const double colFrom = fromSeconds + secondsPerColumn * static_cast<double>(col);
        const double colTo = colFrom + secondsPerColumn;

        const int64_t frameFrom = static_cast<int64_t>(std::floor(colFrom * cache.sampleRate()));
        const int64_t frameTo = static_cast<int64_t>(std::ceil(colTo * cache.sampleRate()));

        int64_t peakFrom = frameFrom / spp;
        int64_t peakTo = frameTo / spp;

        // Zoomed in past the stored resolution: still cover one peak so the lane is drawn.
        if (peakTo <= peakFrom) {
            peakTo = peakFrom + 1;
        }

        peakFrom = std::max<int64_t>(0, std::min<int64_t>(peakFrom, totalPeaks - 1));
        peakTo = std::max<int64_t>(peakFrom + 1, std::min<int64_t>(peakTo, totalPeaks));

        peaks.clear();
        cache.peaksAt(level, peakFrom, peakTo - peakFrom, peaks);
        if (peaks.empty()) {
            continue;
        }

        float mn = peaks[0].min;
        float mx = peaks[0].max;
        for (const PeakPair& p : peaks) {
            mn = std::min(mn, p.min);
            mx = std::max(mx, p.max);
        }

        out[static_cast<size_t>(col)] = AudioWaveformPeak { mn, mx };
    }
}
