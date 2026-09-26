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

#include <cmath>
#include <vector>

#include "audiotrack/audiofilereader.h"
#include "audiotrack/waveformcache.h"

using namespace muse;
using namespace muse::audiotrack;

namespace {
String testFile(const char* name)
{
    return String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT) + "/" + name);
}

const char* SWEEP = "data/sweep-stereo-44100.wav";
const char* METRONOME = "data/metronome-120bpm-48000.wav";
}

TEST(WaveformCacheTests, GeneratesExpectedLevelCountAndShapes)
{
    AudioFileReader r;
    ASSERT_TRUE(r.open(testFile(SWEEP))) << r.errorString();

    WaveformCache cache;
    ASSERT_TRUE(cache.generate(r, 0));

    EXPECT_EQ(cache.frames(), 44100 * 8);
    EXPECT_EQ(cache.sampleRate(), 44100);
    EXPECT_EQ(cache.channels(), 2);
    EXPECT_NEAR(cache.duration(), 8.0, 0.001);

    // L0 = one peak pair per 256 samples.
    const int64_t expectedL0 = (44100 * 8 + 255) / 256;
    EXPECT_EQ(static_cast<int64_t>(cache.peakCountAt(0)), expectedL0);

    // Each coarser level is 1/4 the size (levels only ever get shorter).
    EXPECT_LT(cache.peakCountAt(1), cache.peakCountAt(0));
    EXPECT_EQ(cache.samplesPerPixelAt(0), 256);
    EXPECT_EQ(cache.samplesPerPixelAt(1), 256 * 4);
    EXPECT_EQ(cache.samplesPerPixelAt(2), 256 * 16);
}

TEST(WaveformCacheTests, PeaksBracketTheSignalAndArePerChannel)
{
    AudioFileReader r;
    ASSERT_TRUE(r.open(testFile(SWEEP))) << r.errorString();

    WaveformCache left;
    WaveformCache right;
    ASSERT_TRUE(left.generate(r, 0));
    ASSERT_TRUE(right.generate(r, 1));

    std::vector<PeakPair> l(64), rr(64);
    left.peaksAt(0, 0, 64, l);
    right.peaksAt(0, 0, 64, rr);

    // min <= max always, for every pixel.
    for (const PeakPair& p : l) {
        EXPECT_LE(p.min, p.max);
    }

    // The fixture's right channel is the negation of the left, so for any window
    // right.min == -left.max and right.max == -left.min. This proves the per-channel
    // extraction reads the correct channel rather than duplicating channel 0.
    for (size_t i = 0; i < l.size(); ++i) {
        EXPECT_NEAR(rr[i].min, -l[i].max, 1e-3f) << "pixel " << i;
        EXPECT_NEAR(rr[i].max, -l[i].min, 1e-3f) << "pixel " << i;
    }
}

TEST(WaveformCacheTests, LevelSelectionGrowsMonotonicallyAndStaysInRange)
{
    AudioFileReader r;
    ASSERT_TRUE(r.open(testFile(SWEEP))) << r.errorString();

    WaveformCache cache;
    ASSERT_TRUE(cache.generate(r, 0));

    int prev = -1;
    for (int64_t spp = 256; spp <= 256 * 4 * 4 * 4 * 4; spp *= 2) {
        const int level = cache.levelForSamplesPerPixel(spp);
        EXPECT_GE(level, 0);
        EXPECT_GE(level, prev) << "level must not go backwards as density drops";
        // The chosen level must be fine enough to satisfy the request.
        EXPECT_LE(cache.samplesPerPixelAt(level), spp);
        prev = level;
    }
}

TEST(WaveformCacheTests, PeaksAtOutOfRangeReturnsSilenceNotCrash)
{
    AudioFileReader r;
    ASSERT_TRUE(r.open(testFile(SWEEP))) << r.errorString();

    WaveformCache cache;
    ASSERT_TRUE(cache.generate(r, 0));

    std::vector<PeakPair> out;
    cache.peaksAt(0, 100000000, 8, out);           // way past the end
    ASSERT_EQ(out.size(), 8u);
    for (const PeakPair& p : out) {
        EXPECT_FLOAT_EQ(p.min, 0.f);
        EXPECT_FLOAT_EQ(p.max, 0.f);
    }

    cache.peaksAt(999, 0, 8, out);                 // invalid level
    ASSERT_EQ(out.size(), 8u);
}

TEST(WaveformCacheTests, MetronomePeaksArePulseShaped)
{
    // Each 0.5 s window holds one click and then near-silence. At a coarse level the
    // click must still stand out: that is the property that makes a waveform useful
    // for aligning audio to bar lines.
    AudioFileReader r;
    ASSERT_TRUE(r.open(testFile(METRONOME))) << r.errorString();

    WaveformCache cache;
    ASSERT_TRUE(cache.generate(r, 0));

    // Pick a coarse level so one pixel spans well over a click.
    const int level = cache.levelForSamplesPerPixel(4800);
    const int64_t spp = cache.samplesPerPixelAt(level);

    float loudest = 0.f;
    float quietestNonZero = 1.f;
    const int64_t pixels = cache.peakCountAt(level);
    std::vector<PeakPair> p(static_cast<size_t>(pixels));
    cache.peaksAt(level, 0, pixels, p);

    for (const PeakPair& q : p) {
        loudest = std::max(loudest, q.max);
        if (q.max > 0.f) {
            quietestNonZero = std::min(quietestNonZero, q.max);
        }
    }

    EXPECT_GT(loudest, 0.5f) << "click peaks should survive downsampling";
    EXPECT_LT(quietestNonZero, loudest) << "there must be quiet gaps between clicks";
    EXPECT_GT(spp, 2400);   // sanity: this level is genuinely coarse
}

TEST(WaveformCacheTests, ClearsAndReportsEmpty)
{
    WaveformCache cache;
    EXPECT_TRUE(cache.isEmpty());

    AudioFileReader r;
    ASSERT_TRUE(r.open(testFile(SWEEP))) << r.errorString();
    ASSERT_TRUE(cache.generate(r, 0));
    EXPECT_FALSE(cache.isEmpty());

    cache.clear();
    EXPECT_TRUE(cache.isEmpty());
    EXPECT_EQ(cache.frames(), 0);
}
