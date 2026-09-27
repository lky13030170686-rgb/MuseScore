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
#include "audiofilesourceprovider.h"

#include <memory>

#include "audiotracksource.h"
#include "log.h"

using namespace muse;
using namespace muse::audiotrack;
using namespace muse::audio;
using namespace muse::audio::engine;

IAudioSourcePtr AudioFileSourceProvider::createSource(const std::string& filePath) const
{
    auto source = std::make_shared<AudioTrackSource>();

    // muse::String::fromUtf8: paths on Windows are UTF-8 here, and libsndfile wants
    // UTF-8 bytes, so this round-trips correctly (unlike a local 8-bit conversion).
    if (!source->load(String::fromUtf8(filePath))) {
        LOGE() << "cannot open audio file as a track source: " << filePath
               << ", error: " << source->info().errorString;
        return nullptr;
    }

    LOGI() << "created audio track source for: " << filePath
           << " (" << source->info().sampleRate << " Hz, "
           << source->info().channels << " ch, "
           << source->info().duration << " s)";

    return source;
}

bool AudioFileSourceProvider::isSupportedFile(const std::string& filePath) const
{
    const size_t dot = filePath.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    return isSupportedAudioFormat(String::fromUtf8(filePath.substr(dot + 1)));
}
