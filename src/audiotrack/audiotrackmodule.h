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

#pragma once

#include <memory>

#include "modularity/imodulesetup.h"
#include "audio/engine/iaudiofilesourceprovider.h"
#include "audiofilesourceprovider.h"
#include "audiowaveformprovider.h"
#include "audiowaveformservice.h"

namespace muse::audiotrack {
//! Registers the audio-file decoding capability with the audio engine, and the waveform
//! peak source with the engraving layer.
//!
//! Hooks registered here, because the layers that need this data may not depend on
//! src/audiotrack directly:
//!   - engine::IAudioFileSourceProvider  -> makes an audio track audible
//!   - engraving::IAudioWaveformProvider -> makes the waveform staff drawable
//!   - IAudioWaveformService             -> lets the playback layer prepare a waveform
class AudioTrackModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;
    void registerExports() override;
    void onInit(const muse::IApplication::RunMode& mode) override;

    //! The waveform provider this module registered, so the audio track code can install
    //! a peak cache into it. Null before registerExports() runs.
    std::shared_ptr<AudioWaveformProvider> waveformProvider() const;

private:
    //! Held by its concrete type, not as the engine interface: the same instance is also
    //! registered under its own type so the playback layer can reach the source object and
    //! set the alignment offset. Registering one object under two types needs the concrete
    //! pointer to start from.
    std::shared_ptr<AudioFileSourceProvider> m_provider;
    std::shared_ptr<AudioWaveformProvider> m_waveformProvider;
    std::shared_ptr<AudioWaveformService> m_waveformService;
};
} // namespace muse::audiotrack
