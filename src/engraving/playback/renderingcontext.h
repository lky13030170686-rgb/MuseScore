/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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

#include <algorithm>
#include <cmath>

#include "mpe/events.h"

#include "../dom/chord.h"
#include "../dom/note.h"
#include "../dom/sig.h"

#include "global/realfn.h"

#include "utils/arrangementutils.h"
#include "utils/pitchutils.h"
#include "playbackcontext.h"

namespace mu::engraving {
struct RenderingContext {
    muse::mpe::timestamp_t nominalTimestamp = 0;
    muse::mpe::duration_t nominalDuration = 0;
    muse::mpe::dynamic_level_t nominalDynamicLevel = 0;
    int nominalPositionStartTick = 0;
    int nominalPositionEndTick = 0;
    int nominalDurationTicks = 0;
    int positionTickOffset = 0;

    BeatsPerSecond beatsPerSecond = 0;
    TimeSigFrac timeSignatureFraction;

    muse::mpe::ArticulationMap commonArticulations;

    const Score* score = nullptr;
    const muse::mpe::ArticulationsProfilePtr profile;
    const PlaybackContextPtr playbackCtx;

    bool isValid() const
    {
        return score
               && profile
               && playbackCtx
               && beatsPerSecond > 0
               && nominalDuration > 0
               && nominalDurationTicks > 0;
    }
};

inline RenderingContext buildRenderingCtx(const Chord* chord, const int tickPositionOffset,
                                          const muse::mpe::ArticulationsProfilePtr profile, const PlaybackContextPtr playbackCtx,
                                          const muse::mpe::ArticulationMap& articulations = {})
{
    int chordPosTick = chord->tick().ticks();
    int chordDurationTicks = chord->actualTicks().ticks();
    int chordPosTickWithOffset = chordPosTick + tickPositionOffset;

    const Score* score = chord->score();

    auto chordTnD = timestampAndDurationFromStartAndDurationTicks(score, chordPosTick, chordDurationTicks, tickPositionOffset);

    BeatsPerSecond bps = score->multipliedTempoAtUtick(chordPosTickWithOffset);
    TimeSigFrac timeSignatureFraction = score->sigmap()->timesig(chordPosTick).timesig();

    RenderingContext ctx{ chordTnD.timestamp,
                          chordTnD.duration,
                          playbackCtx->appliableDynamicLevel(chord->track(), chordPosTickWithOffset),
                          chordPosTick,
                          chordPosTick + chordDurationTicks,
                          chordDurationTicks,
                          tickPositionOffset,
                          bps,
                          timeSignatureFraction,
                          articulations,
                          score,
                          profile,
                          playbackCtx };

    return ctx;
}

struct NominalNoteCtx {
    voice_idx_t voiceIdx = 0;
    staff_idx_t staffIdx = 0;
    muse::mpe::timestamp_t timestamp = 0;
    muse::mpe::duration_t duration = 0;
    BeatsPerSecond tempo = 0;
    muse::mpe::dynamic_level_t dynamicLevel = 0;
    float userVelocityFraction = 0.f;

    muse::mpe::pitch_level_t pitchLevel = 0;

    //! [our addition] The loudness SHAPE of this one note, when the piano roll drew one.
    //!
    //! Empty means "the note is equally loud throughout", which is the normal case. A non-empty curve
    //! is a crescendo / diminuendo / fp INSIDE a single note: the key is where in the note (in
    //! thousandths of its length) and the value is how loud it should be there.
    muse::mpe::ExpressionCurve expressionCurve;

    RenderingContext chordCtx;
    muse::mpe::ArticulationMap articulations;

    explicit NominalNoteCtx(const Note* note, const RenderingContext& ctx)
        : voiceIdx(note->voice()),
        staffIdx(note->staffIdx()),
        timestamp(ctx.nominalTimestamp),
        duration(ctx.nominalDuration),
        tempo(ctx.beatsPerSecond),
        dynamicLevel(ctx.nominalDynamicLevel),
        userVelocityFraction(note->userVelocityFraction()),
        pitchLevel(notePitchLevel(note->playingTpc(),
                                  note->playingOctave(),
                                  note->playingTuning())),
        chordCtx(ctx),
        articulations(ctx.commonArticulations)
    {
        applyPlayEventsOverride(note, ctx);
    }

