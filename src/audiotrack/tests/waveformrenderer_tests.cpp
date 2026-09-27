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
#include "audiotrack/waveformrenderer.h"

// LineF is only forward-declared by waveformrenderer.h, but the tests hold it in a
// std::vector, which requires the complete type.
#include "draw/types/geometry.h"

using namespace muse;
using namespace muse::audiotrack;

namespace {
String testFile(const char* name)
{
    return String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT) + "/" + name);
}

const char* SWEEP = "data/sweep-stereo-44100.wav";
const char* METRONOME = "data/metronome-120bpm-48000.wav";

//! Builds a cache from a fixture, since the renderer deliberately only consumes a cache.
bool makeCache(const char* file, int channel, WaveformCache& cache)
{
    AudioFileReader r;
    if (!r.open(testFile(file))) {
        return false;
    }
    return cache.generate(r, channel);
}
}

TEST(WaveformRendererTests, ProducesOneLinePerPixelColumn)
{
    WaveformCache cache;
    ASSERT_TRUE(makeCache(SWEEP, 0, cache));

    WaveformRenderRequest req;
    req.x = 10.0;
    req.y = 20.0;
    req.width = 400.0;
    req.height = 60.0;
    req.fromSeconds = 0.0;
    req.toSeconds = cache.duration();

    const std::vector<muse::LineF> lines = WaveformRenderer::buildLines(cache, req);
    EXPECT_EQ(static_cast<int64_t>(lines.size()), 400);

    // Every line must sit inside the requested rect: xs spread across it, ys within it.
    for (size_t i = 0; i < lines.size(); ++i) {
        const double x = lines[i].p1().x();
        EXPECT_GE(x, req.x);
        EXPECT_LE(x, req.x + req.width);
        EXPECT_NEAR(x, req.x + static_cast<double>(i) + 0.5, 1e-6);

        for (const auto& p : { lines[i].p1(), lines[i].p2() }) {
            EXPECT_GE(p.y(), req.y - 1e-6);
            EXPECT_LE(p.y(), req.y + req.height + 1e-6);
        }
    }
}

TEST(WaveformRendererTests, SilentColumnsCollapseToCentreLine)
{
    // The metronome fixture has a 1 kHz click at each 0.5 s boundary and true digital
    // silence in between (verified against the raw samples: 0.25-0.30 s is all zeros).
    // Silent columns must still produce a visible hairline so the trace reads as a
    // continuous centre line rather than a gap.
    WaveformCache cache;
    ASSERT_TRUE(makeCache(METRONOME, 0, cache));

    WaveformRenderRequest req;
    req.width = 400.0;
    req.height = 50.0;
    req.fromSeconds = 0.25;   // fully inside the silent gap
    req.toSeconds = 0.30;
    req.amplitudeRatio = 0.45;

    const std::vector<muse::LineF> lines = WaveformRenderer::buildLines(cache, req);
    ASSERT_EQ(lines.size(), 400u);

    const double centreY = req.y + req.height * req.centreRatio;
    for (const auto& l : lines) {
        const double len = std::abs(l.p2().y() - l.p1().y());
        EXPECT_GT(len, 0.0) << "silence must still draw a hairline, not a gap";
        EXPECT_LT(len, 0.6) << "a silent column must not draw a tall line";
        EXPECT_NEAR((l.p1().y() + l.p2().y()) / 2.0, centreY, 1e-9)
            << "silent columns must straddle the centre line exactly";
    }
}

TEST(WaveformRendererTests, LoudAudioSpansMoreThanQuietAudio)
{
    // The sweep rises in frequency but keeps its amplitude; the metronome has loud clicks
    // and silent gaps. Rather than compare across files, compare a loud window against a
    // near-silent window of the SAME file, which is the property that matters visually.
    WaveformCache cache;
    ASSERT_TRUE(makeCache(METRONOME, 0, cache));

    auto spanAt = [&](double fromSec, double toSec) {
        WaveformRenderRequest req;
        req.width = 200.0;
        req.height = 100.0;
        req.fromSeconds = fromSec;
        req.toSeconds = toSec;
        const std::vector<muse::LineF> lines = WaveformRenderer::buildLines(cache, req);
        double total = 0.0;
        for (const auto& l : lines) {
            total += std::abs(l.p2().y() - l.p1().y());
        }
        return total;
    };

    // 0.00-0.05 s holds the click's attack; 0.25-0.30 s is the quiet gap before the next.
    const double loud = spanAt(0.0, 0.05);
    const double quiet = spanAt(0.25, 0.30);
    EXPECT_GT(loud, quiet * 2.0) << "click attack should draw far taller than the gap";
}

TEST(WaveformRendererTests, ZoomingIntoAShortWindowStillDraws)
{
    // Regression guard for the peak-index clamping: when the visible window is shorter
    // than one stored peak, peakTo <= peakFrom, and a naive implementation draws nothing.
    WaveformCache cache;
    ASSERT_TRUE(makeCache(SWEEP, 0, cache));

    WaveformRenderRequest req;
    req.width = 500.0;
    req.height = 40.0;
    req.fromSeconds = 2.0;
    req.toSeconds = 2.001;    // ~44 frames, far less than one 256-sample peak

    const std::vector<muse::LineF> lines = WaveformRenderer::buildLines(cache, req);
    EXPECT_EQ(lines.size(), 500u) << "must still emit a line per column when zoomed in";

    double maxLen = 0.0;
    for (const auto& l : lines) {
        maxLen = std::max(maxLen, std::abs(l.p2().y() - l.p1().y()));
    }
    EXPECT_GT(maxLen, 1.0) << "zoomed-in view should show real amplitude, not a flat line";
}

