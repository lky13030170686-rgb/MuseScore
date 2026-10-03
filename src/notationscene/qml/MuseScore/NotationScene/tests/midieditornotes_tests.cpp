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
#include <utility>
#include <vector>

#include <QDir>
#include <QFile>
#include <QtGlobal>

#include "engraving/automation/automationdata.h"
#include "engraving/automation/automationtypes.h"
#include "engraving/automation/internal/automationrw.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tempotimeline.h"
#include "engraving/playback/playbackcontext.h"
#include "engraving/tests/utils/scorerw.h"

#include "mpe/automationpoint.h"

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

//! The key the NOTATION page addresses the Dynamics curve of a staff with: written out here the way
//! NotationAutomationController::curveKeyFor does it, independently of the code under test. If the piano
//! roll ever drifted to a key of its own, the two pages would quietly edit two different curves - and
//! this is what would catch it.
static AutomationCurveKey notationPageKey(const MasterScore* score, size_t staffIdx)
{
    return AutomationCurveKey::staff(AutomationType::Dynamics, score->staff(staffIdx)->id());
}

static const MidiAutomationPoint* findAutomationPoint(const std::vector<MidiAutomationPoint>& points, int tick)
{
    for (const MidiAutomationPoint& point : points) {
        if (point.tick == tick) {
            return &point;
        }
    }

    return nullptr;
}

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

//! A brush stroke has to be ONE undo step, and that is not a nicety.
//!
//! Every startCmd/endCmd pair notifies the whole score, and the subscribers to that notification
//! rebuild things that cost O(score) - the notation view repaints, the playback events are rebuilt.
//! One command per note therefore made drawing over N notes cost N full-score rebuilds, which is
//! exactly why drawing more notes took proportionally longer. The observable form of "one command"
//! is that a single undo takes the whole batch back.
TEST_F(MidiEditorNotesTests, ABatchOfVelocitiesIsASingleUndoStep)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_GE(items.size(), 2) << "this test needs at least two notes";

    std::vector<std::pair<mu::engraving::Note*, int> > changes;
    for (size_t i = 0; i < items.size(); ++i) {
        changes.emplace_back(items[i].note, 40 + int(i));
    }

    EXPECT_EQ(applyNoteVelocities(m_score, changes), int(changes.size()));

    for (size_t i = 0; i < changes.size(); ++i) {
        EXPECT_EQ(changes[i].first->userVelocity(), 40 + int(i));
    }

    m_score->undoRedo(true, nullptr);

    for (const MidiNoteItem& item : items) {
        EXPECT_EQ(item.note->userVelocity(), 0)
            << "one undo left a note behind, so the batch was not written as a single command";
    }
}

//! A batch that changes nothing must not notify the score at all - a stroke over notes that already
//! hold those values would otherwise still cost a full rebuild.
TEST_F(MidiEditorNotesTests, ABatchThatChangesNothingWritesNothing)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    std::vector<std::pair<mu::engraving::Note*, int> > same;
    for (const MidiNoteItem& item : items) {
        same.emplace_back(item.note, item.note->userVelocity());
    }

    EXPECT_EQ(applyNoteVelocities(m_score, same), 0);

    //! And a null entry must not crash or count.
    std::vector<std::pair<mu::engraving::Note*, int> > withNull;
    withNull.emplace_back(nullptr, 100);
    EXPECT_EQ(applyNoteVelocities(m_score, withNull), 0);
    EXPECT_EQ(applyNoteVelocities(nullptr, same), 0);
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

// ── the Dynamics automation curve, i.e. what a crescendo really is ───────────────────────────────
//
// NOTE: the app initialises the automation of every score it loads (EngravingProject::load calls
//       MasterScore::initAutomation), and the generated part of the curve only exists afterwards - so
//       each test below does the same before touching the curve.

