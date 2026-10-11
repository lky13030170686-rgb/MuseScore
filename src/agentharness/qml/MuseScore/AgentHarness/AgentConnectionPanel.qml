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
    Where the agent talks to: the gateway address and the model name.

    \b Why this is in the UI at all. These two used to be settable ONLY by environment variables, which
    meant "can I use the agent" was decided by whoever launched the process - measured 2026-10-10: the
    assistant worked when started from a launcher script and was unusable when started from the desktop
    shortcut, with no way to fix it from inside the app. A preference a user cannot change is a bug in
    the product, not a configuration detail.

    \b What is persisted and what is not. Gateway and model are PREFERENCES and are stored in the app's
    settings (`FieldController::setAgentBaseUrl` / `setAgentModel`), so they survive a restart. The API
    KEY is a secret and is deliberately never written to disk - it is entered in the composer and lives
    in memory for the run (see 交接文档 §六). ⚠️ The environment still wins over both at startup:
    scripted runs must not depend on what a user clicked last week.

    \b Why "in use" is displayed separately from the fields. The fields are what you typed; the lines
    under them are what the transport ACTUALLY holds (`agentBaseUrl()` / `agentModel()`, read back from
    it). Showing only the fields would let the panel claim a gateway that is not the one in force - the
    same "two sources of truth" failure this subsystem refuses everywhere else.
