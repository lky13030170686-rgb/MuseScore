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

# ALIAS so src/app/CMakeLists.txt can pick it up via add_to_link_if_exists(muse::agentharness).
# NOTE: this module currently holds no C++ of its own - its QML target (agentharness_qml) owns the
# information field, because the field is a QML element and therefore has to be compiled into the
# QML module (see qml/MuseScore/AgentHarness/CMakeLists.txt). The module exists as the anchor for
# what comes next: the session log, the LLM client and the tool layer are plain C++ with no QML
# dependency and belong here, not in the QML target.
muse_create_module(agentharness ALIAS muse::agentharness)

if (MUE_BUILD_AGENTHARNESS_QML)
    # NOTE: the QML target's CMakeLists sits at the level that matches its URI
    # (`qml/MuseScore/AgentHarness`), the same layout notationscene uses. Qt checks that the
    # module's OUTPUT_DIRECTORY ends in the URI's target path, and a CMakeLists one level higher
    # fails that check.
    add_subdirectory(qml/MuseScore/AgentHarness)
endif()

# NOTE: the tests subdirectory arrives with the M1 unit tests (the field's derivation rules and
# address arithmetic are pure functions and get tested there). Guarded so the option can stay on
# before that directory exists.
if (MUE_BUILD_AGENTHARNESS_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