TEST(WaveformRendererTests, HandlesDegenerateRequestsWithoutCrashing)
{
    WaveformCache cache;
    ASSERT_TRUE(makeCache(SWEEP, 0, cache));

    WaveformRenderRequest req;

    // zero width
    req.width = 0.0;
    req.height = 50.0;
    req.fromSeconds = 0.0;
    req.toSeconds = 1.0;
    EXPECT_TRUE(WaveformRenderer::buildLines(cache, req).empty());

    // zero height
    req.width = 100.0;
    req.height = 0.0;
    EXPECT_TRUE(WaveformRenderer::buildLines(cache, req).empty());

    // inverted time window
    req.height = 50.0;
    req.fromSeconds = 2.0;
    req.toSeconds = 1.0;
    EXPECT_TRUE(WaveformRenderer::buildLines(cache, req).empty());

    // window entirely past the end of the audio
    req.fromSeconds = 1000.0;
    req.toSeconds = 1001.0;
    const std::vector<muse::LineF> past = WaveformRenderer::buildLines(cache, req);
    EXPECT_EQ(past.size(), 100u) << "still emits columns; they are just silent";
}

TEST(WaveformRendererTests, EmptyCacheProducesNothing)
{
    WaveformCache cache;   // never generated
    EXPECT_TRUE(cache.isEmpty());

    WaveformRenderRequest req;
    req.width = 100.0;
    req.height = 50.0;
    req.fromSeconds = 0.0;
    req.toSeconds = 1.0;

    EXPECT_TRUE(WaveformRenderer::buildLines(cache, req).empty());
    EXPECT_EQ(WaveformRenderer::lineCount(req), 100);
}

TEST(WaveformRendererTests, NormalisationRangeChangesAmplitude)
{
    WaveformCache cache;
    ASSERT_TRUE(makeCache(METRONOME, 0, cache));

    WaveformRenderRequest req;
    req.width = 400.0;
    req.height = 100.0;
    req.fromSeconds = 0.0;
    req.toSeconds = 0.05;

    auto totalSpan = [&](float lo, float hi) {
        double total = 0.0;
        for (const auto& l : WaveformRenderer::buildLines(cache, req, lo, hi)) {
            total += std::abs(l.p2().y() - l.p1().y());
        }
        return total;
    };

    // The click peaks near 0.95, so squeezing the range to -0.5..0.5 must clamp hard and
    // draw visibly taller than the raw -1..1 mapping.
    const double raw = totalSpan(0.f, 0.f);
    const double tight = totalSpan(-0.5f, 0.5f);
    EXPECT_GT(tight, raw) << "a tighter normalisation range should magnify the trace";
}

TEST(WaveformRendererTests, ColumnsMapToTheCorrectTimePosition)
{
    // The strongest check that the time->column mapping is right: the metronome has a
    // click exactly at each 0.5 s boundary, so over a 2 s window the tallest columns must
    // land at the columns that correspond to 0.0 / 0.5 / 1.0 / 1.5 s. A mapping bug
    // (off-by-one, wrong level, swapped from/to) shifts them and fails here.
    WaveformCache cache;
    ASSERT_TRUE(makeCache(METRONOME, 0, cache));

    const double fromSec = 0.0;
    const double toSec = 2.0;
    WaveformRenderRequest req;
    req.width = 400.0;
    req.height = 80.0;
    req.fromSeconds = fromSec;
    req.toSeconds = toSec;

    const std::vector<muse::LineF> lines = WaveformRenderer::buildLines(cache, req);
    ASSERT_EQ(lines.size(), 400u);

    const double secondsPerColumn = (toSec - fromSec) / 400.0;

    // For each expected click, find the loudest column within +-40 ms and check it is
    // clearly taller than the surrounding silence.
    //
    // Threshold note: the fixture's click is a 1 kHz sine with an exponential decay
    // (amplitude 0.95 on the downbeat, 0.6 elsewhere). A 256-sample peak window (5.3 ms at
    // 48 kHz) does not catch the sine crest, so the measured peak is ~0.52 for weak beats
    // (verified against the raw samples). The rendered line length is
    // amplitude * height * amplitudeRatio, so a weak click gives ~0.52 * 80 * 0.45 = 18.7.
    // The bar below sits between that and what a silent column would give (~0.5).
    const double weakClickLineLen = 0.50 * req.height * req.amplitudeRatio;
    const double minClickHeight = weakClickLineLen * 0.8;   // ~14.4, comfortably above gap

    for (double clickSec : { 0.0, 0.5, 1.0, 1.5 }) {
        const int64_t centreCol = static_cast<int64_t>(clickSec / secondsPerColumn);
        const int64_t win = 20;   // +-40 ms at this zoom

        double best = 0.0;
        for (int64_t c = std::max<int64_t>(0, centreCol - win);
             c <= std::min<int64_t>(399, centreCol + win); ++c) {
            best = std::max(best, std::abs(lines[static_cast<size_t>(c)].p2().y()
                                           - lines[static_cast<size_t>(c)].p1().y()));
        }
        EXPECT_GT(best, minClickHeight)
            << "no tall column near the click at " << clickSec << " s";
    }

    // And the gaps between clicks must be much shorter than the clicks.
    const int64_t gapCol = static_cast<int64_t>(0.25 / secondsPerColumn);
    const double gapLen = std::abs(lines[static_cast<size_t>(gapCol)].p2().y()
                                   - lines[static_cast<size_t>(gapCol)].p1().y());
    EXPECT_LT(gapLen, req.height * 0.1) << "the gap between clicks should be near-silent";
}
