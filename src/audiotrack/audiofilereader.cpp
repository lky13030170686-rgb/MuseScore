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
#include "audiofilereader.h"

#include <algorithm>
#include <cmath>

#include <sndfile.h>

#include "log.h"

using namespace muse;
using namespace muse::audiotrack;

AudioFileReader::~AudioFileReader()
{
    close();
}

bool AudioFileReader::open(const muse::String& path)
{
    close();

    m_info = AudioFileInfo();

    if (path.empty()) {
        m_info.errorString = "empty path";
        return false;
    }

    // libsndfile expects a UTF-8 byte path on Windows; muse::String -> std::string is UTF-8.
    const std::string utf8Path = path.toStdString();

    SF_INFO sfInfo {};
    SNDFILE* handle = sf_open(utf8Path.c_str(), SFM_READ, &sfInfo);
    if (!handle) {
        m_info.errorString = sf_strerror(nullptr);
        LOGE() << "failed to open audio file: " << utf8Path << ", error: " << m_info.errorString;
        return false;
    }

    if (sfInfo.frames <= 0 || sfInfo.samplerate <= 0 || sfInfo.channels <= 0) {
        m_info.errorString = "invalid audio file metadata";
        LOGE() << "invalid audio file metadata: " << utf8Path
               << ", frames: " << sfInfo.frames
               << ", samplerate: " << sfInfo.samplerate
               << ", channels: " << sfInfo.channels;
        sf_close(handle);
        return false;
    }

    m_handle = handle;
    m_info.valid = true;
    m_info.sampleRate = sfInfo.samplerate;
    m_info.channels = sfInfo.channels;
    m_info.frames = static_cast<int64_t>(sfInfo.frames);
    m_info.duration = static_cast<double>(m_info.frames) / static_cast<double>(m_info.sampleRate);

    // Ask libsndfile for a human-readable name, e.g. "WAV (Microsoft) signed 16-bit PCM".
    SF_FORMAT_INFO formatInfo {};
    formatInfo.format = sfInfo.format & SF_FORMAT_TYPEMASK;
    if (sf_command(handle, SFC_GET_FORMAT_INFO, &formatInfo, sizeof(formatInfo)) == 0 && formatInfo.name) {
        m_info.formatName = formatInfo.name;
    }

    LOGI() << "opened audio file: " << utf8Path
           << ", format: " << m_info.formatName
           << ", sampleRate: " << m_info.sampleRate
           << ", channels: " << m_info.channels
           << ", frames: " << m_info.frames
           << ", duration: " << m_info.duration << "s";

    return true;
}

void AudioFileReader::close()
{
    if (m_handle) {
        sf_close(m_handle);
        m_handle = nullptr;
    }
    m_scratch.clear();
    m_scratch.shrink_to_fit();
}

bool AudioFileReader::seekToFrame(int64_t frame)
{
    if (!m_handle) {
        return false;
    }

    const sf_count_t pos = sf_seek(m_handle, static_cast<sf_count_t>(frame), SEEK_SET);
    if (pos < 0) {
        LOGE() << "failed to seek audio file to frame " << frame << ", error: " << sf_strerror(m_handle);
        return false;
    }
    return true;
}

int64_t AudioFileReader::readFrames(float* dst, int64_t frameCount, int dstChannels)
{
    if (!m_handle || !dst || frameCount <= 0 || dstChannels <= 0) {
        return 0;
    }

    const int fileChannels = m_info.channels;

    const size_t needed = static_cast<size_t>(frameCount) * static_cast<size_t>(fileChannels);
    if (m_scratch.size() < needed) {
        m_scratch.resize(needed);
    }

    const sf_count_t read = sf_readf_float(m_handle, m_scratch.data(), static_cast<sf_count_t>(frameCount));
    if (read <= 0) {
        return 0;
    }

    // Channel adaptation. The engine resamples the rate for us via setOutputSpec,
    // but it does not change the channel count, so do it here.
    const int64_t frames = static_cast<int64_t>(read);
    for (int64_t f = 0; f < frames; ++f) {
        const float* src = m_scratch.data() + (f * fileChannels);
        float* out = dst + (f * dstChannels);

        if (fileChannels == dstChannels) {
            for (int c = 0; c < dstChannels; ++c) {
                out[c] = src[c];
            }
        } else if (fileChannels == 1) {
            // mono -> N: duplicate to every output channel
            const float v = src[0];
            for (int c = 0; c < dstChannels; ++c) {
                out[c] = v;
            }
        } else if (dstChannels == 1) {
            // N -> mono: average, so the level stays sane instead of summing and clipping
            float sum = 0.f;
            for (int c = 0; c < fileChannels; ++c) {
                sum += src[c];
            }
            out[0] = sum / static_cast<float>(fileChannels);
        } else {
            // general N -> M: copy what overlaps, zero the rest
            for (int c = 0; c < dstChannels; ++c) {
                out[c] = (c < fileChannels) ? src[c] : 0.f;
            }
        }
    }

    return frames;
}

bool AudioFileReader::readAll(std::vector<float>& dst, int dstChannels)
{
    if (!m_handle || dstChannels <= 0) {
        return false;
    }

    if (!seekToFrame(0)) {
        return false;
    }

    const int64_t totalFrames = m_info.frames;
    dst.assign(static_cast<size_t>(totalFrames) * static_cast<size_t>(dstChannels), 0.f);

    int64_t done = 0;
    while (done < totalFrames) {
        const int64_t want = std::min<int64_t>(totalFrames - done, 8192);
        float* out = dst.data() + (done * dstChannels);
        const int64_t got = readFrames(out, want, dstChannels);
        if (got <= 0) {
            break;
        }
        done += got;
    }

    return done == totalFrames;
}