    //! NOTE: [our addition] Honour the per-note playback overrides written by the MIDI (piano roll)
    //! page - `NoteEvent::ontime`, `len` and `velocityMultiplier`.
    //!
    //! Until now the only reader of `Note::playEvents()` was the legacy MIDI export
    //! (compatmidirenderinternal.cpp), so editing those values had no audible effect on playback at
    //! all. That is exactly upstream issue #20235 ("Play durations altered via the Piano Roll Editor
    //! are not conveyed from MS3.6/3.7 to MuseScore 4.1"). Reading them here makes the played timing
    //! and velocity follow.
    //!
    //! The score reader may already fill the list in, but then it holds the neutral default
    //! (ontime 0, len NOTE_LENGTH, velocityMultiplier 1.0), which this function leaves alone: the
    //! timestamp/duration are only recomputed when they actually differ, and the dynamic level only
    //! when the multiplier differs from 1.0. So a score nobody edited in the piano roll renders
    //! exactly what the notation says - the same as before this change.
    void applyPlayEventsOverride(const Note* note, const RenderingContext& ctx)
    {
        const NoteEventList& events = note->playEvents();
        if (events.empty() || !ctx.score) {
            return;
        }

        const NoteEvent& event = events.front();

        // ontime/len are thousandths of the nominal note length, the same unit the legacy renderer
        // uses: on = tick1 + (ticks * ontime) / 1000, off = on + (ticks * len) / 1000.
        if (event.play() && ctx.nominalDurationTicks > 0) {
            const int nominalTicks = ctx.nominalDurationTicks;
            const int shiftTicks = (nominalTicks * event.ontime()) / 1000;
            const int lenTicks = std::max(1, (nominalTicks * event.len()) / 1000);

            if (shiftTicks != 0 || lenTicks != nominalTicks) {
                const muse::mpe::TimestampAndDuration tnD = timestampAndDurationFromStartAndDurationTicks(
                    ctx.score, ctx.nominalPositionStartTick + shiftTicks, lenTicks, ctx.positionTickOffset);
                timestamp = tnD.timestamp;
                duration = tnD.duration;
            }
        }

        if (!muse::RealIsNull(event.velocityMultiplier() - NoteEvent::DEFAULT_VELOCITY_MULTIPLIER)) {
            const long long scaled = std::llround(static_cast<double>(dynamicLevel) * event.velocityMultiplier());
            dynamicLevel = static_cast<muse::mpe::dynamic_level_t>(
                std::clamp(scaled,
                           static_cast<long long>(muse::mpe::MIN_DYNAMIC_LEVEL),
                           static_cast<long long>(muse::mpe::MAX_DYNAMIC_LEVEL)));

            //! NOTE: a note that carries its own velocity gets `ExpressionContext::velocityOverride`
            //! built from `userVelocityFraction`, and the synthesisers prefer that override over the
            //! dynamic level - so scaling only the dynamic level would leave the played velocity
            //! untouched and the multiplier would be silently ignored. Scale the override too, so
            //! the two ways of adjusting one note's loudness compose instead of fighting.
            if (!muse::RealIsNull(userVelocityFraction)) {
                userVelocityFraction = std::clamp(userVelocityFraction * static_cast<float>(event.velocityMultiplier()), 0.f, 1.f);
            }
        }

        //! [our addition] More than one event means the note carries a loudness SHAPE, not just a
        //! value: each event contributes a point at its own ontime, and its multiplier says how loud
        //! the note should be at that point. That is what a crescendo, a diminuendo or an fp inside a
        //! single note needs.
        //!
        //! Note carefully what this is NOT: the note is still ONE note with ONE note-on. The events
        //! only describe how its loudness moves while it sounds. Playing them as separate note-ons -
        //! which is what the raw list looks like - would make a crescendo sound like the note being
        //! struck again and again, which is why the shape is read out here instead.
        //!
        //! `ontime` is in thousandths of the note, while the curve's key is in hundredths of a
        //! percent (HUNDRED_PERCENT == 10000), hence the scaling.
        if (events.size() > 1) {
            muse::mpe::ExpressionCurve curve;

            for (const NoteEvent& each : events) {
                if (!each.play()) {
                    continue;
                }

                const long long scaled = std::llround(static_cast<double>(ctx.nominalDynamicLevel) * each.velocityMultiplier());
                const muse::mpe::dynamic_level_t level = static_cast<muse::mpe::dynamic_level_t>(
                    std::clamp(scaled,
                               static_cast<long long>(muse::mpe::MIN_DYNAMIC_LEVEL),
                               static_cast<long long>(muse::mpe::MAX_DYNAMIC_LEVEL)));

                const muse::mpe::duration_percentage_t at = static_cast<muse::mpe::duration_percentage_t>(
                    std::clamp(each.ontime(), 0, NoteEvent::NOTE_LENGTH) * (muse::mpe::HUNDRED_PERCENT / NoteEvent::NOTE_LENGTH));

                curve.insert_or_assign(at, level);
            }

            //! One point is not a shape - and leaving the curve empty keeps every existing score on
            //! exactly the path it took before.
            if (curve.size() > 1) {
                expressionCurve = curve;
            }
        }
    }
};

inline muse::mpe::NoteEvent buildNoteEvent(const NominalNoteCtx& ctx, const muse::mpe::PitchCurve& pitchCurve = {})
{
    muse::mpe::NoteEvent event(ctx.timestamp,
                               ctx.duration,
                               static_cast<muse::mpe::voice_layer_idx_t>(ctx.voiceIdx),
                               static_cast<muse::mpe::staff_layer_idx_t>(ctx.staffIdx),
                               ctx.pitchLevel,
                               ctx.dynamicLevel,
                               ctx.articulations,
                               ctx.tempo.val,
                               ctx.userVelocityFraction,
                               pitchCurve);

    //! [our addition] The constructor derives the loudness shape from the articulations. A shape
    //! drawn in the piano roll is about THIS note, so it takes precedence when there is one.
    if (!ctx.expressionCurve.empty()) {
        event.setExpressionCurve(ctx.expressionCurve);
    }

    return event;
}
}
