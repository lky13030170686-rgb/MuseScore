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
#include <gtest/gtest.h>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/tests/utils/scorerw.h"

#include "notationscene/qml/MuseScore/NotationScene/midieditor/midieditornotes.h"

using namespace mu;
using namespace mu::engraving;
using namespace mu::notation;

static const String TEST_SCORE_PATH(u"data/test.mscx");

class MidiEditorNotesTests : public ::testing::Test
{
public:
    void SetUp() override
    {
        m_score = ScoreRW::readScore(TEST_SCORE_PATH);
        ASSERT_TRUE(m_score);
    }

    void TearDown() override
    {
        delete m_score;
    }

    static const MidiNoteItem* findItem(const std::vector<MidiNoteItem>& items, const Note* note)
    {
        for (const MidiNoteItem& item : items) {
            if (item.note == note) {
                return &item;
            }
        }

        return nullptr;
    }

    MasterScore* m_score = nullptr;
};

//! The roll must describe exactly the notes the score has - this is what "the MIDI page and the
//! notation page share one data source" means, and it is what a copy-based implementation breaks.
TEST_F(MidiEditorNotesTests, DescribesTheVeryNotesTheScoreHas)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    for (const MidiNoteItem& item : items) {
        ASSERT_NE(item.note, nullptr);

        EXPECT_EQ(item.pitch, item.note->pitch());
        EXPECT_EQ(item.tick, item.note->tick().ticks());
        EXPECT_EQ(item.staffIndex, int(item.note->staffIdx()));
        EXPECT_EQ(item.voice, int(item.note->voice()));

        EXPECT_GE(item.tick, 0);
        EXPECT_GT(item.durationTicks, 0);
        EXPECT_GE(item.pitch, 0);
        EXPECT_LE(item.pitch, 127);
        EXPECT_GE(item.velocity, 1);
        EXPECT_LE(item.velocity, 127);
    }
}

//! Every rectangle must land inside the score timeline.
TEST_F(MidiEditorNotesTests, EveryRectangleStartsInsideTheScore)
{
    const std::vector<MidiMeasureItem> measures = collectMidiMeasures(m_score);
    ASSERT_FALSE(measures.empty());

    for (const MidiNoteItem& item : collectMidiNotes(m_score)) {
        EXPECT_GE(item.tick, measures.front().tick);
        EXPECT_LT(item.tick, measures.back().endTick);
    }
}

TEST_F(MidiEditorNotesTests, MeasuresFollowEachOtherWithoutGaps)
{
    const std::vector<MidiMeasureItem> measures = collectMidiMeasures(m_score);
    ASSERT_FALSE(measures.empty());

    for (size_t i = 0; i + 1 < measures.size(); ++i) {
        EXPECT_EQ(measures[i].endTick, measures[i + 1].tick) << "at measure " << i;
        EXPECT_GT(measures[i].endTick, measures[i].tick);
    }
}

TEST_F(MidiEditorNotesTests, PitchEditReachesTheScoreAndUndoRestoresIt)
{
    const std::vector<MidiNoteItem> before = collectMidiNotes(m_score);
    ASSERT_FALSE(before.empty());

    Note* note = before.front().note;
    const int originalPitch = note->pitch();
    const int targetPitch = originalPitch + 3;

    EXPECT_TRUE(applyNotePitch(m_score, note, targetPitch));
    EXPECT_EQ(note->pitch(), targetPitch);

    //! Re-flattening (what the roll does when it repaints) must already show the new pitch:
    //! it reads the score, it does not keep a private copy.
    const MidiNoteItem* after = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->pitch, targetPitch);

    m_score->undoRedo(true, nullptr);

    EXPECT_EQ(note->pitch(), originalPitch);
    const MidiNoteItem* undone = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(undone, nullptr);
    EXPECT_EQ(undone->pitch, originalPitch);
}

TEST_F(MidiEditorNotesTests, VelocityEditReachesTheScoreAndUndoRestoresIt)
{
    const std::vector<MidiNoteItem> before = collectMidiNotes(m_score);
    ASSERT_FALSE(before.empty());

    Note* note = before.front().note;
    const int originalVelocity = note->userVelocity();

    EXPECT_TRUE(applyNoteVelocity(m_score, note, 111));
    EXPECT_EQ(note->userVelocity(), 111);

    const MidiNoteItem* after = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->velocity, 111);

    m_score->undoRedo(true, nullptr);

    EXPECT_EQ(note->userVelocity(), originalVelocity);
}

//! An unset velocity (0) is shown as 64, so that the lane does not look empty - same convention as
//! the Velocity field of the Properties panel.
TEST_F(MidiEditorNotesTests, ShowsAnUnsetVelocityAsTheDefault)
{
    EXPECT_EQ(64, midiDisplayVelocity(0));
    EXPECT_EQ(1, midiDisplayVelocity(1));
    EXPECT_EQ(127, midiDisplayVelocity(127));
}

TEST_F(MidiEditorNotesTests, PitchIsClampedIntoTheMidiRange)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    Note* note = items.front().note;

    EXPECT_TRUE(applyNotePitch(m_score, note, 999));
    EXPECT_EQ(note->pitch(), 127);

    EXPECT_TRUE(applyNotePitch(m_score, note, -5));
    EXPECT_EQ(note->pitch(), 0);
}

TEST_F(MidiEditorNotesTests, NothingIsCommittedWhenNothingChanges)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    Note* note = items.front().note;

    EXPECT_FALSE(applyNotePitch(m_score, note, note->pitch()));

    //! NOTE: velocity 0 means "unset" and is outside the editable range, so first give it a real
    //!       one and then repeat it - that is the "no change" case.
    ASSERT_TRUE(applyNoteVelocity(m_score, note, 100));
    EXPECT_FALSE(applyNoteVelocity(m_score, note, 100));
    EXPECT_EQ(note->userVelocity(), 100);
}

TEST_F(MidiEditorNotesTests, ApplyingToNothingIsHarmless)
{
    EXPECT_FALSE(applyNotePitch(nullptr, nullptr, 60));
    EXPECT_FALSE(applyNoteVelocity(m_score, nullptr, 64));
    EXPECT_TRUE(collectMidiNotes(nullptr).empty());
    EXPECT_TRUE(collectMidiMeasures(nullptr).empty());
}
