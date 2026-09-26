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
#ifndef MUSE_AUDIOTRACK_AUDIOTRACKSOURCE_H
#define MUSE_AUDIOTRACK_AUDIOTRACKSOURCE_H

#include <memory>
#include <vector>

#include "audiofilereader.h"
#include "audio/engine/iaudiosource.h"
#include "audio/engine/internal/abstractaudiosource.h"
#include "audio/engine/internal/nodes/audiofilenode.h"

namespace muse::audiotrack {
//! Streams a file into the audio engine.
//!
//! Follows the SineSource pattern: subclass AbstractAudioSource and implement only
//! audioChannelsCount() + process(). Everything else (mode, output spec, channel-change
//! channel) comes from the base class.
//!
//! Additionally implements engine::ISeekableAudioSource so the engine can reposition it
//! when the score playhead moves.
//!
//! ⚠️ Rate handling (verified against the engine, not assumed):
//! the engine hands us the OUTPUT spec and does NOT resample for us — SineSource
//! generates at m_outputSpec.sampleRate for exactly this reason (audiofactory.cpp:77
//! sets the source's spec to the engine's). So process() must emit samples already at
//! the output rate. We do a linear-interpolation resample. That is adequate for a
//! reference/backing track; a polyphase resampler (soxr is already in the dependency
//! tree) is the upgrade path if quality ever matters.
//! ⚠️ THREADING (known limitation, first version):
//! process() runs on the audio processing thread, while seekTo()/setPositionSeconds()
//! are called from the engine thread (via AudioFileNode::seek). The buffers mutated by a
//! seek (m_cache, m_lastFrame) can therefore be touched while process() is mid-read.
//! The thread-assert macros do not guard this: ONLY_AUDIO_ENGINE_THREAD and
//! ONLY_AUDIO_PROC_THREAD both resolve to isEngineThread(), so they are no-ops for this
//! pair. Consequences today are limited to a briefly wrong/glitched block after a seek
//! during playback (not a crash on its own), but it is a real data race and must be
//! fixed before this ships: the intended fix is to route seeks through the engine's
//! operation-exec queue (the pattern doSaveSoundTrack uses) so they are serialized with
//! process(), or to double-buffer the cache and swap it atomically.
class AudioTrackSource : public muse::audio::engine::AbstractAudioSource,
                         public muse::audio::engine::ISeekableAudioSource
{
public:
    AudioTrackSource();
    ~AudioTrackSource() override;

    //! Opens the audio file. Returns false on failure (see errorString()).
    //! Must be called before the source is handed to the engine.
    bool load(const muse::String& path);
    void unload();

    bool isValid() const;
    const AudioFileInfo& info() const;

    //! Position control, expressed in seconds relative to the file start.
    void setPositionSeconds(double seconds);
    double positionSeconds() const;

    // ISeekableAudioSource
    void seekTo(const muse::audio::TimePosition& position) override;
    muse::audio::TimePosition position() const override;
    muse::audio::TimePosition duration() const override;

    unsigned int audioChannelsCount() const override;
    muse::audio::samples_t process(float* buffer, muse::audio::samples_t samplesPerChannel) override;

private:
    //! Fills the resampler's source-side cache with the next chunk of file data.
    //! Returns false at end of file.
    bool refill();

    AudioFileReader m_reader;

    //! Engine output spec; set by the base class via setOutputSpec().
    //! process() is called with no rate argument, so we read m_outputSpec here.
    int m_outSampleRate = 0;
    //! Channel count the engine expects (from m_outputSpec.audioChannelCount).
    int m_outChannels = 0;

    //! Read cursor: fractional file-frame position of the NEXT sample to produce.
    //! This is the single source of truth for playback position; everything else is
    //! derived from it.
    double m_readPos = 0.0;

    //! File frame that m_cache[0] corresponds to. Authoritative for "where the current
    //! chunk starts"; set by refill() (and by setPositionSeconds() when the cache is
    //! dropped). May be one frame behind m_readPos while a lookahead frame is held.
    int64_t m_cacheStartFrame = 0;
    //! File frame the reader will return next (what refill() reads from).
    int64_t m_readerFramePos = 0;

    int64_t m_cacheFrames = 0;
    std::vector<float> m_cache;    // interleaved, m_outChannels wide
    //! Channel width m_cache/m_lastFrame were built for. Detects an engine output-spec
    //! change so the caches can be discarded instead of read at the wrong stride.
    int m_cachedChannels = 0;
    //! One frame of lookahead so linear interpolation can straddle chunk boundaries.
    std::vector<float> m_lastFrame;

    double m_ratio = 1.0;          // fileRate / outRate: file frames per output frame

    static constexpr int64_t CACHE_FRAMES = 16384;
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_AUDIOTRACKSOURCE_H
