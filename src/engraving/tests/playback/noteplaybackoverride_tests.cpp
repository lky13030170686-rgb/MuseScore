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
#include <cmath>
#include <memory>

#include "mpe/tests/utils/articulationutils.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/noteevent.h"
#include "engraving/dom/segment.h"

#include "engraving/playback/playbackeventsrenderer.h"

#include "utils/scorerw.h"

using namespace mu::engraving;
using namespace muse;
using namespace muse::mpe;

static const String DATA_DIR("playback/playbackeventsrenderer_data/");

//! 4/4 at 120 BPM, so a quarter note lasts half a second.
static constexpr duration_t QUARTER_NOTE_DURATION = 500000;
static constexpr int QUARTER_NOTE_TICKS = 480;

/*!
 * The MIDI (piano roll) page lets the user override, per note, when it starts playing, how long it
 * plays and how loud it is - that is `NoteEvent::ontime` / `len` / `velocityMultiplier` inside
 * `Note::playEvents()`.
 *
 * These tests pin down both halves of the contract:
 *   * an untouched score (no events at all) renders EXACTLY as before - that is what makes the
 *     change safe for every existing project;
 *   * when an override is present, playback actually follows it - which it did not before this
 *     change (the values were only read by the legacy MIDI export).
 */
class Engraving_NotePlaybackOverrideTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_dummyPatternSegment.arrangementPattern
            = tests::createArrangementPattern(HUNDRED_PERCENT /*duration_factor*/, 0 /*timestamp_offset*/);
        m_dummyPatternSegment.pitchPattern = tests::createSimplePitchPattern(0 /*increment_pitch_diff*/);
        m_dummyPatternSegment.expressionPattern
            = tests::createSimpleExpressionPattern(dynamicLevelFromType(mpe::DynamicType::Natural));
        m_dummyPattern.emplace(0, m_dummyPatternSegment);

        m_defaultProfile = std::make_shared<ArticulationsProfile>();
        m_defaultProfile->setPattern(ArticulationType::Standard, m_dummyPattern);
    }

    //! A single quarter note on F4 in 4/4 at 120 BPM.
    static Score* readSingleNoteScore()
    {
        return ScoreRW::readScore(DATA_DIR + "single_note_no_articulations/no_articulations.mscx");
    }

    static Chord* singleChordOf(Score* score)
    {
        if (!score || !score->firstMeasure()) {
            return nullptr;
        }

        Segment* segment = score->firstMeasure()->segments().firstCRSegment();
        return segment ? toChord(segment->nextChordRest(0)) : nullptr;
    }

    mpe::NoteEvent renderFirstEvent(Chord* chord, const Score* score)
    {
        PlaybackContextPtr ctx = std::make_shared<PlaybackContext>(score);
        PlaybackEventsMap result;
        m_renderer.render(chord, 0, m_defaultProfile, ctx, result);

        EXPECT_EQ(result.size(), 1);
        EXPECT_FALSE(result.begin()->second.empty());

        return std::get<mpe::NoteEvent>(result.begin()->second.front());
    }

    //! Mimics what the piano roll does: set the override and mark the chord as user-edited.
    static void setOverride(Chord* chord, int ontime, int len, double velocityMultiplier)
    {
        mu::engraving::NoteEvent event;
        event.setOntime(ontime);
        event.setLen(len);
        event.setVelocityMultiplier(velocityMultiplier);

        mu::engraving::NoteEventList events;
        events.push_back(event);

        for (Note* note : chord->notes()) {
            note->setPlayEvents(events);
        }

        chord->setPlayEventType(PlayEventType::User);
    }

    ArticulationsProfilePtr m_defaultProfile = nullptr;

    ArticulationPattern m_dummyPattern;
    ArticulationPatternSegment m_dummyPatternSegment;

    PlaybackEventsRenderer m_renderer;
};

