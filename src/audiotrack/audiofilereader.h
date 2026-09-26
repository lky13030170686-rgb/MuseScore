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
#ifndef MUSE_AUDIOTRACK_AUDIOFILEREADER_H
#define MUSE_AUDIOTRACK_AUDIOFILEREADER_H

#include <vector>

#include "audiotracktypes.h"
#include "types/string.h"

//! libsndfile's handle type. Declared here rather than including <sndfile.h> so that
//! this header does not leak a C dependency (and its include path) to every consumer.
//! The tag name must match libsndfile's own typedef exactly, otherwise this would
//! declare a distinct type and the sf_* calls would not accept it.
struct sf_private_tag;
using SNDFILE = sf_private_tag;

namespace muse::audiotrack {
//! RAII wrapper over libsndfile for reading audio files as float PCM.
//!
//! Deliberately NOT thread-safe and NOT shared: one reader per AudioTrackSource,
//! so that seeks on one track never disturb another.
//!
//! Why float throughout: the audio engine (`IAudioSource::process`) consumes
//! interleaved float and applies resampling itself via setOutputSpec, so keeping
//! everything in float avoids a conversion layer and matches the engine's
//! expected sample layout exactly.
class AudioFileReader
{
public:
    AudioFileReader() = default;
    ~AudioFileReader();

    AudioFileReader(const AudioFileReader&) = delete;
    AudioFileReader& operator=(const AudioFileReader&) = delete;

    //! Opens the file and reads its metadata. Returns false on failure; see errorString().
    bool open(const muse::String& path);
    void close();

    bool isOpen() const { return m_handle != nullptr; }
    const AudioFileInfo& info() const { return m_info; }
    const std::string& errorString() const { return m_info.errorString; }

    //! Random access, used on seek. Position is in frames from the file start.
    bool seekToFrame(int64_t frame);

    //! Reads up to frameCount frames into an interleaved float buffer, converted to
    //! the requested channel count (mono duplicated to stereo, extra channels mixed down).
    //! Returns the number of frames actually read; 0 means end of file or error.
    //! Short reads near EOF are normal and must be handled by the caller (zero-fill).
    int64_t readFrames(float* dst, int64_t frameCount, int dstChannels);

    //! Reads the whole file into an interleaved float buffer. Only for short files /
    //! verification; streaming playback should use readFrames().
    bool readAll(std::vector<float>& dst, int dstChannels);

private:
    //! libsndfile handles are raw C pointers. Naming the type via a forward declaration
    //! of libsndfile's own tag (not a local one) keeps the pointer type identical to
    //! what sf_* expect; a locally-declared struct would be a distinct type.
    SNDFILE* m_handle = nullptr;
    AudioFileInfo m_info;

    //! Scratch buffer for interleaved reads in the file's own channel layout,
    //! reused across calls to avoid reallocating on the audio thread.
    std::vector<float> m_scratch;
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_AUDIOFILEREADER_H
