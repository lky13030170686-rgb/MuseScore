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
import QtQuick.Window

import Muse.Ui
import Muse.UiComponents

/*!
    The agent composer's message field: several lines, growing with what you type.

    \b Why not `TextInputField`. It wraps a one-line `TextField` (fixed 30px, no wrapping), so the
    second sentence of a request is invisible while you type it. That is not a cosmetic problem: the
    agent is asked for things that need a sentence or three ("把第 2 小节写成 C4 D4 E4 F4，用小调"), and
    a field that hides the end of the sentence is how a half-written instruction gets sent.

    \b Why not `TextInputArea` either, although it is multi-line and grows. Its Enter key is wired the
    other way round: `allowNewLineByEnter` defaults to true, so Enter inserts a line - and in a chat
    box Enter must SEND. The `false` branch cannot fix that: it answers ShortcutOverride with
    `accepted = false`, which only declines to override a *shortcut*; it does not stop the `TextArea`
    underneath from inserting the newline (that is the `KeyPress`, a different event). So this file
    owns its Enter key instead of depending on that switch. (维护手册.md §4.6)

    \b The keyboard contract - the web client's, which anybody who has used a chat box already knows:
      - `Enter` sends; it does NOT insert a line.
      - `Shift+Enter` inserts a line.
      - `Esc` gives up focus (and sends nothing).
      - The arrow keys move the caret. In a one-line field Up/Down may belong to the UI's navigation;
        in a multi-line field they belong to the text, so the `ShortcutOverrideModel` is asked for
        `All` direction keys.
      - `Ctrl+<key>` stays with the platform: copy/paste/select-all work as everywhere else.

    \b Why the height is clamped rather than free. Free growth in a dock panel would push the
    conversation - the thing being worked in - off the top of the panel, one line at a time. So the
    field grows from one line to `maximumHeight` and then scrolls inside itself, keeping the caret in
    view (the same rule the web client uses; there the ceiling is set in CSS).

    \b `currentText` is a live mirror of the inner `TextArea`, NOT a copy that the caller has to keep
    in sync. `TextInputField` needs a write-back (`onTextChanged: currentText = ...`) because it lets
    the caller own the property, and forgetting that line is a silent, UI-looks-fine failure: the box
    paints every character while the property stays empty, so Send never enables (维护手册.md §4.6,
    lesson 126). Here there is only one copy, so the mistake cannot be made.
*/
FocusScope {
    id: root

    //! The message. Read-only ON PURPOSE - see the note above; `clear()` is how it is emptied.
    //! ⚠️ There is deliberately NO `hasText` / `fieldActiveFocus` here even though `TextInputField` has
    //! them: this component's contract is what the composer actually uses. A property nothing reads is
    //! a property nobody keeps correct - and "the composer hides a hint while you type" was tried and
    //! rejected, because the composer then changes height under the pointer (第 129 条).
    readonly property string currentText: input.text

    property alias hint: input.placeholderText

    //! One line, and the ceiling where growing turns into scrolling.
    property int minimumHeight: 34
    property int maximumHeight: 132

    property real textSidePadding: 10
    property real textVerticalPadding: 6

    readonly property alias inputField: input
    readonly property alias accessible: navCtrl.accessible

    signal accepted()
    signal escaped()

    //! `currentText` is read-only, so the field is emptied through here - one way in, one way out.
    function clear() {
        input.text = ""
    }

    //! The other direction, and `clear()`'s counterpart: fill the field from OUTSIDE, without sending.
    //! The empty state's example questions use it (第 149 条) - a click puts a sentence in the box so
    //! the user can edit it, rather than spending a turn on their behalf. ⚠️ It goes through the same
    //! single copy of the text (`input.text`), so the "the box paints your typing while the property
    //! stays empty" failure (第 126 条) still has nowhere to happen here.
    function setText(value) {
        input.text = value
        input.cursorPosition = input.length
        root.ensureActiveFocus()
    }

    function selectAll() {
        input.selectAll()
    }

    function ensureActiveFocus() {
        if (!root.activeFocus) {
            root.forceActiveFocus()
        }
    }

    implicitWidth: parent ? parent.width : 0
    //! Grow with the text, between the two limits. `contentHeight` is what the text actually needs;
    //! `implicitHeight` is what the control reports - taking the larger of the two keeps this correct
    //! whether or not the padding is already counted in the first one.
    implicitHeight: Math.max(root.minimumHeight,
                             Math.min(root.maximumHeight,
                                      Math.max(input.implicitHeight,
                                               input.contentHeight + 2 * root.textVerticalPadding)))

    opacity: root.enabled ? 1.0 : ui.theme.itemOpacityDisabled

    FocusListener {
        item: root
    }

    //! Hover is tracked with a handler and not a `MouseArea`: an overlay would swallow the press that
    //! places the caret. `HoverHandler` watches without taking the mouse.
    HoverHandler {
        id: hoverHandler
    }

    NavigationControl {
        id: navCtrl
        name: root.objectName !== "" ? root.objectName : "AgentComposerField"
        enabled: root.enabled && root.visible

        accessible.role: MUAccessible.EditableText
        accessible.name: Boolean(input.text) ? input.text : input.placeholderText
        accessible.visualItem: root
        accessible.text: input.text
        accessible.selectedText: input.selectedText
        accessible.selectionStart: input.selectionStart
        accessible.selectionEnd: input.selectionEnd
        accessible.cursorPosition: input.cursorPosition

        onActiveChanged: {
            if (navCtrl.active) {
                root.ensureActiveFocus()
            }
        }

        onTriggered: root.ensureActiveFocus()
    }

    Rectangle {
        id: background

        anchors.fill: parent

        NavigationFocusBorder { navigationCtrl: navCtrl }

        color: ui.theme.textFieldColor
        border.color: input.activeFocus
                      ? ui.theme.accentColor
                      : (hoverHandler.hovered ? Utils.colorWithAlpha(ui.theme.accentColor, 0.6)
                                              : ui.theme.strokeColor)
        border.width: Math.max(ui.theme.borderWidth, 1)
        radius: 3
    }

    Flickable {
        id: flick

        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: width
        contentHeight: input.implicitHeight
        //! Only draggable once the text is longer than the box: while everything fits there is
        //! nothing to scroll, and claiming the drag would cost the ability to select text with it.
        interactive: contentHeight > height + 1

        //! TextField/TextArea style: the control is transparent inside a styled background. Wrapping
        //! is what makes "multi-line" mean more than "a very wide line".
        TextArea {
            id: input

            objectName: "AgentComposerTextArea"

            width: flick.width

            leftPadding: root.textSidePadding
            rightPadding: root.textSidePadding
            topPadding: root.textVerticalPadding
            bottomPadding: root.textVerticalPadding

            color: ui.theme.fontPrimaryColor
            font: ui.theme.bodyFont

            background: Item {}

            //! `activeFocusOnPress`: clicking the text puts the caret where it was clicked. This is why
            //! there is no overlay `MouseArea` here - the press belongs to the text editor.
            focus: false
            activeFocusOnPress: true
            selectByMouse: true
            selectionColor: Utils.colorWithAlpha(ui.theme.accentColor, ui.theme.accentOpacityNormal)
            selectedTextColor: ui.theme.fontPrimaryColor
            placeholderTextColor: Utils.colorWithAlpha(ui.theme.fontPrimaryColor, 0.3)

            text: ""
            wrapMode: TextInput.Wrap
            //! Up/Down move the caret, so in this field they are the TEXT's keys, not navigation's.
            ShortcutOverrideModel {
                id: shortcutOverrideModel
                directionKeysForOverride: ShortcutOverrideModel.All
            }

            Component.onCompleted: {
                shortcutOverrideModel.init()
            }

            Keys.onShortcutOverride: function(event) {
                //! Enter/Return/Escape are claimed first, so that no application shortcut can take
                //! them from the field: Enter is "send" here, and a global shortcut firing instead
                //! would send nothing while making the keystroke look ignored.
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Escape) {
                    event.accepted = true
                    return
                }

                //! Everything the shortcut register does not own belongs to the text (which is how
                //! typing a space does not start playback while a message is being written).
                event.accepted = shortcutOverrideModel.isShortcutOverrideAllowed(event.key, event.modifiers)
            }

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    root.escaped()
                    root.focus = false
                    return
                }

                const isEnter = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                if (!isEnter) {
                    return
                }

                //! Shift+Enter is left to the `TextArea`, which inserts the newline.
                //! 反向验证（弄坏必须变红）：把这一行换成 `if (true) { return }`，真实窗口里输入
                //! Hello 再按 Enter 就只换行、不发消息 —— 输入框里留下 Hello + 一个空行，对话里
                //! 不会多出这条消息（截图 build/v/ah2-red-enter.png），恢复后同一串步骤又变绿。
                if (event.modifiers & Qt.ShiftModifier) {
                    return
                }

                //! ⛔ `event.accepted = true` is the whole point: accepting the key in the attached
                //! handler is what stops the `TextArea` from also inserting a newline, so Enter sends
                //! a message that does not grow an empty line at its end.
                event.accepted = true
                root.accepted()
            }

            onActiveFocusChanged: {
                if (activeFocus) {
                    navCtrl.requestActive()
                }
            }

            //! Keep the caret visible while the text is longer than the box. Without this the field
            //! stops following what you type at exactly the moment it starts scrolling: the caret
            //! slides out of view and the text appears to stop accepting input.
            onCursorRectangleChanged: {
                if (!activeFocus) {
                    return
                }

                const cursorTop = input.cursorRectangle.y
                const cursorBottom = cursorTop + input.cursorRectangle.height
                const visibleTop = flick.contentY
                const visibleBottom = visibleTop + flick.height

                if (cursorBottom + root.textVerticalPadding > visibleBottom) {
                    flick.contentY = Math.max(0, cursorBottom + root.textVerticalPadding - flick.height)
                } else if (cursorTop - root.textVerticalPadding < visibleTop) {
                    flick.contentY = Math.max(0, cursorTop - root.textVerticalPadding)
                }
            }
        }
    }
}
