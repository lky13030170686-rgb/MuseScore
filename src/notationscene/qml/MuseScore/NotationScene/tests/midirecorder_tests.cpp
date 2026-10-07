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
#include <vector>

#include <QDir>
#include <QFile>
#include <QtGlobal>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/tests/utils/scorerw.h"

#include "notationscene/qml/MuseScore/NotationScene/midieditor/midieditornotes.h"
#include "notationscene/qml/MuseScore/NotationScene/midieditor/midirecorder.h"

using namespace mu;
using namespace mu::engraving;
using namespace mu::notation;

//! `data/test.mscx`: 一个小节四个四分音符（60/62/64/65，各 480 tick）+ 一个整小节休止符。
//! 于是它有录制要用的两种地形：**空的小节**（第 2 小节，网格位置上一个段都没有）与
//! **既有音符的小节**（第 1 小节，落点会切在音的内部）。
static const String TEST_SCORE_PATH(u"data/test.mscx");

//! 480 = 一拍；`Constants::DIVISION` 是固定的，所以这些数在任何工程里都成立。
static constexpr int QUARTER = 480;

// ── 采集：按下/抬起配对 ─────────────────────────────────────────────────────────────────────────

class MidiRecorderTests : public ::testing::Test
{
public:
    static MidiRecordedNote note(int pitch, double start, double end, int velocity = 80)
    {
        MidiRecordedNote result;
        result.pitch = pitch;
        result.velocity = velocity;
        result.startTick = start;
        result.endTick = end;
        return result;
    }

    static std::vector<MidiQuantizedNote> quantized(int pitch, int tick, int duration, int velocity = 80)
    {
        MidiQuantizedNote result;
        result.pitch = pitch;
        result.velocity = velocity;
        result.tick = tick;
        result.durationTicks = duration;
        return { result };
    }
};

TEST_F(MidiRecorderTests, PairsEveryNoteOnWithItsNoteOff)
{
    MidiRecorder recorder;
    recorder.start();

    recorder.noteOn(60, 90, 100.0);
    recorder.noteOn(64, 70, 110.0);
    recorder.noteOff(60, 580.0);
    recorder.noteOff(64, 640.0);

    const std::vector<MidiRecordedNote>& notes = recorder.notes();
    ASSERT_EQ(notes.size(), 2u);

    EXPECT_EQ(notes[0].pitch, 60);
    EXPECT_EQ(notes[0].velocity, 90);
    EXPECT_DOUBLE_EQ(notes[0].startTick, 100.0);
    EXPECT_DOUBLE_EQ(notes[0].endTick, 580.0);

    EXPECT_EQ(notes[1].pitch, 64);
    EXPECT_DOUBLE_EQ(notes[1].endTick, 640.0);

    EXPECT_EQ(recorder.heldCount(), 0);
}

//! 同一个音高还没抬起来又按了一次（颤音、连击、键盘丢了一个 note-off）：前一个必须就地封口，
//! 否则两个"还按着"的同音会同时存在，而 note-off 只能配上最近的那个 —— 前一个会一直挂到停止，
//! 在卷帘窗里变成一条拖到录制结束的长条。
TEST_F(MidiRecorderTests, ARetriggerClosesTheNoteStillHeld)
{
    MidiRecorder recorder;
    recorder.start();

    recorder.noteOn(60, 90, 0.0);
    recorder.noteOn(60, 90, 200.0);
    recorder.noteOff(60, 500.0);

    const std::vector<MidiRecordedNote>& notes = recorder.notes();
    ASSERT_EQ(notes.size(), 2u);

    EXPECT_DOUBLE_EQ(notes[0].endTick, 200.0) << "the first press was left hanging";
    EXPECT_DOUBLE_EQ(notes[1].startTick, 200.0);
    EXPECT_DOUBLE_EQ(notes[1].endTick, 500.0);
    EXPECT_EQ(recorder.heldCount(), 0);
}

