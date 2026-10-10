/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2024 MuseScore Limited and others
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

import Muse.Ui
import Muse.UiComponents
import Muse.Dock
import Muse.Extensions
import MuseScore.AppShell

import MuseScore.NotationScene
import MuseScore.Palette
import MuseScore.PropertiesPanel
import MuseScore.InstrumentsScene
import MuseScore.Playback
import MuseScore.AgentHarness

DockPage {
    id: root

    objectName: "Notation"
    uri: "musescore://notation"

    required property NavigationSection topToolbarKeyNavSec

    //! The agent harness's information field, created at window level (see WindowContent.qml) and
    //! handed down to the Agent panel. It is a property rather than something the panel creates,
    //! because `DockPanel` only builds its content while visible - the field has to be recording
    //! before anyone opens the panel.
    property var agentField

    property NotationPageModel pageModel: NotationPageModel {}

    property NavigationSection noteInputKeyNavSec: NavigationSection {
        name: "NoteInputSection"
        order: 2
    }

    property NavigationSection keynavTopPanelSec: NavigationSection {
        name: "NavigationTopPanel"
        enabled: root.visible
        order: 3
    }

    property NavigationSection keynavLeftPanelSec: NavigationSection {
        name: "NavigationLeftPanel"
        enabled: root.visible
        order: 4
    }

    property NavigationSection keynavRightPanelSec: NavigationSection {
        name: "NavigationRightPanel"
        enabled: root.visible
        order: 6
    }

    property NavigationSection keynavBottomPanelSec: NavigationSection {
        name: "NavigationBottomPanel"
        enabled: root.visible
        order: 7
    }

    function navigationPanelSec(location) {
        switch(location) {
        case Location.Top: return keynavTopPanelSec
        case Location.Left: return keynavLeftPanelSec
        case Location.Right: return keynavRightPanelSec
        case Location.Bottom: return keynavBottomPanelSec
        }

        return null
    }

    onInited: {
        Qt.callLater(pageModel.init)
    }

    readonly property int verticalPanelDefaultWidth: 300

    readonly property int horizontalPanelMinHeight: 100
    readonly property int horizontalPanelMaxHeight: 520

    readonly property int panelMinDimension: 10
    readonly property int panelMaxDimension: 7500 //! NOTE: Value found experimentally - see issue #27770

    readonly property string verticalPanelsGroup: "VERTICAL_PANELS"
    readonly property string horizontalPanelsGroup: "HORIZONTAL_PANELS"

    readonly property var verticalPanelDropDestinations: [
        { "dock": root.centralDock, "dropLocation": Location.Left, "dropDistance": root.verticalPanelDefaultWidth },
        { "dock": root.centralDock, "dropLocation": Location.Right, "dropDistance": root.verticalPanelDefaultWidth }
    ]

    readonly property var horizontalPanelDropDestinations: [
        root.panelTopDropDestination,
        root.panelBottomDropDestination
    ]

    property var notationView: null

    mainToolBars: [
        DockToolBar {
            id: notationToolBar

            objectName: "notationToolBar"
            title: qsTrc("appshell", "Notation toolbar")

            floatable: false
            closable: false
            resizable: false
            separatorsVisible: false

            alignment: DockToolBarAlignment.Center
            contentBottomPadding: 2

            compactPriorityOrder: 1

            navigationSection: root.topToolbarKeyNavSec

            NotationToolBar {
                isCompactMode: notationToolBar.isCompact

                navigationPanel.section: notationToolBar.navigationSection
                navigationPanel.order: 2
            }
        },

        DockToolBar {
            id: playbackToolBar

            objectName: root.pageModel.playbackToolBarName()
            title: qsTrc("appshell", "Playback controls")

            separatorsVisible: false
            alignment: DockToolBarAlignment.Right
            resizable: !floating

            contentBottomPadding: floating ? 8 : 2
            contentTopPadding: floating ? 8 : 0

            dropDestinations: [
                { "dock": notationToolBar, "dropLocation": Location.Right }
            ]

            navigationSection: root.topToolbarKeyNavSec

            PlaybackToolBar {
                navigationPanelSection: playbackToolBar.navigationSection
                navigationPanelOrder: 3

                floating: playbackToolBar.floating
            }
        },

        DockToolBar {
            id: extDockToolBar

            objectName: root.pageModel.extensionsToolBarName()
            title: qsTrc("appshell", "Extensions toolbar")

            separatorsVisible: false
            orientation: Qt.Horizontal
            alignment: DockToolBarAlignment.Right

            contentBottomPadding: floating ? 8 : 2
            contentTopPadding: floating ? 8 : 0

            //! NOTE: Opened by page model when there are extensions with toolbar actions
            visible: false

            dropDestinations: [
                { "dock": notationToolBar, "dropLocation": Location.Right },
                { "dock": playbackToolBar, "dropLocation": Location.Right }
            ]

            navigationSection: root.topToolbarKeyNavSec

            ExtensionsToolBar {
                id: extToolBar

                navigationPanel.section: extDockToolBar.navigationSection
                navigationPanel.order: 4
            }
        },

        DockToolBar {
            id: undoRedoToolBar

            objectName: root.pageModel.undoRedoToolBarName()
            title: qsTrc("appshell", "Undo/redo")

            floatable: false
            closable: false
            resizable: false
            separatorsVisible: false

            alignment: DockToolBarAlignment.Right
            contentBottomPadding: 2

            navigationSection: root.topToolbarKeyNavSec

            UndoRedoToolBar {
                navigationPanel.section: undoRedoToolBar.navigationSection
                navigationPanel.order: 5
            }
        }
    ]

    toolBars: [
        DockToolBar {
            id: noteInputBar

            objectName: root.pageModel.noteInputBarName()
            title: qsTrc("appshell", "Note input")

            dropDestinations: [
                root.toolBarTopDropDestination,
                root.toolBarBottomDropDestination,
                root.toolBarLeftDropDestination,
                root.toolBarRightDropDestination
            ]

            thickness: orientation === Qt.Horizontal ? 40 : 76
            resizable: !floating

            navigationSection: root.noteInputKeyNavSec

            NoteInputBar {
                orientation: noteInputBar.orientation
                floating: noteInputBar.floating

                maximumWidth: noteInputBar.width
                maximumHeight: noteInputBar.height

                navigationPanel.section: noteInputBar.navigationSection
                navigationPanel.order: 1
            }
        }
    ]

    panels: [
        DockPanel {
            id: palettesPanel

            objectName: root.pageModel.palettesPanelName()
            title: qsTrc("appshell", "Palettes")

            navigationSection: root.navigationPanelSec(palettesPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            dropDestinations: root.verticalPanelDropDestinations

            PalettesPanel {
                navigationSection: palettesPanel.navigationSection
                navigationOrderStart: palettesPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    palettesPanel.contextMenuModel = contextMenuModel
                }
            }
        },

        DockPanel {
            id: layoutPanel

            objectName: root.pageModel.layoutPanelName()
            title: qsTrc("appshell", "Layout")

            navigationSection: root.navigationPanelSec(layoutPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            dropDestinations: root.verticalPanelDropDestinations

            LayoutPanel {
                navigationSection: layoutPanel.navigationSection
                navigationOrderStart: layoutPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    layoutPanel.contextMenuModel = contextMenuModel
                }
            }
        },

        DockPanel {
            id: propertiesPanel

            objectName: root.pageModel.propertiesPanelName()
            title: qsTrc("appshell", "Properties")

            navigationSection: root.navigationPanelSec(propertiesPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            dropDestinations: root.verticalPanelDropDestinations

            PropertiesPanel {
                navigationSection: propertiesPanel.navigationSection
                navigationOrderStart: propertiesPanel.contentNavigationPanelOrderStart
                notationView: root.notationView
            }
        },

        DockPanel {
            id: selectionFilterPanel

            objectName: root.pageModel.selectionFiltersPanelName()
            title: qsTrc("appshell", "Selection filter")

            navigationSection: root.navigationPanelSec(selectionFilterPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            dropDestinations: root.verticalPanelDropDestinations

            SelectionFilterPanel {
                navigationSection: selectionFilterPanel.navigationSection
                navigationOrderStart: selectionFilterPanel.contentNavigationPanelOrderStart
            }
        },

        DockPanel {
            id: undoHistoryPanel

            objectName: root.pageModel.undoHistoryPanelName()
            title: qsTrc("notation", "History")

            navigationSection: root.navigationPanelSec(undoHistoryPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup
            location: Location.Right

            //! NOTE: hidden by default
            visible: false

            dropDestinations: root.verticalPanelDropDestinations

            UndoHistoryPanel {
                navigationSection: undoHistoryPanel.navigationSection
                navigationOrderStart: undoHistoryPanel.contentNavigationPanelOrderStart
            }
        },
        
        // =============================================
        // Horizontal Panels
        // =============================================

        DockPanel {
            id: mixerPanel

            objectName: root.pageModel.mixerPanelName()
            title: qsTrc("appshell", "Mixer")

            height: 368
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(mixerPanel.location)

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
        },

        DockPanel {
            id: pianoKeyboardPanel

            objectName: root.pageModel.pianoKeyboardPanelName()
            title: qsTrc("appshell", "Piano keyboard")

            height: 200
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(pianoKeyboardPanel.location)

            PianoKeyboardPanel {
                navigationSection: pianoKeyboardPanel.navigationSection
                contentNavigationPanelOrderStart: pianoKeyboardPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    pianoKeyboardPanel.contextMenuModel = contextMenuModel
                }
            }
        },

        DockPanel {
            id: agentPanel

            //! ⚠️ THE NAME COMES FROM THE MODEL, and that is not tidiness - the View-menu entry
            //! (`toggle-agent-panel`) is matched to this panel BY THIS NAME, so a second copy of the
            //! literal would drift into "the menu item is there and clicking it does nothing".
            objectName: root.pageModel.agentPanelName()
            title: qsTrc("appshell", "Agent")

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            //! NOTE: hidden by default, like the piano keyboard and timeline panels. The agent is
            //! opt-in; a panel that is always there would take score width away from every user
            //! who never opens it.
            //!
            //! ⛔⛔ DO NOT TRY TO OPEN IT FROM IN HERE. Two things are true at once and both bite:
            //!   1. `DockBase::init()` calls `setVisible(m_dockWidget->isOpen())`, and a C++
            //!      assignment to a QML property DESTROYS the binding - so `visible: PanelConfig
            //!      .forceOpen` documents the default but cannot open anything by itself;
            //!   2. anything written inside this block is content, not a live object (see the Timer
            //!      note at the bottom of this file).
            //! The panel is opened through the View menu (`toggle-agent-panel`) or, for scripted
            //! verification, by the `MUSE_AGENT_PANEL` timer at the bottom of this file.
            visible: false


            Component.onCompleted: {
                agentPanel.contextMenuModel = contextMenuModel
            }

            Component.onDestruction: {
                agentPanel.contextMenuModel = null
            }

            location: Location.Right

            dropDestinations: root.verticalPanelDropDestinations

            navigationSection: root.navigationPanelSec(agentPanel.location)

            AgentPanel {
                field: root.agentField
                navigationSection: agentPanel.navigationSection
                contentNavigationPanelOrderStart: agentPanel.contentNavigationPanelOrderStart
            }
        },

        DockPanel {
            id: timelinePanel

            objectName: root.pageModel.timelinePanelName()
            title: qsTrc("appshell", "Timeline")

            height: 200
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(timelinePanel.location)

            Timeline {
                navigationSection: timelinePanel.navigationSection
                contentNavigationPanelOrderStart: timelinePanel.contentNavigationPanelOrderStart
            }
        },

        DockPanel {
            id: percussionPanel

            objectName: root.pageModel.percussionPanelName()
            title: qsTrc("appshell", "Percussion")

            height: 200
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(percussionPanel.location)

            PercussionPanel {
                id: percussionComponent

                navigationSection: percussionPanel.navigationSection
                contentNavigationPanelOrderStart: percussionPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    percussionPanel.toolbarComponent = toolbarComponent
                }

                Component.onDestruction: {
                    percussionPanel.toolbarComponent = null
                }

                onResizeRequested: function(newWidth, newHeight) {
                    percussionPanel.resize(newWidth, newHeight)
                }

                Connections {
                    target: percussionPanel
                    function onPanelShown() {
                        percussionComponent.resizePanelToContentHeight()
                    }
                }
            }
        }
    ]

    central: NotationView {
        id: notationView
        name: "MainNotationView"

        isNavigatorVisible: root.pageModel.isNavigatorVisible
        isBraillePanelVisible: root.pageModel.isBraillePanelVisible
        isMainView: true

        Component.onCompleted: {
            root.notationView = notationView.paintView

            root.setDefaultNavigationControl(notationView.defaultNavigationControl)
        }

        Component.onDestruction: {
            root.setDefaultNavigationControl(null)
        }
    }

    statusBar: DockStatusBar {
        objectName: root.pageModel.statusBarName()

        navigationSection: content.navigationSection

        NotationStatusBar {
            id: content
        }
    }

    //! ── MUSE_AGENT_PANEL: open the Agent panel without clicking ────────────────────────────────────
    //!
    //! ⛔⛔ THIS TIMER CANNOT LIVE INSIDE THE `DockPanel` ABOVE, AND PUTTING IT THERE COST A ROUND.
    //! `DockPanel.qml` declares its children as `default property alias contentComponent :
    //! contentLoader.sourceComponent` - so anything written inside the `DockPanel { }` block is NOT
    //! created; it is stashed as the panel's content and only instantiated when the panel becomes
    //! visible. A `Timer` in there is therefore never constructed and never fires, silently: the
    //! panel stays hidden, nothing is logged, and the switch looks broken.
    //!
    //! ⚠️ And the reason it must retry rather than fire once: the panel's visible state is restored
    //! from the persisted layout after the dock is inited, so an early open is simply overwritten.
    //! `running` turns itself off the moment the dock reports open.
    //!
    //! ⚠️ It calls `openAgentPanel()` (→ the `dock-set-open` action the View menu also uses) and NOT
    //! `agentPanel.open()`: `open()` sets the item's `visible` and calls the dock widget's `open()`,
    //! which the layout restore undoes. Both were measured (第 123 条).
    Timer {
        running: PanelConfig.forceOpen && !agentPanel.isOpen()
        interval: 150
        repeat: true
        onTriggered: root.pageModel.openAgentPanel()
    }

    tours: [
        {
            "eventCode": "online_sounds_added",
            "tour": {
                "id": "online-sounds-first-use",
                "steps": [
                    {
                        "title": qsTrc("playback", "This sound processes online"),
                        "description": qsTrc("playback", "Audio is processed in the background while you work. To trigger processing yourself, turn off automatic processing in Preferences > Audio & MIDI > Online sounds."),
                        "controlUri": "control://NotationStatusBar/NotationStatusBar/OnlineSoundsStatusView",
                        "previewImageOrGifUrl": "qrc:/resources/OnlineSoundsPreview.gif",
                        "videoExplanationUrl": "https://youtu.be/hQ4YqmHM3BE?utm_source=mss-app-yt-4.6-cantai&utm_medium=mss-app-yt-4.6-cantai&utm_campaign=mss-app-yt-4.6-cantai"
                    }
                ]
            }
        },
        {
            "eventCode": "online_sounds_manual_processing_allowed",
            "tour": {
                "id": "online-sounds-manual-process",
                "steps": [
                    {
                        "title": qsTrc("playback", "Online sounds"),
                        "description": qsTrc("playback", "Click to manually process online sounds."),
                        "controlUri": "control://NotationStatusBar/NotationStatusBar/OnlineSoundsStatusView",
                    }
                ]
            }
        },
    ]
}
