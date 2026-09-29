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

//! Sets the source up the way the engine does once playback begins: the output spec, and the
//! transport running.
//!
//! A file source is silent unless the transport runs -- that is what stops a backing track
//! from playing the moment it is loaded -- so a test that expects audio has to start it, just
//! as AudioContext does. Tests that specifically want a stopped source use setSpec() alone.
void prepareForPlayback(AudioTrackSource& src, int rate, int channels)
{
    setSpec(src, rate, channels);
    src.setMode(ProcessMode::Playing);
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

    prepareForPlayback(src, 44100, 2);   // same rate as the file: no resampling

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
        prepareForPlayback(src, c.outRate, 2);

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
    prepareForPlayback(src, 44100, 2);

    std::vector<float> atStart(1024 * 2, 0.f);
    src.process(atStart.data(), 1024);

    src.seekTo(TimePosition::fromTime(secs_t(4.0), 44100));

    // NOTE: seeks are DEFERRED. seekTo() only publishes a target; the audio thread applies
    // it at the top of the next process(). That deferral is what makes the source
    // thread-safe (the audio thread is the sole writer of playback state), so the position
    // must NOT have moved yet — asserting otherwise would be asserting the old race.
    std::vector<float> atFour(1024 * 2, 0.f);
    src.process(atFour.data(), 1024);

    // After consuming one block we are at the seek target plus that block.
    EXPECT_NEAR(src.positionSeconds(), 4.0 + 1024.0 / 44100.0, 0.01);

    // A sweep is a different waveform at 0 s and 4 s, so the blocks must differ.
    EXPECT_NE(atStart, atFour);
    EXPECT_GT(peak(atFour), 0.05f) << "seeking into the middle must still yield audio";
}

TEST(AudioTrackSourceTests, DeferredSeekIsAppliedExactlyOnce)
{
    // Guards the thread-safety contract: a published seek must be consumed by the very
    // next process() call and must not be re-applied afterwards (re-applying would snap
    // playback back and make audio stutter on every block).
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    prepareForPlayback(src, 44100, 2);

    src.seekTo(TimePosition::fromTime(secs_t(2.0), 44100));
    std::vector<float> first(512 * 2, 0.f);
    src.process(first.data(), 512);

    const double afterFirst = src.positionSeconds();
    ASSERT_NEAR(afterFirst, 2.0 + 512.0 / 44100.0, 0.01);

    // A second block must simply continue; it must not jump back to 2.0 s again.
    std::vector<float> second(512 * 2, 0.f);
    src.process(second.data(), 512);

    EXPECT_NEAR(src.positionSeconds(), afterFirst + 512.0 / 44100.0, 0.01)
        << "seek was re-applied on a later block";
}