//! 没按下过的抬起（录制开始那一刻手已经按在键上、或设备补发）：忽略。
//! 这里绝不能"造一个音"—— 那会在谱面上凭空多出一个没人弹过的音。
TEST_F(MidiRecorderTests, ANoteOffWithoutANoteOnIsIgnored)
{
    MidiRecorder recorder;
    recorder.start();

    recorder.noteOff(60, 100.0);
    EXPECT_EQ(recorder.noteCount(), 0);

    recorder.noteOn(62, 80, 200.0);
    recorder.noteOff(60, 300.0);     //!< 另一个音高的抬起，同样不该碰到 62
    EXPECT_EQ(recorder.noteCount(), 1);
    EXPECT_EQ(recorder.heldCount(), 1);
}

//! 停止时手还按着：按那一刻封口，长度是 0 而**不是负数** —— 负数会一路传到写谱那里变成一个
//! 往回走的音。
TEST_F(MidiRecorderTests, StoppingClosesWhatIsStillHeldWithoutGoingBackwards)
{
    MidiRecorder recorder;
    recorder.start();

    recorder.noteOn(60, 90, 500.0);
    recorder.stop(400.0);            //!< 停止比按下还早（同一个 tick 上的极端情形）

    ASSERT_EQ(recorder.notes().size(), 1u);
    EXPECT_DOUBLE_EQ(recorder.notes()[0].endTick, 500.0);
    EXPECT_GE(recorder.notes()[0].heldTicks(), 0.0);
    EXPECT_EQ(recorder.heldCount(), 0);

    //! 停止之后再来事件：一个都不收。
    recorder.noteOn(64, 80, 600.0);
    EXPECT_EQ(recorder.noteCount(), 1);
}

TEST_F(MidiRecorderTests, NothingIsCapturedBeforeStart)
{
    MidiRecorder recorder;
    recorder.noteOn(60, 90, 0.0);
    recorder.noteOff(60, 100.0);
    EXPECT_EQ(recorder.noteCount(), 0);
}

// ── 量化 ────────────────────────────────────────────────────────────────────────────────────────

TEST_F(MidiRecorderTests, QuantizeSnapsToTheNearestStep)
{
    //! 十六分音符网格（120 tick）：137 离 120 比离 240 近。
    EXPECT_EQ(quantizeTickValue(137.0, 120, 100), 120);
    EXPECT_EQ(quantizeTickValue(181.0, 120, 100), 240);
    EXPECT_EQ(quantizeTickValue(179.999, 120, 100), 120);

    //! 三连音网格不是二连音的一半：八分三连音 160，十六分三连音 80。
    EXPECT_EQ(quantizeTickValue(150.0, 160, 100), 160);
    EXPECT_EQ(quantizeTickValue(95.0, 80, 100), 80);

    //! 负数（理论上到不了这里）夹到 0，绝不给出一个负 tick。
    EXPECT_EQ(quantizeTickValue(-20.0, 120, 100), 0);
}

//! 强度是"往回拉多少"，不是"吸到哪一格"：50% 正好落在演奏位置与格子中间 —— 保留一半人味。
TEST_F(MidiRecorderTests, QuantizeStrengthBlendsPlayedAndGrid)
{
    EXPECT_EQ(quantizeTickValue(170.0, 120, 0), 170) << "0% must not move anything";
    EXPECT_EQ(quantizeTickValue(170.0, 120, 50), 145);
    EXPECT_EQ(quantizeTickValue(170.0, 120, 100), 120);
}

TEST_F(MidiRecorderTests, WithoutAGridTheTimingIsKept)
{
    EXPECT_EQ(quantizeTickValue(137.4, 0, 100), 137);
    EXPECT_EQ(quantizeTickValue(137.6, 0, 100), 138);
}

