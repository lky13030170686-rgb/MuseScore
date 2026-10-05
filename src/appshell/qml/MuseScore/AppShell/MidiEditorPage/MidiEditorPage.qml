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

import MuseScore.AppShell

import MuseScore.NotationScene
import MuseScore.Playback

DockPage {
    id: root

    objectName: "Midi"
    uri: "musescore://midi"

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