TEST(AudioTrackSourceTests, SeeksLandOnTheExactRequestedSample)
{
    // The strong form of the seek check: at output rate == file rate the source is a
    // pass-through, so its output must equal the raw file bytes at the seek offset.
    // This is what catches an off-by-one or a stale-cache splice after a seek.
    const int64_t targetFrame = 176400;   // 4.000 s

    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    prepareForPlayback(src, 44100, 2);
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
        prepareForPlayback(s, 44100, 2);
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
    prepareForPlayback(src, 44100, 2);

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

    prepareForPlayback(src, 44100, 1);   // mono out, matching the mono file
    std::vector<float> mono(512, 0.f);
    src.process(mono.data(), 512);
    EXPECT_GT(peak(mono), 0.05f);

    prepareForPlayback(src, 44100, 2);   // widen to stereo
    std::vector<float> stereo(512 * 2, 0.f);
    src.process(stereo.data(), 512);
    EXPECT_GT(peak(stereo), 0.05f);

    prepareForPlayback(src, 44100, 1);   // and back down again
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

    prepareForPlayback(src, 44100, 2);
    std::vector<float> stereo(256 * 2, 0.f);
    src.process(stereo.data(), 256);
    EXPECT_GT(peak(stereo), 0.05f) << "stereo output must carry signal";

    AudioTrackSource src2;
    ASSERT_TRUE(src2.load(testFile(SWEEP)));
    // The transport has to be running, otherwise the silence asserted below would be the
    // "transport stopped" silence rather than the downmix cancellation being demonstrated.
    prepareForPlayback(src2, 44100, 1);
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
    prepareForPlayback(src, 44100, 1);

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

// ---- alignment offset ----------------------------------------------------
//
// The engine seeks this source by TRANSPORT position, so the source is what turns that into
// a position in the file: file = transport - offset. These tests pin the mapping by
// comparing against the raw file bytes, the same way the seek tests above do -- a wrong
// mapping that merely sounds "about right" would still pass a looser check.

TEST(AudioTrackSourceTests, StartOffsetDelaysWhereTheFileBegins)
{
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    prepareForPlayback(src, 44100, 2);

    // The audio begins 2 s into the score.
    src.setStartOffsetSeconds(2.0);
    src.seekTo(TimePosition::fromTime(secs_t(0.0), 44100));

    std::vector<float> firstBlock(512 * 2, 12345.f);
    src.process(firstBlock.data(), 512);
    EXPECT_FLOAT_EQ(peak(firstBlock), 0.f)
        << "the file must not be heard before its offset has been reached";

    // Consume everything up to, but not including, the entry point.
    const samples_t remaining = static_cast<samples_t>(2.0 * 44100) - 512;
    std::vector<float> upToEntry(static_cast<size_t>(remaining) * 2, 0.f);
    src.process(upToEntry.data(), remaining);
    EXPECT_FLOAT_EQ(peak(upToEntry), 0.f) << "still silent right up to the offset";

    // What follows must be the file's own beginning, sample for sample.
    const int64_t n = 256;
    std::vector<float> afterEntry(static_cast<size_t>(n) * 2, 0.f);
    src.process(afterEntry.data(), n);

    AudioFileReader direct;
    ASSERT_TRUE(direct.open(testFile(SWEEP)));
    ASSERT_TRUE(direct.seekToFrame(0));
    std::vector<float> fromFile(static_cast<size_t>(n) * 2, 0.f);
    ASSERT_EQ(direct.readFrames(fromFile.data(), n, 2), n);

    for (int64_t i = 0; i < n * 2; ++i) {
        EXPECT_NEAR(afterEntry[i], fromFile[i], 1e-6f)
            << "sample " << i << ": the entry must be the file's first sample, not a "
               "block-aligned approximation of it";
    }
}

TEST(AudioTrackSourceTests, StartOffsetShiftsWhereTheFilePlaysFrom)
{
    // With the audio starting 2 s into the score, the transport at 5 s is hearing the file
    // at 3 s.
    const double offset = 2.0;
    const double scorePos = 5.0;

    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    prepareForPlayback(src, 44100, 2);
    src.setStartOffsetSeconds(offset);
    src.seekTo(TimePosition::fromTime(secs_t(scorePos), 44100));

    const int64_t n = 256;
    std::vector<float> fromSource(static_cast<size_t>(n) * 2, 0.f);
    src.process(fromSource.data(), n);

    AudioFileReader direct;
    ASSERT_TRUE(direct.open(testFile(SWEEP)));
    ASSERT_TRUE(direct.seekToFrame(static_cast<int64_t>((scorePos - offset) * 44100)));
    std::vector<float> fromFile(static_cast<size_t>(n) * 2, 0.f);
    ASSERT_EQ(direct.readFrames(fromFile.data(), n, 2), n);

    for (int64_t i = 0; i < n * 2; ++i) {
        EXPECT_NEAR(fromSource[i], fromFile[i], 1e-6f) << "sample " << i;
    }
}

TEST(AudioTrackSourceTests, NegativeStartOffsetMeansTheFileIsAlreadyUnderway)
{
    // A file whose beginning belongs before the score does: at the score's start it is
    // already 2 s in. This is the count-in case, and it is the sign that a plain "delay"
    // cannot express.
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    prepareForPlayback(src, 44100, 2);
    src.setStartOffsetSeconds(-2.0);
    src.seekTo(TimePosition::fromTime(secs_t(0.0), 44100));

    const int64_t n = 256;
    std::vector<float> fromSource(static_cast<size_t>(n) * 2, 0.f);
    src.process(fromSource.data(), n);

    AudioFileReader direct;
    ASSERT_TRUE(direct.open(testFile(SWEEP)));
    ASSERT_TRUE(direct.seekToFrame(static_cast<int64_t>(2.0 * 44100)));
    std::vector<float> fromFile(static_cast<size_t>(n) * 2, 0.f);
    ASSERT_EQ(direct.readFrames(fromFile.data(), n, 2), n);

    for (int64_t i = 0; i < n * 2; ++i) {
        EXPECT_NEAR(fromSource[i], fromFile[i], 1e-6f) << "sample " << i;
    }
}

TEST(AudioTrackSourceTests, ChangingTheOffsetRelaysTheAudioImmediately)
{
    // The offset is meant to be dialled in by ear while the music plays, so a change has to
    // take effect at once rather than at the next seek.
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    prepareForPlayback(src, 44100, 2);

    src.seekTo(TimePosition::fromTime(secs_t(4.0), 44100));
    std::vector<float> block(512 * 2, 0.f);
    src.process(block.data(), 512);

    // Shift the audio one second later without seeking.
    src.setStartOffsetSeconds(1.0);
    std::vector<float> afterShift(512 * 2, 0.f);
    src.process(afterShift.data(), 512);

    // The transport has not moved on beyond the block just consumed, so this block must come
    // from one second earlier in the file.
    const double expectedFilePos = (4.0 + 512.0 / 44100.0) - 1.0;

    AudioFileReader direct;
    ASSERT_TRUE(direct.open(testFile(SWEEP)));
    ASSERT_TRUE(direct.seekToFrame(static_cast<int64_t>(std::llround(expectedFilePos * 44100))));
    std::vector<float> fromFile(512 * 2, 0.f);
    ASSERT_EQ(direct.readFrames(fromFile.data(), 512, 2), 512);

    for (int i = 0; i < 512 * 2; ++i) {
        EXPECT_NEAR(afterShift[i], fromFile[i], 1e-6f) << "sample " << i;
    }
}

TEST(AudioTrackSourceTests, OffsetDefaultsToZeroSoTheTrackStartsWithTheScore)
{
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));
    prepareForPlayback(src, 44100, 2);

    EXPECT_DOUBLE_EQ(src.startOffsetSeconds(), 0.0);

    src.seekTo(TimePosition::fromTime(secs_t(3.0), 44100));
    const int64_t n = 128;
    std::vector<float> fromSource(static_cast<size_t>(n) * 2, 0.f);
    src.process(fromSource.data(), n);

    AudioFileReader direct;
    ASSERT_TRUE(direct.open(testFile(SWEEP)));
    ASSERT_TRUE(direct.seekToFrame(static_cast<int64_t>(3.0 * 44100)));
    std::vector<float> fromFile(static_cast<size_t>(n) * 2, 0.f);
    ASSERT_EQ(direct.readFrames(fromFile.data(), n, 2), n);

    for (int64_t i = 0; i < n * 2; ++i) {
        EXPECT_NEAR(fromSource[i], fromFile[i], 1e-6f) << "sample " << i;
    }
}