//! 轻轻一碰（按住不到半格）吸完会变成零长度甚至负长度 —— 零长度的音在谱面上不存在。
TEST_F(MidiRecorderTests, ATapStillGetsAnAudibleLength)
{
    MidiQuantizeSettings settings;
    settings.gridTicks = 120;

    EXPECT_EQ(quantizeMinDurationTicks(settings), 60);

    const std::vector<MidiQuantizedNote> notes = quantizeRecordedNotes({ note(60, 480.0, 490.0) }, settings);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].tick, 480);
    EXPECT_EQ(notes[0].durationTicks, 60) << "a 10-tick tap must still become a note";
}

TEST_F(MidiRecorderTests, QuantizingSortsAndDropsWhatIsStillHeld)
{
    std::vector<MidiRecordedNote> played;
    played.push_back(note(64, 500.0, 900.0));

    MidiRecordedNote held;
    held.pitch = 60;
    held.velocity = 90;
    held.startTick = 200.0;
    held.endTick = -1.0;             //!< 还按着
    played.push_back(held);

    const std::vector<MidiQuantizedNote> notes = quantizeRecordedNotes(played, MidiQuantizeSettings {});
    ASSERT_EQ(notes.size(), 1u) << "a note that is still held has no length to write";
    EXPECT_EQ(notes[0].pitch, 64);

    //! 排序：起点在前，同起点按音高。
    std::vector<MidiRecordedNote> two;
    two.push_back(note(72, 480.0, 900.0));
    two.push_back(note(60, 480.0, 900.0));
    two.push_back(note(64, 240.0, 400.0));

    const std::vector<MidiQuantizedNote> sorted = quantizeRecordedNotes(two, MidiQuantizeSettings {});
    ASSERT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0].tick, 240);
    EXPECT_EQ(sorted[1].pitch, 60);
    EXPECT_EQ(sorted[2].pitch, 72);
}

// ── 分组：量化后的音 → 谱面上的和弦 ──────────────────────────────────────────────────────────────

TEST_F(MidiRecorderTests, SimultaneousNotesBecomeOneChord)
{
    std::vector<MidiQuantizedNote> notes = quantized(60, 480, QUARTER);
    notes.push_back(quantized(64, 480, QUARTER).front());
    notes.push_back(quantized(67, 480, QUARTER).front());

    const std::vector<MidiRecordedChord> chords = buildRecordedChords(notes);
    ASSERT_EQ(chords.size(), 1u) << "three notes on one tick are ONE chord, not three";
    EXPECT_EQ(chords[0].notes.size(), 3u);
    EXPECT_EQ(chords[0].voice, 0);
    EXPECT_EQ(chords[0].durationTicks, QUARTER);
}

//! 记谱上"一个和弦只能有一个时值"，所以同一 tick 上时值不同的音必须分到不同声部 ——
//! 截成一样长会丢掉演奏的长度。短的先落 0 声部（确定的、也讲得通的分法）。
TEST_F(MidiRecorderTests, DifferentLengthsAtOneTickLandInDifferentVoices)
{
    std::vector<MidiQuantizedNote> notes = quantized(48, 480, QUARTER * 2);      //!< 长音
    notes.push_back(quantized(72, 480, QUARTER / 2).front());                     //!< 短音

    const std::vector<MidiRecordedChord> chords = buildRecordedChords(notes);
    ASSERT_EQ(chords.size(), 2u);

    const MidiRecordedChord* shortOne = nullptr;
    const MidiRecordedChord* longOne = nullptr;
    for (const MidiRecordedChord& chord : chords) {
        (chord.durationTicks == QUARTER / 2 ? shortOne : longOne) = &chord;
    }

    ASSERT_NE(shortOne, nullptr);
    ASSERT_NE(longOne, nullptr);
    EXPECT_EQ(shortOne->voice, 0);
    EXPECT_EQ(longOne->voice, 1);

    //! 声部不够时并回最后一个，宁可挤也不丢音。
    std::vector<MidiQuantizedNote> many = quantized(48, 480, QUARTER * 4);
    many.push_back(quantized(50, 480, QUARTER * 2).front());
    many.push_back(quantized(52, 480, QUARTER).front());
    many.push_back(quantized(54, 480, QUARTER / 2).front());
    many.push_back(quantized(56, 480, QUARTER / 4).front());

    EXPECT_EQ(buildRecordedChords(many, 4).size(), 5u) << "no chord may be dropped";
}