//! What the lane paints is a run of points, and what it reads back has to be exactly that - through the
//! notation page's own key, because the crescendo drawn here is the one drawn there.
TEST_F(MidiEditorNotesTests, ADrawnCurveIsReadBackAndIsTheCurveTheNotationPageReads)
{
    m_score->initAutomation();

    const std::vector<MidiAutomationPoint> drawn { { 240, 0.25 }, { 960, 0.5 }, { 1680, 0.75 } };
    EXPECT_EQ(applyAutomationPoints(m_score, 0, drawn), int(drawn.size()));

    const std::vector<MidiAutomationPoint> read = collectAutomationPoints(m_score, 0);
    ASSERT_FALSE(read.empty());

    //! In tick order: the curve is a function of time, and the lane draws it as one line.
    for (size_t i = 1; i < read.size(); ++i) {
        EXPECT_LT(read[i - 1].tick, read[i].tick) << "the curve came back out of order";
    }

    for (const MidiAutomationPoint& point : drawn) {
        const MidiAutomationPoint* written = findAutomationPoint(read, point.tick);
        ASSERT_NE(written, nullptr) << "no point was written at tick " << point.tick;
        EXPECT_DOUBLE_EQ(written->value, point.value);
        EXPECT_TRUE(written->authored) << "a point the lane wrote is the user's own";
    }

    //! The SAME points, found under the key the notation page reads.
    const AutomationCurve& shared = m_score->automationData()->curve(notationPageKey(m_score, 0));
    for (const MidiAutomationPoint& point : drawn) {
        const AutomationCurve::const_iterator it = shared.find(point.tick);
        ASSERT_NE(it, shared.end()) << "the notation page cannot see the point at tick " << point.tick;
        EXPECT_DOUBLE_EQ(double(it->second.value.outValue), point.value)
            << "the two pages are not looking at one curve";
    }
}

//! A stroke is ONE undo step, for the reason a velocity stroke is: every command notifies the whole
//! score, and the subscribers rebuild things that cost O(score).
TEST_F(MidiEditorNotesTests, AStrokeOfTheCurveIsASingleUndoStep)
{
    m_score->initAutomation();

    //! The score's own marks already put points on this curve, so compare against the curve as it was
    //! rather than against "empty".
    const std::vector<MidiAutomationPoint> before = collectAutomationPoints(m_score, 0);

    std::vector<MidiAutomationPoint> stroke;
    for (int tick = 120; tick <= 1800; tick += 120) {
        stroke.push_back(MidiAutomationPoint { tick, 0.1 + double(tick) / 2400.0 });
    }

    ASSERT_EQ(applyAutomationPoints(m_score, 0, stroke), int(stroke.size()));

    for (const MidiAutomationPoint& point : stroke) {
        const MidiAutomationPoint* written = findAutomationPoint(collectAutomationPoints(m_score, 0), point.tick);
        ASSERT_NE(written, nullptr) << "no point was written at tick " << point.tick;
    }

    m_score->undoRedo(true, nullptr);

    const std::vector<MidiAutomationPoint> undone = collectAutomationPoints(m_score, 0);
    ASSERT_EQ(undone.size(), before.size()) << "one undo did not take the whole stroke back";
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(undone[i].tick, before[i].tick);
        EXPECT_DOUBLE_EQ(undone[i].value, before[i].value);
        EXPECT_EQ(undone[i].authored, before[i].authored);
    }
}

//! A stroke that changes nothing must not cost an undo step - otherwise Ctrl+Z would appear to do
//! nothing, which is worse than a wasted rebuild.
TEST_F(MidiEditorNotesTests, AStrokeOfTheCurveThatChangesNothingCostsNoUndoStep)
{
    m_score->initAutomation();

    const std::vector<MidiAutomationPoint> drawn { { 240, 0.3 }, { 720, 0.6 } };
    EXPECT_EQ(applyAutomationPoints(m_score, 0, drawn), int(drawn.size()));

    EXPECT_EQ(applyAutomationPoints(m_score, 0, drawn), 0) << "an unchanged stroke was written again";

    //! Nothing was pushed in between, so this one undo takes the FIRST write back.
    m_score->undoRedo(true, nullptr);

    const std::vector<MidiAutomationPoint> undone = collectAutomationPoints(m_score, 0);
    EXPECT_EQ(findAutomationPoint(undone, 240), nullptr)
        << "the stroke is still there, so an empty command was pushed";
    EXPECT_EQ(findAutomationPoint(undone, 720), nullptr);
}

