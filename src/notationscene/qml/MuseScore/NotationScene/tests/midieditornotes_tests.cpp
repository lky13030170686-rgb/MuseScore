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
#include "engraving/automation/tempovalues.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/repeatlist.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tempotimeline.h"
#include "engraving/playback/playbackcontext.h"
#include "engraving/playback/playbackloopexpansion.h"
#include "engraving/tests/utils/scorerw.h"

#include "mpe/automationpoint.h"

#include "uicomponents/qml/Muse/UiComponents/internal/polylinebend.h"

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

//! engraving 的曲线元素是 mpe 的 `AutomationPoint` **外面包了一层**（多 `itemId` / `generated`），
//! 所以剥掉那层就能直接用上游的求值函数 —— 这正是播放侧做的事
//! （`PlaybackContext::dynamicLevelLayers()` 也是把 `point.value` 放进 mpe 的 map）。
//! 测试里求值一律走这条路，避免"自己复刻一份公式"而测出假结果。
static muse::mpe::AutomationCurve<int> playableCurve(const AutomationCurve& curve)
{
    muse::mpe::AutomationCurve<int> result;
    for (const auto& [tick, point] : curve) {
        result.insert_or_assign(tick, point.value);
    }

    return result;
}

//! 一个"带弯折的到达段"的 mpe 点：起点值 prevOut 由调用方给，本点的到达值与弯折在这里。
static muse::mpe::AutomationPoint bentPoint(double arrival, double easeT, double easeValue)
{
    muse::mpe::AutomationPoint point;
    point.outValue = muse::real_t::make(arrival);
    point.inValue = muse::mpe::AutomationPoint::ExplicitArrival {
        muse::real_t::make(arrival),
        muse::mpe::AutomationPoint::Ease { muse::real_t::make(easeT), muse::real_t::make(easeValue) },
    };
    return point;
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

//! 点一个音符块 = 试听**那个**音。这一层里唯一会静默出错的东西是**行号**。
//!
//! 视图只画选中的谱表，所以命中函数给的是 `visibleRows` 的下标，而模型的编辑接口（以及
//! 试听用的 `playNote()`）要的是 `notes()` 的下标 —— 两者在过滤之后**不再相等**。
//! 这里把视图那段过滤（`notes[i].staffIndex === currentStaff`）原样走一遍，再按
//! `visibleRows[k].row` 映回去，检查拿到的就是被点中的那个音符。
//! 映射一错，试听会响成**别的音**：不报错、不崩，只是"点这个响那个"，所以必须钉住。
//!
//! ⚠️ **必须用双谱表的谱**（`test-two-staves.mscx`）：单谱表时"可见下标"与"`notes()` 下标"
//! 恰好相等，这个测试会**空转** —— 第一次就是这么写的，把过滤条件改坏它照样绿（反向验证抓到的）。
//!
//! NOTE: 试听真正发出去的那一步（`IPlaybackController::playElements()`）不在这里测 ——
//!       它是**记谱页点音符用的同一个调用**，行为由 engraving 侧的
//!       `Engraving_PlaybackModelTests.Note_Entry_Playback_*` 覆盖；这一页只需要保证
//!       交出去的是**对的那个音符**。听感（点一下就出声）由人在真实程序里验收。
TEST_F(MidiEditorNotesTests, ClickingANoteAuditionsThatVeryNote)
{
    MasterScore* twoStaves = ScoreRW::readScore(String(u"data/test-two-staves.mscx"));
    ASSERT_TRUE(twoStaves);
    ASSERT_GE(twoStaves->nstaves(), 2u) << "this test needs a second staff, or the filter changes nothing";

    const std::vector<MidiNoteItem> notes = collectMidiNotes(twoStaves);
    ASSERT_FALSE(notes.empty());

    //! 视图默认编辑 0 号谱表（`currentStaff` 的初值），过滤规则与 QML 里那一行相同。
    std::vector<size_t> visibleRows;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (notes[i].staffIndex == 0) {
            visibleRows.push_back(i);
        }
    }

    ASSERT_FALSE(visibleRows.empty()) << "this test needs notes on the staff the roll starts on";

    //! 过滤**真的**起了作用（别的谱表有音符）—— 否则"两个下标相等"，下面测不出任何东西。
    //! ⚠️ 不能假设"0 号谱表的音符排在前面"：`collectMidiNotes()` 是**按时间**走的，
    //! 所以另一谱表的音可能夹在中间（第一版就是按"前缀"写的，被这条断言当场抓住）。
    ASSERT_LT(visibleRows.size(), notes.size()) << "no note of another staff, so the mapping cannot be wrong";

    size_t shifted = 0;
    for (size_t k = 0; k < visibleRows.size(); ++k) {
        if (visibleRows[k] != k) {
            ++shifted;
        }
    }
    EXPECT_GT(shifted, size_t(0)) << "the visible index equals the row for every note, so this test proves nothing";

    for (size_t k = 0; k < visibleRows.size(); ++k) {
        const size_t row = visibleRows[k];

        EXPECT_EQ(notes[row].staffIndex, 0)
            << "visible row " << k << " maps to a note of another staff - clicking it would sound the wrong instrument";

        //! 反向也要成立：可见列表里没有**别的**音符能替代这一行 ——
        //! 一个把 row 用成 visibleRows 下标、或把过滤忘掉的实现会在这里露馅。
        for (size_t other = 0; other < notes.size(); ++other) {
            if (other == row) {
                continue;
            }

            const bool samePlace = notes[other].staffIndex == notes[row].staffIndex
                                   && notes[other].tick == notes[row].tick
                                   && notes[other].pitch == notes[row].pitch;
            EXPECT_FALSE(samePlace) << "two notes share a place, so a wrong row would be invisible here";
        }
    }

    delete twoStaves;
}

//! 拖动改音高时要响**拖到的那个音高**（用户 2026-10-07 报的「上下拖动响的是同一个音」）。
//!
//! 卷帘窗拖动中**不写谱**（松手才提交一次），所以播放层拿原音符渲染的话，响的永远是拖动前的音高。
//! 修法是造一个临时音符（`midiNoteToAudition()`）交给**同一个** `playElements()`。这里钉住临时音符
//! 必须满足的两件事：
//!
//!   1. 音高 = **要试听的那个**（这就是修的东西）；
//!   2. 轨道/谱表/声部与原音符**一致** —— 播放层靠 `makeInstrumentTrackId()`（→ `item->part()`）
//!      找轨道，认不出来就**静默无声**（不报错、不崩），所以这一条必须钉住。
TEST_F(MidiEditorNotesTests, AuditioningAPitchBuildsANoteThatSoundsIt)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    for (size_t i = 0; i < items.size(); ++i) {
        const Note* source = items[i].note;
        const int target = (source->pitch() + 7) % 128;      //! 一个与原来不同的音高

        const MidiAuditionNote audition = midiNoteToAudition(m_score, items[i].note, target);
        ASSERT_TRUE(audition.isValid()) << "no temporary note at index " << i;

        EXPECT_EQ(audition.note->pitch(), target) << "the temporary note does not sound the requested pitch";
        EXPECT_NE(audition.note->pitch(), source->pitch()) << "this test needs a different pitch";

        EXPECT_EQ(audition.note->track(), source->track()) << "a different track means a different instrument";
        EXPECT_EQ(audition.note->staffIdx(), source->staffIdx());
        EXPECT_EQ(audition.note->voice(), source->voice());

        //! 播放层就是拿这个找轨道的（PlaybackModel::idKey -> makeInstrumentTrackId -> item->part()）。
        ASSERT_NE(audition.note->part(), nullptr) << "the playback layer cannot resolve a track without a part";
        EXPECT_EQ(audition.note->part(), source->part());

        //! 同一个时间位置：试听不该把音挪到别处去。
        EXPECT_EQ(audition.note->tick(), source->tick());

        //! 删除和弦就够了 —— 它会删掉自己的音符（`Chord::~Chord()` 里 DeleteAll(m_notes)），
        //! 所以**不能**再单独 delete note（那是二次释放）。这一行同时也是"清理真的能跑"的验证。
        delete audition.chord;
    }
}

//! 音高夹到 MIDI 范围、空输入不炸：拖动时指针可能算出界，试听不该因此改状态或崩。
TEST_F(MidiEditorNotesTests, AuditioningClampsThePitchAndSurvivesNothing)
{
    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    ASSERT_FALSE(items.empty());

    Note* source = items.front().note;

    const MidiAuditionNote low = midiNoteToAudition(m_score, source, -5);
    ASSERT_TRUE(low.isValid());
    EXPECT_EQ(low.note->pitch(), 0);
    delete low.chord;

    const MidiAuditionNote high = midiNoteToAudition(m_score, source, 999);
    ASSERT_TRUE(high.isValid());
    EXPECT_EQ(high.note->pitch(), 127);
    delete high.chord;

    EXPECT_FALSE(midiNoteToAudition(nullptr, source, 60).isValid());
    EXPECT_FALSE(midiNoteToAudition(m_score, nullptr, 60).isValid());
}

