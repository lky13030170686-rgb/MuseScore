/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited
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

import Muse.UiComponents
import Muse.Ui
import MuseScore.Project

ExportSettingsPage {
    id: root

    property bool isFFmpegAvailable: Boolean(root.model) && root.model.isFFmpegAvailable

    isExportAvailable: root.isFFmpegAvailable

    Loader {
        width: parent.width

        sourceComponent: root.isFFmpegAvailable ? videoSettingsComp : ffmpegMissingComp
    }

    Component {
        id: ffmpegMissingComp

        Column {
            width: parent.width
            spacing: 12

            StyledTextLabel {
                width: parent.width
                text: qsTrc("project/export", "To export to this file format, an FFmpeg installation is required. You can also configure this in Preferences > Video.")
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
            }

            FlatButton {
                id: downloadButton

                text: qsTrc("project/export", "Download FFmpeg")
                icon: IconCode.OPEN_LINK
                isNarrow: true
                orientation: Qt.Horizontal

                navigation.name: "ExportDownloadFFmpeg"
                navigation.panel: root.navigationPanel
                navigation.row: root.navigationOrder

                onClicked: {
                    api.launcher.openUrl("https://handbook.musescore.org/video/installing-ffmpeg")
                }
            }

            FilePicker {
                width: parent.width

                pickerType: FilePicker.PickerType.Directory
                dialogTitle: qsTrc("project/export", "Choose FFmpeg library folder")

                buttonType: FlatButton.TextOnly
                buttonText: qsTrc("project/export", "Locate existing installation")
                showPathField: false

                dir: root.model ? root.model.ffmpegDir : ""
                path: root.model ? root.model.ffmpegDir : ""

                navigation: root.navigationPanel
                navigationRowOrderStart: downloadButton.navigation.order + 1

                onPathEdited: function(newPath) {
                    if (root.model) {
                        root.model.ffmpegDir = newPath
                    }
                }
            }
        }
    }

    Component {
        id: videoSettingsComp

        Column {
            width: parent.width
            spacing: 12

            AudioSettings {
                id: audioSection

                model: root.model
                navigationPanel: root.navigationPanel
                navigationOrderStart: root.navigationOrder
                showBitRateControl: true
            }

            // LKY fork: "Video resolution" and "Pages on screen" share one row -- the dialog has a
            // fixed content height, and a second label+dropdown row pushed the check boxes below
            // onto the footer buttons (real overlap, seen on 2026-11-01).
            RowLayout {
                width: parent.width
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: (parent.width - parent.spacing) / 2
                    spacing: 8

                    StyledTextLabel {
                        id: videoResolutionLabel
                        Layout.fillWidth: true
                        text: qsTrc("project/export", "Video resolution")
                        horizontalAlignment: Text.AlignLeft
                    }

                    StyledDropdown {
                        id: videoResolutionDropdown
                        Layout.fillWidth: true

                        navigation.name: "VideoResolutionDropdown"
                        navigation.panel: root.navigationPanel
                        navigation.row: audioSection.navigationOrderEnd + 1
                        navigation.accessible.name: videoResolutionLabel.text + " " + currentText

                        model: root.model ? root.model.availableVideoResolutions().map(function(res) {
                            return { text: res, value: res }
                        }) : []

                        currentIndex: root.model ? indexOfValue(root.model.videoResolution) : -1

                        onActivated: function(index, value) {
                            root.model.videoResolution = value
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: (parent.width - parent.spacing) / 2
                    spacing: 8

                    StyledTextLabel {
                        id: pagesPerScreenLabel
                        Layout.fillWidth: true
                        text: qsTrc("project/export", "Pages on screen")
                        horizontalAlignment: Text.AlignLeft
                    }

                    // LKY fork: several score pages in one frame (upstream showed exactly one)
                    StyledDropdown {
                        id: pagesPerScreenDropdown
                        Layout.fillWidth: true

                        navigation.name: "VideoPagesPerScreenDropdown"
                        navigation.panel: root.navigationPanel
                        navigation.row: videoResolutionDropdown.navigation.row + 1
                        navigation.accessible.name: pagesPerScreenLabel.text + " " + currentText

                        model: root.model ? root.model.availablePagesPerScreen() : []

                        currentIndex: root.model ? indexOfValue(root.model.videoPagesPerScreen) : -1

                        onActivated: function(index, value) {
                            root.model.videoPagesPerScreen = value
                        }
                    }
                }
            }

            RadioButtonGroup {
                id: viewModeGroup

                orientation: ListView.Vertical
                spacing: 8
                width: parent.width

                model: root.model ? root.model.availableViewModes().map(function(obj) {
                    return { text: obj.text, value: obj.value }
                }) : []

                delegate: RoundedRadioButton {
                    required property var modelData
                    required property int index

                    text: modelData.text
                    checked: root.model.viewMode === modelData.value
                    onToggled: root.model.viewMode = modelData.value

                    navigation.name: modelData.text
                    navigation.panel: root.navigationPanel
                    navigation.row: viewModeGroup.rowStart + index
                    navigation.accessible.name: modelData.text
                }

                // LKY fork: rows are numbered from here so the controls below stay stable
                // even when the number of view modes changes
                readonly property int rowStart: pagesPerScreenDropdown.navigation.row + 1
            }

            // LKY fork: the title card and the "Made with MuseScore" clip are optional.
            // While dropping one of them the matching silence in the audio track is dropped too,
            // so picture and sound stay in sync.
            // NOTE Both live in ONE row on purpose: the dialog's right column does not scroll
            // (contentHeight 420), and a second row pushed the footer checkbox under the buttons.
            RowLayout {
                width: parent.width
                spacing: 16

                CheckBox {
                    id: titlePageCheckBox
                    text: qsTrc("project/export", "Show title page")

                    navigation.name: "VideoTitlePageCheckBox"
                    navigation.panel: root.navigationPanel
                    navigation.row: viewModeGroup.rowStart + viewModeGroup.count

                    checked: root.model ? root.model.videoTitlePage : true
                    onClicked: {
                        root.model.videoTitlePage = !checked
                    }
                }

                CheckBox {
                    id: endLogoCheckBox
                    text: qsTrc("project/export", "Show end logo")

                    navigation.name: "VideoEndLogoCheckBox"
                    navigation.panel: root.navigationPanel
                    navigation.row: titlePageCheckBox.navigation.row + 1

                    checked: root.model ? root.model.videoEndLogo : true
                    onClicked: {
                        root.model.videoEndLogo = !checked
                    }
                }
            }

            StyledTextLabel {
                width: parent.width
                text: qsTrc("project/export", "Each selected part will be exported as a separate video file.")
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
            }
        }
    }
}
