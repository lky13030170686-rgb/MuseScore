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

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QtGlobal>

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

//! The velocity lane groups notes by staff - one band per staff - so every note has to carry a
//! staff index the roll can actually address, and the staff list the model builds must be able to
//! cover it. A note whose index fell outside that range would land in no band at all and could
//! never be edited.
TEST_F(MidiEditorNotesTests, EveryNoteCarriesAStaffIndexTheLaneCanAddress)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    const int staffCount = int(m_score->nstaves());
    ASSERT_GE(staffCount, 1);

    for (const MidiNoteItem& item : items) {
        EXPECT_GE(item.staffIndex, 0);
        EXPECT_LT(item.staffIndex, staffCount)
            << "a note outside the staff range would fall into no velocity band";
    }

    //! And a band exists for every staff the notes actually use - that is what the lane draws.
    std::vector<int> used;
    for (const MidiNoteItem& item : items) {
        used.push_back(item.staffIndex);
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());

    EXPECT_FALSE(used.empty());
    EXPECT_LE(int(used.size()), staffCount);
}

//! Every rectangle must land inside the score timeline.
TEST_F(MidiEditorNotesTests, EveryRectangleStartsInsideTheScore){
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
    EXPECT_FALSE(applyNotePlayOverride(m_score, nullptr, 0, 480, 100));
    EXPECT_TRUE(collectMidiNotes(nullptr).empty());
    EXPECT_TRUE(collectMidiMeasures(nullptr).empty());
}

//! The played layer: what the piano roll writes into `Note::playEvents()`.
TEST_F(MidiEditorNotesTests, PlayedTimingIsWrittenThroughThePlayEventsOfTheNote)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    Note* note = items.front().note;
    const int originalTick = items.front().tick;
    const int originalDuration = items.front().durationTicks;

    ASSERT_FALSE(items.front().hasPlayOverride) << "a fresh score has nothing to show as played";

    //! Play it a quarter of the note later and half as long.
    const int startTick = originalTick + originalDuration / 4;
    const int durationTicks = originalDuration / 2;

    EXPECT_TRUE(applyNotePlayOverride(m_score, note, startTick, durationTicks, 100));

    const MidiNoteItem* played = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(played, nullptr);

    EXPECT_TRUE(played->hasPlayOverride);
    EXPECT_EQ(played->tick, originalTick) << "the notated position must not move";
    EXPECT_EQ(played->durationTicks, originalDuration) << "the notated length must not move";
    EXPECT_EQ(played->playTick, startTick);
    EXPECT_EQ(played->playDurationTicks, durationTicks);
    EXPECT_EQ(played->playVelocityPercent, 100);

    EXPECT_FALSE(note->playEvents().empty());
    EXPECT_EQ(note->playEvents().front().ontime(), 250);
    EXPECT_EQ(note->playEvents().front().len(), 500);

    m_score->undoRedo(true, nullptr);

    const MidiNoteItem* undone = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(undone, nullptr);
    EXPECT_FALSE(undone->hasPlayOverride);
    EXPECT_EQ(undone->playTick, originalTick);
    EXPECT_EQ(undone->playDurationTicks, originalDuration);
}

TEST_F(MidiEditorNotesTests, TheVelocityMultiplierIsCarriedToo)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    Note* note = items.front().note;

    ASSERT_TRUE(applyNotePlayOverride(m_score, note, items.front().tick, items.front().durationTicks, 50));

    const MidiNoteItem* played = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(played, nullptr);
    EXPECT_TRUE(played->hasPlayOverride);
    EXPECT_EQ(played->playVelocityPercent, 50);
}

/*!
 * "Keep the dynamics in charge, but let me tweak one note."
 *
 * Setting a velocity gives the note its own one (which overrides the dynamic marks in playback);
 * setting it back to 0 hands the note to the dynamics again. The second half is what makes the tweak
 * undoable - and the roll shows which of the two states a note is in.
 */
TEST_F(MidiEditorNotesTests, AnOwnVelocityCanBeSetAndClearedAgain)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    Note* note = items.front().note;

    ASSERT_FALSE(items.front().hasVelocityOverride) << "a fresh note must follow the dynamics";

    ASSERT_TRUE(applyNoteVelocity(m_score, note, 100));

    const MidiNoteItem* own = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(own, nullptr);
    EXPECT_TRUE(own->hasVelocityOverride) << "the note should no longer follow the dynamics";
    EXPECT_EQ(own->velocity, 100);

    //! And back to following the dynamics.
    ASSERT_TRUE(applyNoteVelocity(m_score, note, 0));

    const MidiNoteItem* freed = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(freed, nullptr);
    EXPECT_FALSE(freed->hasVelocityOverride) << "clearing must hand the note back to the dynamics";
    EXPECT_EQ(freed->velocity, 64) << "an unset velocity is still SHOWN as 64";
    EXPECT_EQ(note->userVelocity(), 0);

    //! Undo has to bring the tweak back.
    m_score->undoRedo(true, nullptr);

    const MidiNoteItem* undone = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(undone, nullptr);
    EXPECT_TRUE(undone->hasVelocityOverride);
    EXPECT_EQ(undone->velocity, 100);
}

