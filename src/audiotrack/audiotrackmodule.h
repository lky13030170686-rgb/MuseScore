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

namespace muse::audiotrack {
//! Registers the audio-file decoding capability with the audio engine.
//!
//! The engine (muse submodule) cannot create file-backed sources itself — it must not
//! depend on src/. This module supplies the bridge: it registers an
//! engine::IAudioFileSourceProvider, which is what makes
//! `IPlayback::addTrack(name, filePath, params)` able to produce sound.
class AudioTrackModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;
    void registerExports() override;
    void onInit(const muse::IApplication::RunMode& mode) override;

private:
    std::shared_ptr<muse::audio::engine::IAudioFileSourceProvider> m_provider;
};
} // namespace muse::audiotrack
