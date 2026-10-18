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
        # The model catalog's stored form: which ids a gateway serves, keyed by gateway, and when we
        # were told. Pure (text in, text out) for the same reason `llmclient`'s wire handling is - the
        # part that breaks is the shape of the document, not the socket (第 131 条).
        modelcatalog.cpp
        modelcatalog.h
        # What the conversation SHOWS of a turn: runs of consecutive tool rows folded into one row.
        # Pure, for the same reason as the two above - "nothing was reordered, nothing was dropped, an
        # error is never hidden" are claims about the conversation, so they are tests rather than
        # something somebody has to notice on a screenshot (第 132 条).
        transcriptfold.cpp
        transcriptfold.h
        # How the ANSWER is drawn (第 150 条): the assistant's markdown, turned into the rich-text subset
        # QML's `Text` can render. Pure for the same reason as the fold above - the two rules that matter
        # (escape before you insert a tag; only openable URLs become links) are invisible on a screenshot,
        # so they are tests instead.
        markdowntext.cpp
        markdowntext.h
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
        # Editor state that is NOT score data: what is selected. Its own file because a caller has to be
        # able to tell "did this change my music?" from the name of the thing it called (第 138 条).
        editorstate.cpp
        editorstate.h
        # Which ACTION a raw notification belongs to (第 139 条): an automation change inside a score
        # transaction is part of it, not a separate event. Pure logic, so it is unit-tested without an app.
        eventattribution.cpp
        eventattribution.h
        # More than one conversation (第 141 条): the rows the list shows and the archived conversations
        # behind them. A plain container plus two free functions, so "which row says what" and "which
        # conversation survives a click" are tests rather than opinions. ⛔ The live log itself stays in
        # `fieldcontroller.cpp` - `AgentLoop` holds its address and it must never be replaced.
        sessionstore.cpp
        sessionstore.h
        # Conversations on disk (第 142 条): one `<id>.jsonl` per conversation, and everything the list
        # shows derived from that file. ⛔ No manifest - a second record of the same conversations is a list
        # that can disagree with what is in the files, and a crash while rewriting it costs all of them.
        sessionpersist.cpp
        sessionpersist.h
        # Saving the score to the file it came from (第 146 条). Its own file because it is NOT a score
        # edit: `scorerecipes` is "edits, inside a transaction, undoable with Ctrl+Z", and this is a file
        # write that nothing in the editor can take back. The refusal rule and the "did the file actually
        # change?" judge are pure, so both are unit-tested without an application.
        scoresave.cpp
        scoresave.h
    QML_FILES
        AgentPanel.qml
        AgentTranscriptRow.qml
        # A folded run of tool calls (第 132 条): one line, and the rows themselves on request. Its own
        # file because it is the only transcript row with a control in it, and because the fold is what
        # keeps a four-tool turn from pushing the answer below the fold.
        AgentToolGroupRow.qml
        AgentTimelineRow.qml
        # The three pieces the panel and the Agent page share. They are separate files because both
        # surfaces must show the same conversation, timeline and composer: a second hand-written list
        # or input row would be a second place to get "can this be sent" right.
        AgentConversationView.qml
        AgentTimelineList.qml
        AgentComposer.qml
        # The Agent page's right column can show two things now (its timeline or the conversation), so it
        # needs the one control that says which - shaped like the DSH dock kit's tab strip, with every
        # chip a real `FlatButton` so the strip is reachable from the automation tree (第 135 条).
        AgentTabStrip.qml
        # The composer's message field. Its own file because it is the one control with a keyboard
        # contract to get right (Enter sends, Shift+Enter adds a line) - and because `TextInputField`
        # could not carry it: that one is single-line, and `TextInputArea`'s Enter inserts a line.
        AgentComposerField.qml
        # The Agent page's own parts. The page shell itself lives in appshell (AgentPage/AgentPage.qml)
        # next to the other pages; what it arranges is here, with the agent.
        AgentWorkbench.qml
        # The drag boundary between two columns. One file for two boundaries, because the only difference
        # between them - which way is "wider" - belongs to the page, not to the handle.
        AgentColumnHandle.qml
        AgentFactsPanel.qml
        # The conversations of this process (第 141 条). One component for two surfaces - the Agent page's
        # sidebar and the dock panel - because a second hand-written list would be a second place to get
        # "which conversation am I in" right, the same argument the composer and the timeline already made.
        AgentSessionList.qml
        AgentToolInspector.qml
        AgentConnectionPanel.qml
        # The provider's key and its model catalog - DSH's "模型" section. Its own file because the
        # page is about the CATALOG (fetch, search, use, remove, add by hand) rather than about the
        # address, which stays in the Connection section (第 131 条).
        AgentModelsSection.qml
        # The settings surface, shaped like the DSH web client's shell (mask + centered panel + nav rail).
        # It embeds AgentConnectionPanel as its Connection section, so the gateway/model editors stay one
        # implementation; the panel and the page each open their own instance of this dialog.
        AgentSettingsDialog.qml

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