*/
Item {
    id: root

    property var field

    //! What the transport really holds, refreshed when something is applied. A `Q_INVOKABLE` getter has
    //! no notify signal, so a plain binding here would be evaluated once and then quietly go stale; this
    //! is the same reason `AgentFactsPanel` touches a notifying property.
    property string appliedGateway: ""
    property string appliedModel: ""

    //! True between "the user asked for the model list" and "the answer arrived". The menu is opened from
    //! the answer, not from the click - clicking and opening an empty menu would look like a gateway that
    //! serves nothing (AudioResourceControl.qml does the same thing for the same reason).
    property bool awaitingModelList: false

    implicitHeight: content.implicitHeight

    function refresh() {
        if (!root.field) {
            return
        }
        root.appliedGateway = root.field.agentBaseUrl()
        root.appliedModel = root.field.agentModel()
    }

    //! The menu's contents. `id` is what comes back from `onHandleMenuItem`; `title` is what is shown.
    function modelMenuItems() {
        if (!root.field) {
            return []
        }

        const ids = root.field.agentModelIds
        const items = []
        for (let i = 0; i < ids.length; ++i) {
            items.push({ title: ids[i], id: ids[i], enabled: true })
        }
        return items
    }

    //! Applying a model: set it on the controller (which persists it) and then SHOW it.
    //! ⛔ Writing `modelField.currentText` alone is not enough, and this is 第 126 条 again: once the user
    //! has typed in a `TextInputField`, the inner `TextField`'s own text is no longer bound to
    //! `currentText`, so assigning the property leaves the OLD text on screen while the value behind it is
    //! the new one - the panel would show one model and use another. Writing both keeps them equal; it is
    //! the same reason the component's `clear()` writes both.
    function applyModel(modelId) {
        if (!root.field || modelId.length === 0) {
            return
        }

        root.field.setAgentModel(modelId)
        modelField.currentText = modelId
        modelField.inputField.text = modelId
        root.refresh()
    }

    //! ⚠️ Seeded HERE and not in the fields' own `Component.onCompleted`: a child completes BEFORE its
    //! parent, so seeding there would copy the still-empty `appliedGateway` into the fields and leave
    //! them blank while the "in use" line below showed the real values - the panel would contradict
    //! itself on first open.
    Component.onCompleted: {
        root.refresh()
        gatewayField.currentText = root.appliedGateway
        modelField.currentText = root.appliedModel
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        spacing: 3

        StyledTextLabel {
            Layout.fillWidth: true
            Layout.topMargin: 6
            horizontalAlignment: Text.AlignLeft
            color: ui.theme.fontSecondaryColor
            text: qsTrc("agentharness", "CONNECTION")
        }

        TextInputField {
            id: gatewayField

            Layout.fillWidth: true
            accessible.name: qsTrc("agentharness", "Gateway address")
            hint: qsTrc("agentharness", "Gateway, e.g. https://api.deepseek.com")

            //! ⛔ `TextInputField` does not write back to `currentText` by itself - the caller owns the
            //! property (第 126 条). Without this the field would look filled in and `currentText` would be
            //! empty, so pressing Enter would apply nothing at all.
            onTextChanged: function(newTextValue) {
                gatewayField.currentText = newTextValue
            }

            onAccepted: {
                if (root.field && gatewayField.currentText.length > 0) {
                    root.field.setAgentBaseUrl(gatewayField.currentText)
                    root.refresh()
                }
            }
        }

        //! ── The model, and the list of models the gateway actually serves ──────────────────────────
        //! WHY A LIST AT ALL: a model id is the one setting nobody can get right by reasoning - it is the
        //! GATEWAY's vocabulary. Measured: pointed at an opencode route the harness asked for
        //! `deepseek-chat`, which that route does not serve, and the result looked like a broken agent
        //! rather than a wrong string. The field stays editable (some gateways do not answer `/models`, and
        //! then the user's own string is the only option), the button is how you find one.
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            TextInputField {
                id: modelField

                Layout.fillWidth: true
                accessible.name: qsTrc("agentharness", "Model")
                hint: qsTrc("agentharness", "Model, e.g. deepseek-chat")

                onTextChanged: function(newTextValue) {
                    modelField.currentText = newTextValue
                }

                onAccepted: {
                    if (root.field && modelField.currentText.length > 0) {
                        root.field.setAgentModel(modelField.currentText)
                        root.refresh()
                    }
                }
            }

            FlatButton {
                id: modelListButton

                Layout.alignment: Qt.AlignVCenter
                accessible.name: qsTrc("agentharness", "List the models this gateway serves")
                toolTipTitle: qsTrc("agentharness", "List the models this gateway serves")
                text: qsTrc("agentharness", "List")
                enabled: !root.awaitingModelList

                onClicked: {
                    if (!root.field) {
                        return
                    }

                    //! A list already in hand is shown straight away; otherwise ask, and the answer opens
                    //! the menu (below). Re-asking every time would make the button feel broken on a slow
                    //! gateway, and change nothing when nothing changed.
                    if (root.field.agentModelIds.length > 0) {
                        modelMenuLoader.toggleOpened(root.modelMenuItems())
                        return
                    }

                    root.awaitingModelList = true
                    root.field.refreshAgentModels()
                }
            }

            StyledMenuLoader {
                id: modelMenuLoader

                //! 63 ids is a normal answer for a gateway, and a menu you cannot search is a menu you
                //! scroll instead of use.
                isSearchable: true

                onHandleMenuItem: function(itemId) {
                    root.applyModel(itemId)
                }
            }
        }

        //! The answer arrives as a notification on the field, so the menu is opened from HERE.
        //! ⚠️ `fieldChanged` also fires for every streamed token, so this handler stays cheap: it does
        //! nothing at all unless a list was actually requested.
        Connections {
            target: root.field

            function onFieldChanged() {
                if (!root.awaitingModelList || !root.field) {
                    return
                }
                if (root.field.agentModelsLoading) {
                    return
                }

                root.awaitingModelList = false

                if (root.field.agentModelIds.length > 0) {
                    modelMenuLoader.toggleOpened(root.modelMenuItems())
                }
            }
        }

        //! Why the list is empty, when it is. Without this "the gateway serves nothing" and "the request
        //! failed" look exactly alike - the failure this project keeps paying for.
        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            color: ui.theme.fontSecondaryColor
            visible: text.length > 0
            text: {
                if (!root.field || root.awaitingModelList) {
                    return ""
                }
                if (root.field.agentModelIds.length > 0) {
                    return qsTrc("agentharness", "%1 model(s) listed - press List to choose one")
                           .arg(root.field.agentModelIds.length)
                }
                if (root.field.agentModelsError.length > 0) {
                    return qsTrc("agentharness", "Could not list models: %1")
                           .arg(root.field.agentModelsError)
                }
                return ""
            }
        }

        //! The values actually in force. Press Enter in a field to apply it.
        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            color: ui.theme.fontSecondaryColor
            text: qsTrc("agentharness", "In use: %1 · %2")
                  .arg(root.appliedGateway.length > 0 ? root.appliedGateway : "—")
                  .arg(root.appliedModel.length > 0 ? root.appliedModel : "—")
        }

        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
            color: ui.theme.fontSecondaryColor
            text: qsTrc("agentharness", "Press Enter in a field to apply. The key is entered in the composer and is never saved.")
        }
    }
}
