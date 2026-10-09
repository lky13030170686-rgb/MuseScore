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

#pragma once

#include <QObject>
#include <qqmlintegration.h>

namespace muse::agentharness {
//! Read-only switches the Agent UI reads from the environment.
//!
//! WHY THIS TYPE EXISTS AT ALL: QML cannot read environment variables, and the Agent panel is
//! hidden by default - so there would be no way to put it on screen for a screenshot without
//! either shipping it visible or hardcoding a build-time flag. This project already answered that
//! question on the MIDI page with the `MUSE_MIDIEDITOR_DEMO_*` family: an environment switch that
//! needs no rebuild to toggle, which is what makes reverse verification cheap enough that it
//! actually gets done (`维护手册.md` §6.2).
//!
//! Deliberately NOT `Contextable`: it has no injections, and a QML singleton with a context would
//! not resolve (a singleton has no QML parent to walk up from). Being context-free is what lets it
//! be registered as a singleton and read from any QML file.
class PanelConfig : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    //! True when MUSE_AGENT_PANEL is set (any value, including "0" - the presence of the variable
    //! is the switch, matching how MUSE_AGENT_FIELD_TRACE and the MIDI demo hooks behave).
    Q_PROPERTY(bool forceOpen READ forceOpen CONSTANT)

public:
    explicit PanelConfig(QObject* parent = nullptr) : QObject(parent) {}

    bool forceOpen() const;
};
} // namespace muse::agentharness