//! A dynamic mark and a crescendo put points on the curve themselves. They are shown - the lane has to
//! show what the synthesiser follows - but they are not the lane's to delete: the score regenerates
//! them, so erasing one would look like a dead gesture. The notation page's lane refuses the same ones.
TEST_F(MidiEditorNotesTests, APointThatCameFromAMarkIsShownButIsNotTheLanesToRemove)
{
    m_score->initAutomation();

    const MidiAutomationPoint* generated = nullptr;
    for (const MidiAutomationPoint& point : collectAutomationPoints(m_score, 0)) {
        if (!point.authored) {
            generated = &point;
            break;
        }
    }

    //! The fixture carries a `pp` and a crescendo hairpin, so the score must have generated points.
    ASSERT_NE(generated, nullptr) << "the score generated no point for its dynamic mark / hairpin";

    const int tick = generated->tick;
    const double markValue = generated->value;

    EXPECT_FALSE(eraseAutomationPoint(m_score, 0, tick)) << "the mark's own point was removed";
    EXPECT_NE(findAutomationPoint(collectAutomationPoints(m_score, 0), tick), nullptr)
        << "the mark's own point is gone";

    //! Drawing over it takes it over: from then on it is the user's, and can be taken back.
    const double drawn = markValue > 0.5 ? 0.05 : 0.95;
    EXPECT_EQ(applyAutomationPoints(m_score, 0, { { tick, drawn } }), 1);

    const MidiAutomationPoint* taken = findAutomationPoint(collectAutomationPoints(m_score, 0), tick);
    ASSERT_NE(taken, nullptr);
    EXPECT_TRUE(taken->authored) << "a point the user drew over is still the mark's";
    EXPECT_DOUBLE_EQ(taken->value, drawn);

    EXPECT_TRUE(eraseAutomationPoint(m_score, 0, tick)) << "the user's own point must be removable";
    EXPECT_EQ(findAutomationPoint(collectAutomationPoints(m_score, 0), tick), nullptr);
}

TEST_F(MidiEditorNotesTests, TheCurveClampsItsValuesAndIgnoresWhatIsNotAPoint)
{
    m_score->initAutomation();

    EXPECT_EQ(applyAutomationPoints(m_score, 0, { { 100, 5.0 }, { 200, -3.0 } }), 2);

    const std::vector<MidiAutomationPoint> read = collectAutomationPoints(m_score, 0);
    const MidiAutomationPoint* high = findAutomationPoint(read, 100);
    const MidiAutomationPoint* low = findAutomationPoint(read, 200);
    ASSERT_NE(high, nullptr);
    ASSERT_NE(low, nullptr);
    EXPECT_DOUBLE_EQ(high->value, 1.0) << "the level is a 0..1 fraction";
    EXPECT_DOUBLE_EQ(low->value, 0.0);

    //! A negative tick is not a point in time, and a staff that does not exist is not a curve.
    EXPECT_EQ(applyAutomationPoints(m_score, 0, { { -1, 0.5 } }), 0);
    EXPECT_EQ(applyAutomationPoints(m_score, 7, { { 100, 0.5 } }), 0);
    EXPECT_TRUE(collectAutomationPoints(m_score, 7).empty());
    EXPECT_FALSE(eraseAutomationPoint(m_score, 7, 0));

    //! And nothing at all is a no-op rather than a crash.
    EXPECT_EQ(applyAutomationPoints(nullptr, 0, { { 100, 0.5 } }), 0);
    EXPECT_TRUE(collectAutomationPoints(nullptr, 0).empty());
    EXPECT_FALSE(eraseAutomationPoint(nullptr, 0, 100));
    EXPECT_EQ(applyAutomationPoints(m_score, 0, {}), 0);
}