TEST_F(MidiRecorderTests, TheSamePitchIsKeptOnceInAChord)
{
    std::vector<MidiQuantizedNote> notes = quantized(60, 480, QUARTER);
    notes.push_back(quantized(60, 480, QUARTER).front());

    const std::vector<MidiRecordedChord> chords = buildRecordedChords(notes);
    ASSERT_EQ(chords.size(), 1u);
    EXPECT_EQ(chords[0].notes.size(), 1u);
}

// ── 写回真谱（这里才需要 Score）─────────────────────────────────────────────────────────────────

class MidiRecordedWriteTests : public ::testing::Test
{
public:
    void SetUp() override
    {
        m_score = ScoreRW::readScore(TEST_SCORE_PATH);
        ASSERT_TRUE(m_score);

        m_secondMeasure = m_score->firstMeasure() ? m_score->firstMeasure()->nextMeasure() : nullptr;
        ASSERT_NE(m_secondMeasure, nullptr) << "this test needs the second (empty) measure";
    }

    void TearDown() override
    {
        delete m_score;
    }

    static MidiRecordedChord chord(int tick, int duration, std::vector<int> pitches, int velocity = 80)
    {
        MidiRecordedChord result;
        result.tick = tick;
        result.durationTicks = duration;
        for (int pitch : pitches) {
            result.notes.push_back(MidiRecordedChord::Note { pitch, velocity });
        }
        return result;
    }

    //! 谱面上某个 tick 上的音（按音高找）。
    static const MidiNoteItem* itemAt(const std::vector<MidiNoteItem>& items, int tick, int pitch)
    {
        for (const MidiNoteItem& item : items) {
            if (item.tick == tick && item.pitch == pitch) {
                return &item;
            }
        }

        return nullptr;
    }

    MasterScore* m_score = nullptr;
    Measure* m_secondMeasure = nullptr;
};

//! 空小节（整小节休止符）是录制最常见的地形：网格位置上一个段都没有，
//! 写回的第一步必须自己把休止符切开（见 recordSegmentAt）。
TEST_F(MidiRecordedWriteTests, ATakeIsWrittenIntoAnEmptyMeasure)
{
    const int start = m_secondMeasure->tick().ticks();

    std::vector<MidiRecordedChord> take;
    take.push_back(chord(start + 0 * QUARTER, QUARTER, { 60 }));
    take.push_back(chord(start + 1 * QUARTER, QUARTER, { 62 }));
    take.push_back(chord(start + 2 * QUARTER, QUARTER, { 64, 67 }));
    take.push_back(chord(start + 3 * QUARTER, QUARTER, { 65 }));

    const MidiRecordedWriteResult written = applyRecordedChords(m_score, 0, 0, take);

    EXPECT_EQ(written.chordsWritten, 4);
    EXPECT_EQ(written.chordsSkipped, 0);
    EXPECT_EQ(written.notesWritten, 5) << "the two-note chord must write two notes";
    EXPECT_EQ(written.lastTick, start + 3 * QUARTER);

    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);

    const MidiNoteItem* first = itemAt(items, start, 60);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->durationTicks, QUARTER);

    EXPECT_NE(itemAt(items, start + 1 * QUARTER, 62), nullptr);
    EXPECT_NE(itemAt(items, start + 2 * QUARTER, 64), nullptr);
    EXPECT_NE(itemAt(items, start + 2 * QUARTER, 67), nullptr) << "the chord lost one of its notes";
    EXPECT_NE(itemAt(items, start + 3 * QUARTER, 65), nullptr);
}

