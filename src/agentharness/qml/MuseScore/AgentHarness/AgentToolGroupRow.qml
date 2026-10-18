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
    One folded run of tool calls: a single line that says what the agent used, and the rows themselves
    when the user asks for them.

    \b Why a folded row exists at all. Measured on a real turn (2026-10-11): the model read the score with
    FOUR tools, and the conversation showed it as EIGHT rows between the question and the answer - and the
    step ceiling is 8, so the worst case is sixteen. The answer, which is the only part the user asked for,
    ends up below the fold. The DSH web client folds a run of tool calls into one line for exactly this
    reason, and the fold itself lives in C++ (`foldToolRows`) so that "nothing was reordered, nothing was
    dropped" is a unit test rather than something somebody has to notice.

    \b Why the header is a real button. It is the only control inside a scrolling transcript, and both
    surfaces must stay usable while the agent works. A hand-drawn toggle would not be in the accessibility
    tree, so the panel could not be driven or verified from a script (维护手册.md §7.6); a `FlatButton`
    with an `accessible.name` can.

    \b ⛔ An error is never hidden. `hasError` opens the group by DEFAULT (the view passes it in through
    `expanded`), because a failed tool call is the explanation for everything that follows it: "the agent
    tried something and it did not work" must not be one line the user has to think to click. The header
    also says how many failed, so a collapsed group still tells the truth.
*/
Item {
    id: root

    //! The folded row, exactly as `foldToolRows` produced it: `callCount`, `resultCount`, `hasError`,
    //! `names`, `items`.
    required property var group

    //! Whether the rows are shown. The VIEW owns this state (it survives the model being rebuilt on every
    //! streamed token) and hands it down; the row only asks for a change.
    property bool expanded: false

    //! The two surfaces render rows differently (see AgentTranscriptRow), so the folded row has to know
    //! which one it is in - otherwise the panel would draw web-style tool lines inside a folded group.
    property bool webStyle: false

    signal toggleRequested()

    readonly property int callCount: root.group ? Number(root.group.callCount) || 0 : 0
    readonly property int resultCount: root.group ? Number(root.group.resultCount) || 0 : 0
    readonly property bool hasError: root.group ? root.group.hasError === true : false
    readonly property string names: root.group && root.group.names ? String(root.group.names) : ""
    readonly property var items: root.group && root.group.items ? root.group.items : []

    //! How many results came back as errors - so a collapsed group can still say that something failed.
    readonly property int errorCount: {
        let n = 0
        for (let i = 0; i < root.items.length; ++i) {
            if (root.items[i] && root.items[i].isError === true) {
                ++n
            }
        }
        return n
    }

    //! "2 tool calls · score_overview, score_window". ⚠️ A run can also be results-only (a result whose
    //! call was recorded in an earlier message), and then saying "0 tool calls" would be a lie about what
    //! the user is looking at.
    readonly property string headerText: {
        const noun = root.callCount > 0
                     ? qsTrc("agentharness", "%n tool call(s)", "", root.callCount)
                     : qsTrc("agentharness", "%n tool result(s)", "", root.resultCount)
        return root.names.length > 0 ? noun + " · " + root.names : noun
    }

    implicitHeight: content.implicitHeight

    Column {
        id: content

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 2

        FlatButton {
            id: header

            width: parent.width
            accessible.name: root.expanded
                             ? qsTrc("agentharness", "Hide the details of %1").arg(root.headerText)
                             : qsTrc("agentharness", "Show the details of %1").arg(root.headerText)
            toolTipTitle: qsTrc("agentharness", "Tool calls the agent made here")

            onClicked: {
                root.toggleRequested()
            }

            //! A custom content item because `FlatButton` centres its label, and this line is a list
            //! header: it reads left-aligned, in the same column as the rows it stands for. ⚠️ The arrow
            //! is the state, so it is not decoration - an unlabelled twisty is how a folded row becomes
            //! invisible furniture.
            contentItem: Component {
                RowLayout {
                    spacing: 6

                    StyledIconLabel {
                        Layout.alignment: Qt.AlignVCenter
                        iconCode: root.expanded ? IconCode.SMALL_ARROW_DOWN : IconCode.SMALL_ARROW_RIGHT
                        color: root.hasError ? ui.theme.buttonColor : ui.theme.fontSecondaryColor
                    }

                    StyledTextLabel {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        color: root.hasError ? ui.theme.buttonColor : ui.theme.fontSecondaryColor
                        text: root.hasError
                              ? root.headerText + " · " + qsTrc("agentharness", "%n failed", "", root.errorCount)
                              : root.headerText
                    }
                }
            }
        }

        //! The rows themselves, exactly as the transcript would have shown them unfolded - the fold is
        //! lossless, so expanding gives back the original conversation, in order.
        Column {
            id: details

            visible: root.expanded
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            spacing: 2

            Repeater {
                model: root.items

                delegate: AgentTranscriptRow {
                    required property var modelData

                    width: details.width
                    kind: modelData.kind
                    roleLabel: modelData.role
                    body: modelData.text
                    //! Carried through the fold untouched (第 150 条): `foldToolRows` passes unknown fields
                    //! on, so an expanded group draws its rows exactly as the unfolded transcript would -
                    //! which is the whole claim the fold makes about itself.
                    richBody: modelData.rich || ""
                    isError: modelData.isError === true
                    webStyle: root.webStyle
                }
            }
        }
    }
}
