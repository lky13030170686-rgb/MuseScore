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
#ifndef MUSE_AUDIOTRACK_AUDIOTRACKTYPES_H
#define MUSE_AUDIOTRACK_AUDIOTRACKTYPES_H

#include <string>

#include "types/string.h"

namespace muse::audiotrack {
//! Metadata read from an audio file at open time. Read-only: the audio track is a
//! reference layer, so nothing here is ever edited back into the file.
struct AudioFileInfo
{
    bool valid = false;
    int sampleRate = 0;
    int channels = 0;
    //! Total frame count. One frame holds one sample per channel.
    int64_t frames = 0;
    //! Duration in seconds, derived from frames / sampleRate.
    double duration = 0.0;
    //! Decoder-reported format name, e.g. "WAV (Microsoft) signed 16-bit PCM".
    std::string formatName;
    std::string errorString;
};

//! Formats we can read with the libsndfile build shipped by MuseScore.
//! MP3 is deliberately absent: the official libsndfile recipe sets ENABLE_MPEG=OFF
//! (see 音频轨/拼接方案.md §4). MP3 needs dr_mp3 or mpg123 later.
inline bool isSupportedAudioFormat(const muse::String& suffix)
{
    static const muse::String supported[] = {
        u"wav", u"wave", u"flac", u"ogg", u"oga", u"opus", u"aiff", u"aif", u"aifc", u"w64", u"caf"
    };

    muse::String s = suffix.toLower();
    for (const muse::String& candidate : supported) {
        if (s == candidate) {
            return true;
        }
    }
    return false;
}
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_AUDIOTRACKTYPES_H
