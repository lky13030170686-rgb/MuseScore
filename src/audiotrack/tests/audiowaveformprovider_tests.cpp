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

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "audiotrack/audiofilereader.h"
#include "audiotrack/audiowaveformprovider.h"
#include "audiotrack/waveformcache.h"

using namespace muse;
using namespace muse::audiotrack;
using mu::engraving::AudioWaveformPeak;

namespace {
const char* METRONOME = "data/metronome-120bpm-48000.wav";
const char* SWEEP = "data/sweep-stereo-44100.wav";

String testFile(const char* name)
{
    return String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT) + "/" + name);
}

std::shared_ptr<WaveformCache> makeCache(const char* file)
{
    AudioFileReader r;
    if (!r.open(testFile(file))) {
        return nullptr;
    }
    auto cache = std::make_shared<WaveformCache>();
    if (!cache->generate(r, 0)) {
        return nullptr;
    }
    return cache;
}
}

TEST(AudioWaveformProviderTests, ReportsNoWaveformUntilACacheIsInstalled)
{
    AudioWaveformProvider provider;
    EXPECT_FALSE(provider.hasWaveform());
    EXPECT_DOUBLE_EQ(provider.waveformDuration(), 0.0);

    std::vector<AudioWaveformPeak> peaks;
    provider.waveformPeaks(0.0, 1.0, 16, peaks);
    ASSERT_EQ(peaks.size(), 16u);
    for (const auto& p : peaks) {
        EXPECT_FLOAT_EQ(p.min, 0.f);
        EXPECT_FLOAT_EQ(p.max, 0.f);
    }
}

TEST(AudioWaveformProviderTests, ReportsDurationAndPeaksAfterInstall)
{
    auto cache = makeCache(METRONOME);
    ASSERT_TRUE(cache);

    AudioWaveformProvider provider;
    provider.setCache(cache);

    EXPECT_TRUE(provider.hasWaveform());
    EXPECT_NEAR(provider.waveformDuration(), cache->duration(), 1e-9);

    // Peaks over the first click must be loud; peaks over a gap must be silent.
    std::vector<AudioWaveformPeak> click, gap;
    provider.waveformPeaks(0.0, 0.05, 64, click);
    provider.waveformPeaks(0.25, 0.30, 64, gap);
    ASSERT_EQ(click.size(), 64u);
    ASSERT_EQ(gap.size(), 64u);

    float clickMax = 0.f;
    for (const auto& p : click) {
        clickMax = std::max(clickMax, p.max);
    }
    float gapMax = 0.f;
    for (const auto& p : gap) {
        gapMax = std::max(gapMax, p.max);
    }

    EXPECT_GT(clickMax, 0.4f) << "the click should be visible";
    EXPECT_LT(gapMax, 0.01f) << "the gap should be silent";
}

TEST(AudioWaveformProviderTests, ClearsCache)
{
    auto cache = makeCache(SWEEP);
    ASSERT_TRUE(cache);

    AudioWaveformProvider provider;
    provider.setCache(cache);
    ASSERT_TRUE(provider.hasWaveform());

    provider.clearCache();
    EXPECT_FALSE(provider.hasWaveform());
    EXPECT_DOUBLE_EQ(provider.waveformDuration(), 0.0);
}

TEST(AudioWaveformProviderTests, HandlesDegenerateQueriesWithoutCrashing)
{
    auto cache = makeCache(SWEEP);
    ASSERT_TRUE(cache);

    AudioWaveformProvider provider;
    provider.setCache(cache);

    std::vector<AudioWaveformPeak> peaks;

    // Zero count, inverted window, window past the end, and a window far beyond the file.
    provider.waveformPeaks(0.0, 1.0, 0, peaks);
    EXPECT_TRUE(peaks.empty());

    provider.waveformPeaks(2.0, 1.0, 8, peaks);
    EXPECT_EQ(peaks.size(), 8u);

    provider.waveformPeaks(1000.0, 1001.0, 8, peaks);
    EXPECT_EQ(peaks.size(), 8u);

    provider.waveformPeaks(-5.0, -4.0, 8, peaks);
    EXPECT_EQ(peaks.size(), 8u);
}

TEST(AudioWaveformProviderTests, WindowRequestsArePositionallyCorrect)
{
    // The property engraving depends on: the peaks returned for a window must describe
    // THAT window. Over the metronome, the window containing a click must be loud and the
    // window containing the following gap must be silent, for several windows in a row.
    auto cache = makeCache(METRONOME);
    ASSERT_TRUE(cache);

    AudioWaveformProvider provider;
    provider.setCache(cache);

    for (int beat = 0; beat < 4; ++beat) {
        const double beatSec = 0.5 * beat;

        std::vector<AudioWaveformPeak> onBeat, offBeat;
        provider.waveformPeaks(beatSec, beatSec + 0.03, 32, onBeat);
        provider.waveformPeaks(beatSec + 0.25, beatSec + 0.28, 32, offBeat);

        float onMax = 0.f;
        for (const auto& p : onBeat) {
            onMax = std::max(onMax, p.max);
        }
        float offMax = 0.f;
        for (const auto& p : offBeat) {
            offMax = std::max(offMax, p.max);
        }

        EXPECT_GT(onMax, 0.4f) << "beat " << beat << " should show its click";
        EXPECT_LT(offMax, 0.01f) << "beat " << beat << " gap should be silent";
    }
}

TEST(AudioWaveformProviderTests, ZoomedInPastStoredResolutionStillReturnsData)
{
    // Engraving can ask for far more columns than there are stored peaks once a measure
    // is zoomed in. The provider must still fill every column rather than returning zeros.
    auto cache = makeCache(SWEEP);
    ASSERT_TRUE(cache);

    AudioWaveformProvider provider;
    provider.setCache(cache);

    // 1 ms across 500 columns is ~44 frames per column at L0=256 samples per peak, i.e.
    // well below the stored resolution.
    std::vector<AudioWaveformPeak> peaks;
    provider.waveformPeaks(2.0, 2.001, 500, peaks);
    ASSERT_EQ(peaks.size(), 500u);

    int nonZero = 0;
    for (const auto& p : peaks) {
        if (p.max > 0.f || p.min < 0.f) {
            ++nonZero;
        }
    }
    EXPECT_GT(nonZero, 400) << "zoomed-in columns should carry real amplitude";
}
