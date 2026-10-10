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

set(MODULE_TEST agentharness_tests)

set(MODULE_TEST_SRC
    # The shared score read/write helper, so the grid can be exercised against a REAL score
    # rather than a hand-built stub. A stub would test our arithmetic against our own assumptions,
    # which is exactly the shape of test this project has been burned by (维护手册.md §4.4).
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h

    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/addressing_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/semanticderive_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scoredigest_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/notelocator_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scorerecipes_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spill_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sessionlog_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llmclient_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/systemprompt_tests.cpp
)

set(MODULE_TEST_LINK
    # Qt::Quick because agentharness_qml's public headers include qqmlintegration.h
    # (FieldController is a QML_ELEMENT) - without it the test target cannot even parse the header.
    Qt::Quick
    engraving
    notation
    agentharness_qml
)

# Test scores live next to the tests, as in the other module test trees.
set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)