//! The whole point of the "empty list short-circuit": an untouched score must not change at all.
TEST_F(Engraving_NotePlaybackOverrideTests, AScoreNobodyEditedRendersExactlyAsTheNotationSays)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    //! NOTE: a fresh score may or may not carry an event list - the score reader fills one in - but
    //!       whatever it carries has to be the NEUTRAL default. That is what makes the change safe:
    //!       with neutral values the played timing is the notated one, bit for bit.
    for (const Note* note : chord->notes()) {
        for (const mu::engraving::NoteEvent& event : note->playEvents()) {
            EXPECT_EQ(event.ontime(), 0);
            EXPECT_EQ(event.len(), mu::engraving::NoteEvent::NOTE_LENGTH);
            EXPECT_DOUBLE_EQ(event.velocityMultiplier(), mu::engraving::NoteEvent::DEFAULT_VELOCITY_MULTIPLIER);
        }
    }

    const mpe::NoteEvent event = renderFirstEvent(chord, score);
    const mpe::timestamp_t timestamp = event.arrangementCtx().nominalTimestamp;
    const mpe::duration_t duration = event.arrangementCtx().nominalDuration;
    const mpe::dynamic_level_t dynamicLevel = event.expressionCtx().nominalDynamicLevel;

    EXPECT_EQ(timestamp, 0);
    EXPECT_EQ(duration, QUARTER_NOTE_DURATION);

    delete score;

    //! A second render of the same (untouched) score has to agree - no hidden state.
    Score* again = readSingleNoteScore();
    ASSERT_TRUE(again);

    const mpe::NoteEvent repeat = renderFirstEvent(singleChordOf(again), again);
    EXPECT_EQ(repeat.arrangementCtx().nominalTimestamp, timestamp);
    EXPECT_EQ(repeat.arrangementCtx().nominalDuration, duration);
    EXPECT_EQ(repeat.expressionCtx().nominalDynamicLevel, dynamicLevel);

    delete again;
}

//! ontime is in thousandths of the nominal length, so 500 pushes the start by half a quarter note.
TEST_F(Engraving_NotePlaybackOverrideTests, OnTimePushesThePlayedStartLater)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    setOverride(chord, 500 /*ontime*/, mu::engraving::NoteEvent::NOTE_LENGTH /*len*/,
                mu::engraving::NoteEvent::DEFAULT_VELOCITY_MULTIPLIER);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);

    //! Starting later does not shorten the note: len is still the full nominal length.
    EXPECT_EQ(event.arrangementCtx().nominalTimestamp, QUARTER_NOTE_DURATION / 2);
    EXPECT_EQ(event.arrangementCtx().nominalDuration, QUARTER_NOTE_DURATION);

    delete score;
}

//! len is a multiplier in thousandths, so 2000 plays the note twice as long as notated.
TEST_F(Engraving_NotePlaybackOverrideTests, LengthScalesThePlayedDuration)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    setOverride(chord, 0, 2000, mu::engraving::NoteEvent::DEFAULT_VELOCITY_MULTIPLIER);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);

    EXPECT_EQ(event.arrangementCtx().nominalTimestamp, 0);
    EXPECT_EQ(event.arrangementCtx().nominalDuration, QUARTER_NOTE_DURATION * 2);

    delete score;
}

//! A shorter-than-notated length is what a staccato is; make sure it is honoured too.
TEST_F(Engraving_NotePlaybackOverrideTests, ShorteningKeepsTheNoteOnTheSameBeat)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    setOverride(chord, 0, 250, mu::engraving::NoteEvent::DEFAULT_VELOCITY_MULTIPLIER);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);

    EXPECT_EQ(event.arrangementCtx().nominalTimestamp, 0);
    EXPECT_EQ(event.arrangementCtx().nominalDuration, QUARTER_NOTE_DURATION / 4);

    delete score;
}

//! The velocity multiplier only ever lowers the level (1.0 = untouched).
TEST_F(Engraving_NotePlaybackOverrideTests, VelocityMultiplierLowersTheDynamicLevel)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    const mpe::dynamic_level_t plain = renderFirstEvent(chord, score).expressionCtx().nominalDynamicLevel;

    setOverride(chord, 0, mu::engraving::NoteEvent::NOTE_LENGTH, 0.5);

    const mpe::dynamic_level_t quieter = renderFirstEvent(chord, score).expressionCtx().nominalDynamicLevel;

    EXPECT_LT(quieter, plain);
    EXPECT_EQ(quieter, static_cast<mpe::dynamic_level_t>(std::llround(static_cast<double>(plain) * 0.5)));

    delete score;
}

