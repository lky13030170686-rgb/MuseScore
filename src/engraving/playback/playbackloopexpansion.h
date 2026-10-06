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

#pragma once

#include <vector>

namespace mu::engraving {
class Measure;
class RepeatList;

//--------------------------------------------------------------------------------------------------
// PlaybackLoopExpansion
//
//! NOTE: Loop playback, done the way the native repeat playback is done.
//!
//! Native repeats never seek: RepeatList expands the score into a utick timeline and
//! PlaybackModel lays the events out on it, so the player just plays forward. A loop, however,
//! used to be implemented by seeking the sequencer back to the loop start on every wrap, which
//! flushes the sound sources (an audible seam), drops the last block before the loop end and
//! forces the engine to fight the "tracks are rendered before the clock advances" order.
//!
//! This struct describes the same trick applied to the loop: the region [loopInUtick, loopOutUtick)
//! of the native timeline is laid out `passes` times in place, and everything after it is shifted
//! accordingly:
//!
//!     [0, loopIn) | loop | loop | ... | loop | [loopOut, baseTicks) + (passes - 1) * loopLength
//!
//! The playback timeline stays linear and strictly forward - one seek, no flush, no gap, and the
//! loop is exactly as long as the notation says.
//--------------------------------------------------------------------------------------------------

struct PlaybackLoopExpansion
{
    bool enabled = false;
    int loopInUtick = 0;
    int loopOutUtick = 0;
    int passes = 1;      //! total number of times the loop region is played (>= 2 when active)
    int baseTicks = 0;   //! length of the not-expanded (native repeats only) playback timeline

    bool isActive() const { return enabled && passes >= MIN_PASSES_WHEN_ACTIVE && loopOutUtick > loopInUtick && baseTicks > 0; }

    int loopLength() const { return loopOutUtick - loopInUtick; }

    //! Length of the expanded playback timeline
    int expandedTicks() const { return isActive() ? baseTicks + (passes - 1) * loopLength() : baseTicks; }

    //! Expanded utick -> utick on the native (repeats only) timeline
    int toBaseUtick(int utick) const;

    //! Native utick -> its first occurrence on the expanded timeline
    int fromBaseUtick(int utick) const;

    //! A loop is only expanded when the region is played at least twice
    static constexpr int MIN_PASSES_WHEN_ACTIVE = 2;

    //! How much loop playback the expansion should cover, and how many passes that may cost
    static constexpr double EXPANSION_BUDGET_SECS = 600.0;
    static constexpr int EXPANSION_MAX_PASSES = 128;
};

inline bool operator==(const PlaybackLoopExpansion& lhs, const PlaybackLoopExpansion& rhs)
{
    return lhs.enabled == rhs.enabled
           && lhs.loopInUtick == rhs.loopInUtick
           && lhs.loopOutUtick == rhs.loopOutUtick
           && lhs.passes == rhs.passes
           && lhs.baseTicks == rhs.baseTicks;
}

inline bool operator!=(const PlaybackLoopExpansion& lhs, const PlaybackLoopExpansion& rhs)
{
    return !(lhs == rhs);
}

//--------------------------------------------------------------------------------------------------
// PlaybackTimelineSegment
//
//! NOTE: A contiguous piece of the playback timeline. Everything the measures list contains is
//! rendered at `utick + (its own tick - tick)`, exactly like a RepeatSegment.
//--------------------------------------------------------------------------------------------------

struct PlaybackTimelineSegment
{
    int utick = 0;
    int tick = 0;
    int endTick = 0;
    std::vector<const Measure*> measures;
};

//! Lays out the playback timeline: the native repeat segments, split where the loop region begins
//! and ends, with the loop region repeated `passes` times in place.
std::vector<PlaybackTimelineSegment> buildPlaybackTimeline(const RepeatList& repeats, const PlaybackLoopExpansion& loop);

//! `loopSeconds` is how long the loop region lasts; the number of passes is chosen so that the
//! expansion covers EXPANSION_BUDGET_SECS of looping, clamped to EXPANSION_MAX_PASSES.
PlaybackLoopExpansion makePlaybackLoopExpansion(const RepeatList& repeats, int loopInRawTick, int loopOutRawTick, double loopSeconds,
                                                bool enabled);
}