TEST_F(MidiEditorNotesTests, PitchEditReachesTheScoreAndUndoRestoresIt)
{
    const std::vector<MidiNoteItem> before = collectMidiNotes(m_score);    ASSERT_FALSE(before.empty());

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

//! 复现用户报的"新增点后立刻拖动会回弹"：模型层同一个序列，点应当被移走而不是留在原地。
//! 用户日志（2026-10-05）里**同一组参数第一次失败、第二次成功** —— 差别只在第一次的点是
//! 刚新增的。这个测试把那个序列原样走一遍。
TEST_F(MidiEditorNotesTests, ANewlyAddedPointCanBeMovedRightAway)
{
    m_score->initAutomation();

    // [GIVEN] 刚新增一个点（用户按下空白）
    ASSERT_EQ(applyAutomationPoints(m_score, 0, { { 3720, 0.26 } }), 1);
    ASSERT_NE(findAutomationPoint(collectAutomationPoints(m_score, 0), 3720), nullptr)
        << "新增的点没进曲线";

    // [WHEN] 立刻把它拖到别处（用户不松手直接拖）
    EXPECT_TRUE(applyAutomationPointMove(m_score, 0, 3720, 3780, 0.587))
        << "刚新增的点立刻移动被拒绝了";

    // [THEN] 点在新位置、旧位置没有
    const std::vector<MidiAutomationPoint> after = collectAutomationPoints(m_score, 0);
    EXPECT_EQ(findAutomationPoint(after, 3720), nullptr) << "旧位置的点没被移走（= 回弹）";
    const MidiAutomationPoint* moved = findAutomationPoint(after, 3780);
    ASSERT_NE(moved, nullptr) << "新位置没有点（= 回弹）";
    EXPECT_NEAR(moved->value, 0.587, 1e-6);
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

//! 新增的点必须是"从前一点斜到本点"（`ExplicitArrival`），**不能**是 `ArrivalFromPrevious` ——
//! 后者的含义是"到达值 = 前一个点的值"，于是每一段是**平的**、值在点处跳变，一串这样的点
//! 画出来是**阶梯**而不是渐强。
//!
//! 2026-10-03 用户要求把交互换成贝塞尔控制，根子就在这里：刷出来的曲线是台阶。
TEST_F(MidiEditorNotesTests, ADrawnPointSlopesIntoTheNextOneInsteadOfStepping)
{
    m_score->initAutomation();

    const std::vector<MidiAutomationPoint> drawn { { 0, 0.2 }, { 1920, 0.8 } };
    ASSERT_EQ(applyAutomationPoints(m_score, 0, drawn), int(drawn.size()));

    const AutomationCurve& curve = m_score->automationData()->curve(notationPageKey(m_score, 0));
    ASSERT_EQ(curve.size(), 2u);

    //! 段中点：斜坡应当是 0.5；若是阶梯，会一直停在 0.2 直到下一个点。
    EXPECT_NEAR(double(muse::mpe::evaluateCurveAt(playableCurve(curve), 960)), 0.5, 0.01)
        << "新增点之间不是斜坡 —— 又写回阶梯了";

    for (const auto& [tick, point] : curve) {
        EXPECT_TRUE(std::holds_alternative<AutomationPoint::ExplicitArrival>(point.value.inValue))
            << "tick " << tick << " 又被写成了 ArrivalFromPrevious（阶梯）";
    }
}

//! 拖手柄 = 改二次贝塞尔的弯折点（`Ease`）。写进去、读得回来、形状真的变了、一次撤销能还原。
//! 一条力度曲线有两把键（不带 voice 的共用曲线 / 带 voice 的声部曲线），这里测的是
//! 记谱页与 MIDI 页都写的那把（见 §4.8「两把键」）。
TEST_F(MidiEditorNotesTests, TheHandleBendsTheSegmentAndUndoStraightensIt)
{
    m_score->initAutomation();

    const std::vector<MidiAutomationPoint> drawn { { 0, 0.2 }, { 1920, 0.8 } };
    ASSERT_EQ(applyAutomationPoints(m_score, 0, drawn), int(drawn.size()));

    const AutomationCurveKey key = notationPageKey(m_score, 0);
    const double straightAtMid
        = double(muse::mpe::evaluateCurveAt(playableCurve(m_score->automationData()->curve(key)), 960));
    EXPECT_NEAR(straightAtMid, 0.5, 0.01) << "起点状态应当是一条直线";

    // [WHEN] 把弯折点拖到 (t = 0.3, value = 0.8)：前 30% 走完 80% 的行程 = 先快后慢
    EXPECT_TRUE(applyAutomationPointEase(m_score, 0, 1920, 0.3, 0.8));

    // [THEN] 读回来带着这个弯折
    const MidiAutomationPoint* bent = findAutomationPoint(collectAutomationPoints(m_score, 0), 1920);
    ASSERT_NE(bent, nullptr);
    EXPECT_TRUE(bent->hasEase);
    EXPECT_NEAR(bent->controlT, 0.3, 1e-6);
    EXPECT_NEAR(bent->controlValue, 0.8, 1e-6);

    // [AND] 形状真的变了：t = 0.3 处已经到 0.68（= 0.2 + 0.8 × 0.6），直线在同处只有 0.38
    const muse::mpe::AutomationCurve<int> bentCurve = playableCurve(m_score->automationData()->curve(key));
    EXPECT_NEAR(double(muse::mpe::evaluateCurveAt(bentCurve, 576)), 0.68, 0.02);
    EXPECT_GT(double(muse::mpe::evaluateCurveAt(bentCurve, 960)), straightAtMid)
        << "先快后慢的弯折，中点应当比直线更高";

    // [AND] 一次撤销回到直线：`Ease::none()` 就是弯折点落在段中点且不弯（{0.5, 0.5}），
    //      所以这里看的是"弯折回到 none + 形状回到直线"，而不是 hasEase 变 false
    //      （`hasEase` 只表示 inValue 是显式到达，直线也是显式的）。
    m_score->undoRedo(true, nullptr);
    const MidiAutomationPoint* straight = findAutomationPoint(collectAutomationPoints(m_score, 0), 1920);
    ASSERT_NE(straight, nullptr);
    EXPECT_NEAR(straight->controlT, 0.5, 1e-6) << "撤销之后弯折位置没有回到中点";
    EXPECT_NEAR(straight->controlValue, 0.5, 1e-6) << "撤销之后弯折幅度没有回到直线";
    EXPECT_NEAR(double(muse::mpe::evaluateCurveAt(
                    playableCurve(m_score->automationData()->curve(key)), 960)), 0.5, 0.01)
        << "撤销之后曲线形状没有回到直线";
}

//! 拖控制点 = 移动它（tick + 值），旧 tick 上的点消失，一次撤销能还原。
TEST_F(MidiEditorNotesTests, AControlPointCanBeMoved)
{
    m_score->initAutomation();

    ASSERT_EQ(applyAutomationPoints(m_score, 0, { { 0, 0.2 }, { 1920, 0.8 } }), 2);

    EXPECT_TRUE(applyAutomationPointMove(m_score, 0, 1920, 1440, 0.9));

    const std::vector<MidiAutomationPoint> after = collectAutomationPoints(m_score, 0);
    EXPECT_EQ(findAutomationPoint(after, 1920), nullptr) << "旧位置上的点没有被移走";
    const MidiAutomationPoint* moved = findAutomationPoint(after, 1440);
    ASSERT_NE(moved, nullptr);
    EXPECT_NEAR(moved->value, 0.9, 1e-6);

    //! 原地不动、以及不存在的点：都不写（不压空的撤销步）。
    EXPECT_FALSE(applyAutomationPointMove(m_score, 0, 1440, 1440, 0.9));
    EXPECT_FALSE(applyAutomationPointMove(m_score, 0, 999, 1200, 0.5));
    EXPECT_FALSE(applyAutomationPointEase(m_score, 0, 999, 0.3, 0.7));

    m_score->undoRedo(true, nullptr);

    const std::vector<MidiAutomationPoint> undone = collectAutomationPoints(m_score, 0);
    EXPECT_NE(findAutomationPoint(undone, 1920), nullptr) << "撤销没有把点移回去";
    EXPECT_EQ(findAutomationPoint(undone, 1440), nullptr);
}

//! ⭐ **记谱页车道画出来的那一段，就是合成器播放的那一段。**
//!
//! 这是记谱页曲率的全部要害。车道把一段画成两条在弯折点相切的二次贝塞尔弧
//! （`polylinebend.h`，`PolylinePlot` 用它绘制），播放则是 `muse::mpe::evaluateAt()` 的同一套
//! 两段贝塞尔。两边一旦算得不一样，屏幕上就出现一条**看着像渐强、听着是台阶**的线 ——
//! 这类 bug 不报错、不崩，只是"看到的 ≠ 听到的"，是这个编辑器里最难查的一种。
//! 所以这里不去复刻公式对照，而是**直接拿播放侧那份函数**逐点比对。
TEST_F(MidiEditorNotesTests, TheDrawnSegmentIsTheSegmentTheSynthesiserPlays)
{
    const double prevOut = 0.2;
    const double arrival = 0.8;
    const double easeT = 0.3;
    const double easeValue = 0.8;   // 先快后慢

    const muse::mpe::AutomationPoint played = bentPoint(arrival, easeT, easeValue);

    //! 屏幕上的纵轴是"值 → 显示"的仿射映射：MIDI 页是 0..1 直接画，记谱页把 pp..ffff 铺满谱表框
    //! （也是一次仿射变换）。仿射不改变"两条贝塞尔弧"这个形状，所以两种映射都试：
    //! 若哪天有人把绘制换成折线或换一套控制点，仿射下也照样会被抓出来。
    const auto displayMaps = std::vector<std::pair<double, double> > {
        { 0.0, 1.0 },      // MIDI 页：值 0..1 直接就是 0..1
        { 0.12, 0.92 },    // 记谱页：pp..ffff 的显示子区间（缩放 + 平移）
    };

    for (const auto& [displayFrom, displayTo] : displayMaps) {
        const auto yFor = [displayFrom, displayTo](double value) {
            return 1.0 - (displayFrom + value * (displayTo - displayFrom));
        };

        const QPointF from(100.0, yFor(prevOut));
        const QPointF to(500.0, yFor(arrival));

        //! 弯折手柄的落点：横向在段的 t 处，纵向在弯折值（prevOut + 幅度 × 该段行程）的高度上。
        //! 这正是记谱页车道 `bendHandlePoint()` 与 MIDI 页 `automationHandleAt()` 算的东西。
        const QPointF bend(from.x() + (to.x() - from.x()) * easeT,
                           yFor(prevOut + easeValue * (arrival - prevOut)));

        for (int i = 0; i <= 20; ++i) {
            const double s = i / 20.0;
            const QPointF drawn = muse::uicomponents::polyline::bendPointAt(from, to, bend, s);

            //! 画出来的点的横向位置反算回"这一段的比例" —— 顺便钉住"x 对参数是线性的"，
            //! 否则曲线在时间轴上会被拉歪（画出来的 x 与求值用的 t 不是一回事）。
            const double t = (drawn.x() - from.x()) / (to.x() - from.x());
            EXPECT_NEAR(t, s, 1e-9) << "画出来的横向位置与参数不成线性（s=" << s << "）";

            const double playedValue = double(muse::mpe::evaluateAt(played, muse::real_t::make(prevOut), muse::real_t::make(t)));
            EXPECT_NEAR(drawn.y(), yFor(playedValue), 1e-9)
                << "画出来的曲线与播放的曲线不是同一条（s=" << s << "，显示区间 "
                << displayFrom << ".." << displayTo << "）";
        }
    }
}

//! 没有弯折（`Ease::none()`）时，画出来的两段弧必须**正好**是直线 —— 手柄落在弦的中点，
//! 而中点的手柄不能把线画弯（否则"没调过曲率的段"看上去也像调过）。
TEST_F(MidiEditorNotesTests, AHandleAtTheMiddleOfTheChordDrawsAStraightLine)
{
    const QPointF from(0.0, 80.0);
    const QPointF to(400.0, 10.0);
    const QPointF midOfChord((from.x() + to.x()) * 0.5, (from.y() + to.y()) * 0.5);

    for (int i = 0; i <= 20; ++i) {
        const double s = i / 20.0;
        const QPointF drawn = muse::uicomponents::polyline::bendPointAt(from, to, midOfChord, s);
        EXPECT_NEAR(drawn.x(), from.x() + (to.x() - from.x()) * s, 1e-9);
        EXPECT_NEAR(drawn.y(), from.y() + (to.y() - from.y()) * s, 1e-9)
            << "落在弦中点的手柄把直线画弯了（s=" << s << "）";
    }
}

//! 改曲率**只改弯折**：到达值（`ExplicitArrival::value`）必须原样保留。
//! 那可能是渐强线终点、或记谱页上某个点自己的到达值 —— 拖手柄把它换成 outValue，
//! 等于悄悄改了另一件事（那一段会停在错的值上）。
TEST_F(MidiEditorNotesTests, BendingKeepsTheArrivalValueOfTheSegment)
{
    AutomationPoint existing;
    existing.value.outValue = muse::real_t::make(0.9);
    existing.value.inValue = AutomationPoint::ExplicitArrival { muse::real_t::make(0.4), AutomationPoint::Ease::none() };

    const std::optional<AutomationPoint> bent = bentAutomationPoint(existing, 0.25, 0.75);
    ASSERT_TRUE(bent.has_value());

    EXPECT_NEAR(double(bent->value.outValue), 0.9, 1e-9) << "出值被改了";
    const AutomationPoint::ExplicitArrival& arrival = std::get<AutomationPoint::ExplicitArrival>(bent->value.inValue);
    EXPECT_NEAR(double(arrival.value), 0.4, 1e-9) << "到达值没有保留（这一段现在会停在错的值上）";
    EXPECT_NEAR(double(arrival.ease.t), 0.25, 1e-9);
    EXPECT_NEAR(double(arrival.ease.value), 0.75, 1e-9);
}

//! 原本是 `ArrivalFromPrevious`（一段平的跳变）的点，一拖手柄就升级成"到达本点的值" ——
//! 否则这一段的行程是 0，怎么弯都是平的（用户拖了半天什么都没变）。
TEST_F(MidiEditorNotesTests, BendingAFlatArrivalMakesItASlopeFirst)
{
    AutomationPoint existing;
    existing.value.outValue = muse::real_t::make(0.6);
    existing.value.inValue = AutomationPoint::ArrivalFromPrevious {};

    const std::optional<AutomationPoint> bent = bentAutomationPoint(existing, 0.5, 0.5);
    ASSERT_TRUE(bent.has_value());

    const AutomationPoint::ExplicitArrival& arrival = std::get<AutomationPoint::ExplicitArrival>(bent->value.inValue);
    EXPECT_NEAR(double(arrival.value), 0.6, 1e-9) << "升级后的到达值应当是本点自己的值";
}

//! 记号生成的点一被碰就**归用户**（清 `generated` / `itemId`）—— 与"拖点即接管"同一条规则。
//! 不清的话，记号会在下一次重建时把旧形状原样生成回来，用户看到的是"拖了又弹回去"。
TEST_F(MidiEditorNotesTests, BendingTakesThePointOverFromTheMarkThatGeneratedIt)
{
    AutomationPoint existing;
    existing.value.outValue = muse::real_t::make(0.5);
    existing.value.inValue = AutomationPoint::ExplicitArrival { muse::real_t::make(0.9), AutomationPoint::Ease::none() };
    existing.generated = true;
    existing.itemId = EID::newUnique();

    const std::optional<AutomationPoint> bent = bentAutomationPoint(existing, 0.3, 0.3);
    ASSERT_TRUE(bent.has_value());
    EXPECT_FALSE(bent->generated);
    EXPECT_FALSE(bent->itemId.has_value());
}

//! 弯折点没变、点又已经是用户的 → **什么都不写**：一次没改变任何东西的手势不该压一个撤销步
//! （否则 Ctrl+Z 看起来"没反应"）。
TEST_F(MidiEditorNotesTests, AHandleDragThatChangesNothingWritesNothing)
{
    AutomationPoint existing;
    existing.value.outValue = muse::real_t::make(0.5);
    existing.value.inValue = AutomationPoint::ExplicitArrival {
        muse::real_t::make(0.9), AutomationPoint::Ease { muse::real_t::make(0.3), muse::real_t::make(0.7) }
    };

    EXPECT_FALSE(bentAutomationPoint(existing, 0.3, 0.7).has_value());
    EXPECT_TRUE(bentAutomationPoint(existing, 0.3, 0.71).has_value());

    //! 但**记号生成的点**即使弯折一样也要写：那一次写入的意义是"接管"。
    existing.generated = true;
    EXPECT_TRUE(bentAutomationPoint(existing, 0.3, 0.7).has_value());
}

//! 车道写入走的那条路（`bentAutomationPoint` + `SetPoint`）确实落到曲线上、并且一次撤销能还原 ——
//! 记谱页控制器用的就是这个组合（它把同一个 `SetPoint` 交给 `INotationAutomation::editPoints`）。
TEST_F(MidiEditorNotesTests, TheLanesBendWriteReachesTheCurveAndUndoesInOneStep)
{
    m_score->initAutomation();

    ASSERT_EQ(applyAutomationPoints(m_score, 0, { { 0, 0.2 }, { 1920, 0.8 } }), 2);

    const AutomationCurveKey key = notationPageKey(m_score, 0);
    const AutomationCurve& before = m_score->automationData()->curve(key);
    const auto it = before.find(1920);
    ASSERT_NE(it, before.end());

    const std::optional<AutomationPoint> written = bentAutomationPoint(it->second, 0.3, 0.8);
    ASSERT_TRUE(written.has_value());

    m_score->startCmd(muse::TranslatableString("test", "Bend"));
    AutomationPointEdits edits { { 1920, AutomationPointEdit::SetPoint { *written } } };
    m_score->editAutomationPoints(key, edits);
    m_score->endCmd();

    const MidiAutomationPoint* readBack = findAutomationPoint(collectAutomationPoints(m_score, 0), 1920);
    ASSERT_NE(readBack, nullptr);
    EXPECT_TRUE(readBack->hasEase);
    EXPECT_NEAR(readBack->controlT, 0.3, 1e-6);
    EXPECT_NEAR(readBack->controlValue, 0.8, 1e-6);

    m_score->undoRedo(true, nullptr);
    const MidiAutomationPoint* straight = findAutomationPoint(collectAutomationPoints(m_score, 0), 1920);
    ASSERT_NE(straight, nullptr);
    EXPECT_NEAR(straight->controlT, 0.5, 1e-6);
    EXPECT_NEAR(straight->controlValue, 0.5, 1e-6);
}

//! 标尺上拖出来的一段，就是循环区间：两端按网格吸附、顺序无关、越界夹住。
TEST_F(MidiEditorNotesTests, ARulerDragBecomesALoopOnTheGrid)
{
    const MidiLoopRange forward = midiLoopRangeFromDrag(500, 1400, 1920, 120);
    EXPECT_TRUE(forward.valid);
    EXPECT_EQ(forward.inTick, 480);
    EXPECT_EQ(forward.outTick, 1440);

    //! 反着拖是同一段：手势方向不该改变结果。
    const MidiLoopRange backward = midiLoopRangeFromDrag(1400, 500, 1920, 120);
    EXPECT_TRUE(backward.valid);
    EXPECT_EQ(backward.inTick, forward.inTick);
    EXPECT_EQ(backward.outTick, forward.outTick);

    //! 拖到谱子外面去：夹在 [0, 总长] 里，而不是画出一段播不到的区域。
    const MidiLoopRange beyond = midiLoopRangeFromDrag(-5000, 999999, 1920, 120);
    EXPECT_TRUE(beyond.valid);
    EXPECT_EQ(beyond.inTick, 0);
    EXPECT_EQ(beyond.outTick, 1920);
}

//! 一格都没跨过去的一下，是"点一下"（定位播放起点），不是"拖一段"（循环）——
//! 两者共用同一条标尺，判据就在这里。
TEST_F(MidiEditorNotesTests, ARulerClickIsNotALoop)
{
    const MidiLoopRange sameCell = midiLoopRangeFromDrag(500, 520, 1920, 120);
    EXPECT_FALSE(sameCell.valid);

    const MidiLoopRange tiny = midiLoopRangeFromDrag(0, 2, 1920, 120);
    EXPECT_FALSE(tiny.valid);

    //! ⚠️ 顺便钉住那个上游陷阱：`addLoopBoundary()` 把 0/1/2 当 `BoundaryTick` 读
    //! （1 = 谱面光标处、2 = 全曲末尾）。吸附之后**只可能**返回 0，不可能是 1 或 2。
    for (int release = 0; release <= 240; ++release) {
        const MidiLoopRange range = midiLoopRangeFromDrag(0, release, 1920, 120);
        EXPECT_NE(range.inTick, 1);
        EXPECT_NE(range.inTick, 2);
        EXPECT_NE(range.outTick, 1);
        EXPECT_NE(range.outTick, 2);
    }

    //! 一格就是一格：跨过去一格就算数。
    EXPECT_TRUE(midiLoopRangeFromDrag(0, 120, 1920, 120).valid);
}

//! 播放时视口跟着走：在舒服的范围内**一动不动**（否则谱子会在播放头下自己爬），
//! 出了范围才跳到左边留出 15% 余量的位置，并且被谱子的两端夹住。
TEST_F(MidiEditorNotesTests, TheViewFollowsThePlayheadOnlyWhenItHasTo)
{
    const double viewport = 1000.0;
    const double margin = viewport * midiFollowMargin;

    //! 视野正中：不动。
    EXPECT_DOUBLE_EQ(midiFollowScrollX(400.0, 500.0, viewport, 5000.0), 400.0);
    EXPECT_DOUBLE_EQ(midiFollowScrollX(400.0, margin, viewport, 5000.0), 400.0);
    EXPECT_DOUBLE_EQ(midiFollowScrollX(400.0, viewport - margin, viewport, 5000.0), 400.0);

    //! 跑出右边：滚到"播放头回到左边余量处"为止 —— 900 要落回 150，于是滚动量 = 400 + 750。
    const double outRight = midiFollowScrollX(400.0, viewport - margin + 50.0, viewport, 5000.0);
    EXPECT_DOUBLE_EQ(outRight, 1150.0);

    //! 跑出左边（用户往回拖了）：同样补回来 —— 400 + (-100 - 150) = 150。
    EXPECT_DOUBLE_EQ(midiFollowScrollX(400.0, -100.0, viewport, 5000.0), 150.0);

    //! 两端夹住：不会滚出一个空白的视口。
    EXPECT_DOUBLE_EQ(midiFollowScrollX(0.0, -500.0, viewport, 5000.0), 0.0);
    EXPECT_DOUBLE_EQ(midiFollowScrollX(4900.0, 5000.0, viewport, 5000.0), 5000.0);

    //! 视口还没量出来时不动（第一帧）。
    EXPECT_DOUBLE_EQ(midiFollowScrollX(123.0, 10.0, 0.0, 5000.0), 123.0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  无缝循环回卷：循环 = 播放时间线上的"额外重复"
//
//  用户 2026-10-06 指定的方向是「参考原生重复的工作逻辑」。原生重复根本不 seek：
//  RepeatList 把谱面展开成 utick 时间线，PlaybackModel 把事件按 tickPositionOffset 铺上去，
//  播放器一路向前播 —— 所以重复处天然无缝。
//  循环原来靠"播到循环终点就 seek 回起点"实现，代价是：末尾最多一块（≈21ms）不播、
//  回卷要 flush 音源（听得见的接缝）、还要跟"音轨先渲染、时钟后推进"的一帧顺序斗。
//
//  下面这些测试钉住的就是替换后的那套：**时间线上多出来的那几遍**。
// ─────────────────────────────────────────────────────────────────────────────

//! 谱子（data/test.mscx）是 2 个 4/4 小节 = 3840 tick（division 480），没有反复记号。
static constexpr int TEST_SCORE_TICKS = 3840;

static PlaybackLoopExpansion testLoopExpansion(const RepeatList& repeats, int inTick, int outTick, int passes)
{
    PlaybackLoopExpansion loop;
    loop.enabled = true;
    loop.baseTicks = repeats.ticks();
    loop.loopInUtick = inTick;
    loop.loopOutUtick = outTick;
    loop.passes = passes;

    return loop;
}

//! 循环区间在时间线上**原地铺 K 遍**，后面的音乐整体后移 —— 于是播放器一路向前播，
//! 没有任何一次 seek。这里逐段钉住铺出来的样子。
TEST_F(MidiEditorNotesTests, TheLoopIsLaidOutOnTheTimelineAsExtraRepeats)
{
    const RepeatList& repeats = m_score->repeatList(/*expandRepeats*/ true);
    ASSERT_EQ(repeats.ticks(), TEST_SCORE_TICKS);

    const PlaybackLoopExpansion loop = testLoopExpansion(repeats, /*inTick*/ 960, /*outTick*/ 1920, /*passes*/ 3);

    const std::vector<PlaybackTimelineSegment> timeline = buildPlaybackTimeline(repeats, loop);

    //! 前缀 + 3 遍循环 + 后缀 = 5 段（每段都被循环边界切在该切的地方）
    ASSERT_EQ(timeline.size(), size_t(5));

    //! 前缀原样不动
    EXPECT_EQ(timeline[0].utick, 0);
    EXPECT_EQ(timeline[0].tick, 0);
    EXPECT_EQ(timeline[0].endTick, 960);

    //! 三遍循环：谱面位置都是 [960,1920)，时间线位置依次后移一个循环长度
    for (int pass = 0; pass < 3; ++pass) {
        const PlaybackTimelineSegment& segment = timeline[1 + pass];
        EXPECT_EQ(segment.tick, 960) << "第 " << pass << " 遍的谱面位置";
        EXPECT_EQ(segment.endTick, 1920) << "第 " << pass << " 遍的谱面位置";
        EXPECT_EQ(segment.utick, 960 + pass * 960) << "第 " << pass << " 遍的时间线位置";
        EXPECT_FALSE(segment.measures.empty()) << "第 " << pass << " 遍没有铺到小节";
    }

    //! 后缀整体后移 (passes - 1) 个循环长度 —— 这正是"没有 seek"的代价所在：时间线变长了
    const PlaybackTimelineSegment& suffix = timeline[4];
    EXPECT_EQ(suffix.tick, 1920);
    EXPECT_EQ(suffix.endTick, TEST_SCORE_TICKS);
    EXPECT_EQ(suffix.utick, 1920 + 2 * 960);

    //! 铺出来的总长 = 前缀 + K 遍循环 + 后缀
    EXPECT_EQ(loop.expandedTicks(), TEST_SCORE_TICKS + 2 * 960);
    int laidOutTicks = 0;
    for (const PlaybackTimelineSegment& segment : timeline) {
        laidOutTicks += segment.endTick - segment.tick;
    }
    EXPECT_EQ(laidOutTicks, loop.expandedTicks());
}

//! 每段"铺"出来的东西必须正好是它自己的窗口：循环起点/终点可以落在小节中间，
//! 那时**窗口之前的那半个小节不能跟着重播**（否则循环边界会多出一点"提前响"的音）。
TEST_F(MidiEditorNotesTests, APassCarriesItsOwnWindowAndNothingBeforeIt)
{
    const RepeatList& repeats = m_score->repeatList(true);

    const PlaybackLoopExpansion loop = testLoopExpansion(repeats, /*inTick*/ 960, /*outTick*/ 1920, /*passes*/ 2);
    const std::vector<PlaybackTimelineSegment> timeline = buildPlaybackTimeline(repeats, loop);

    ASSERT_EQ(timeline.size(), size_t(4));

    //! 每段的窗口都是半开的 [tick, endTick)，且与 utick 的偏移一致
    for (const PlaybackTimelineSegment& segment : timeline) {
        EXPECT_LT(segment.tick, segment.endTick);
        for (const Measure* measure : segment.measures) {
            EXPECT_GT(measure->endTick().ticks(), segment.tick) << "窗口之前的小节被带进来了";
            EXPECT_LT(measure->tick().ticks(), segment.endTick) << "窗口之后的小节被带进来了";
        }
    }

    //! 后半小节（960..1920）的第 1 小节：循环第一遍的窗口就从它中间开始
    EXPECT_EQ(timeline[1].tick, 960);
    ASSERT_EQ(timeline[1].measures.size(), size_t(1));
    EXPECT_EQ(timeline[1].measures.front()->tick().ticks(), 0) << "窗口落在这个小节里，但小节本身还是它";
}

//! 时间线是**时间**，不只是位置：多出来的那几遍必须真的用掉"循环一段"那么长的时间，
//! 而且循环区间里的速度变化要**一遍一遍原样重现**，循环之后的音乐则整体平移。
//! （只把前缀拉长的实现会在这里露馅：循环之后的绝对时间会少掉 (K-1) 个循环长度。）
TEST_F(MidiEditorNotesTests, TheLoopIsExtraTimeAndNotASeek)
{
    m_score->initAutomation();

    //! [GIVEN] 循环起点处 1 bps（60 BPM），循环中间 1440 处回到 2 bps（120 BPM）
    AutomationPoint slow;
    slow.value.outValue = normalizeTempo(BeatsPerSecond(1.0));
    AutomationPoint fast;
    fast.value.outValue = normalizeTempo(BeatsPerSecond(2.0));

    AutomationPointEdits tempoEdits { { 960, AutomationPointEdit::SetPoint { slow } }, { 1440, AutomationPointEdit::SetPoint { fast } } };
    m_score->editAutomationPoints(TEMPO_KEY, tempoEdits, /*undoable*/ false);

    const RepeatList& repeats = m_score->repeatList(true);
    const TempoTimeline& base = m_score->tempoTimeline(true);

    //! [GIVEN] 原生时间线就是按谱面来的：0..960 用默认 2 bps，960..1440 是 1 bps，之后 2 bps
    EXPECT_NEAR(base.utick2utime(960), 1.0, 1e-9);
    EXPECT_NEAR(base.utick2utime(1440), 2.0, 1e-9);
    EXPECT_NEAR(base.utick2utime(1920), 2.5, 1e-9);
    EXPECT_NEAR(base.utick2utime(TEST_SCORE_TICKS), 4.5, 1e-9);

    const double loopSeconds = base.utick2utime(1920) - base.utick2utime(960);
    EXPECT_NEAR(loopSeconds, 1.5, 1e-9);

    //! [WHEN] 循环 [960,1920) 原地铺 3 遍
    PlaybackLoopExpansion loop = testLoopExpansion(repeats, 960, 1920, 3);
    m_score->setPlaybackLoopExpansion(loop);

    const TempoTimeline& expanded = m_score->tempoTimeline(true);

    //! [THEN] 前缀与第一遍完全没动
    EXPECT_NEAR(expanded.utick2utime(960), 1.0, 1e-9);
    EXPECT_NEAR(expanded.utick2utime(1440), 2.0, 1e-9);

    //! [AND] 每一遍正好用掉"循环一段"的时间，一遍接一遍
    EXPECT_NEAR(expanded.utick2utime(1920), 1.0 + loopSeconds, 1e-9) << "第 1 遍结束";
    EXPECT_NEAR(expanded.utick2utime(2880), 1.0 + 2 * loopSeconds, 1e-9) << "第 2 遍结束";
    EXPECT_NEAR(expanded.utick2utime(960 + 3 * 960), 1.0 + 3 * loopSeconds, 1e-9) << "第 3 遍结束";

    //! [AND] 循环里的速度变化一遍一遍原样重现：1440 的 2 bps 在第 2 遍里也是 1440+960
    EXPECT_NEAR(expanded.tempo(960).val, 1.0, 1e-9) << "循环起点处是 1 bps";
    EXPECT_NEAR(expanded.tempo(1920).val, 1.0, 1e-9) << "第 2 遍起点也必须是 1 bps（不能继承上一遍结尾的速度）";
    EXPECT_NEAR(expanded.tempo(2400).val, 2.0, 1e-9) << "第 2 遍里的速度变化点";
    EXPECT_NEAR(expanded.utick2utime(2400), 1.0 + loopSeconds + 1.0, 1e-9);

    //! [AND] 后缀整体后移 (K-1) 个循环长度，且自己那一份速度保持
    const int suffixUtick = 1920 + 2 * 960;
    EXPECT_NEAR(expanded.utick2utime(suffixUtick), 2.5 + 2 * loopSeconds, 1e-9) << "循环之后的音乐没有整体后移";
    EXPECT_NEAR(expanded.utick2utime(loop.expandedTicks()), 4.5 + 2 * loopSeconds, 1e-9);

    //! [AND] 关掉循环之后，时间线必须**回到原样**（不能留下痕迹）
    m_score->setPlaybackLoopExpansion(PlaybackLoopExpansion());
    EXPECT_NEAR(m_score->tempoTimeline(true).utick2utime(TEST_SCORE_TICKS), 4.5, 1e-9);
}

//! 谱面 tick 与播放时间线 tick 的来回映射：位置显示、游标、seek 全靠它。
//! "从这里开始播"要的是**第一次出现**；播放器报回来的位置则要映射回谱面里那个小节。
TEST_F(MidiEditorNotesTests, ARawTickMapsToItsFirstOccurrenceAndComesBackFromEveryPass)
{
    const RepeatList& repeats = m_score->repeatList(true);
    const PlaybackLoopExpansion loop = testLoopExpansion(repeats, 960, 1920, 3);

    //! 循环之前的谱面 tick 原地不动
    EXPECT_EQ(loop.fromBaseUtick(0), 0);
    EXPECT_EQ(loop.fromBaseUtick(959), 959);

    //! 循环区间里：第一次出现就是它自己（于是"点哪儿从哪儿播"落在这里）
    EXPECT_EQ(loop.fromBaseUtick(960), 960);
    EXPECT_EQ(loop.fromBaseUtick(1440), 1440);
    EXPECT_EQ(loop.fromBaseUtick(1919), 1919);

    //! 循环之后的谱面 tick 落在后缀上（后移 K-1 个循环长度）
    EXPECT_EQ(loop.fromBaseUtick(1920), 1920 + 2 * 960);
    EXPECT_EQ(loop.fromBaseUtick(TEST_SCORE_TICKS), TEST_SCORE_TICKS + 2 * 960);

    //! 反过来：时间线上的任何位置都能回到它对应的谱面位置 —— 第 2、3 遍回到循环区间本身
    EXPECT_EQ(loop.toBaseUtick(960 + 480), 1440);
    EXPECT_EQ(loop.toBaseUtick(1920 + 480), 1440) << "第 2 遍里的位置";
    EXPECT_EQ(loop.toBaseUtick(2880 + 480), 1440) << "第 3 遍里的位置";
    EXPECT_EQ(loop.toBaseUtick(1920 + 2 * 960 + 100), 2020) << "后缀上的位置";

    //! 每一遍的同一个位置映射回**同一个谱面位置**（这正是游标一遍遍扫过同一段的原因）
    for (int offset = 0; offset < 960; offset += 240) {
        EXPECT_EQ(loop.toBaseUtick(loop.fromBaseUtick(960 + offset)), 960 + offset);
        EXPECT_EQ(loop.toBaseUtick(960 + 960 + offset), 960 + offset);
    }

    //! 没开循环时两个方向都是恒等映射（普通播放一行都不受影响）
    const PlaybackLoopExpansion off;
    EXPECT_FALSE(off.isActive());
    EXPECT_EQ(off.toBaseUtick(1234), 1234);
    EXPECT_EQ(off.fromBaseUtick(1234), 1234);
}

//! 遍数是按"让循环至少能放 EXPANSION_BUDGET_SECS"算出来的，并且两端都夹住：
//! 太短的循环不会铺出天量的遍数，太长的循环也至少铺两遍（不然等于没有循环）。
TEST_F(MidiEditorNotesTests, TheNumberOfPassesIsBoundedByTheBudget)
{
    const RepeatList& repeats = m_score->repeatList(true);

    //! 4 秒的循环：10 分钟 / 4 秒 = 150 遍 → 夹到上限
    const PlaybackLoopExpansion shortLoop = makePlaybackLoopExpansion(repeats, 0, TEST_SCORE_TICKS, 4.0, true);
    EXPECT_TRUE(shortLoop.isActive());
    EXPECT_EQ(shortLoop.passes, PlaybackLoopExpansion::EXPANSION_MAX_PASSES);

    //! 10 秒的循环：60 遍
    const PlaybackLoopExpansion tenSeconds = makePlaybackLoopExpansion(repeats, 0, TEST_SCORE_TICKS, 10.0, true);
    EXPECT_EQ(tenSeconds.passes, 60);

    //! 特别长的循环：至少两遍（否则"循环"根本听不出来）
    const PlaybackLoopExpansion longLoop = makePlaybackLoopExpansion(repeats, 0, TEST_SCORE_TICKS, 100000.0, true);
    EXPECT_EQ(longLoop.passes, PlaybackLoopExpansion::MIN_PASSES_WHEN_ACTIVE);

    //! 没开循环 / 空区间：不铺，时间线还是原样
    EXPECT_FALSE(makePlaybackLoopExpansion(repeats, 0, TEST_SCORE_TICKS, 4.0, false).isActive());
    EXPECT_FALSE(makePlaybackLoopExpansion(repeats, 960, 960, 4.0, true).isActive());
    EXPECT_EQ(makePlaybackLoopExpansion(repeats, 0, TEST_SCORE_TICKS, 4.0, false).expandedTicks(), TEST_SCORE_TICKS);
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//  结构性编辑：增 / 删 / 移 / 改记谱时长 / 复制粘贴
//
//  这一组和前四个写入函数**不是一回事**：前四个只改属性，这一组改的是**谱面结构**（哪一段上有没有
//  和弦），走的是上游输入音符那套（`Score::setNoteRest()` / `Score::changeCRlen()` /
//  `Score::deleteItem()`）。所以这里的断言分三层：
//    ① 数据对不对（音高/时值/力度/演奏层）；
//    ② **小节还是满的**（`sanityCheck()` —— 「测试全绿但工程打不开」正是第十轮的翻车方式）；
//    ③ 一次手势 = **一次撤销**（整批一个命令）。
// ══════════════════════════════════════════════════════════════════════════════════════════════════

//! 测试用的夹具：这份谱子是 4/4 × 2 小节 —— 第 1 小节四个四分音符（60/62/64/65），
//! 第 2 小节一个全小节休止符。下面所有"起点/落点"的假设都基于它。
static constexpr int TEST_QUARTER = 480;
static constexpr int TEST_MEASURE = 1920;

static Chord* chordAt(const Score* score, int tick, int staffIndex = 0, int voice = 0)
{
    const Measure* measure = score->tick2measure(Fraction::fromTicks(tick));
    if (!measure) {
        return nullptr;
    }

    Segment* segment = measure->findSegment(SegmentType::ChordRest, Fraction::fromTicks(tick));
    if (!segment) {
        return nullptr;
    }

    EngravingItem* item = segment->element(staff2track(staff_idx_t(staffIndex)) + track_idx_t(voice));
    return (item && item->isChord()) ? toChord(item) : nullptr;
}

static Note* noteAtTick(const Score* score, int tick, int pitch)
{
    Chord* chord = chordAt(score, tick);
    if (!chord) {
        return nullptr;
    }

    for (Note* note : chord->notes()) {
        if (note->pitch() == pitch) {
            return note;
        }
    }

    return nullptr;
}

static int noteCount(const Score* score)
{
    return int(collectMidiNotes(score).size());
}

//! 删掉和弦的**最后一个音** = 整个和弦换成同时值的休止符（记谱页按 Delete 的语义）——
//! 重点是**小节不能留空**：`sanityCheck` 一失败，真实程序里那份工程就直接打不开。
TEST_F(MidiEditorNotesTests, DeletingTheLastNoteOfAChordLeavesARestOfTheSameLength)
{
    Note* first = noteAtTick(m_score, 0, 60);
    ASSERT_TRUE(first);

    EXPECT_EQ(deleteMidiNotes(m_score, { first }), 1);

    EXPECT_EQ(noteCount(m_score), 3) << "the note should be gone";
    EXPECT_FALSE(chordAt(m_score, 0)) << "the chord should have become a rest";

    //! 休止符占着原来的时值 —— 小节仍然是 4/4。
    Measure* measure = m_score->tick2measure(Fraction::fromTicks(0));
    ASSERT_TRUE(measure);
    EXPECT_EQ(measure->endTick().ticks(), TEST_MEASURE);
    EXPECT_TRUE(m_score->sanityCheck()) << "a deleted note must not leave the measure incomplete";

    m_score->undoRedo(true, nullptr);
    EXPECT_EQ(noteCount(m_score), 4) << "one undo should bring the note back";
    EXPECT_TRUE(noteAtTick(m_score, 0, 60));
}

//! 和弦里还有别的音时，只去掉被选中的那一个（否则删一个和弦音会把整个和弦清掉）。
TEST_F(MidiEditorNotesTests, DeletingOneNoteOfAChordKeepsTheRestOfIt)
{
    //! 先在同一 tick 上加一个音，做出一个真正的二音和弦。
    ASSERT_TRUE(insertMidiNote(m_score, 0, 0, 0, TEST_QUARTER, 64));
    Chord* chord = chordAt(m_score, 0);
    ASSERT_TRUE(chord);
    ASSERT_EQ(chord->notes().size(), 2u) << "the insert should have added a chord tone";

    EXPECT_EQ(deleteMidiNotes(m_score, { noteAtTick(m_score, 0, 60) }), 1);

    chord = chordAt(m_score, 0);
    ASSERT_TRUE(chord) << "the chord must survive deleting one of its notes";
    ASSERT_EQ(chord->notes().size(), 1u);
    EXPECT_EQ(chord->notes().front()->pitch(), 64);
    EXPECT_TRUE(m_score->sanityCheck());
}

//! 整批一次撤销：一次手势删十个音，`Ctrl+Z` 必须是**一次**退回去。
TEST_F(MidiEditorNotesTests, ABatchOfDeletionsIsASingleUndoStep)
{
    std::vector<Note*> doomed { noteAtTick(m_score, 0, 60), noteAtTick(m_score, TEST_QUARTER, 62) };
    ASSERT_TRUE(doomed[0]);
    ASSERT_TRUE(doomed[1]);

    EXPECT_EQ(deleteMidiNotes(m_score, doomed), 2);
    EXPECT_EQ(noteCount(m_score), 2);

    m_score->undoRedo(true, nullptr);
    EXPECT_EQ(noteCount(m_score), 4) << "one undo left a note behind, so the batch was not one command";
    EXPECT_TRUE(m_score->sanityCheck());
}

//! 在**休止符**上插入：走 `setNoteRest()`，余下的时值由上游补成休止符（小节仍然满）。
TEST_F(MidiEditorNotesTests, InsertingANoteOnARestKeepsTheMeasureComplete)
{
    //! 第 2 小节是整小节休止符 —— 正是"空谱表上画一个音"的情形。
    ASSERT_TRUE(insertMidiNote(m_score, 0, 0, TEST_MEASURE, TEST_QUARTER, 67));

    Note* inserted = noteAtTick(m_score, TEST_MEASURE, 67);
    ASSERT_TRUE(inserted) << "the note was not written";
    EXPECT_EQ(inserted->chord()->ticks().ticks(), TEST_QUARTER);

    Measure* measure = m_score->tick2measure(Fraction::fromTicks(TEST_MEASURE));
    ASSERT_TRUE(measure);
    EXPECT_EQ(measure->endTick().ticks(), 2 * TEST_MEASURE) << "no measure may have been added";
    EXPECT_TRUE(m_score->sanityCheck());

    m_score->undoRedo(true, nullptr);
    EXPECT_FALSE(noteAtTick(m_score, TEST_MEASURE, 67)) << "one undo should take the note away again";
}

//! 已经有和弦时插入 = **加和弦音**，而且**同一个音高不会写第二遍**（重复音高在谱面上是脏数据）。
TEST_F(MidiEditorNotesTests, InsertingOnAChordAddsAToneAndNeverDuplicatesAPitch)
{
    ASSERT_TRUE(insertMidiNote(m_score, 0, 0, 0, TEST_QUARTER, 67));
    Chord* chord = chordAt(m_score, 0);
    ASSERT_TRUE(chord);
    EXPECT_EQ(chord->notes().size(), 2u);
    EXPECT_TRUE(noteAtTick(m_score, 0, 67));

    //! 同一个音高再来一次：**无操作**（返回 false），和弦不变。
    EXPECT_FALSE(insertMidiNote(m_score, 0, 0, 0, TEST_QUARTER, 67));
    EXPECT_EQ(chordAt(m_score, 0)->notes().size(), 2u);
    EXPECT_TRUE(m_score->sanityCheck());
}

//! 谱面之外一律不写 —— 这是**故意**的边界：`setNoteRest()` 其实会自己加小节，
//! 但那会让"点错一下"变成"莫名其妙多出几十小节"。
TEST_F(MidiEditorNotesTests, InsertingOutsideTheScoreWritesNothing)
{
    const int measuresBefore = int(collectMidiMeasures(m_score).size());
    const int notesBefore = noteCount(m_score);

    EXPECT_FALSE(insertMidiNote(m_score, 0, 0, 2 * TEST_MEASURE + TEST_QUARTER, TEST_QUARTER, 67));
    EXPECT_FALSE(insertMidiNote(m_score, 0, 0, 2 * TEST_MEASURE, TEST_QUARTER, 67));

    EXPECT_EQ(int(collectMidiMeasures(m_score).size()), measuresBefore) << "no measure may be added";
    EXPECT_EQ(noteCount(m_score), notesBefore);
    EXPECT_FALSE(insertMidiNote(m_score, 99, 0, 0, TEST_QUARTER, 67)) << "unknown staff";
    EXPECT_FALSE(insertMidiNote(m_score, 0, 0, 0, 0, 67)) << "zero length";
}

//! 移动：**音高 / 力度 / 演奏层都跟着走**，原位置变回休止符，小节仍然满。
//! ⚠️ 连音线、记号不在复刻之列（结构性编辑的已知边界，与录制的写回同源）。
TEST_F(MidiEditorNotesTests, MovingANoteKeepsWhatMakesItSoundAndLeavesARestBehind)
{
    Note* source = noteAtTick(m_score, TEST_QUARTER, 62);
    ASSERT_TRUE(source);

    ASSERT_TRUE(applyNoteVelocity(m_score, source, 111));
    //! 演奏层：晚 1/4 出声、只演奏一半、力度乘子 80%。
    ASSERT_TRUE(applyNotePlayOverride(m_score, source, TEST_QUARTER + TEST_QUARTER / 4, TEST_QUARTER / 2, 80));

    const std::vector<Note*> moved = [this, source]() {
                                         std::vector<MidiNoteMove> moves;
                                         MidiNoteMove move;
                                         move.note = source;
                                         move.tick = TEST_MEASURE;    //! 第 2 小节（原来是个休止符）
                                         moves.push_back(move);

                                         std::vector<Note*> out;
                                         EXPECT_EQ(moveMidiNotes(m_score, moves, true, &out), 1);
                                         return out;
                                     }();

    ASSERT_EQ(moved.size(), 1u);
    Note* landed = noteAtTick(m_score, TEST_MEASURE, 62);
    ASSERT_TRUE(landed) << "the note is not where it was moved to";
    EXPECT_EQ(landed->userVelocity(), 111) << "the velocity must travel with the note";

    //! 演奏层按**相对偏移**平移：原来晚 120 tick，搬完之后还是晚 120 tick。
    const NoteEventList& events = landed->playEvents();
    ASSERT_FALSE(events.empty());
    const int shift = (TEST_QUARTER * events.front().ontime()) / NoteEvent::NOTE_LENGTH;
    EXPECT_EQ(shift, TEST_QUARTER / 4) << "the played start should keep its offset";
    EXPECT_EQ((TEST_QUARTER * events.front().len()) / NoteEvent::NOTE_LENGTH, TEST_QUARTER / 2);
    EXPECT_NEAR(events.front().velocityMultiplier(), 0.8, 1e-9);

    EXPECT_FALSE(noteAtTick(m_score, TEST_QUARTER, 62)) << "the original position must be freed";
    EXPECT_EQ(noteCount(m_score), 4) << "moving must not lose or duplicate a note";
    EXPECT_TRUE(m_score->sanityCheck());

    m_score->undoRedo(true, nullptr);
    EXPECT_TRUE(noteAtTick(m_score, TEST_QUARTER, 62)) << "one undo should put it back";
    EXPECT_FALSE(noteAtTick(m_score, TEST_MEASURE, 62));
}

//! 批内**互相踩**的情形：两个音互换位置。做法是"先把源音全删掉、再在目标上重建"，
//! 所以这一种必须两个都活下来（逐个搬的话后一个会把前一个刚搬过去的音盖掉）。
TEST_F(MidiEditorNotesTests, MovingTwoNotesOntoEachOtherKeepsBoth)
{
    Note* a = noteAtTick(m_score, 0, 60);
    Note* b = noteAtTick(m_score, TEST_QUARTER, 62);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::vector<MidiNoteMove> moves;
    moves.push_back({ a, TEST_QUARTER, 0 });
    moves.push_back({ b, 0, 0 });

    EXPECT_EQ(moveMidiNotes(m_score, moves), 2);

    EXPECT_TRUE(noteAtTick(m_score, 0, 62));
    EXPECT_TRUE(noteAtTick(m_score, TEST_QUARTER, 60));
    EXPECT_EQ(noteCount(m_score), 4) << "a swap must not lose a note";
    EXPECT_TRUE(m_score->sanityCheck());

    m_score->undoRedo(true, nullptr);
    EXPECT_TRUE(noteAtTick(m_score, 0, 60)) << "one undo should undo the whole swap";
    EXPECT_TRUE(noteAtTick(m_score, TEST_QUARTER, 62));
}

//! 改**记谱时长**（外框）：走上游的 `changeCRlen()`，所以变长会自动按小节切开并连音、
//! 被盖住的音由 `makeGap()` 让位，而**小节永远是满的**。
TEST_F(MidiEditorNotesTests, ChangingTheNotatedLengthReshapesTheScoreAndKeepsItMeasurable)
{
    Note* first = noteAtTick(m_score, 0, 60);
    ASSERT_TRUE(first);

    //! 四分 → 二分：后一个音（62）的时间被让出去。
    EXPECT_EQ(changeMidiNoteDurations(m_score, { { first, 2 * TEST_QUARTER } }), 1);

    Chord* resized = chordAt(m_score, 0);
    ASSERT_TRUE(resized);
    EXPECT_EQ(resized->ticks().ticks(), 2 * TEST_QUARTER);
    EXPECT_EQ(resized->notes().front()->pitch(), 60) << "resizing must not change the pitch";

    Measure* measure = m_score->tick2measure(Fraction::fromTicks(0));
    ASSERT_TRUE(measure);
    EXPECT_EQ(measure->endTick().ticks(), TEST_MEASURE);
    EXPECT_TRUE(m_score->sanityCheck());

    m_score->undoRedo(true, nullptr);
    EXPECT_EQ(chordAt(m_score, 0)->ticks().ticks(), TEST_QUARTER);
    EXPECT_TRUE(noteAtTick(m_score, TEST_QUARTER, 62)) << "one undo should bring the swallowed note back";
}

//! ⭐ **外框与实心条是两件事**（用户 2026-07-10 明确要求保留这两种状态）：
//! 改记谱时值动的是外框；演奏层存的是**千分比**，所以它跟着标称时值等比缩放，形状不变。
TEST_F(MidiEditorNotesTests, TheNotatedFrameAndThePlayedBarAreTwoDifferentEdges)
{
    Note* first = noteAtTick(m_score, 0, 60);
    ASSERT_TRUE(first);

    //! 演奏时值 = 标称的一半（‰ = 500）。
    ASSERT_TRUE(applyNotePlayOverride(m_score, first, 0, TEST_QUARTER / 2, 100));
    ASSERT_TRUE(collectMidiNotes(m_score).front().hasPlayOverride);

    //! 只改外框：四分 → 二分。
    EXPECT_EQ(changeMidiNoteDurations(m_score, { { first, 2 * TEST_QUARTER } }), 1);

    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
    const MidiNoteItem* resized = findItem(items, noteAtTick(m_score, 0, 60));
    ASSERT_TRUE(resized);

    EXPECT_EQ(resized->durationTicks, 2 * TEST_QUARTER) << "the frame is what the notated length drives";
    EXPECT_TRUE(resized->hasPlayOverride) << "the played layer must survive a notated resize";
    EXPECT_EQ(resized->playDurationTicks, TEST_QUARTER)
        << "the played length is a thousandth of the nominal one, so it scales with the frame";
    EXPECT_TRUE(m_score->sanityCheck());
}

//! 批量改记谱时值：一个和弦只做一次（时值是 ChordRest 级的，和弦里的音不可能各自有时值）。
TEST_F(MidiEditorNotesTests, AChordIsResizedOnceHoweverManyOfItsNotesAreSelected)
{
    ASSERT_TRUE(insertMidiNote(m_score, 0, 0, 0, TEST_QUARTER, 67));
    Chord* chord = chordAt(m_score, 0);
    ASSERT_TRUE(chord);
    ASSERT_EQ(chord->notes().size(), 2u);

    //! 同一个和弦的两个音各请求一次：只应产生**一次**改动。
    EXPECT_EQ(changeMidiNoteDurations(m_score, { { chord->notes()[0], 2 * TEST_QUARTER },
                                                 { chord->notes()[1], 2 * TEST_QUARTER } }), 1);
    EXPECT_EQ(chordAt(m_score, 0)->ticks().ticks(), 2 * TEST_QUARTER);
    EXPECT_TRUE(m_score->sanityCheck());
}

//! ⭐ 演奏力度（`NoteEvent::velocityMultiplier`）批量写入 —— 力度车道的**第二个通道**。
//! ⚠️ 它只动乘子：`ontime` / `len` 一个都不能被顺手改掉（那是另一条通道、另一条手势）。
TEST_F(MidiEditorNotesTests, ABatchOfPlayedVelocitiesChangesTheMultiplierAndNothingElse)
{
    Note* first = noteAtTick(m_score, 0, 60);
    Note* second = noteAtTick(m_score, TEST_QUARTER, 62);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    //! 先给第一个音一条非中性的演奏时值，用来证明"改乘子不会碰它"。
    ASSERT_TRUE(applyNotePlayOverride(m_score, first, TEST_QUARTER / 4, TEST_QUARTER / 2, 100));

    EXPECT_EQ(applyNotePlayVelocities(m_score, { { first, 150 }, { second, 60 } }), 2);

    const NoteEventList& firstEvents = first->playEvents();
    ASSERT_FALSE(firstEvents.empty());
    EXPECT_NEAR(firstEvents.front().velocityMultiplier(), 1.5, 1e-9);
    EXPECT_EQ((TEST_QUARTER * firstEvents.front().ontime()) / NoteEvent::NOTE_LENGTH, TEST_QUARTER / 4)
        << "the played start must not move when the played velocity is written";
    EXPECT_EQ((TEST_QUARTER * firstEvents.front().len()) / NoteEvent::NOTE_LENGTH, TEST_QUARTER / 2);

    ASSERT_FALSE(second->playEvents().empty());
    EXPECT_NEAR(second->playEvents().front().velocityMultiplier(), 0.6, 1e-9);
    //! 时值没被动过：这个音原来没有演奏层覆盖，写完之后**仍然没有**。
    EXPECT_EQ((TEST_QUARTER * second->playEvents().front().len()) / NoteEvent::NOTE_LENGTH, TEST_QUARTER);

    //! 回到 100% = 中性 = "没调过"（画法跟着回到实心块）。
    EXPECT_EQ(applyNotePlayVelocities(m_score, { { second, 100 } }), 1);
    {
        const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);
        const MidiNoteItem* entry = findItem(items, second);
        ASSERT_TRUE(entry);
        EXPECT_FALSE(entry->hasPlayOverride) << "100% with a neutral timing is 'untouched' again";
        EXPECT_EQ(entry->playVelocityPercent, 100);
    }

    m_score->undoRedo(true, nullptr);
    EXPECT_NEAR(second->playEvents().front().velocityMultiplier(), 0.6, 1e-9)
        << "one undo should take the whole batch back";
}

//! 批量改音高：一次拖动提交一次，**一个命令**（N 个音 = N 次全谱通知是踩过的坑）。
TEST_F(MidiEditorNotesTests, ABatchOfPitchesIsASingleUndoStep)
{
    Note* first = noteAtTick(m_score, 0, 60);
    Note* second = noteAtTick(m_score, TEST_QUARTER, 62);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    EXPECT_EQ(applyNotePitches(m_score, { { first, 62 }, { second, 64 } }), 2);
    EXPECT_EQ(noteAtTick(m_score, 0, 62), first);
    EXPECT_EQ(noteAtTick(m_score, TEST_QUARTER, 64), second);

    //! 整批无变化 → 不写、不开命令。
    EXPECT_EQ(applyNotePitches(m_score, { { first, 62 }, { second, 64 } }), 0);

    m_score->undoRedo(true, nullptr);
    EXPECT_EQ(noteAtTick(m_score, 0, 60), first) << "one undo should take the whole batch back";
    EXPECT_EQ(noteAtTick(m_score, TEST_QUARTER, 62), second);
}

//! 复制粘贴：**音高 / 时值 / 力度 / 演奏层**一起过去 —— 复制的是"这个音听起来是什么样"。
TEST_F(MidiEditorNotesTests, PastingBringsBackWhatMakesTheNotesSound)
{
    Note* first = noteAtTick(m_score, 0, 60);
    ASSERT_TRUE(first);
    ASSERT_TRUE(applyNoteVelocity(m_score, first, 99));
    ASSERT_TRUE(applyNotePlayOverride(m_score, first, TEST_QUARTER / 4, TEST_QUARTER / 2, 70));

    std::vector<Note*> sources;
    for (const MidiNoteItem& item : collectMidiNotes(m_score)) {
        sources.push_back(item.note);
    }
    const std::vector<MidiClipboardNote> clipboard = copyMidiNotes(sources);
    ASSERT_EQ(clipboard.size(), 4u);
    EXPECT_EQ(clipboard.front().tickOffset, 0) << "the offsets are relative to the earliest note";
    EXPECT_EQ(clipboard.back().tickOffset, 3 * TEST_QUARTER);

    std::vector<Note*> pasted;
    EXPECT_EQ(pasteMidiNotes(m_score, 0, 0, TEST_MEASURE, clipboard, true, &pasted), 4);
    EXPECT_EQ(pasted.size(), 4u);

    Note* copiedFirst = noteAtTick(m_score, TEST_MEASURE, 60);
    ASSERT_TRUE(copiedFirst);
    EXPECT_EQ(copiedFirst->userVelocity(), 99) << "the velocity must be pasted too";

    const NoteEventList& events = copiedFirst->playEvents();
    ASSERT_FALSE(events.empty());
    EXPECT_NEAR(events.front().velocityMultiplier(), 0.7, 1e-9);
    EXPECT_EQ((TEST_QUARTER * events.front().len()) / NoteEvent::NOTE_LENGTH, TEST_QUARTER / 2);

    EXPECT_EQ(noteCount(m_score), 8);
    EXPECT_TRUE(m_score->sanityCheck());

    //! 粘完一次撤销**整块**。
    m_score->undoRedo(true, nullptr);
    EXPECT_EQ(noteCount(m_score), 4) << "one undo should take the whole paste back";
}

//! 同一个 tickOffset 上的音落成**一个和弦**（复制的是和弦，粘出来的也得是）。
TEST_F(MidiEditorNotesTests, PastingTwoNotesOfTheSameTickBuildsOneChord)
{
    std::vector<MidiClipboardNote> clipboard;
    clipboard.push_back({ 0, TEST_QUARTER, 60, 0, 0, TEST_QUARTER, 100 });
    clipboard.push_back({ 0, TEST_QUARTER, 64, 90, 0, TEST_QUARTER, 100 });

    EXPECT_EQ(pasteMidiNotes(m_score, 0, 0, TEST_MEASURE, clipboard), 2);

    Chord* chord = chordAt(m_score, TEST_MEASURE);
    ASSERT_TRUE(chord);
    EXPECT_EQ(chord->notes().size(), 2u);
    EXPECT_EQ(chord->notes()[0]->userVelocity(), 0);
    EXPECT_EQ(chord->notes()[1]->userVelocity(), 90);
    EXPECT_TRUE(m_score->sanityCheck());
}

//! 谱面之外的部分跳过，但**不报错也不写坏**：粘一半也要把能放下的放下。
TEST_F(MidiEditorNotesTests, PastingPastTheEndOfTheScoreSkipsWhatDoesNotFit)
{
    std::vector<MidiClipboardNote> clipboard;
    clipboard.push_back({ 0, TEST_QUARTER, 60, 0, 0, TEST_QUARTER, 100 });
    clipboard.push_back({ 4 * TEST_QUARTER, TEST_QUARTER, 62, 0, 0, TEST_QUARTER, 100 });   //! 落到谱面之外

    EXPECT_EQ(pasteMidiNotes(m_score, 0, 0, TEST_MEASURE, clipboard), 1);
    EXPECT_TRUE(noteAtTick(m_score, TEST_MEASURE, 60));
    EXPECT_FALSE(noteAtTick(m_score, TEST_MEASURE + 4 * TEST_QUARTER, 62));
    EXPECT_TRUE(m_score->sanityCheck());
}

//! 空输入 / 空指针一律**无操作**（试听与编辑的入口都会被脚本或快捷键打进来越界值）。
TEST_F(MidiEditorNotesTests, StructuralEditsSurviveNothingBeingThere)
{
    EXPECT_EQ(deleteMidiNotes(m_score, {}), 0);
    EXPECT_EQ(deleteMidiNotes(nullptr, { noteAtTick(m_score, 0, 60) }), 0);
    EXPECT_EQ(deleteMidiNotes(m_score, { nullptr }), 0);

    EXPECT_EQ(moveMidiNotes(m_score, {}), 0);
    EXPECT_EQ(moveMidiNotes(m_score, { { nullptr, 0, 0 } }), 0);
    EXPECT_EQ(moveMidiNotes(nullptr, {}), 0);

    EXPECT_EQ(changeMidiNoteDurations(m_score, {}), 0);
    EXPECT_EQ(changeMidiNoteDurations(m_score, { { nullptr, 480 } }), 0);
    EXPECT_EQ(changeMidiNoteDurations(nullptr, { { noteAtTick(m_score, 0, 60), 480 } }), 0);

    EXPECT_TRUE(copyMidiNotes({}).empty());
    EXPECT_EQ(pasteMidiNotes(m_score, 0, 0, 0, {}), 0);
    EXPECT_EQ(pasteMidiNotes(nullptr, 0, 0, 0, { { 0, 480, 60, 0, 0, 480, 100 } }), 0);

    EXPECT_EQ(noteCount(m_score), 4) << "nothing above may have changed the score";
    EXPECT_TRUE(m_score->sanityCheck());
}

//! ⭐ **存得住、读得回、真实程序打得开** —— 结构编辑最后一道关。
//!
//! 三种结构改动各来一次（删 / 改记谱时值 / 在休止符上插入），存盘再读回：
//!  * 单测用的是 engraving 层的 `compat::loadMsczOrMscx`，**不跑 `Score::sanityCheck`**；
//!  * 所以这里先自己断言 `sanityCheck()`，再由真实 exe 打开这份文件（`-o out.png`，exit 0 = 能打开）
//!    —— 第十轮"测试全绿但工程打不开"就是漏了后面那一步。
//! 设 `MUSE_MIDIEDITOR_KEEP_SCORE=1` 时把产物留在 `%TEMP%\dsh-midieditor-structural.mscx`。
TEST_F(MidiEditorNotesTests, StructuralEditsSurviveSaveAndReload)
{
    const bool keepScore = qEnvironmentVariableIsSet("MUSE_MIDIEDITOR_KEEP_SCORE");
    const QString savedPath = keepScore
                              ? QDir::tempPath() + QStringLiteral("/dsh-midieditor-structural.mscx")
                              : QDir::tempPath() + QStringLiteral("/dsh-midieditor-structural-roundtrip.mscx");
    QFile::remove(savedPath);

    {
        MasterScore* score = ScoreRW::readScore(TEST_SCORE_PATH);
        ASSERT_TRUE(score);

        //! ① 删掉第 1 小节第 3 拍那个音（原位应变成休止符）
        ASSERT_EQ(deleteMidiNotes(score, { noteAtTick(score, 2 * TEST_QUARTER, 64) }), 1);

        //! ② 把第 1 个音改短成八分（记谱层 = 外框）
        ASSERT_EQ(changeMidiNoteDurations(score, { { noteAtTick(score, 0, 60), TEST_QUARTER / 2 } }), 1);

        //! ③ 在第 2 小节的休止符上插一个音
        ASSERT_TRUE(insertMidiNote(score, 0, 0, TEST_MEASURE, TEST_QUARTER, 67));

        EXPECT_TRUE(score->sanityCheck()) << "the score must stay loadable before it is even saved";

        ASSERT_TRUE(ScoreRW::saveScore(score, savedPath)) << "could not save the score";
        delete score;
    }

    ASSERT_TRUE(QFile::exists(savedPath)) << "the saved score is not on disk";

    MasterScore* reloaded = ScoreRW::readScore(savedPath, /*isAbsolutePath*/ true);
    ASSERT_TRUE(reloaded) << "could not reopen the saved score";
    EXPECT_TRUE(reloaded->sanityCheck());

    const std::vector<MidiNoteItem> again = collectMidiNotes(reloaded);
    ASSERT_EQ(again.size(), 4u) << "the structural edits did not survive the round trip";

    //! 四个音应当正好是"短了的 60 / 62 / 插进去的 67"，而 64 已经不在了。
    int atEightNote = 0;
    int atSecondMeasure = 0;
    bool sawDeleted = false;
    for (const MidiNoteItem& item : again) {
        if (item.pitch == 60 && item.tick == 0) {
            atEightNote = item.durationTicks;
        }
        if (item.pitch == 67 && item.tick == TEST_MEASURE) {
            atSecondMeasure = item.durationTicks;
        }
        if (item.pitch == 64) {
            sawDeleted = true;
        }
    }

    EXPECT_EQ(atEightNote, TEST_QUARTER / 2) << "the notated length was lost on save/reload";
    EXPECT_EQ(atSecondMeasure, TEST_QUARTER) << "the inserted note was lost on save/reload";
    EXPECT_FALSE(sawDeleted) << "the deleted note came back";

    delete reloaded;
}


