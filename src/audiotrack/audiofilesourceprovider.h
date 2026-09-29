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
#ifndef MUSE_AUDIOTRACK_AUDIOFILESOURCEPROVIDER_H
#define MUSE_AUDIOTRACK_AUDIOFILESOURCEPROVIDER_H

#include <memory>

#include "audio/engine/iaudiofilesourceprovider.h"
#include "audiotracksource.h"

namespace muse::audiotrack {
using AudioTrackSourcePtr = std::shared_ptr<AudioTrackSource>;

//! Application-side implementation of the engine's file-source hook.
//!
//! The engine cannot construct AudioTrackSource itself: it must not depend on src/.
//! Registering this provider is what lets `AudioContext::addTrack(name, path)` work.
class AudioFileSourceProvider : public muse::audio::engine::IAudioFileSourceProvider
{
public:
    muse::audio::engine::IAudioSourcePtr createSource(const std::string& filePath) const override;
    bool isSupportedFile(const std::string& filePath) const override;

    //! The most recently created source, or null once the engine has released it.
    //!
    //! The playback layer needs it to set the alignment offset, and to change it while the
    //! music is playing. See createSource() for why this reference lives here.
    AudioTrackSourcePtr lastCreatedSource() const;

private:
    mutable std::weak_ptr<AudioTrackSource> m_lastSource;
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_AUDIOFILESOURCEPROVIDER_H
