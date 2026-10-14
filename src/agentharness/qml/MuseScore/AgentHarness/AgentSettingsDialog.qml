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
    The agent's settings surface, shaped like the DSH web client's.

    \b Where the shape comes from. DSH's settings is a shell, not a dialog with a form in it
    (`packages/client/ui-settings-general/src/client/SettingsRoot.tsx` + `SettingsRoot.module.css`):
    a centered panel that is always 800 wide and `min(800, viewport - 48)` tall, split into a fixed
    188px nav rail (title + section cells) and a content column (a 54px header holding the actions and
    the close button, above a scrolling options area). The measurements below are copied from that file
    rather than invented, because "looks like the web client" is mostly a statement about proportions
    (第 124 条的同一做法).

    ⛔ \b Why the panel has ONE height for every section. Also copied, and it is not cosmetic: the
    sections differ by hundreds of pixels, and a panel sized to its content would resize under the
    pointer on every nav click. Whatever does not fit scrolls inside the options area.

    \b What is NOT copied, and why: the mask. DSH dims the page behind the panel (`#000` at 24%) and
    closes on a mask click. Our first version did the same with a popup sized to the whole window - and
    it was DISMISSED the moment a text field inside it took focus (measured 2026-10-11: the step field
    and the gateway field both closed it; clicking a button did not). `TextInputField` clears
    `Qt.WindowDoesNotAcceptFocus` on its window when it takes focus, which re-creates the native window;
    a popup that covers the whole window does not survive that, a panel-sized one does. So the panel is
    a `Popup` the size of the panel plus its margin, and "click away to dismiss" comes back from
    `CloseOnPressOutside` - the same gesture, without the dim (第 130 条).

    \b Why it is a `Popup` over `Overlay.overlay` and not a MuseScore dialog window. A `DialogView` is a
    separate native window: it would sit outside the main window's stacking, carry its own title bar,
    and could not be dismissed by clicking away. It also has to fit the DSH proportions inside a window
    whose chrome is not ours. The popup keeps the panel in the application's own layer, and one
    implementation serves both surfaces - the notation page's dock and the Agent page (维护手册.md §4.5).

    \b Close paths, and the one that is not free. The close button, a click outside (`CloseOnPressOutside`)
    and Escape all work - but Escape needed a `ShortcutOverride` claim, because it is registered as an
    application-wide shortcut (第 122 条): `closePolicy: CloseOnEscape` never fired (the popup is not the
    window's focus item), and a plain `Keys.onEscapePressed` never saw the key either. Measured
    2026-10-11; the reasoning is written where the handler is.

    \b Where our sections differ from DSH's, and why. DSH's nav lists what DSH has (通用 / 模型 / 插件 /
    归档会话 …). This harness has exactly four real groups of settings, so the nav lists those four and
    nothing else - a nav row whose section is empty is furniture, which is the same judgement the Agent
    page made about DSH's session list (第 124 条).

    \b Accessibility is a build requirement (维护手册.md §7.6). Every nav cell, action and close button
    is a real `FlatButton` with an `accessible.name`.
*/
Popup {
    id: root

    //! The information field (window level; the same one the panel and the page share).
    property var field

    //! Which section is on screen. DSH keeps this inside the panel (component-local viewing state).
    property string activeSection: "connection"

    //! The page's column widths are its own live state, so only the page can put them back - see the
    //! `layoutResetRequested` note in AgentWorkbench.qml. The panel leaves this false.
    property bool layoutResettable: false

    //! Asked for by the 界面 section; the page answers by dropping its dragged widths.
    signal layoutResetRequested()

    //! ── The DSH measurements ──────────────────────────────────────────────────────────────────────
    readonly property int panelWidth: 800
    readonly property int panelMaxHeight: 800
    readonly property int panelMargin: 24      //!< the panel never comes closer than this to the window edge
    readonly property int navWidth: 188
    readonly property int navCellHeight: 40
    readonly property int headerHeight: 54

    readonly property var sections: [
        { id: "connection", label: qsTrc("agentharness", "Connection"), icon: IconCode.LINK },
        //! DSH's "模型" section. It sits next to Connection rather than replacing it: Connection answers
        //! "where does it talk to", Models answers "which model, with which key, out of which list" - and
        //! the second question is the one that needed a page (第 131 条).
        { id: "models", label: qsTrc("agentharness", "Models"), icon: IconCode.LIST },
        { id: "assistant", label: qsTrc("agentharness", "Assistant"), icon: IconCode.CLOCK },
        { id: "interface", label: qsTrc("agentharness", "Interface"), icon: IconCode.GRID },
        { id: "about", label: qsTrc("agentharness", "About"), icon: IconCode.INFO }
    ]

    //! ⛔ The section list is data, but the section BODIES are not built from it: each is a real item in
    //! this file, so a section that stops existing stops compiling instead of turning into an empty page.
    function sectionIndex(id) {
        for (let i = 0; i < root.sections.length; ++i) {
            if (root.sections[i].id === id) {
                return i
            }
        }
        return 0
    }

    //! The settings file, for the About section. Read when the panel opens rather than bound: it is a
    //! constant for the process, and `Q_INVOKABLE` getters have nothing for a binding to connect to.
    property string settingsFilePath: ""

    parent: Overlay.overlay
    //! ⛔ THE POPUP IS ONLY AS LARGE AS THE PANEL, AND THAT IS LOAD-BEARING. Measured 2026-10-11: with the
    //! popup sized to the whole window (1920x1032) the dialog was DISMISSED the instant a text field
    //! inside it took focus - the assistant section's step field and the connection section's gateway
    //! field both closed it, while clicking a button did not. At 848x848 it survives. `TextInputField`
    //! clears `Qt.WindowDoesNotAcceptFocus` on its window when it takes focus (upstream,
    //! `TextInputField.qml`), which re-creates the native window; a popup that covers the whole window
    //! does not survive that, a small one does. ⚠️ Because the popup is small, "click outside to dismiss"
    //! is `CloseOnPressOutside` again - which is the same gesture DSH gets from its mask.
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: root.panelWidth + 2 * root.panelMargin
    height: Math.min(root.panelMaxHeight + 2 * root.panelMargin,
                     (parent ? parent.height : root.panelMaxHeight + 2 * root.panelMargin) - 16)
    padding: 0
    //! The mask is not drawn: see the sizing note above (a whole-window popup does not survive a text
    //! field taking focus here). `CloseOnPressOutside` gives the same "click away to dismiss" gesture.
    dim: false
    modal: true
    //! ⛔ Esc IS HANDLED BY THE DIALOG ITSELF, not by `CloseOnEscape`. Measured 2026-10-11: with the
    //! policy set, Escape left the dialog open - the popup never became the window's focus item, so the
    //! key went to the page behind it and the policy never fired. A `Shortcut` would be the other wrong
    //! answer (an application-wide key that fires while the dialog is closed). The content item is a
    //! `FocusScope` that takes focus when the popup opens, and answers Escape itself.
    //! ⚠️ With a text field focused the FIRST Escape is consumed by `TextInputField` (it drops focus -
    //! upstream behaviour); the second one closes the dialog.
    focus: true
    closePolicy: Popup.CloseOnPressOutside
    background: null

    onOpened: {
        if (root.field) {
            root.settingsFilePath = root.field.agentSettingsFilePath()
        }
        //! The Connection section re-reads what is in force every time the panel opens: a value typed but
        //! never applied (no Enter) must not come back looking like the gateway in use.
        connectionSection.reload()
        modelsSection.reload()
        overlay.forceActiveFocus()
    }

    //! ⛔ ENTERING A SECTION RELOADS IT, and that is what makes it safe for two sections to describe the
    //! same value. The sections are exclusive (`visible: false`), so only one is ever on screen - but
    //! hidden items still exist and still hold their old text. Without this, switching the model from the
    //! catalog and then walking back to Connection would show the OLD model in the field while the "in use"
    //! line under it showed the new one: the panel contradicting itself, which is the exact failure the
    //! read-back lines exist to prevent (第 130 条). Reloading on entry costs nothing and makes "what is on
    //! screen" equal "what is in force" for whichever section the user is looking at.
    onActiveSectionChanged: {
        if (root.activeSection === "connection") {
            connectionSection.reload()
        } else if (root.activeSection === "models") {
            modelsSection.reload()
        }
    }

    contentItem: FocusScope {
        id: overlay

        focus: true

        //! ⛔ Esc closes the dialog from HERE - see the note on the popup's `focus` above. A text field
        //! inside consumes the first Escape (upstream `TextInputField` gives up focus on it), which is
        //! also why this is a handler on the scope rather than a `Shortcut`.
        //!
        //! ⛔⛔ AND IT MUST BE CLAIMED IN `ShortcutOverride` FIRST. **Escape is registered as an
        //! application-wide shortcut** (see 维护手册.md §4.2 / 第 122 条: Del, Esc, Ctrl+C/V/A are all in
        //! `shortcuts.xml`), so without this the key is matched by the shortcut map before any of our
        //! handlers sees it - measured 2026-10-11: `Keys.onEscapePressed` alone left the dialog open
        //! while the close button and a click outside both worked.
        Keys.onShortcutOverride: function(event) {
            if (event.key === Qt.Key_Escape) {
                event.accepted = true
            }
        }

        Keys.onEscapePressed: function(event) {
            event.accepted = true
            root.close()
        }

        //! ── The panel (DSH: figma Settings 501:29947; 1080x700 shrunk to 800 wide, r32) ───────────
        //! ⚠️ The radius is the one DSH number NOT copied literally: 32 is a web-scale corner on a
        //! desktop dialog, and every other surface in this application rounds at 3-12. 16 keeps the
        //! "large rounded panel" read without looking like a different product.
        RoundedRectangle {
            id: panel

            anchors.centerIn: parent
            //! The content item IS the margin box (the popup is panel + 2 x margin), so the panel simply
            //! takes what is left after the margin - and cannot exceed it by construction.
            width: Math.max(0, overlay.width - 2 * root.panelMargin)
            height: Math.max(0, overlay.height - 2 * root.panelMargin)
            radius: 16
            color: ui.theme.backgroundSecondaryColor
            border.color: ui.theme.strokeColor
            border.width: Math.max(ui.theme.borderWidth, 1)

            RowLayout {
                anchors.fill: parent
                spacing: 0

                //! ══ The nav rail (DSH `.nav`: 188 wide, padding 22/12/0, gap 18) ══════════════════
                //! ⛔ THE WIDTH IS PINNED ON ALL THREE (preferred AND min AND max). With only
                //! `Layout.preferredWidth` the rail was measured at 628px and the content column was
                //! squeezed to 148 - the rail behaved like a `fillWidth` item and swallowed everything the
                //! content column did not claim as its minimum. A rail is a fixed-width strip in DSH (it
                //! never stretches), so stating that as min == max == preferred is also the honest
                //! expression of what it is, not just a workaround.
                ColumnLayout {
                    id: navColumn

                    Layout.preferredWidth: root.navWidth
                    Layout.minimumWidth: root.navWidth
                    Layout.maximumWidth: root.navWidth
                    Layout.fillHeight: true
                    Layout.topMargin: 22
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    spacing: 18

                    StyledTextLabel {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        horizontalAlignment: Text.AlignLeft
                        font: ui.theme.bodyBoldFont
                        text: qsTrc("agentharness", "Settings")
                    }

                    //! The cell stack (DSH `.navList`: gap 4). Each cell is a real button so it is in the
                    //! accessibility tree, with a custom content item because DSH's cell is
                    //! "icon at the left, label left-aligned" - `FlatButton`'s default centres the label.
                    //! ⚠️ The content `Component` reads the DELEGATE's own `icon`/`text` rather than
                    //! `modelData`: an inline `Component` gets its own context, so the model roles the
                    //! Repeater injects into the delegate are not visible inside it.
                    ColumnLayout {
                        id: navList

                        Layout.fillWidth: true
                        spacing: 4

                        Repeater {
                            model: root.sections

                            FlatButton {
                                id: navCell

                                Layout.fillWidth: true
                                Layout.preferredHeight: root.navCellHeight

                                backgroundRadius: 12
                                //! DSH fills the active cell and leaves the rest transparent; hover is
                                //! `FlatButton`'s own state, so it works on both.
                                transparent: root.activeSection !== modelData.id
                                accessible.name: modelData.label
                                text: modelData.label
                                icon: modelData.icon

                                contentItem: Component {
                                    RowLayout {
                                        spacing: 8

                                        StyledIconLabel {
                                            Layout.alignment: Qt.AlignVCenter
                                            iconCode: navCell.icon
                                            color: ui.theme.fontPrimaryColor
                                        }

                                        StyledTextLabel {
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignVCenter
                                            horizontalAlignment: Text.AlignLeft
                                            text: navCell.text
                                        }
                                    }
                                }

                                onClicked: {
                                    root.activeSection = modelData.id
                                }
                            }
                        }
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }

                //! ══ The content column (DSH `.content`: header + options) ═══════════════════════════
                ColumnLayout {
                    id: contentColumn

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0

                    //! ── Header (DSH `.header`: h54, padding 20/14/8/10, actions at the right) ──────
                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.headerHeight

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 14
                            anchors.topMargin: 20
                            anchors.bottomMargin: 8
                            spacing: 8

                            Item {
                                Layout.fillWidth: true
                            }

                            //! The `settings.action` seat. DSH's own action is 「打开配置文件」 (open the
                            //! configuration file). ⛔ Ours says 「打开设置文件位置」 and opens the FOLDER,
                            //! because the application owns that .ini while it runs: QSettings rewrites
                            //! the whole file on its next sync, so an edit made in an external editor
                            //! would be silently thrown away. Revealing it cannot lose anything.
                            FlatButton {
                                Layout.alignment: Qt.AlignVCenter
                                visible: root.settingsFilePath.length > 0
                                accessible.name: qsTrc("agentharness", "Show the settings file location")
                                toolTipTitle: root.settingsFilePath
                                text: qsTrc("agentharness", "Show settings file")
                                transparent: true

                                onClicked: {
                                    const folder = root.settingsFilePath.substring(
                                                     0, root.settingsFilePath.lastIndexOf("/") + 1)
                                    if (folder.length > 0) {
                                        Qt.openUrlExternally("file:///" + folder)
                                    }
                                }
                            }

                            //! The close button (DSH `.close`: 28x28, r28, 14px glyph).
                            FlatButton {
                                id: closeButton

                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28

                                backgroundRadius: 28
                                transparent: true
                                icon: IconCode.CLOSE_X_ROUNDED
                                accessible.name: qsTrc("agentharness", "Close settings")
                                toolTipTitle: qsTrc("agentharness", "Close")

                                onClicked: {
                                    root.close()
                                }
                            }
                        }
                    }

                    //! ── Options (DSH `.options`: padding 0/24/24, scrolls) ──────────────────────────
                    //! ⛔ The panel's height does NOT follow the section (see the header note), so this is
                    //! where a long section goes instead of making the panel grow.
                    Flickable {
                        id: options

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.leftMargin: 24
                        Layout.rightMargin: 24
                        Layout.bottomMargin: 24

                        contentWidth: width
                        contentHeight: sectionColumn.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        interactive: contentHeight > height

                        ColumnLayout {
                            id: sectionColumn

                            width: options.width
                            spacing: 12

                            //! ── Connection ────────────────────────────────────────────────────────
                            //! The gateway and the model fields are NOT re-implemented here: this is the
                            //! same `AgentConnectionPanel` the Agent page's left column used to show, so
                            //! there is one place that knows how to read them back and apply them
                            //! (维护手册.md §4.5).
                            AgentConnectionPanel {
                                id: connectionSection

                                Layout.fillWidth: true
                                visible: root.activeSection === "connection"
                                field: root.field
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "connection"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                color: ui.theme.fontSecondaryColor
                                text: qsTrc("agentharness",
                                            "The API key has its own section, next to this one, because a key belongs to a model list rather than to an address. It can be entered in the composer too (“Set key”).")
                            }

                            //! ── Models ───────────────────────────────────────────────────────────
                            //! DSH's "模型" section: the provider, its key, and the catalog you pick from.
                            //! The whole section is one component (`AgentModelsSection.qml`), for the same
                            //! reason the Connection section is: a second copy of the key field, the
                            //! catalog list or the "switch the model in use" call would be a second place
                            //! for them to disagree (维护手册.md §4.5).
                            AgentModelsSection {
                                id: modelsSection

                                Layout.fillWidth: true
                                visible: root.activeSection === "models"
                                field: root.field
                            }

                            //! ── Assistant ─────────────────────────────────────────────────────────
                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "assistant"
                                horizontalAlignment: Text.AlignLeft
                                font: ui.theme.bodyBoldFont
                                text: qsTrc("agentharness", "How much work one message may cost")
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "assistant"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                color: ui.theme.fontSecondaryColor
                                text: qsTrc("agentharness",
                                            "One turn = up to this many model round-trips. A model that keeps calling tools without converging is stopped here, and the conversation says so (“stopped after N steps…”) - the limit is ours, not yours, which is why it can be changed.")
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.activeSection === "assistant"
                                spacing: 12

                                StyledTextLabel {
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignLeft
                                    text: qsTrc("agentharness", "Steps per turn")
                                }

                                IncrementalPropertyControl {
                                    id: maxStepsControl

                                    Layout.preferredWidth: 120
                                    Layout.alignment: Qt.AlignVCenter

                                    minValue: root.field ? root.field.agentMaxStepsMin : 1
                                    maxValue: root.field ? root.field.agentMaxStepsMax : 100
                                    step: 1
                                    decimals: 0
                                    //! Read back from the field (which reads back from the loop): the
                                    //! control shows the value that is IN FORCE, not the one that was
                                    //! asked for - the setter clamps (第 130 条).
                                    currentValue: root.field ? root.field.agentMaxSteps : 0
                                    //! ⚠️ `IncrementalPropertyControl` has no `accessible` alias (it
                                    //! wraps a `TextInputField`): the name goes through `navigation`,
                                    //! and writing it the short way is a runtime load error.
                                    navigation.accessible.name: qsTrc("agentharness", "Steps per turn")

                                    onValueEdited: function(newValue) {
                                        if (root.field) {
                                            root.field.setAgentMaxSteps(newValue)
                                        }
                                    }
                                }
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "assistant"
                                horizontalAlignment: Text.AlignLeft
                                color: ui.theme.fontSecondaryColor
                                text: qsTrc("agentharness", "In force: %1 step(s) per turn")
                                      .arg(root.field ? root.field.agentMaxSteps : 0)
                            }

                            //! ── Interface ─────────────────────────────────────────────────────────
                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "interface"
                                horizontalAlignment: Text.AlignLeft
                                font: ui.theme.bodyBoldFont
                                text: qsTrc("agentharness", "Agent page columns")
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "interface"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                color: ui.theme.fontSecondaryColor
                                text: qsTrc("agentharness",
                                            "Drag the vertical boundaries on the Agent page to change them. The widths are remembered: left %1 px, right %2 px (defaults 280 / 320).")
                                      .arg(root.field && root.field.agentSidebarWidth > 0
                                           ? root.field.agentSidebarWidth : 280)
                                      .arg(root.field && root.field.agentRightbarWidth > 0
                                           ? root.field.agentRightbarWidth : 320)
                            }

                            FlatButton {
                                Layout.alignment: Qt.AlignLeft
                                visible: root.activeSection === "interface" && root.layoutResettable
                                accessible.name: qsTrc("agentharness", "Reset the Agent page columns")
                                text: qsTrc("agentharness", "Reset columns")
                                icon: IconCode.UNDO

                                onClicked: {
                                    root.layoutResetRequested()
                                }
                            }

                            //! ── About ─────────────────────────────────────────────────────────────
                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "about"
                                horizontalAlignment: Text.AlignLeft
                                font: ui.theme.bodyBoldFont
                                text: qsTrc("agentharness", "What is in force")
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "about"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                color: ui.theme.fontSecondaryColor
                                text: {
                                    if (!root.field) {
                                        return ""
                                    }
                                    //! ⛔ "Score revision" is the FENCE (第 140 条): the number `score_revision`
                                    //! prints and `expectRevision` compares. `Session events` above it is the
                                    //! other number - how much this field has recorded - and the two are
                                    //! deliberately not the same. The sibling line in `About` used to read
                                    //! "Score revision" while printing the event count.
                                    return qsTrc("agentharness", "Gateway: %1\nModel: %2\nKey: %3\nTools: %4\nSession events: %5\nScore revision (write fence): %6")
                                           .arg(root.field.agentBaseUrl)
                                           .arg(root.field.agentModel)
                                           .arg(root.field.agentApiKeySource)
                                           .arg(root.field.toolCount)
                                           .arg(root.field.eventCount)
                                           .arg(root.field.revision)
                                }
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "about"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                font: ui.theme.bodyBoldFont
                                color: ui.theme.fontSecondaryColor
                                text: qsTrc("agentharness", "Where the preferences are stored")
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "about"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WrapAnywhere
                                color: ui.theme.fontSecondaryColor
                                text: root.settingsFilePath
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "about"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                color: ui.theme.fontSecondaryColor
                                text: qsTrc("agentharness",
                                            "Environment variables win over these preferences: MUSE_AGENT_API_BASE, MUSE_AGENT_MODEL and MUSE_AGENT_MAX_STEPS are read at startup, so a value set there cannot be changed from this panel.")
                            }

                            FlatButton {
                                Layout.alignment: Qt.AlignLeft
                                visible: root.activeSection === "about"
                                accessible.name: qsTrc("agentharness", "Reset every agent preference")
                                text: qsTrc("agentharness", "Reset all agent preferences")
                                icon: IconCode.UNDO

                                onClicked: {
                                    if (root.field) {
                                        root.field.resetAgentPreferences()
                                        root.layoutResetRequested()
                                    }
                                }
                            }

                            StyledTextLabel {
                                Layout.fillWidth: true
                                visible: root.activeSection === "about"
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                color: ui.theme.fontSecondaryColor
                                text: qsTrc("agentharness",
                                            "Resets the gateway, the model, the step guard and the column widths. The API key is untouched - it is never stored.")
                            }
                        }
                    }
                }
            }
        }
    }
}
