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

set(MODULE_TEST audiotrack_tests)

set(MODULE_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiofilereader_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiotracksource_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/waveformcache_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/waveformrenderer_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/waveformrenderer_output_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiowaveformprovider_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiowaveformservice_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiotrackinstrument_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/waveformlayout_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/waveformlanepreview_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/waveformlane_output_tests.cpp

    # waveformlayout_tests.cpp reads a real score. ScoreRW is the engraving suite's score
    # loader and lives in its test tree rather than in the engraving library, so it is
    # compiled straight into this target.
    ${CMAKE_CURRENT_LIST_DIR}/../../engraving/tests/utils/scorerw.cpp
)

set(MODULE_TEST_LINK
    audiotrack
    muse_audio_engine
    muse::draw
    engraving
)

# waveformlayout_tests.cpp drives a real score through the engraving layout, so it needs the
# engraving suite's configuration mock to satisfy IEngravingConfiguration.
# The engraving directory itself is here because waveformlanepreview_tests.cpp draws through
# TDraw, and tdraw.h includes its neighbours the way the engraving library does ("dom/...").
set(MODULE_TEST_INCLUDE
    ${CMAKE_CURRENT_LIST_DIR}/../../engraving/tests
    ${CMAKE_CURRENT_LIST_DIR}/../../engraving
)

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)
