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

    //! 面板的导航区。底部的混音器用它 —— 与记谱页同一套写法（每个方位一个区）。
    property NavigationSection keynavBottomPanelSec: NavigationSection {
        name: "MidiBottomPanel"
        enabled: root.visible
        order: 7
    }

    //! 面板尺寸与拖放目标的常量，照抄记谱页（那些值就是上游的实验值）。
    readonly property int panelMinDimension: 10
    readonly property int panelMaxDimension: 7500 //! NOTE: Value found experimentally - see issue #27770

    readonly property int horizontalPanelMinHeight: 100
    readonly property int horizontalPanelMaxHeight: 520

    readonly property string horizontalPanelsGroup: "MIDI_HORIZONTAL_PANELS"

    readonly property var horizontalPanelDropDestinations: [
        root.panelTopDropDestination,
        root.panelBottomDropDestination
    ]

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
                id: playback

                navigationPanelSection: playbackToolBar.navigationSection
                navigationPanelOrder: 2

                floating: playbackToolBar.floating

                //! 🆕 录制按钮做在**走带按钮行**里（节拍器图标左边）—— 与播放/循环/节拍器同一排，
                //! 因为它们本来就是一件事：按下它 = 从这里开始播 + 开始记。
                //!
                //! 这一页**不另建一套状态**：这里给的是"按钮长什么样"（图标/文字/亮不亮/能不能按），
                //! 真正的录制、量化与写回都在 `midiModel` 里（见 `维护手册.md` §4.8.4）。
                extraItem: {
                    return {
                        //! ⚠️ 文字在这里就翻译好（C++ 那边按"不可翻译"处理，不会再翻一次）。
                        "title": qsTrc("appshell", "Record"),
                        "description": midiModel.isRecording
                                         ? qsTrc("appshell", "Stop and write what was played into the score (one undo step). "
                                                             + "No MIDI keyboard? Play with the computer keyboard: C D E F G A B, Z/X = octave")
                                         : (midiModel.canRecord
                                            ? qsTrc("appshell", "Play along from the playhead: everything you play is captured, "
                                                               + "then quantized and written into the selected staff. "
                                                               + "No MIDI keyboard? Play with the computer keyboard: C D E F G A B, Z/X = octave")
                                            : qsTrc("appshell", "No MIDI input device: pick one in Preferences → Audio & MIDI. "
                                                               + "The computer keyboard still plays: C D E F G A B, Z/X = octave")),
                        "icon": IconCode.RECORD_FILL,
                        "checked": midiModel.isRecording,
                        //! ⚠️ 没有 MIDI 设备时**不再**整键禁用：电脑键盘也能弹（用户 2026-10-07 报的
                        //! 「按 c.d.e.f.g 没反应」就是这一条）—— 没有硬件不该等于不能录。
                        "enabled": true
                    }
                }

                onExtraItemTriggered: {
                    midiModel.toggleRecording()
                }
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

    panels: [
        //! The mixer, next to the piano roll - the same panel the Score page has, so the faders,
        //! mutes and solos are the ones the score is playing through. Reusing MixerPanel is the whole
        //! point: it already knows the tracks, the effects and the solo/mute rules, and the roll's
        //! "only this staff" switch writes that very solo state.
        //!
        //! ⚠️ The object name must NOT be the notation page's "mixerPanel": KDDockWidgets keeps
        //! dock widgets in one registry per window and warns about (and mixes up) duplicate names, so
        //! every dock of this page is prefixed with the page name - the toolbars above do the same.
        DockPanel {
            id: mixerPanel

            objectName: "Midi_mixerPanel"
            title: qsTrc("appshell", "Mixer")

            height: 368
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: unlike the notation page's mixer this one starts OPEN: on this page the mixer is
            //! not an extra to be discovered, it is what the user asked to have next to the roll.
            //! It can be closed with the Mixer button in the roll's own toolbar.
            visible: true

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.keynavBottomPanelSec

            MixerPanel {
                id: mixerPanelComponent

                navigationSection: mixerPanel.navigationSection
                contentNavigationPanelOrderStart: mixerPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    mixerPanel.contextMenuModel = contextMenuModel
                    mixerPanel.toolbarComponent = toolbarComponent
                }

                Component.onDestruction: {
                    mixerPanel.contextMenuModel = null
                    mixerPanel.toolbarComponent = null
                }

                onResizeRequested: function(newWidth, newHeight) {
                    mixerPanel.resize(newWidth, newHeight)
                }

                Connections {
                    target: mixerPanel
                    function onPanelShown() {
                        mixerPanelComponent.resizePanelToContentHeight()
                    }
                }
            }
        }
    ]

    central: MidiEditorView {
        id: midiEditor

        model: midiModel

        //! The Mixer button in the roll's toolbar. Read from the panel, and toggled through the dock
        //! system's own open()/close() so the panel state stays what the dock saves and restores.
        mixerOpen: mixerPanel.visible

        onMixerToggleRequested: {
            if (mixerPanel.visible) {
                mixerPanel.close()
            } else {
                mixerPanel.open()
            }
        }
    }

    statusBar: DockStatusBar {
        objectName: root.objectName + "_statusBar"

        navigationSection: content.navigationSection

        NotationStatusBar {
            id: content
        }
    }
}
