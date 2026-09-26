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
#include <vector>

#include "audiotrack/audiotracksource.h"

using namespace muse;
using namespace muse::audiotrack;
using namespace muse::audio;

namespace {
String testFile(const char* name)
{
    return String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT) + "/" + name);
}

const char* SWEEP = "data/sweep-stereo-44100.wav";
const char* TONE = "data/tone-a440-mono-44100.wav";
const char* METRONOME = "data/metronome-120bpm-48000.wav";

void setSpec(AudioTrackSource& src, int rate, int channels)
{
    OutputSpec spec;
    spec.sampleRate = static_cast<sample_rate_t>(rate);
    spec.samplesPerChannel = 512;
    spec.audioChannelCount = static_cast<audioch_t>(channels);
    src.setOutputSpec(spec);
}

//! Peak absolute value, used to assert "this block is not silent".
float peak(const std::vector<float>& v)
{
    float m = 0.f;
    for (float s : v) {
        m = std::max(m, std::fabs(s));
    }
    return m;
}
}

TEST(AudioTrackSourceTests, ReportsFileInfoAndChannelCount)
{
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    EXPECT_TRUE(src.isValid());
    EXPECT_EQ(src.info().sampleRate, 44100);
    EXPECT_EQ(src.audioChannelsCount(), 2u);
    EXPECT_NEAR(src.info().duration, 8.0, 0.001);
}

TEST(AudioTrackSourceTests, ProducesNonSilentAudioAtFileRate)
{
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));

    setSpec(src, 44100, 2);   // same rate as the file: no resampling

    std::vector<float> buf(512 * 2, 0.f);
    src.process(buf.data(), 512);

    EXPECT_GT(peak(buf), 0.05f) << "a 20Hz..2kHz sweep should not be silent";
}

TEST(AudioTrackSourceTests, ResamplingKeepsBlockCadenceAcrossRates)
{
    // Regression guard for the resampler's position bookkeeping: for each output rate,
    // consuming N output frames must advance the source by N * (fileRate / outRate)
    // source frames, within one frame of slack.
    struct Case { int outRate; };
    const Case cases[] = { { 44100 }, { 48000 }, { 22050 }, { 88200 } };

    for (const Case& c : cases) {
        AudioTrackSource src;
        ASSERT_TRUE(src.load(testFile(SWEEP))) << "rate " << c.outRate;
        setSpec(src, c.outRate, 2);

        const int64_t frames = 4096;
        std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.f);
        src.process(buf.data(), frames);

        const double expected = static_cast<double>(frames) * 44100.0 / c.outRate;
        EXPECT_NEAR(src.positionSeconds() * 44100.0, expected, 1.0)
            << "outRate=" << c.outRate;
    }
}

TEST(AudioTrackSourceTests, SeekChangesPositionAndAudioContent)
{
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    setSpec(src, 44100, 2);

    std::vector<float> atStart(1024 * 2, 0.f);
    src.process(atStart.data(), 1024);

    src.seekTo(TimePosition::fromTime(secs_t(4.0), 44100));
    EXPECT_NEAR(src.positionSeconds(), 4.0, 0.01);

    std::vector<float> atFour(1024 * 2, 0.f);
    src.process(atFour.data(), 1024);

    // A sweep is a different waveform at 0 s and 4 s, so the blocks must differ.
    EXPECT_NE(atStart, atFour);
    EXPECT_GT(peak(atFour), 0.05f) << "seeking into the middle must still yield audio";
}

TEST(AudioTrackSourceTests, SeeksLandOnTheExactRequestedSample)
{
    // The strong form of the seek check: at output rate == file rate the source is a
    // pass-through, so its output must equal the raw file bytes at the seek offset.
    // This is what catches an off-by-one or a stale-cache splice after a seek.
    const int64_t targetFrame = 176400;   // 4.000 s

    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    setSpec(src, 44100, 2);
    src.seekTo(TimePosition::fromTime(secs_t(4.0), 44100));

    const int64_t n = 256;
    std::vector<float> fromSource(static_cast<size_t>(n) * 2, 0.f);
    src.process(fromSource.data(), n);

    AudioFileReader direct;
    ASSERT_TRUE(direct.open(testFile(SWEEP)));
    ASSERT_TRUE(direct.seekToFrame(targetFrame));
    std::vector<float> fromFile(static_cast<size_t>(n) * 2, 0.f);
    ASSERT_EQ(direct.readFrames(fromFile.data(), n, 2), n);

    for (int64_t i = 0; i < n * 2; ++i) {
        EXPECT_NEAR(fromSource[i], fromFile[i], 1e-6f) << "sample " << i;
    }
}

