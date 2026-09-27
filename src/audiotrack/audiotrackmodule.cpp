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
#include "audiotrackmodule.h"

#include "modularity/ioc.h"

#include "audiofilesourceprovider.h"

using namespace muse;
using namespace muse::audiotrack;
using namespace muse::modularity;

static const std::string mname("audiotrack");

std::string AudioTrackModule::moduleName() const
{
    return mname;
}

void AudioTrackModule::registerExports()
{
    m_provider = std::make_shared<AudioFileSourceProvider>();

    // Registered as the engine's file-source hook. Note this is a GLOBAL export: the
    // audio engine lives in a separate thread/context and resolves it globally.
    // If this registration is missing, adding a file-backed track fails with
    // Err::InvalidAudioFilePath (345) and the engine logs "no IAudioFileSourceProvider
    // registered" — which is the first thing to check when a backing track stays silent.
    globalIoc()->registerExport<audio::engine::IAudioFileSourceProvider>(mname, m_provider);
}

void AudioTrackModule::onInit(const IApplication::RunMode&)
{
}
