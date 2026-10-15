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
    # Editor state that is NOT score data (第 138 条): the measure-range selection the selection-driven
    # commands act on. Its own suite because what it pins - "the range is exactly what was asked for, and
    # nothing was written" - is a claim about the editor, not about the score.
    ${CMAKE_CURRENT_LIST_DIR}/editorstate_tests.cpp
    # Which action a raw notification belongs to (第 139 条). Pure logic: one split produced four rows in
    # the timeline, and the rule that collapses them has three branches worth pinning.
    ${CMAKE_CURRENT_LIST_DIR}/eventattribution_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spill_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sessionlog_tests.cpp
    # More than one conversation (第 141 条): which rows the list has, what they are called, and which
    # conversation survives a click. Pure container plus two free functions - no window, no score, no key.
    # ⚠️ The FIELD's half of it (the live log is restored INTO, never replaced, because `AgentLoop` holds
    # its address) is not reachable from here and is judged on the real program.
    ${CMAKE_CURRENT_LIST_DIR}/sessionstore_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llmclient_tests.cpp
    # The stored model catalog: the JSON shape of "which ids this gateway serves, and when we were
    # told" (第 131 条). Pure text in / text out, which is why it can be tested without a settings
    # file, a gateway or a window.
    ${CMAKE_CURRENT_LIST_DIR}/modelcatalog_tests.cpp
    # What the conversation view shows: the tool-call fold (第 132 条). Also pure - no window, no score.
    ${CMAKE_CURRENT_LIST_DIR}/transcriptfold_tests.cpp
    # The per-turn step guard, now that its value arrives from a settings field (第 130 条).
    ${CMAKE_CURRENT_LIST_DIR}/agentloop_tests.cpp
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
