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

#ifndef MU_APPSHELL_APPSHELLTYPES_H
#define MU_APPSHELL_APPSHELLTYPES_H

#include <QString>

namespace mu::appshell {
using DockName = QString;

// Panels:
static const DockName PALETTES_PANEL_NAME("palettesPanel");
static const DockName LAYOUT_PANEL_NAME("layoutPanel");
static const DockName PROPERTIES_PANEL_NAME("propertiesPanel");
static const DockName SELECTION_FILTERS_PANEL_NAME("selectionFiltersPanel");
static const DockName UNDO_HISTORY_PANEL_NAME("undoHistoryPanel");

static const DockName NOTATION_NAVIGATOR_PANEL_NAME("notationNavigatorPanel");
static const DockName NOTATION_BRAILLE_PANEL_NAME("notationBraillePanel");

static const DockName MIXER_PANEL_NAME("mixerPanel");
static const DockName PIANO_KEYBOARD_PANEL_NAME("pianoKeyboardPanel");
static const DockName TIMELINE_PANEL_NAME("timelinePanel");
static const DockName PERCUSSION_PANEL_NAME("percussionPanel");

//! The built-in Agent harness panel (`src/agentharness/`). Added by this fork: the panel existed on
//! the notation page from M0, but nothing could OPEN it - the View-menu entries are built from the
//! toggle actions below, which this panel was never registered in. It was reachable only through a
//! QML `visible:` binding that `DockBase::init()` overwrites, so in practice it was unreachable
//! (第 123 条). ⚠️ Defined ONCE here and read through `NotationPageModel::agentPanelName()` - a
//! second copy of the literal would drift and the failure would be a menu entry that does nothing.
static const DockName AGENT_PANEL_NAME("AgentHarnessPanel");

// Toolbars:
static const DockName NOTATION_TOOLBAR_NAME("notationToolBar");
static const DockName UNDO_REDO_TOOLBAR_NAME("undoRedoToolBar");
static const DockName NOTE_INPUT_BAR_NAME("noteInputBar");
static const DockName PLAYBACK_TOOLBAR_NAME("playbackToolBar");
static const DockName EXTENSIONS_TOOLBAR_NAME("extensionsToolBar");

// Other:
static const DockName NOTATION_STATUSBAR_NAME("notationStatusBar");

enum class StartupModeType
{
    StartEmpty,
    ContinueLastSession,
    StartWithNewScore,
    StartWithScore,
    Recovery
};
}

#endif // MU_APPSHELL_APPSHELLTYPES_H
