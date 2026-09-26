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
#ifndef MUSE_AUDIOTRACK_WAVEFORMCACHE_H
#define MUSE_AUDIOTRACK_WAVEFORMCACHE_H

#include <cstdint>
#include <vector>

#include "audiofilereader.h"

namespace muse::audiotrack {
//! One (min, max) pair per pixel, matching BBC audiowaveform's data model.
struct PeakPair
{
    float min = 0.f;
    float max = 0.f;
};

//! Multi-level peak cache (mipmap), copied from BBC audiowaveform's approach.
//!
//! 5 minutes of 44.1 kHz stereo is ~26 M samples; drawing per sample is impossible.
//! Instead we keep a pyramid of (min,max) pairs and draw ONE vertical line per pixel.
//!
//! Levels use samplesPerPixel = 256 * 4^k, matching the recommendation in
//! 音频轨/拼接方案.md §8.1.
//!
//! ⚠️ Levels can only be built coarse FROM fine, never the reverse — the same
//! constraint audiowaveform documents.
class WaveformCache
{
public:
    static constexpr int64_t BASE_SAMPLES_PER_PIXEL = 256;
    static constexpr int LEVEL_FACTOR = 4;
    static constexpr size_t MAX_LEVELS = 8;

    //! Builds all levels by streaming the file once. Per-channel.
    //! Generation must run off the UI thread; nothing here touches Qt.
    bool generate(AudioFileReader& reader, int channel);

    void clear();
    bool isEmpty() const { return m_levels[0].empty(); }

    int64_t frames() const { return m_frames; }
    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }
    int channel() const { return m_channel; }
    double duration() const;

    int64_t samplesPerPixelAt(int level) const;
    size_t peakCountAt(int level) const { return m_levels[level].size(); }

    //! Picks the finest level whose samplesPerPixel is <= the requested density,
    //! so each output pixel reads a bounded number of peaks.
    int levelForSamplesPerPixel(int64_t spp) const;

    //! Reads the peaks for [startPixel, startPixel + count) at the given level.
    //! Out-of-range entries come back as silence.
    void peaksAt(int level, int64_t startPixel, int64_t count, std::vector<PeakPair>& out) const;

private:
    std::vector<PeakPair> m_levels[MAX_LEVELS];
    int m_levelCount = 0;
    int64_t m_frames = 0;
    int m_sampleRate = 0;
    int m_channels = 0;
    int m_channel = 0;
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_WAVEFORMCACHE_H
