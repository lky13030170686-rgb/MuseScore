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
    The newest tool call, in full: which tool, with which arguments, and what came back.

    \b Why this exists next to the conversation. The conversation shows a tool call as ONE elided line -
    the right shape for reading a dialogue, the wrong shape for answering "what exactly did it just
    ask for". The two are not duplicates: the row is the narrative, this is the record.

    \b Why it reads the transcript instead of keeping its own copy. The transcript IS the session log's
    projection (`agentTranscript()`), so the arguments and results shown here are the same bytes the
    model saw. A second store would be free to disagree with the log, and the disagreement would only
    be visible to whoever compared them.

    \b Why it says "no tool call yet" rather than showing empty fields. An empty box looks like a broken
    panel; a sentence says the agent has not done anything, which is a different (and true) statement.
*/
Item {
    id: root

    property var field

    //! The newest call and the newest result, found by scanning the transcript backwards. They are
    //! reported separately on purpose: a call whose result has not arrived yet is a real state (the
    //! agent is waiting), and pairing them by position would show the PREVIOUS result while thinking.
    readonly property var lastCall: {
        const entries = root.field ? root.field.agentTranscript : []
        for (let i = entries.length - 1; i >= 0; --i) {
            if (entries[i].kind === "toolCall") {
                return entries[i]
            }
        }
        return null
    }

    readonly property var lastResult: {
        const entries = root.field ? root.field.agentTranscript : []
        for (let i = entries.length - 1; i >= 0; --i) {
            if (entries[i].kind === "toolResult") {
                return entries[i]
            }
        }
        return null
    }

    //! The tool's NAME, out of the call's text. The transcript stores a call as "<name> <arguments>"
    //! (see `FieldController::agentTranscript`), and both the call and its result carry the role "tool" -
    //! so the role is not the name, and a header that showed "tool" would name the category, not the tool.
    readonly property string lastCallName: {
        if (!root.lastCall) {
            return ""
        }
        const text = String(root.lastCall.text)
        const cut = text.indexOf(" ")
        return cut > 0 ? text.substring(0, cut) : text
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            StyledTextLabel {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
                text: root.lastCall
                      ? root.lastCallName
                      : qsTrc("agentharness", "No tool call yet")
            }

            //! Straight from the field's own verdict (`lastToolOk`), not parsed out of the text: the
            //! tool table decides success, and a badge that decided for itself would be a second judge.
            StyledTextLabel {
                visible: root.lastResult !== null
                color: (root.field && root.field.lastToolOk()) ? ui.theme.fontSecondaryColor : ui.theme.buttonColor
                text: (root.field && root.field.lastToolOk())
                      ? qsTrc("agentharness", "OK")
                      : qsTrc("agentharness", "FAILED")
            }
        }

        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            color: ui.theme.fontSecondaryColor
            text: qsTrc("agentharness", "Arguments")
        }

        Flickable {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(90, argumentsText.implicitHeight)
            contentWidth: width
            contentHeight: argumentsText.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Text {
                id: argumentsText

                width: parent.width
                wrapMode: Text.WordWrap
                font: ui.theme.bodyFont
                color: ui.theme.fontPrimaryColor
                text: root.lastCall ? root.lastCall.text : ""
            }
        }

        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            color: ui.theme.fontSecondaryColor
            text: qsTrc("agentharness", "Result")
        }

        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            contentWidth: width
            contentHeight: resultText.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Text {
                id: resultText

                width: parent.width
                wrapMode: Text.WordWrap
                font: ui.theme.bodyFont
                color: root.lastResult && root.field && !root.field.lastToolOk()
                       ? ui.theme.buttonColor
                       : ui.theme.fontPrimaryColor
                text: root.lastResult ? root.lastResult.text : ""
            }
        }
    }
}
