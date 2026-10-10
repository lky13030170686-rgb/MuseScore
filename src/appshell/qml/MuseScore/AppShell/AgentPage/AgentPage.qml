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

//! NOTE: The "Agent" page - a sibling of Home / Score / MIDI / Publish (see MainToolBarModel).
//! This file is the page SHELL (`AgentPage`): it declares the page's identity and hands the shared
//! information field to the workspace that does the work (`AgentWorkbench`, in the agent harness
//! module). The split follows MidiEditorPage + MidiEditorView: pages live in appshell, content lives
//! with its feature.
//!
//! It is the agent harness's own workspace: the same information field the notation page's Agent panel
//! shows, with the room a full page has - the conversation in the middle, the score facts and tool
//! table on the left, the timeline and the newest tool call on the right. The panel stays where it is;
//! the two are windows onto ONE field, which is why the field is created at window level
//! (WindowContent.qml) and handed to both.
//!
//! ⚠️ Three places have to agree for this page to exist at all, and none of them is a compile error:
//! the URI is registered in `AppShellModule::resolveImports` (`musescore://agent`), the page is mounted
//! in `WindowContent.qml`, and the tab is built in `MainToolBarModel::load`. Registering the URI
//! without the tab gives a page nobody can reach - the failure this project already paid for once
//! (维护手册.md §4.9, and the Agent panel's own missing entry).

pragma ComponentBehavior: Bound

import QtQuick

import Muse.Ui
import Muse.UiComponents
import Muse.Dock

import MuseScore.AppShell
import MuseScore.AgentHarness

DockPage {
    id: root

    objectName: "Agent"
    uri: "musescore://agent"

    required property NavigationSection topToolbarKeyNavSec

    //! The information field, created at window level and shared with the notation page's panel. Passed
    //! in rather than created here for the same reason the panel does it: a page is instantiated and
    //! destroyed by the dock on navigation, and a field that comes and goes would record the score only
    //! while the user happens to be looking at it.
    property var agentField

    central: AgentWorkbench {
        field: root.agentField
    }
}
