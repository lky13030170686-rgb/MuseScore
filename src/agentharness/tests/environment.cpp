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

#include "testing/environment.h"

#include "draw/drawmodule.h"
#include "engraving/engravingmodule.h"
#include "engraving/tests/utils/scorerw.h"

#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/mscore.h"

//! Same shape as the notationscene suite's environment: engraving needs its instrument templates and
//! its no-GUI flags before a score can be read, and `ScoreRW` needs to be told where this module's
//! test data lives (the `agentharness_tests_DATA_ROOT` define comes from SetupGTest).
static muse::testing::SuiteEnvironment agentharness_se
    = muse::testing::SuiteEnvironment()
      .setDependencyModules({ new muse::draw::DrawModule(), new mu::engraving::EngravingModule() })
      .setPostInit([]() {
    LOGI() << "agentharness tests suite post init";

    mu::engraving::ScoreRW::setRootPath(muse::String::fromUtf8(agentharness_tests_DATA_ROOT));

    mu::engraving::MScore::testMode = true;
    mu::engraving::MScore::testWriteStyleToScore = false;
    mu::engraving::MScore::noGui = true;

    mu::engraving::loadInstrumentTemplates(":/engraving/instruments/instruments.xml");
});