TEST(AudioTrackSourceTests, RepeatedSeeksAreIdempotent)
{
    // Seeking to the same place twice, with audio consumed in between, must yield the
    // same samples: a regression guard for cache/lookahead state leaking across seeks.
    const double t = 2.5;

    auto capture = [&](std::vector<float>& out) {
        AudioTrackSource s;
        EXPECT_TRUE(s.load(testFile(SWEEP)));
        setSpec(s, 44100, 2);
        s.seekTo(TimePosition::fromTime(secs_t(t), 44100));
        out.assign(512 * 2, 0.f);
        s.process(out.data(), 512);
    };

    std::vector<float> a, b;
    capture(a);
    capture(b);
    EXPECT_EQ(a, b);
}

TEST(AudioTrackSourceTests, SilenceAfterEndOfFile)
{
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    setSpec(src, 44100, 2);

    // Jump just past the end (file is 8.0 s).
    src.seekTo(TimePosition::fromTime(secs_t(8.001), 44100));

    std::vector<float> buf(512 * 2, 12345.f);
    const samples_t got = src.process(buf.data(), 512);

    EXPECT_EQ(got, 512u) << "process() should still report the block it filled";
    EXPECT_FLOAT_EQ(peak(buf), 0.f) << "past EOF must be digital silence";
}

TEST(AudioTrackSourceTests, HandlesOutputChannelChangeWithoutCrash)
{
    // The engine can change its output spec (device switch, export). The caches are
    // strided by channel count, so a change must be handled rather than read blindly.
    //
    // Uses the MONO fixture deliberately: the stereo sweep has its right channel in
    // anti-phase, and averaging anti-phase stereo to mono is exactly silence by
    // construction. That property is useful for AudioFileReaderTests (it proves the
    // channels are not swapped) but it would make this test unable to tell "correctly
    // silent" from "broken".
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(TONE)));

    setSpec(src, 44100, 1);   // mono out, matching the mono file
    std::vector<float> mono(512, 0.f);
    src.process(mono.data(), 512);
    EXPECT_GT(peak(mono), 0.05f);

    setSpec(src, 44100, 2);   // widen to stereo
    std::vector<float> stereo(512 * 2, 0.f);
    src.process(stereo.data(), 512);
    EXPECT_GT(peak(stereo), 0.05f);

    setSpec(src, 44100, 1);   // and back down again
    std::vector<float> mono2(512, 0.f);
    src.process(mono2.data(), 512);
    EXPECT_GT(peak(mono2), 0.05f);
}

TEST(AudioTrackSourceTests, AntiPhaseStereoDownmixIsSilentByConstruction)
{
    // Documents the property the previous test works around, so the next person does not
    // rediscover it as a "bug": the sweep fixture's channels cancel under a mono downmix.
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));

    setSpec(src, 44100, 2);
    std::vector<float> stereo(256 * 2, 0.f);
    src.process(stereo.data(), 256);
    EXPECT_GT(peak(stereo), 0.05f) << "stereo output must carry signal";

    AudioTrackSource src2;
    ASSERT_TRUE(src2.load(testFile(SWEEP)));
    setSpec(src2, 44100, 1);
    std::vector<float> mono(256, 0.f);
    src2.process(mono.data(), 256);
    EXPECT_FLOAT_EQ(peak(mono), 0.f) << "anti-phase downmix cancels exactly";
}

TEST(AudioTrackSourceTests, MetronomePeaksLandOnBeats)
{
    // The metronome fixture is 120 BPM: a short 1 kHz click exactly every 0.5 s.
    // Peaks must therefore be centred on multiples of 0.5 s. This checks that the
    // resampled stream stays on the original timeline (48k file -> 44.1k device).
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(METRONOME)));
    setSpec(src, 44100, 1);

    std::vector<float> all(static_cast<size_t>(44100) * 4, 0.f);   // first 4 s
    src.process(all.data(), 44100 * 4);

    // Find the loudest sample per 0.5 s window; each window holds exactly one click.
    for (int beat = 0; beat < 8; ++beat) {
        const size_t begin = static_cast<size_t>(beat) * 22050;
        const size_t end = begin + 22050;

        size_t loudest = begin;
        for (size_t i = begin; i < end; ++i) {
            if (std::fabs(all[i]) > std::fabs(all[loudest])) {
                loudest = i;
            }
        }

        const double t = static_cast<double>(loudest) / 44100.0;
        const double offsetInBeat = t - std::floor(t * 2.0) / 2.0;

        EXPECT_LT(offsetInBeat, 0.03)
            << "beat " << beat << ": click peak at " << t
            << "s is not near a 0.5s boundary";
    }
}

TEST(AudioTrackSourceTests, LoadFailureIsReportedNotCrashed)
{
    AudioTrackSource src;
    EXPECT_FALSE(src.load(testFile("data/nope.wav")));
    EXPECT_FALSE(src.isValid());

    // process() on an unloaded source must be safe and silent.
    setSpec(src, 44100, 2);
    std::vector<float> buf(64 * 2, 999.f);
    src.process(buf.data(), 64);
    EXPECT_FLOAT_EQ(peak(buf), 0.f);
}
