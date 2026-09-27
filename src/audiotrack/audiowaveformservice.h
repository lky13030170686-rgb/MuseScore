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
#ifndef MUSE_AUDIOTRACK_AUDIOWAVEFORMSERVICE_H
#define MUSE_AUDIOTRACK_AUDIOWAVEFORMSERVICE_H

#include <atomic>
#include <future>
#include <memory>
#include <mutex>

#include "iaudiowaveformservice.h"
#include "audiowaveformprovider.h"
#include "waveformcache.h"

namespace muse::audiotrack {
//! Decodes an audio file into a peak cache off the UI thread and installs it into the
//! provider that engraving reads.
//!
//! Cancellation: each loadWaveform() call bumps a generation counter and captures it.
//! When the background work finishes it only installs its result if its generation is
//! still current, so rapidly switching files cannot leave a stale waveform on screen
//! (and the earlier work is not wasted on the UI thread either).
class AudioWaveformService : public IAudioWaveformService
{
public:
    //! The provider to install caches into. Set once at startup; may be null in builds
    //! where the engraving hook is unavailable, in which case this service is inert.
    explicit AudioWaveformService(std::shared_ptr<AudioWaveformProvider> provider);
    ~AudioWaveformService() override;

    void loadWaveform(const muse::io::path_t& path) override;
    void clearWaveform() override;
    bool hasWaveform() const override;
    muse::async::Notification waveformChanged() const override;

private:
    //! Decodes and reduces the file. Runs on a worker thread; must not touch UI state.
    static std::shared_ptr<WaveformCache> buildCache(const muse::io::path_t& path);

    std::shared_ptr<AudioWaveformProvider> m_provider;
    muse::async::Notification m_waveformChanged;

    //! Guards m_generation and m_pending.
    mutable std::mutex m_mutex;
    std::atomic<uint64_t> m_generation { 0 };
    //! Kept so the previous load's future is not destroyed while still running.
    std::future<void> m_pending;
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_AUDIOWAVEFORMSERVICE_H
