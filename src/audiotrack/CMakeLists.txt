# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2026 MuseScore Limited and others
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

muse_create_module(audiotrack)

target_sources(audiotrack PRIVATE
    audiotracktypes.h
    audiofilereader.cpp
    audiofilereader.h
    audiotracksource.cpp
    audiotracksource.h
    waveformcache.cpp
    waveformcache.h
)

# libsndfile: already a project dependency (muse_deps/prebuilt.lock, all platforms).
# It is pulled in by `require_dep(libsndfile)` in buildscripts/cmake/DependencyManifest.cmake,
# which registers the imported target SndFile::sndfile via muse_deps/recipes/libsndfile/meta.cmake.
# Do NOT add find_package(SndFile) here: the prebuilt deps are not on CMAKE_PREFIX_PATH, so
# find_package would fail. Just link the target.
#
# NOTE: ENABLE_MPEG=OFF in the official recipe, so WAV/FLAC/OGG work but MP3 does not.
target_link_libraries(audiotrack PRIVATE
    SndFile::sndfile
    muse_audio_engine
)

if (MUE_BUILD_AUDIOTRACK_TESTS)
    add_subdirectory(tests)
endif()
