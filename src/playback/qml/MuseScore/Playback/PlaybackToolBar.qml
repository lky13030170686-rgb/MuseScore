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
 
import QtQuick

import Muse.Ui
import Muse.UiComponents
import MuseScore.Playback

import "internal"

Item {
    id: root

    property alias floating: thePlaybackModel.isToolbarFloating

    //! 🆕 宿主页面可以往走带按钮行里插**一个**按钮（MIDI 页用它把"录制"放到节拍器左边）。
    //! 形状：{ title, description, icon, checked, enabled }；点它发 `extraItemTriggered()`。
    //! 不设它就是纯上游那条走带（记谱页就是这样）。
    property alias extraItem: thePlaybackModel.extraItem

    signal extraItemTriggered()

    property NavigationPanel navigationPanel: NavigationPanel {
        id: navPanel
        name: "PlaybackToolBar"
        enabled: root.enabled && root.visible
        accessible.name: qsTrc("playback", "Playback toolbar")
    }

    property alias navigationPanelSection: navPanel.section
    property alias navigationPanelOrder: navPanel.order

    PlaybackToolBarModel {
        id: thePlaybackModel

        onExtraItemTriggered: root.extraItemTriggered()
    }

    width: content.width + (root.floating ? 12 : 0)
    height: content.height

    Component.onCompleted: {
        thePlaybackModel.load()
    }

    Column {
        id: content

        spacing: 8

        width: childrenRect.width

        enabled: thePlaybackModel.isPlayAllowed

        PlaybackToolBarActions {
            id: playbackActions

            playbackModel: thePlaybackModel
            floating: root.floating

            navPanel: root.navigationPanel
        }

        Loader {
            active: root.floating

            width: childrenRect.width

            sourceComponent: Column {
                spacing: 8

                width: childrenRect.width

                StyledSlider {
                    id: playPositionSlider

                    width: playbackActions.width

                    value: thePlaybackModel.playPosition
                    stepSize: 0.05

                    navigation.panel: navPanel
                    navigation.order: playbackActions.navigationOrderEnd + 1

                    onMoved: {
                        thePlaybackModel.playPosition = value
                    }
                }

                PlaybackSpeedSlider {
                    width: playbackActions.width

                    playbackModel: thePlaybackModel

                    navigationPanel: navPanel
                    navigationOrderStart: playPositionSlider.navigation.order + 1
                }
            }
        }
    }
}