TEST_F(MidiEditorNotesTests, ClearingAnAlreadyClearVelocityChangesNothing)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    EXPECT_FALSE(applyNoteVelocity(m_score, items.front().note, 0));
}

//! Writing the neutral values again is "no change", not an edit - this is what keeps an untouched
//! score from silently gaining an override.
TEST_F(MidiEditorNotesTests, WritingTheNeutralOverrideChangesNothing)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    Note* note = items.front().note;

    EXPECT_FALSE(applyNotePlayOverride(m_score, note,
                                       items.front().tick, items.front().durationTicks, 100));

    const MidiNoteItem* played = findItem(collectMidiNotes(m_score), note);
    ASSERT_NE(played, nullptr);
    EXPECT_FALSE(played->hasPlayOverride);
}

//! The played layer has to survive saving - otherwise the piano roll would silently lose the user's
//! work, and the values would never reach the MIDI export either.
//!
//! Set MUSE_MIDIEDITOR_KEEP_SCORE to keep the saved score, so the real application can be pointed at
//! it: that is how the played-vs-notated drawing is looked at end to end (same idea as
//! MUSE_AUDIOTRACK_KEEP_SCORE in the audio track tests).
TEST_F(MidiEditorNotesTests, PlayedTimingSurvivesSaveAndReload)
{
    const bool keepScore = qEnvironmentVariableIsSet("MUSE_MIDIEDITOR_KEEP_SCORE");
    const QString savedPath = keepScore
                              ? QDir::tempPath() + QStringLiteral("/dsh-midieditor-played.mscx")
                              : QDir::tempPath() + QStringLiteral("/dsh-midieditor-played-roundtrip.mscx");
    QFile::remove(savedPath);

    int touched = 0;
    int ownVelocity = 0;

    {
        MasterScore* score = ScoreRW::readScore(TEST_SCORE_PATH);
        ASSERT_TRUE(score);

        //! Give every note a played timing that is unmistakably different from the notated one:
        //! a quarter of the note later, half as long, and every other one quieter.
        //!
        //! Notes of EVEN-numbered staves additionally get their own velocity, so a score kept with
        //! MUSE_MIDIEDITOR_KEEP_SCORE shows every state the velocity lane can be in at once:
        //! one band per staff, faint bars that follow the dynamics, and solid ones that do not.
        for (const MidiNoteItem& item : collectMidiNotes(score)) {
            const int start = item.tick + item.durationTicks / 4;
            const int duration = std::max(1, item.durationTicks / 2);
            if (applyNotePlayOverride(score, item.note, start, duration, touched % 2 == 0 ? 50 : 100)) {
                ++touched;
            }

            if (item.staffIndex % 2 == 0 && applyNoteVelocity(score, item.note, 40)) {
                ++ownVelocity;
            }
        }

        EXPECT_GT(touched, 0) << "no note accepted a played override";
        EXPECT_GT(ownVelocity, 0) << "no note accepted an own velocity";

        ASSERT_TRUE(ScoreRW::saveScore(score, savedPath)) << "could not save the score";
        delete score;
    }

    ASSERT_TRUE(QFile::exists(savedPath)) << "the saved score is not on disk";

    MasterScore* reloaded = ScoreRW::readScore(savedPath, /*isAbsolutePath*/ true);
    ASSERT_TRUE(reloaded) << "could not reopen the saved score";

    const std::vector<MidiNoteItem> again = collectMidiNotes(reloaded);
    ASSERT_FALSE(again.empty());

    int withOverride = 0;
    int withQuieter = 0;
    int withOwnVelocity = 0;
    for (const MidiNoteItem& item : again) {
        if (item.hasPlayOverride) {
            ++withOverride;
        }
        if (item.playVelocityPercent != 100) {
            ++withQuieter;
        }
        if (item.hasVelocityOverride) {
            ++withOwnVelocity;
        }
    }

    EXPECT_EQ(withOverride, touched) << "the played timing was lost on save/reload";
    EXPECT_GT(withQuieter, 0) << "the velocity multiplier was lost on save/reload";
    EXPECT_EQ(withOwnVelocity, ownVelocity) << "an own velocity was lost on save/reload";

    delete reloaded;
}
