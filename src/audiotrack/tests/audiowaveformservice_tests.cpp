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

// Covers AudioWaveformService, the decode-and-install step that sits between "user imports a
// file" and "engraving has peaks to draw". It had no tests, and it is the only part of that
// chain which runs on a worker thread, reports completion asynchronously, and can be
// interrupted by a second import or by closing the score.

#include <gtest/gtest.h>

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audiotrack/audiowaveformprovider.h"
#include "audiotrack/audiowaveformservice.h"
#include "audiotrack/iaudiowaveformservice.h"

using namespace muse;
using namespace muse::audiotrack;

namespace {
const char* METRONOME = "data/metronome-120bpm-48000.wav";
const char* SWEEP = "data/sweep-stereo-44100.wav";

String testFile(const char* name)
{
    return String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT) + "/" + name);
}

//! Waits for `pred` to come true, so the tests do not depend on a fixed sleep being long
//! enough on a loaded machine. Returns false on timeout.
//!
//! UIKit notifications raised on a worker thread are delivered through a queue owned by the
//! receiving thread (see kors channelimpl sendAuto/sendToQueue), so a subscriber on this
//! thread only runs once this thread drains that queue. The real application does that in
//! its event loop; here processEvents() stands in for it. Without this, a test that asserts
//! "the notification arrived" would fail even though the service behaved correctly.
template<class Pred>
bool waitFor(Pred pred, int timeoutMs = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return pred();
}
}

// The basic contract: loading a real file makes peaks available and reports the change.
TEST(AudioWaveformServiceTests, LoadsFileAndInstallsCache)
{
    auto provider = std::make_shared<AudioWaveformProvider>();
    AudioWaveformService service(provider);

    std::atomic<int> notified { 0 };
    service.waveformChanged().onNotify(nullptr, [&notified]() {
        notified.fetch_add(1);
    });

    EXPECT_FALSE(service.hasWaveform());

    service.loadWaveform(testFile(METRONOME));

    ASSERT_TRUE(waitFor([&]() { return service.hasWaveform(); }))
        << "the service never made a waveform available";
    EXPECT_TRUE(provider->hasWaveform());

    // The metronome fixture is a few seconds long; the point is that real audio reached the
    // provider rather than a zero-length placeholder.
    EXPECT_GT(provider->waveformDuration(), 1.0);

    EXPECT_TRUE(waitFor([&]() { return notified.load() > 0; }))
        << "waveformChanged was never fired after a successful load";
}

// A file that cannot be decoded must clear the provider instead of leaving the previous
// waveform on screen, and must still report the change so the score re-lays out.
TEST(AudioWaveformServiceTests, UndecodableFileClearsAndNotifies)
{
    auto provider = std::make_shared<AudioWaveformProvider>();
    AudioWaveformService service(provider);

    service.loadWaveform(testFile(METRONOME));
    ASSERT_TRUE(waitFor([&]() { return service.hasWaveform(); }));

    std::atomic<int> notified { 0 };
    service.waveformChanged().onNotify(nullptr, [&notified]() {
        notified.fetch_add(1);
    });

    service.loadWaveform(testFile("data/does-not-exist.wav"));

    // hasWaveform() must go false: the old track's picture must not survive a failed import.
    ASSERT_TRUE(waitFor([&]() { return !service.hasWaveform(); }))
        << "a failed load left the previous waveform installed";
    EXPECT_FALSE(provider->hasWaveform());

    EXPECT_TRUE(waitFor([&]() { return notified.load() > 0; }))
        << "waveformChanged was never fired after a failed load";
}

// clearWaveform() is what runs when the audio track goes away; the provider must end up
// empty and listeners must hear about it.
TEST(AudioWaveformServiceTests, ClearDropsTheWaveform)
{
    auto provider = std::make_shared<AudioWaveformProvider>();
    AudioWaveformService service(provider);

    service.loadWaveform(testFile(METRONOME));
    ASSERT_TRUE(waitFor([&]() { return service.hasWaveform(); }));

    service.clearWaveform();

    EXPECT_FALSE(service.hasWaveform());
    EXPECT_FALSE(provider->hasWaveform());
    EXPECT_DOUBLE_EQ(provider->waveformDuration(), 0.0);
}

// Rapidly switching files is the case the generation counter exists for. Whichever file is
// requested last must be the one that ends up installed, no matter what order the two
// decodes happen to finish in.
TEST(AudioWaveformServiceTests, RapidSwitchKeepsTheLastRequest)
{
    auto provider = std::make_shared<AudioWaveformProvider>();
    AudioWaveformService service(provider);

    // Two different files, so "which one won" is observable through the duration.
    service.loadWaveform(testFile(METRONOME));
    service.loadWaveform(testFile(SWEEP));

    ASSERT_TRUE(waitFor([&]() { return service.hasWaveform(); }));

    // Give any late-finishing first decode a chance to (wrongly) overwrite the second.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const double duration = provider->waveformDuration();
    EXPECT_GT(duration, 0.0);

    // Compare against a reference service loaded with the sweep alone. If the metronome's
    // decode had won, the duration would match the metronome instead.
    auto sweepOnly = std::make_shared<AudioWaveformProvider>();
    AudioWaveformService reference(sweepOnly);
    reference.loadWaveform(testFile(SWEEP));
    ASSERT_TRUE(waitFor([&]() { return sweepOnly->hasWaveform(); }));

    EXPECT_NEAR(duration, sweepOnly->waveformDuration(), 1e-9)
        << "the earlier request overwrote the later one";
}

// Closing a score (or the app) while a decode is in flight must not crash. This is the
// destructor's whole reason for waiting on the pending future.
TEST(AudioWaveformServiceTests, DestroyingDuringLoadIsSafe)
{
    auto provider = std::make_shared<AudioWaveformProvider>();

    for (int i = 0; i < 20; ++i) {
        AudioWaveformService service(provider);
        service.loadWaveform(testFile(METRONOME));
        // Destroyed immediately, usually while the worker is still decoding.
    }

    // Reaching here without a crash or a hang is the assertion. The provider may hold a
    // cache if one of the decodes happened to finish first, which is fine.
    SUCCEED();
}

// Switching files and clearing at the same time must leave the provider consistent with the
// last instruction rather than a torn mixture.
TEST(AudioWaveformServiceTests, ClearDuringLoadWins)
{
    auto provider = std::make_shared<AudioWaveformProvider>();
    AudioWaveformService service(provider);

    service.loadWaveform(testFile(SWEEP));
    service.clearWaveform();

    // The in-flight decode must not resurrect the waveform after the clear.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_FALSE(service.hasWaveform())
        << "an in-flight load installed its cache after clearWaveform()";
    EXPECT_FALSE(provider->hasWaveform());
}
