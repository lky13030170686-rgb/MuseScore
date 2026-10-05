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

//! NOTE: The "MIDI" page - a sibling of Home / Score / Publish (see MainToolBarModel).
//! It shows the same project as the Score page, but as a piano roll instead of notation.

pragma ComponentBehavior: Bound

import QtQuick

import Muse.Ui
import Muse.UiComponents
import Muse.Dock
import Muse.Shortcuts

import MuseScore.AppShell

import MuseScore.NotationScene
import MuseScore.Playback

DockPage {
    id: root

    objectName: "Midi"
    uri: "musescore://midi"

    //! ⚠️ 观测点（排查 Ctrl+Z 在 MIDI 页不生效，用完可删）：
    //! 全局快捷键的 `Shortcut.enabled` 绑在 `shortcutsRegister()->active()` 上
    //! （`Muse/Shortcuts/Shortcuts.qml:57`），而那个值由
    //! `ShortcutsController::init()` 在每次切页面时算成
    //! `!interactive()->topWindowIsWidget()`。这里直接把它读出来，
    //! 就能确认"MIDI 页快捷键失效"是不是 `active == false` 造成的。
    ShortcutsInstanceModel {
        id: shortcutsProbe

        Component.onCompleted: {
            shortcutsProbe.init()

            //! 观测点：active 的值 + **快捷键表里到底有没有 Ctrl+Z**。
            //! 上面那条链（active → resolveAction 的两个条件）都已排除，
            //! 剩下的可能就是这个序列压根没被注册进来。
            var keys = []
            for (var k in shortcutsProbe.shortcuts) {
                keys.push(k)
            }

            console.warn("[midi-automation] shortcuts active=" + shortcutsProbe.active
                         + " count=" + keys.length
                         + " hasCtrlZ=" + (shortcutsProbe.shortcuts["Ctrl+Z"] !== undefined)
                         + " zKeys=" + keys.filter(function(s) { return s.indexOf("Z") >= 0 }).join("|")
                         + " undoLike=" + keys.filter(function(s) { return s.indexOf("Ctrl+") === 0 }).join("|"))
        }
    }

    required property NavigationSection topToolbarKeyNavSec

    property NavigationSection midiToolBarKeyNavSec: NavigationSection {
        name: "MidiToolBarSection"
        order: 2
    }

    MidiEditorModel {
        id: midiModel
    }

    onInited: {
        midiModel.init()
    }

    mainToolBars: [
        DockToolBar {
            id: playbackToolBar

            objectName: root.objectName + "_playbackToolBar"
            title: qsTrc("appshell", "Playback controls")

            floatable: false
            closable: false
            separatorsVisible: false

            alignment: DockToolBarAlignment.Center
            contentBottomPadding: 2

            navigationSection: root.topToolbarKeyNavSec

            PlaybackToolBar {
                navigationPanelSection: playbackToolBar.navigationSection
                navigationPanelOrder: 2

                floating: playbackToolBar.floating
            }
        },

        DockToolBar {
            id: undoRedoToolBar

            objectName: root.objectName + "_undoRedoToolBar"
            title: qsTrc("appshell", "Undo/redo toolbar")

            floatable: false
            closable: false
            resizable: false
            separatorsVisible: false

            alignment: DockToolBarAlignment.Right
            contentBottomPadding: 2

            navigationSection: root.topToolbarKeyNavSec

            UndoRedoToolBar {
                navigationPanel.section: undoRedoToolBar.navigationSection
                navigationPanel.order: 3
            }
        }
    ]

    central: MidiEditorView {
        id: midiEditor

        model: midiModel
    }

    statusBar: DockStatusBar {
        objectName: root.objectName + "_statusBar"

        navigationSection: content.navigationSection

        NotationStatusBar {
            id: content
        }
    }
}
