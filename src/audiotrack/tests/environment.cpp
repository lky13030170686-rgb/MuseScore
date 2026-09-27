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

#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/mscore.h"

#include "mocks/engravingconfigurationmock.h"

#include "utils/scorerw.h"

#include "log.h"

// waveformlayout_tests.cpp reads a real score and lays it out, so the engraving module has
// to be initialised exactly as it is for the engraving test suite. Everything else in this
// suite only needs libsndfile and the audio engine types.
static const mu::engraving::IEngravingConfiguration::DebuggingOptions debugOpt {};

static muse::testing::SuiteEnvironment audiotrack_se(
{
    new muse::draw::DrawModule(),
    new mu::engraving::EngravingModule()
},
    nullptr,
    []() {
    // ScoreRW resolves relative names against this root. audiotrack's own data directory
    // holds only audio fixtures, so the score used by waveformlayout_tests.cpp lives under
    // an "engraving-data" subtree that mirrors what the engraving suite provides.
    mu::engraving::ScoreRW::setRootPath(muse::String::fromUtf8(audiotrack_tests_DATA_ROOT));
    LOGI() << "audiotrack tests: score root = " << mu::engraving::ScoreRW::rootPath();

    mu::engraving::MScore::testMode = true;
    mu::engraving::MScore::noGui = true;

    mu::engraving::loadInstrumentTemplates(":/engraving/instruments/instruments.xml");

    using ECMock = ::testing::NiceMock<mu::engraving::EngravingConfigurationMock>;

    std::shared_ptr<ECMock> configurator(new ECMock(), [](ECMock*) {}); // no delete
    ON_CALL(*configurator, defaultColor()).WillByDefault(::testing::Return(muse::draw::Color::BLACK));
    ON_CALL(*configurator, displayedDefaultColor(::testing::_)).WillByDefault(::testing::Return(muse::draw::Color::BLACK));
    ON_CALL(*configurator, debuggingOptions()).WillByDefault(::testing::ReturnRef(debugOpt));
    ON_CALL(*configurator, allowReadingImagesFromOutsideMscz()).WillByDefault(::testing::Return(true));

    muse::modularity::globalIoc()->unregister<mu::engraving::IEngravingConfiguration>("utests");
    muse::modularity::globalIoc()->registerExport<mu::engraving::IEngravingConfiguration>("utests", configurator);
},

    []() {
    std::shared_ptr<mu::engraving::IEngravingConfiguration> mock
        = muse::modularity::globalIoc()->resolve<mu::engraving::IEngravingConfiguration>("utests");
    muse::modularity::globalIoc()->unregister<mu::engraving::IEngravingConfiguration>("utests");

    //! HACK (same as the engraving suite): some score objects keep a live pointer to the
    //! mock, so it is deleted manually to silence the "not deleted" report.
    mu::engraving::IEngravingConfiguration* ecptr = mock.get();
    delete ecptr;
}
    );
