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

muse_create_qml_module(agentharness_qml FOR agentharness)

qt_add_qml_module(agentharness_qml
    URI MuseScore.AgentHarness
    VERSION 1.0
    SOURCES
        # The field lives here, not in the plain `agentharness` library, because it is a
        # QML_ELEMENT: Qt's QML type registration only sees types compiled into the QML module
        # itself. It also needs a *context* (IGlobalContext is a context interface), and being a
        # QML element is what gets it one - QML resolves it from the object's own QQmlContext.
        fieldcontroller.cpp
        fieldcontroller.h
        panelconfig.cpp
        panelconfig.h
        # The information field's other two layers. Both are deliberately pure functions of plain
        # data (a measure grid and the raw event list) - no score, no context, no clock - so they are
        # unit-testable without an application, which is what makes "the derivation is correct" a
        # checkable claim rather than an opinion.
        addressing.cpp
        addressing.h
        semanticderive.cpp
        semanticderive.h
        scoredigest.cpp
        scoredigest.h
        tools.cpp
        tools.h
        # The agent loop's verifiable core. `sessionlog` is the single source of truth (the model's
        # history is DERIVED from it); `llmclient`'s wire handling is pure so it can be tested without
        # a network - the parts of an SSE client that actually break are the reassembly rules, and
        # those need no socket.
        sessionlog.cpp
        sessionlog.h
        llmclient.cpp
        llmclient.h
        systemprompt.cpp
        systemprompt.h
        # The loop body and its socket. The transport uses QNetworkAccessManager directly rather than
        # `muse::network` because SSE is a streaming read and that wrapper has no streaming hook (and
        # no way to read response headers at all).
        agentloop.cpp
        agentloop.h
        llmtransport.cpp
        llmtransport.h
        # The write side's single entry point: enabled-gate + batch-as-one-undo-step.
        scoreactiongateway.cpp
    spill.cpp
    spill.h
        scoreactiongateway.h
        # Note-level writes: locating the note to edit, and the recipes that edit it. Separate from the
        # command layer because a command URI cannot name a note - see scorerecipes.h.
        notelocator.cpp
        notelocator.h
        scorerecipes.cpp
        scorerecipes.h
    QML_FILES
        AgentPanel.qml
        AgentTranscriptRow.qml
        AgentTimelineRow.qml
        # The three pieces the panel and the Agent page share. They are separate files because both
        # surfaces must show the same conversation, timeline and composer: a second hand-written list
        # or input row would be a second place to get "can this be sent" right.
        AgentConversationView.qml
        AgentTimelineList.qml
        AgentComposer.qml
        # The Agent page's own parts. The page shell itself lives in appshell (AgentPage/AgentPage.qml)
        # next to the other pages; what it arranges is here, with the agent.
        AgentWorkbench.qml
        AgentFactsPanel.qml
        AgentToolInspector.qml

    IMPORTS
        TARGET muse_ui_qml
        TARGET muse_uicomponents_qml
)

fixup_qml_module_dependencies(agentharness_qml)

# engraving: the field reads `Score::changesChannel()` and the `ScoreChanges` payload.
# notation:  it reaches the score and the undo stack through `INotation`, never through a
#            cached `Score*` - see the ownership note in fieldcontroller.h.
target_link_libraries(agentharness_qml PRIVATE
    muse::uicomponents_qml
    engraving
    notation
    # Already linked into the application (the cloud module uses it), so this adds no new dependency.
    # Needed for the agent loop's SSE transport.
    Qt::Network
)

# Necessary for the auto-generated sources (moc output includes the header by bare name).
target_include_directories(agentharness_qml PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
