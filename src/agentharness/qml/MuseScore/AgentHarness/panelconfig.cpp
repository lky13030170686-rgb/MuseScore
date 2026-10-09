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
#include "panelconfig.h"

#include <QtGlobal>

using namespace muse::agentharness;

bool PanelConfig::forceOpen() const
{
    //! `qEnvironmentVariableIsSet` rather than a value test: every other switch in this project
    //! (`MUSE_MIDIEDITOR_DEMO_*`, `MUSE_AGENT_FIELD_TRACE`) treats presence as "on", and a switch
    //! whose meaning depends on the value is one more thing to get wrong when verifying.
    static const bool forced = qEnvironmentVariableIsSet("MUSE_AGENT_PANEL");
    return forced;
}
