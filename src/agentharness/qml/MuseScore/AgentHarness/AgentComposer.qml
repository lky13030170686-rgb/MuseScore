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
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

/*!
    The agent composer: the message field, Send, and the API key row.

    \b Why it is a component, and why that matters more than tidiness. There is exactly ONE way to
    send a message to the agent (`FieldController::sendToAgent`) and one rule for when it is allowed.
    Two hand-written composers - one in the notation page's panel, one on the Agent page - would be
    two places to keep that rule right, and the failure mode is not a crash: it is a Send button that
    looks enabled and quietly does nothing, or one that stays disabled while the agent is idle. The
    same argument applies to the key: it is entered here, kept in memory by the transport, and the
    label reports where it came from.

    \b Why `submit()` refuses rather than disabling alone. The button's `enabled` and the Enter key
    must not be able to disagree, so both go through one function that re-checks the same conditions.

    \b Controls are real `FlatButton`s / `TextInputField`s with `accessible.name`. That is a build
    requirement, not a nicety: a hand-drawn `Rectangle` + `MouseArea` is unreachable for UI Automation,
    which would make the agent impossible to drive from a verification script (维护手册.md §7.6).
*/
Item {
    id: root

    //! The information field (created at window level - see WindowContent.qml).
    property var field

    //! Narrow (the dock panel) or wide (the Agent page's composer card). In the panel the buttons share
    //! the row's width - there is nothing else on it, and a small button under a full-width field looks
    //! unfinished. On a page-wide card the actions move to the right end and keep their natural size,
    //! which is also how the DSH web client's composer reads.
    property bool compactButtons: true

    readonly property bool configured: root.field ? root.field.agentConfigured : false
    readonly property bool running: root.field ? root.field.agentRunning : false
    //! The page shows more than one thing at a time, so it reads this to render its own header.
    readonly property string keySource: root.field ? root.field.agentApiKeySource : "none"

    implicitHeight: content.implicitHeight

    ColumnLayout {
        id: content

        anchors.fill: parent
        spacing: 4

        TextInputField {
            id: promptField

            Layout.fillWidth: true
            //! The accessible name is what a screen reader announces and what a UI-automation script
            //! looks the field up by; without it the surface cannot be driven at all.
            accessible.name: qsTrc("agentharness", "Message to the agent")
            hint: root.configured
                  ? qsTrc("agentharness", "Ask about this score, or tell it what to change")
                  : qsTrc("agentharness", "Set an API key first")
            enabled: root.configured && !root.running

            onAccepted: {
                root.submit()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            FlatButton {
                id: sendButton

                Layout.fillWidth: root.compactButtons
                accessible.name: qsTrc("agentharness", "Send to agent")
                text: qsTrc("agentharness", "Send")
                enabled: root.configured && !root.running && promptField.currentText.length > 0

                onClicked: {
                    root.submit()
                }
            }

            //! Push the actions to the right end on a wide card (see `compactButtons`).
            Item {
                Layout.fillWidth: true
                visible: !root.compactButtons
            }

            FlatButton {
                id: keyButton

                accessible.name: qsTrc("agentharness", "Set API key")
                //! The source is shown because "it works on my machine" is usually an environment
                //! variable someone forgot they set - and a key that came from the environment cannot
                //! be cleared from here.
                text: root.keySource === "none"
                      ? qsTrc("agentharness", "Set key")
                      : qsTrc("agentharness", "Key: %1").arg(root.keySource)
                enabled: !root.running

                onClicked: {
                    keyField.visible = !keyField.visible
                }
            }
        }

        //! ── Key entry, revealed on demand ────────────────────────────────────────────────────
        //! Hidden by default so the surface does not look like it is asking for a secret the user has
        //! already provided. The key is kept in memory for this session by the transport - it is never
        //! written to disk (see AgentHarness/交接文档.md §六).
        TextInputField {
            id: keyField

            Layout.fillWidth: true
            visible: false
            accessible.name: qsTrc("agentharness", "API key")
            hint: qsTrc("agentharness", "Paste an API key (kept in memory for this session)")

            onAccepted: {
                if (root.field && keyField.currentText.length > 0) {
                    root.field.setAgentApiKey(keyField.currentText)
                    keyField.currentText = ""
                    keyField.visible = false
                }
            }
        }
    }

    //! One place that decides "can this be sent", so the button's enabled state and the Enter key
    //! cannot disagree about it.
    function submit() {
        if (!root.field || !root.configured || root.running) {
            return
        }

        const text = promptField.currentText
        if (text.length === 0) {
            return
        }

        //! Cleared BEFORE sending: if the turn fails immediately the text is gone, which is the honest
        //! outcome - it was sent, and the transcript shows what happened to it.
        promptField.currentText = ""
        root.field.sendToAgent(text)
    }
}