//! 播放侧读的是"该声部自己的曲线"，而记谱页车道与 MIDI 页写的是"整谱表共用"的那条（不带 voice）。
//!
//! 原来的取法是「声部曲线非空就用它，共用曲线整条丢弃」→ 只要谱表里存在**一个**声部专用的点
//! （`VoiceAssignment` 非 ALL_VOICE_IN_INSTRUMENT 的力度记号 / 渐强线就会生成这样的点），
//! 用户画在车道或卷帘窗上的**整条渐强对播放完全无效**。
//!
//! 2026-10-03 用户实测报的「曲线未能作用于最终播放」就是它：真实合奏谱（17 谱表）里
//! 5/7/16/17 号谱表都有声部曲线，而画出来的那条是共用曲线 - 在那些谱表上画什么都听不见。
TEST_F(MidiEditorNotesTests, ADrawnCurveStillReachesPlaybackWhenTheVoiceCurveExists)
{
    m_score->initAutomation();

    // [GIVEN] 用户画的一条（不带 voice 的共用曲线，记谱页车道与 MIDI 页写的都是它）
    const std::vector<MidiAutomationPoint> drawn { { 0, 0.1 }, { 1920, 0.9 } };
    ASSERT_EQ(applyAutomationPoints(m_score, 0, drawn), int(drawn.size()));

    // [AND] 该声部自己的一个点（模拟 CURRENT_VOICE_ONLY 的力度记号生成的点）
    AutomationPoint own;
    own.value.outValue = muse::real_t::make(0.5);
    AutomationPointEdits ownEdits { { 960, AutomationPointEdit::SetPoint { own } } };
    m_score->editAutomationPoints(AutomationCurveKey::staff(AutomationType::Dynamics, m_score->staff(0)->id(), size_t(0)),
                                  ownEdits, /*undoable*/ false);

    // [WHEN] 播放侧取第 0 轨的力度曲线（这正是渲染音符力度与发轨道 dynamics 事件用的那条）
    PlaybackContext ctx(m_score);
    const muse::mpe::DynamicAutomationLayers layers = ctx.dynamicLevelLayers(0, 1);
    ASSERT_FALSE(layers.empty()) << "播放侧没有拿到任何力度曲线";

    const muse::mpe::DynamicAutomationMap& curve = layers.begin()->second;
    ASSERT_FALSE(curve.empty());

    const TempoTimeline& timeline = m_score->tempoTimeline(/*expandRepeats*/ true);
    auto valueAt = [&curve, &timeline](int tick) {
        return double(muse::mpe::evaluateCurveAt(curve, timeline.utick2utime(tick) * 1000000));
    };

    // [THEN] 用户曲线的两端与声部自己的那个点，三者都在
    EXPECT_NEAR(valueAt(0), 0.1, 0.001) << "用户画的起点没有进播放";
    EXPECT_NEAR(valueAt(960), 0.5, 0.001) << "声部自己的点丢了";
    EXPECT_NEAR(valueAt(1920), 0.9, 0.001) << "用户画的终点没有进播放";
}

//! What the lane draws is what the score FILE keeps. The saver writes the curve with
//! `writeGenerated = false` (MscSaver), so the user's own points go in and the marks' generated ones are
//! left out - they are rebuilt from the marks on the next load, and writing them as well would put a
//! second, stale copy of the same crescendo in the file.
TEST_F(MidiEditorNotesTests, ADrawnCurveIsWhatTheScoreFileKeeps)
{
    m_score->initAutomation();

    const std::vector<MidiAutomationPoint> drawn { { 240, 0.125 }, { 1440, 0.875 } };
    ASSERT_EQ(applyAutomationPoints(m_score, 0, drawn), int(drawn.size()));

    //! Exactly the call the saver makes when it writes a score.
    const muse::ByteArray saved = AutomationRW::write(*m_score->automationData(), /*writeGenerated*/ false);
    ASSERT_FALSE(saved.empty()) << "a drawn curve never reached the file";

    AutomationData loaded;
    AutomationRW::read(loaded, saved);

    const AutomationCurve& curve = loaded.curve(notationPageKey(m_score, 0));
    for (const MidiAutomationPoint& point : drawn) {
        const AutomationCurve::const_iterator it = curve.find(point.tick);
        ASSERT_NE(it, curve.end()) << "the point at tick " << point.tick << " was lost with the score";
        EXPECT_DOUBLE_EQ(double(it->second.value.outValue), point.value)
            << "the point at tick " << point.tick << " came back changed";
    }

    for (const MidiAutomationPoint& point : collectAutomationPoints(m_score, 0)) {
        if (!point.authored) {
            EXPECT_EQ(curve.find(point.tick), curve.end())
                << "a generated point was written to the file at tick " << point.tick;
        }
    }
}