//! 一次 Ctrl+Z 撤销**整次录制**，而不是一个音一个音地撤。
TEST_F(MidiRecordedWriteTests, AWholeTakeIsOneUndoStep)
{
    const int start = m_secondMeasure->tick().ticks();

    std::vector<MidiRecordedChord> take;
    take.push_back(chord(start, QUARTER, { 60 }));
    take.push_back(chord(start + QUARTER, QUARTER, { 62 }));

    ASSERT_EQ(applyRecordedChords(m_score, 0, 0, take).chordsWritten, 2);
    ASSERT_NE(itemAt(collectMidiNotes(m_score), start, 60), nullptr);

    m_score->undoRedo(true, nullptr);

    const std::vector<MidiNoteItem> after = collectMidiNotes(m_score);
    EXPECT_EQ(itemAt(after, start, 60), nullptr) << "one undo left part of the take behind";
    EXPECT_EQ(itemAt(after, start + QUARTER, 62), nullptr);
}

//! 力度写的是 `Pid::USER_VELOCITY`（力度车道与 Properties 写的同一个属性），
//! 而且必须在事务里写 —— 直接 setUserVelocity() 的话，撤销再重做之后力度会回到 0。
TEST_F(MidiRecordedWriteTests, VelocitySurvivesUndoAndRedo)
{
    const int start = m_secondMeasure->tick().ticks();

    std::vector<MidiRecordedChord> take;
    take.push_back(chord(start, QUARTER, { 60 }, 99));

    ASSERT_EQ(applyRecordedChords(m_score, 0, 0, take).notesWritten, 1);

    const MidiNoteItem* written = itemAt(collectMidiNotes(m_score), start, 60);
    ASSERT_NE(written, nullptr);
    EXPECT_EQ(written->velocity, 99);
    EXPECT_TRUE(written->hasVelocityOverride);

    m_score->undoRedo(true, nullptr);
    m_score->undoRedo(false, nullptr);

    const MidiNoteItem* again = itemAt(collectMidiNotes(m_score), start, 60);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->velocity, 99) << "the velocity was not part of the undoable command";
}

//! 落点在既有音符**内部**：前半必须按原样保留（是什么音就还是什么音，只是变短），
//! 后半也要留着。切一刀就把那个音抹掉是这一功能最容易犯的错，而且用户只会觉得"我的谱被吃了"。
TEST_F(MidiRecordedWriteTests, LandingInsideANoteKeepsThePartBeforeIt)
{
    //! `data/test.mscx` 第 1 小节：60@0、62@480、64@960、65@1440（各 480）。
    const int inside = 720;          //!< 62 那个四分音符（480..960）的正中间

    std::vector<MidiRecordedChord> take;
    take.push_back(chord(inside, 120, { 71 }));

    const MidiRecordedWriteResult written = applyRecordedChords(m_score, 0, 0, take);
    EXPECT_EQ(written.chordsWritten, 1);
    EXPECT_EQ(written.notesWritten, 1);

    const std::vector<MidiNoteItem> items = collectMidiNotes(m_score);

    const MidiNoteItem* newNote = itemAt(items, inside, 71);
    ASSERT_NE(newNote, nullptr);
    EXPECT_EQ(newNote->durationTicks, 120);

    const MidiNoteItem* head = itemAt(items, 480, 62);
    ASSERT_NE(head, nullptr) << "the note before the take was wiped out";
    EXPECT_EQ(head->durationTicks, 240) << "the head should have been shortened, not deleted";

    const MidiNoteItem* tail = itemAt(items, inside + 120, 62);
    ASSERT_NE(tail, nullptr) << "the tail of the split note disappeared";
    EXPECT_EQ(tail->durationTicks, 120);
}

