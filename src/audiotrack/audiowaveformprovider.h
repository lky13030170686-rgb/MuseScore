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
#ifndef MUSE_AUDIOTRACK_AUDIOWAVEFORMPROVIDER_H
#define MUSE_AUDIOTRACK_AUDIOWAVEFORMPROVIDER_H

#include <memory>
#include <mutex>

#include "engraving/dom/iaudiowaveformprovider.h"
#include "waveformcache.h"

namespace muse::audiotrack {
//! Application-side implementation of the engraving layer's waveform hook.
//!
//! Holds the peak cache for the currently loaded audio track and answers windowed peak
//! queries. Engraving calls this during layout, which can happen on a different thread
//! from the one that builds the cache, so access is guarded by a mutex.
//!
//! A single shared instance is registered in the global IoC; the audio track code hands
//! it the cache whenever an audio file is opened.
class AudioWaveformProvider : public mu::engraving::IAudioWaveformProvider
{
public:
    AudioWaveformProvider() = default;

    //! Installs (or clears, with nullptr) the cache that queries read from.
    //! Called by the audio track code, not by engraving.
    void setCache(std::shared_ptr<const WaveformCache> cache);
    void clearCache();

    bool hasWaveform() const override;
    double waveformDuration() const override;
    void waveformPeaks(double fromSeconds, double toSeconds, int64_t count,
                       std::vector<mu::engraving::AudioWaveformPeak>& out) const override;

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const WaveformCache> m_cache;
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_AUDIOWAVEFORMPROVIDER_H
