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
import QtQuick.Controls
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

/*!
    The agent's Models page: which gateway, which key, and which models it serves.

    \b Where the shape comes from. DSH's Models settings section is a page of PROVIDERS
    (`ui-settings-models/src/client/ModelsSection.tsx` + `ProviderEditor.tsx` + `ModelListEditor.tsx`):
    one card per provider holding its base URL, its API key, and its MODEL CATALOG - and the catalog is not
    decoration, it is what the user picks from. Three details of that page are copied here because they are
    the page's actual content:

      - **the key's state is stated, not implied**: "API key configured" / "API key missing", and when one
        is configured the field says "Configured - enter a new value to replace" (`keyStored`);
      - **the catalog is editable**: models can be added by hand, deleted, and FETCHED from the provider
        (`fetchModels`: "Fetch available models" → "Asking the provider…"), because a gateway that does not
        answer `/models` must still be usable (`modelsEmpty`: "Unlisted IDs can still be sent directly");
      - **a key may come from the launch environment**, in which case it is read-only (`keyEnvLocked`).

    \b What is different, and why. DSH lists several providers at once and we have exactly one gateway, so
    this page is ONE provider card rather than a list with an "Add provider" flow. The base URL / model in
    use editors are not re-implemented either: they are `AgentConnectionPanel`, the component that already
    knows how to apply them and read them back (维护手册.md §4.5).

    \b The one place this page changes a rule. Until 第 131 条 the API key was never written to disk. The
    user asked for the opencode-go configuration to be SAVED, so the key can now be remembered - opt-in, off
    by default, with the consequence spelled out next to the checkbox, and erased the moment it is unticked.
    DSH stores provider keys too (in its credentials domain, which even works when the settings document is
    read-only); the difference is that we say plainly where ours goes.
*/
ColumnLayout {
    id: root

    //! The information field (window level; the same one the panel and the page share).
    property var field

    //! The catalog filter. A gateway can serve 63 ids (measured), which is a list you search, not one you
    //! scroll.
    property string filter: ""

    readonly property var modelIds: root.field ? root.field.agentModelIds : []
    readonly property string fetchedAt: root.field ? root.field.agentModelCatalogFetchedAt : ""
    readonly property bool loading: root.field ? root.field.agentModelsLoading : false
    readonly property string listError: root.field ? root.field.agentModelsError : ""
    readonly property string keySource: root.field ? root.field.agentApiKeySource : "none"
    readonly property string modelInUse: root.field ? root.field.agentModel : ""

    //! The catalogue as the list shows it: filtered by `filter`, case-insensitively.
    readonly property var shownModels: {
        const all = root.modelIds
        const needle = root.filter.trim().toLowerCase()
        if (needle.length === 0) {
            return all
        }

        const out = []
        for (let i = 0; i < all.length; ++i) {
            if (String(all[i]).toLowerCase().indexOf(needle) >= 0) {
                out.push(all[i])
            }
        }
        return out
    }

    spacing: 10

    //! What the key's provenance means, in words. `none` is the only one that asks the user to do something,
    //! and it says what.
    function keySourceLabel() {
        switch (root.keySource) {
        case "environment":
            return qsTrc("agentharness", "API key provided by the launch environment (read-only)")
        case "stored":
            return qsTrc("agentharness", "API key configured and remembered in the settings file")
        case "set":
            return qsTrc("agentharness", "API key configured for this session only")
        default:
            return qsTrc("agentharness", "API key missing - the agent cannot send anything yet")
        }
    }

    //! ⛔ One place that switches the model in use, so a catalog row, the composer's model menu and a
    //! hand-typed id cannot disagree (维护手册.md §4.5). The Connection section is NOT re-opened from here:
    //! it reloads what is in force every time the dialog opens or the section is entered, so the value
    //! changed here is the one it shows.
    function useModel(modelId) {
        if (!root.field || modelId.length === 0) {
            return
        }
        root.field.setAgentModel(modelId)
    }

    //! Put the page back in step with the field. Called when the section is entered, because the model in
    //! use can have been changed elsewhere (the composer's model menu, the Connection section).
    function reload() {
        root.filter = ""
        filterField.currentText = ""
        filterField.inputField.text = ""
        addField.currentText = ""
        addField.inputField.text = ""
    }

    //! ── The provider card ─────────────────────────────────────────────────────────────────────────
    StyledTextLabel {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        font: ui.theme.bodyBoldFont
        text: qsTrc("agentharness", "Provider")
    }

    //! The gateway's identity and its key state, on one line - DSH's row shape (name + a confirmed
    //! configured/missing dot). ⚠️ Both halves are READ BACK from the field, never from the text in a field:
    //! the row is a statement about what is in force.
    StyledTextLabel {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.WordWrap
        color: ui.theme.fontSecondaryColor
        text: qsTrc("agentharness", "%1 · %2")
              .arg(root.field ? root.field.agentBaseUrl : "—")
              .arg(root.keySource === "none"
                   ? qsTrc("agentharness", "API key missing")
                   : qsTrc("agentharness", "API key configured"))
    }

    //! ⛔ The gateway is SHOWN here, not editable here - and that is a decision, not a gap. The address
    //! already has an editor in the Connection section, and a second live copy would be two fields holding
    //! one value: whichever one the user did not touch would sit there displaying a stale address while
    //! looking equally authoritative (the two-sources-of-truth failure this subsystem removes everywhere
    //! else). The pointer to where it CAN be changed is what keeps this line honest.
    StyledTextLabel {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.WordWrap
        color: ui.theme.fontSecondaryColor
        text: qsTrc("agentharness", "The gateway address is edited in the Connection section. Every model below belongs to it.")
    }

    //! ── The API key ───────────────────────────────────────────────────────────────────────────────
    StyledTextLabel {
        Layout.fillWidth: true
        Layout.topMargin: 6
        horizontalAlignment: Text.AlignLeft
        font: ui.theme.bodyBoldFont
        text: qsTrc("agentharness", "API key")
    }

    StyledTextLabel {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.WordWrap
        color: ui.theme.fontSecondaryColor
        text: root.keySourceLabel()
    }

    //! ⚠️ The key field itself is only offered when the environment is NOT the source: a key that came from
    //! the environment cannot be replaced from here, and a field that looks editable and changes nothing is
    //! the failure this project keeps refusing (DSH draws the same line: `keyEnvLocked`).
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: root.keySource !== "environment"

        //! ⛔ The same layout trap as in the connection panel: `TextInputField` reports
        //! `implicitWidth: parent.width`, so as a direct layout child it is a self-reference and Qt aborts
        //! the arrangement ("Detected recursive rearrange", 第 130 条). The wrapper breaks the cycle.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: keyField.implicitHeight

            TextInputField {
                id: keyField

                anchors.fill: parent
                accessible.name: qsTrc("agentharness", "API key")
                hint: root.keySource === "none"
                      ? qsTrc("agentharness", "Enter your API key")
                      : qsTrc("agentharness", "Configured - enter a new value to replace")
                //! A secret is not a sentence: the field echoes dots, and `currentText` still holds what was
                //! typed (the write-back below is the same one 第 126 条 is about).
                inputField.echoMode: TextInput.Password

                onTextChanged: function(newTextValue) {
                    keyField.currentText = newTextValue
                }

                onAccepted: {
                    if (root.field && keyField.currentText.length > 0) {
                        root.field.setAgentApiKey(keyField.currentText)
                        keyField.clear()
                    }
                }
            }
        }

        FlatButton {
            Layout.alignment: Qt.AlignVCenter
            accessible.name: qsTrc("agentharness", "Use this API key")
            text: qsTrc("agentharness", "Save")
            enabled: keyField.currentText.length > 0

            onClicked: {
                if (root.field && keyField.currentText.length > 0) {
                    root.field.setAgentApiKey(keyField.currentText)
                    keyField.clear()
                }
            }
        }
    }

    //! The opt-in, and the sentence that makes it a decision rather than a checkbox. ⚠️ The wording says
    //! WHERE it goes: a user who ticks this is choosing to have a secret in a plain text file, and that is
    //! only an informed choice if the file is named.
    //! ⛔ HIDDEN WHEN THE KEY CAME FROM THE ENVIRONMENT, together with the field and the Forget button: one
    //! rule for the whole block - an environment key is read-only, so nothing here pretends to change it.
    //! (Without this, ticking the box would COPY the launch environment's key onto disk, which is the one
    //! thing a key kept out of the program's own storage was being kept out of it for.)
    CheckBox {
        Layout.fillWidth: true
        visible: root.keySource !== "environment"
        text: qsTrc("agentharness", "Remember the key in the settings file (plain text, on this machine)")
        checked: root.field ? root.field.agentRememberApiKey : false
        //! ⚠️ Through `navigation`, not `accessible` - `CheckBox` has the former alias and not the latter
        //! (the same trap as `IncrementalPropertyControl` in 第 130 条; the short form is a runtime load
        //! error, not a warning). ⚠️ And the box does NOT set `checked` when clicked - it only emits
        //! `clicked`, so the binding above stays alive and `fieldChanged` is what moves the tick.
        navigation.accessible.name: qsTrc("agentharness", "Remember the API key in the settings file")

        onClicked: {
            if (root.field) {
                root.field.setAgentRememberApiKey(!root.field.agentRememberApiKey)
            }
        }
    }

    FlatButton {
        Layout.alignment: Qt.AlignLeft
        visible: root.keySource !== "none" && root.keySource !== "environment"
        accessible.name: qsTrc("agentharness", "Forget the API key")
        text: qsTrc("agentharness", "Forget key")
        icon: IconCode.UNDO

        onClicked: {
            if (root.field) {
                root.field.forgetAgentApiKey()
            }
        }
    }

    //! ── The model catalog ─────────────────────────────────────────────────────────────────────────
    StyledTextLabel {
        Layout.fillWidth: true
        Layout.topMargin: 6
        horizontalAlignment: Text.AlignLeft
        font: ui.theme.bodyBoldFont
        text: qsTrc("agentharness", "Models")
    }

    //! What we know about this gateway's list, and where it came from. ⚠️ "0 models" without the second half
    //! is indistinguishable from "we never asked"; the timestamp and the gateway are what make it a fact.
    StyledTextLabel {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.WordWrap
        color: ui.theme.fontSecondaryColor
        text: {
            if (root.loading) {
                return qsTrc("agentharness", "Asking the provider…")
            }
            if (root.listError.length > 0) {
                return qsTrc("agentharness", "Could not list models: %1").arg(root.listError)
            }
            if (root.modelIds.length === 0) {
                return qsTrc("agentharness", "No models known for this gateway yet. Fetch them, or add one by hand - an id the provider does not list can still be used.")
            }
            return qsTrc("agentharness", "%1 model(s) remembered%2")
                   .arg(root.modelIds.length)
                   .arg(root.fetchedAt.length > 0
                        ? qsTrc("agentharness", ", fetched %1").arg(root.fetchedAt)
                        : "")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: filterField.implicitHeight

            TextInputField {
                id: filterField

                anchors.fill: parent
                accessible.name: qsTrc("agentharness", "Search models")
                hint: qsTrc("agentharness", "Search models")
                enabled: root.modelIds.length > 0

                onTextChanged: function(newTextValue) {
                    filterField.currentText = newTextValue
                    root.filter = newTextValue
                }
            }
        }

        FlatButton {
            Layout.alignment: Qt.AlignVCenter
            accessible.name: qsTrc("agentharness", "Fetch available models")
            text: qsTrc("agentharness", "Fetch")
            enabled: !root.loading

            onClicked: {
                if (root.field) {
                    root.field.refreshAgentModels()
                }
            }
        }
    }

    //! The catalog itself. ⚠️ A page-level list (a `Repeater` inside this column) rather than a bounded
    //! `ListView`: the settings panel's options area already scrolls, and nesting a second scroll region
    //! inside it makes the wheel behave differently depending on where the pointer is.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2

        Repeater {
            model: root.shownModels

            Rectangle {
                id: modelRow

                required property var modelData

                Layout.fillWidth: true
                Layout.preferredHeight: 28
                color: "transparent"

                RowLayout {
                    anchors.fill: parent
                    spacing: 8

                    StyledTextLabel {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        //! The model in use is marked, not merely listed: the list is also the selector, and
                        //! "which one am I on" is the first question it has to answer.
                        color: modelRow.modelData === root.modelInUse ? ui.theme.accentColor
                                                                      : ui.theme.fontPrimaryColor
                        text: modelRow.modelData === root.modelInUse
                              ? qsTrc("agentharness", "%1 · in use").arg(modelRow.modelData)
                              : modelRow.modelData
                    }

                    FlatButton {
                        Layout.alignment: Qt.AlignVCenter
                        visible: modelRow.modelData !== root.modelInUse
                        transparent: true
                        accessible.name: qsTrc("agentharness", "Use %1").arg(modelRow.modelData)
                        text: qsTrc("agentharness", "Use")

                        onClicked: {
                            root.useModel(modelRow.modelData)
                        }
                    }

                    FlatButton {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        backgroundRadius: 28
                        transparent: true
                        icon: IconCode.CLOSE_X_ROUNDED
                        accessible.name: qsTrc("agentharness", "Remove %1 from the list").arg(modelRow.modelData)
                        toolTipTitle: qsTrc("agentharness", "Remove from the list")

                        onClicked: {
                            if (root.field) {
                                root.field.removeAgentModel(modelRow.modelData)
                            }
                        }
                    }
                }
            }
        }
    }

    StyledTextLabel {
        Layout.fillWidth: true
        visible: root.modelIds.length > 0 && root.shownModels.length === 0
        horizontalAlignment: Text.AlignLeft
        color: ui.theme.fontSecondaryColor
        text: qsTrc("agentharness", "No matching models.")
    }

    //! ── Add one by hand ──────────────────────────────────────────────────────────────────────────
    StyledTextLabel {
        Layout.fillWidth: true
        Layout.topMargin: 6
        horizontalAlignment: Text.AlignLeft
        color: ui.theme.fontSecondaryColor
        wrapMode: Text.WordWrap
        text: qsTrc("agentharness", "A model id the gateway does not list can still be used - add it here and it appears in the selector like any other.")
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: addField.implicitHeight

            TextInputField {
                id: addField

                anchors.fill: parent
                accessible.name: qsTrc("agentharness", "Add a model id")
                hint: qsTrc("agentharness", "Model id, e.g. opencode-go/deepseek-v4.1-flash")

                onTextChanged: function(newTextValue) {
                    addField.currentText = newTextValue
                }

                onAccepted: {
                    root.addTypedModel()
                }
            }
        }

        FlatButton {
            Layout.alignment: Qt.AlignVCenter
            accessible.name: qsTrc("agentharness", "Add this model to the list")
            text: qsTrc("agentharness", "Add")
            enabled: addField.currentText.trim().length > 0

            onClicked: {
                root.addTypedModel()
            }
        }
    }

    function addTypedModel() {
        const id = addField.currentText.trim()
        if (!root.field || id.length === 0) {
            return
        }

        root.field.addAgentModel(id)
        addField.clear()
    }
}