//! 谱面之外不写：`setNoteRest` 其实会自己加小节，但那会让"跟着伴奏录一段"变成
//! "莫名其妙多出几十小节"。跳过的数量必须报出来 —— 丢音是静默的，用户只会觉得"录少了"。
TEST_F(MidiRecordedWriteTests, WhatIsPastTheEndOfTheScoreIsReportedNotWritten)
{
    const int beyond = m_score->endTick().ticks() + QUARTER;

    std::vector<MidiRecordedChord> take;
    take.push_back(chord(beyond, QUARTER, { 60 }));
    take.push_back(chord(beyond + QUARTER, QUARTER, { 62 }));

    const MidiRecordedWriteResult written = applyRecordedChords(m_score, 0, 0, take);

    EXPECT_EQ(written.chordsWritten, 0);
    EXPECT_EQ(written.notesWritten, 0);
    EXPECT_EQ(written.chordsSkipped, 2);
    EXPECT_EQ(written.lastTick, -1);
    EXPECT_EQ(itemAt(collectMidiNotes(m_score), beyond, 60), nullptr);
}

TEST_F(MidiRecordedWriteTests, ApplyingNothingIsHarmless)
{
    EXPECT_EQ(applyRecordedChords(m_score, 0, 0, {}).chordsWritten, 0);
    EXPECT_EQ(applyRecordedChords(nullptr, 0, 0, { chord(0, QUARTER, { 60 }) }).chordsWritten, 0);
    EXPECT_EQ(applyRecordedChords(m_score, 99, 0, { chord(0, QUARTER, { 60 }) }).chordsWritten, 0);
    EXPECT_EQ(applyRecordedChords(m_score, 0, 0, { chord(0, QUARTER, {}) }).chordsSkipped, 1);
}

//! 录进去的东西必须**存得住**、而且存出来的工程还能被真实程序打开。
//! 设 `MUSE_MIDIEDITOR_KEEP_SCORE` 会把文件留在 %TEMP%，供真实 exe 端到端复核
//! （与 `PlayedTimingSurvivesSaveAndReload` 同一套做法，见 `维护手册.md` §6.3）。
TEST_F(MidiRecordedWriteTests, ATakeSurvivesSaveAndReload)
{
    const QString savedPath = QDir::tempPath() + QStringLiteral("/dsh-midieditor-recorded.mscx");
    QFile::remove(savedPath);

    const int start = m_secondMeasure->tick().ticks();

    std::vector<MidiRecordedChord> take;
    take.push_back(chord(start + 0 * QUARTER, QUARTER, { 60 }, 90));
    take.push_back(chord(start + 1 * QUARTER, QUARTER, { 62, 64 }, 80));
    take.push_back(chord(start + 2 * QUARTER, QUARTER * 2, { 67 }, 100));

    ASSERT_EQ(applyRecordedChords(m_score, 0, 0, take).notesWritten, 4);

    ASSERT_TRUE(ScoreRW::saveScore(m_score, savedPath)) << "could not save the score";
    delete m_score;
    m_score = nullptr;

    MasterScore* reloaded = ScoreRW::readScore(savedPath, /*isAbsolutePath*/ true);
    ASSERT_TRUE(reloaded) << "could not reopen the saved score";

    const std::vector<MidiNoteItem> again = collectMidiNotes(reloaded);

    const MidiNoteItem* first = itemAt(again, start, 60);
    ASSERT_NE(first, nullptr) << "the recorded note was lost on save/reload";
    EXPECT_EQ(first->durationTicks, QUARTER);
    EXPECT_EQ(first->velocity, 90);

    EXPECT_NE(itemAt(again, start + QUARTER, 62), nullptr);
    EXPECT_NE(itemAt(again, start + QUARTER, 64), nullptr);
    EXPECT_NE(itemAt(again, start + 2 * QUARTER, 67), nullptr);

    delete reloaded;
}