TEST(AudioTrackSourceTests, LoadFailureIsReportedNotCrashed)
{
    AudioTrackSource src;
    EXPECT_FALSE(src.load(testFile("data/nope.wav")));
    EXPECT_FALSE(src.isValid());

    // process() on an unloaded source must be safe and silent.
    prepareForPlayback(src, 44100, 2);
    std::vector<float> buf(64 * 2, 999.f);
    src.process(buf.data(), 64);
    EXPECT_FLOAT_EQ(peak(buf), 0.f);
}

// The transport decides whether a loaded track is audible.
//
// The mixer runs continuously -- it keeps asking for samples while the score sits stopped --
// so a source that produces sound whenever it is asked plays the moment its track is added to
// the engine and keeps going regardless of the transport, which also means the cursor has no
// effect on what is heard. The engine starts the transport (ProcessMode::Playing, driven by
// the player's isActiveChanged) only while the score is playing, and re-seeks every source on
// each play, pause and seek, so honouring the mode is all the audio needs to follow the
// cursor.
TEST(AudioTrackSourceTests, IsSilentUnlessTheTransportIsRunning)
{
    AudioTrackSource src;
    ASSERT_TRUE(src.load(testFile(SWEEP)));

    // Spec only: the engine has not started the transport.
    setSpec(src, 44100, 2);

    std::vector<float> stopped(512 * 2, 12345.f);
    src.process(stopped.data(), 512);
    EXPECT_FLOAT_EQ(peak(stopped), 0.f)
        << "a loaded track must be silent until the score is played";

    // And it must not have advanced while stopped, so play resumes where the cursor left it
    // rather than from wherever the file happened to have been left.
    const double posWhileStopped = src.positionSeconds();
    std::vector<float> stopped2(512 * 2, 0.f);
    src.process(stopped2.data(), 512);
    EXPECT_DOUBLE_EQ(src.positionSeconds(), posWhileStopped)
        << "a stopped track must not run on";

    // Starting the transport makes it audible.
    src.setMode(ProcessMode::Playing);
    std::vector<float> playing(512 * 2, 0.f);
    src.process(playing.data(), 512);
    EXPECT_GT(peak(playing), 0.05f) << "playing the score must produce audio";

    // Stopping silences it again, and keeps the position.
    const double posWhilePlaying = src.positionSeconds();
    src.setMode(ProcessMode::Idle);
    std::vector<float> idle(512 * 2, 12345.f);
    src.process(idle.data(), 512);
    EXPECT_FLOAT_EQ(peak(idle), 0.f) << "pausing must silence the track";
    EXPECT_DOUBLE_EQ(src.positionSeconds(), posWhilePlaying)
        << "pausing must keep the position so resuming continues from it";

    // Export is offline playback, and the backing track belongs in the exported file.
    src.setMode(ProcessMode::PlayingOffline);
    std::vector<float> exporting(512 * 2, 0.f);
    src.process(exporting.data(), 512);
    EXPECT_GT(peak(exporting), 0.05f) << "offline export must include the backing track";
}