//! ontime and len together describe "starts a bit late and plays short" - the usual result of
//! dragging a note in a piano roll.
TEST_F(Engraving_NotePlaybackOverrideTests, OnTimeAndLengthCombine)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    setOverride(chord, 250 /*late by a quarter of the note*/, 500 /*half the note*/, 1.0);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);

    EXPECT_EQ(event.arrangementCtx().nominalTimestamp, QUARTER_NOTE_DURATION / 4);
    EXPECT_EQ(event.arrangementCtx().nominalDuration, QUARTER_NOTE_DURATION / 2);

    delete score;
}

//! A zero or negative length must never produce a negative duration.
TEST_F(Engraving_NotePlaybackOverrideTests, ADegenerateLengthStillProducesAValidDuration)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    setOverride(chord, 0, 0, 1.0);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);

    EXPECT_GT(event.arrangementCtx().nominalDuration, 0);

    delete score;
}

/*!
 * The per-note velocity path.
 *
 * `Pid::USER_VELOCITY` does NOT feed the dynamic level - it becomes
 * `ExpressionContext::velocityOverride`, which every synthesiser prefers over the dynamic level.
 * That is precisely what makes "keep the dynamics in charge, but tweak a single note" possible, and
 * these tests pin both directions of it: setting it takes over, clearing it hands control back.
 */
TEST_F(Engraving_NotePlaybackOverrideTests, AnOwnVelocityBecomesAVelocityOverride)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    Note* note = chord->notes().front();
    ASSERT_TRUE(note);

    //! Nothing set: the note follows the dynamic marks, so there must be no override.
    ASSERT_FALSE(renderFirstEvent(chord, score).expressionCtx().velocityOverride.has_value())
        << "a note with no own velocity must not override the dynamics";

    note->setUserVelocity(100);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);
    ASSERT_TRUE(event.expressionCtx().velocityOverride.has_value());
    EXPECT_NEAR(event.expressionCtx().velocityOverride.value(), 100.f / 127.f, 0.01f);

    //! Clearing it again has to hand the note back to the dynamics - otherwise a tweak would be
    //! one-way, which is exactly what the MIDI page's right-click is for.
    note->setUserVelocity(0);

    EXPECT_FALSE(renderFirstEvent(chord, score).expressionCtx().velocityOverride.has_value())
        << "clearing the own velocity must return the note to the dynamic marks";

    delete score;
}

//! The velocity override is stronger than the dynamic level, so the played-length multiplier has to
//! reach it as well - otherwise it would be silently ignored on a note that has its own velocity.
TEST_F(Engraving_NotePlaybackOverrideTests, TheMultiplierAlsoReachesAnOwnVelocity)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    Note* note = chord->notes().front();
    ASSERT_TRUE(note);
    note->setUserVelocity(100);

    setOverride(chord, 0, mu::engraving::NoteEvent::NOTE_LENGTH, 0.5);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);

    ASSERT_TRUE(event.expressionCtx().velocityOverride.has_value());
    EXPECT_NEAR(event.expressionCtx().velocityOverride.value(), (100.f / 127.f) * 0.5f, 0.01f);

    delete score;
}

//! ...and with no own velocity the multiplier keeps working through the dynamic level as before.
TEST_F(Engraving_NotePlaybackOverrideTests, TheMultiplierStillWorksWithoutAnOwnVelocity)
{
    Score* score = readSingleNoteScore();
    ASSERT_TRUE(score);

    Chord* chord = singleChordOf(score);
    ASSERT_TRUE(chord);

    const mpe::dynamic_level_t plain = renderFirstEvent(chord, score).expressionCtx().nominalDynamicLevel;

    setOverride(chord, 0, mu::engraving::NoteEvent::NOTE_LENGTH, 0.5);

    const mpe::NoteEvent event = renderFirstEvent(chord, score);
    EXPECT_FALSE(event.expressionCtx().velocityOverride.has_value());
    EXPECT_LT(event.expressionCtx().nominalDynamicLevel, plain);

    delete score;
}
