/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

pragma ComponentBehavior: Bound

import QtQuick

import Muse.Dock
import Muse.Interactive
import Muse.Ui
import Muse.UiComponents
import MuseScore.AppShell
import MuseScore.AgentHarness

import "./HomePage"
import "./NotationPage"
import "./MidiEditorPage"
import "./PublishPage"
import "./DevTools"

DockWindow {
    id: root

    objectName: "WindowContent"

    onPageLoaded: {
        console.log("WindowContent::onPageLoaded")
        interactiveProvider.onPageOpened()
    }

    InteractiveProvider {
        id: interactiveProvider
        topParent: root

        onRequestedDockPage: function(uri, params) {
            root.loadPage(uri, params)
        }
    }

    NavigationSection {
        id: topToolbarKeyNavSec
        name: "TopTool"
        order: 1
    }

    toolBars: [
        DockToolBar {
            id: mainToolBar

            objectName: "mainToolBar"
            title: qsTrc("appshell", "Main toolbar")

            floatable: false
            closable: false

            navigationSection: topToolbarKeyNavSec

            MainToolBar {
                id: toolBar
                navigation.section: mainToolBar.navigationSection
                navigation.order: 1

                currentUri: root.currentPageUri

                navigation.onActiveChanged: {
                    if (navigation.active) {
                        mainToolBar.forceActiveFocus()
                    }
                }

                onSelected: function(uri) {
                    root.openPage(uri)
                }

                Component.onCompleted: {
                    toolBar.focusOnFirst()
                }
            }
        }
    ]

    //! ── The agent harness's information field ──────────────────────────────────────────────
    //! It lives here, at window level, for two reasons that both matter:
    //!
    //!   1. **It must outlive any one panel.** `DockPanel` only instantiates its content while it
    //!      is visible (`DockPanel.qml`: `contentLoader.active: root.visible && root.inited`), so
    //!      a field declared inside the Agent panel would record nothing until the user opened
    //!      that panel - and a time-ordered record that only starts when you look at it is
    //!      worthless.
    //!   2. **It needs a context.** `IGlobalContext` is a context interface, so the field must be
    //!      created where one resolves; a QML-declared element resolves it from its own
    //!      QQmlContext (`iocCtxForQmlObject`), and this file is instantiated inside the window.
    FieldController {
        id: agentField

        Component.onCompleted: init()
    }

    pages: [
        HomePage {
            window: root.window
        },

        NotationPage {
            topToolbarKeyNavSec: topToolbarKeyNavSec
            agentField: agentField
        },

        MidiEditorPage {
            topToolbarKeyNavSec: topToolbarKeyNavSec
        },

        PublishPage {
            topToolbarKeyNavSec: topToolbarKeyNavSec
        },

        DevToolsPage {}
    ]
}
