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

// The audio track is added through the ordinary instrument flow: it is declared in
// instruments.xml as an instrument whose staff type is the waveform lane. These tests pin
// the parts of that declaration which are easy to get silently wrong:
//
//  * the instrument must exist and be findable by id, or it never appears in the instrument
//    dialog and the user has no way to add it;
//  * its staff type preset must resolve to WAVEFORM rather than fall back to a normal staff,
//    which is what would happen if the preset name or the staff group did not match;
//  * it must be in the "common" genre, because that is the genre the instrument dialog
//    opens on -- an instrument in no genre is present in the file but invisible in the UI.
//
// A wrong staff type here is invisible in the XML and only shows up as a five-line staff
// with no waveform in the running application.

#include <gtest/gtest.h>

#include <string>

#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafflabel.h"
#include "engraving/dom/stafftype.h"
#include "engraving/types/types.h"

#include "utils/scorerw.h"

using namespace mu::engraving;
using namespace muse;

namespace {
constexpr const char* AUDIO_TRACK_ID = "audio-track";
}

namespace mu::audiotrack::tests {
// The instrument exists and is reachable by the id the instrument dialog looks it up with.
TEST(AudioTrackInstrumentTests, IsDeclaredAndFindable)
{
    const InstrumentTemplate* templ = searchTemplate(String::fromAscii(AUDIO_TRACK_ID));
    ASSERT_TRUE(templ) << "no instrument with id \"" << AUDIO_TRACK_ID
                       << "\" in instruments.xml: it cannot be added from the instrument dialog";

    EXPECT_FALSE(templ->trackName.empty()) << "the instrument has no track name";
    EXPECT_FALSE(templ->instrumentName.longName().empty())
        << "the instrument has no long name to show in the list";
    EXPECT_EQ(templ->staffCount, 1u) << "the audio lane must be a single staff";
}

// The whole feature hinges on this: the instrument's staff must be the waveform lane.
TEST(AudioTrackInstrumentTests, UsesTheWaveformStaffType)
{
    const InstrumentTemplate* templ = searchTemplate(String::fromAscii(AUDIO_TRACK_ID));
    ASSERT_TRUE(templ);

    ASSERT_TRUE(templ->staffTypePreset)
        << "staffTypePreset did not resolve; the lane would be created as a normal staff";

    EXPECT_TRUE(templ->staffTypePreset->isWaveformStaff())
        << "the instrument resolves to staff type \""
        << templ->staffTypePreset->xmlName().toStdString()
        << "\" instead of the waveform lane";
}

// The dialog opens on the Common genre, so an instrument without it is effectively hidden.
TEST(AudioTrackInstrumentTests, IsVisibleInTheDefaultGenre)
{
    const InstrumentTemplate* templ = searchTemplate(String::fromAscii(AUDIO_TRACK_ID));
    ASSERT_TRUE(templ);

    bool inCommonGenre = false;
    for (const InstrumentGenre* genre : templ->genres) {
        if (genre && genre->id == u"common") {
            inCommonGenre = true;
            break;
        }
    }
    EXPECT_TRUE(inCommonGenre)
        << "the audio track is not in the \"common\" genre, so the instrument dialog will not "
           "list it until the user changes the genre filter";
}

// The lane must not advertise itself as a pitched instrument with a usable range: it holds no
// notes, and a wrong range would let the user try to write music into it.
TEST(AudioTrackInstrumentTests, IsNotAUsableNotationStaff)
{
    const InstrumentTemplate* templ = searchTemplate(String::fromAscii(AUDIO_TRACK_ID));
    ASSERT_TRUE(templ);
    ASSERT_TRUE(templ->staffTypePreset);

    // The waveform staff type draws the audio instead of notation, so engraving must never
    // generate clefs, key signatures or time signatures on it.
    const StaffType* st = templ->staffTypePreset;
    EXPECT_FALSE(st->genClef()) << "a clef would be drawn on the audio lane";
    EXPECT_FALSE(st->genKeysig()) << "a key signature would be drawn on the audio lane";
    EXPECT_FALSE(st->genTimesig()) << "a time signature would be drawn on the audio lane";
}

// Adding the instrument has to actually produce a waveform staff, not just declare one.
//
// This exercises the path a plugin takes (Score::appendPart(instrumentId)), which is the one
// route into the score that builds a part straight from an InstrumentTemplate. The instrument
// dialog goes through notation instead and honours the preset there, so a difference between
// the two would mean the same instrument produces a waveform lane in one case and an ordinary
// five-line staff in the other -- with no error either way.
TEST(AudioTrackInstrumentTests, AddingTheInstrumentCreatesAWaveformStaff)
{
    const InstrumentTemplate* templ = searchTemplate(String::fromAscii(AUDIO_TRACK_ID));
    ASSERT_TRUE(templ);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/plain-staff.mscx");
    ASSERT_TRUE(score);

    const size_t stavesBefore = score->nstaves();
    ASSERT_EQ(stavesBefore, 1u) << "the fixture is expected to start with one staff";

    score->appendPart(templ);
    score->doLayout();

    ASSERT_EQ(score->nstaves(), stavesBefore + 1) << "the audio track did not add a staff";

    const Staff* added = score->staff(stavesBefore);
    ASSERT_TRUE(added);
    EXPECT_TRUE(added->isWaveformStaff(Fraction(0, 1)))
        << "adding the audio track instrument produced a \"" << added->staffType(Fraction(0, 1))->xmlName().toStdString()
        << "\" staff instead of the waveform lane";

    delete score;
}
} // namespace mu::audiotrack::tests
