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

#include "playbackloopexpansion.h"

#include <algorithm>
#include <cmath>

#include "../dom/measure.h"
#include "../dom/repeatlist.h"

using namespace mu::engraving;

int PlaybackLoopExpansion::toBaseUtick(int utick) const
{
    if (!isActive()) {
        return utick;
    }

    const int length = loopLength();

    if (utick < loopInUtick) {
        return utick;
    }

    const int loopedEnd = loopInUtick + passes * length;
    if (utick < loopedEnd) {
        return loopInUtick + (utick - loopInUtick) % length;
    }

    return utick - (passes - 1) * length;
}

int PlaybackLoopExpansion::fromBaseUtick(int utick) const
{
    if (!isActive() || utick < loopOutUtick) {
        //! NOTE: the prefix and the first pass keep their positions
        return utick;
    }

    return utick + (passes - 1) * loopLength();
}

static void appendTimelineRange(std::vector<PlaybackTimelineSegment>& result, const RepeatList& repeats,
                                int fromUtick, int toUtick, int utickShift)
{
    if (fromUtick >= toUtick) {
        return;
    }

    for (const RepeatSegment* repeat : repeats) {
        const int repeatEndUtick = repeat->utick + (repeat->endTick() - repeat->tick);

        const int from = std::max(repeat->utick, fromUtick);
        const int to = std::min(repeatEndUtick, toUtick);
        if (from >= to) {
            continue;
        }

        PlaybackTimelineSegment segment;
        segment.utick = from + utickShift;
        segment.tick = repeat->tick + (from - repeat->utick);
        segment.endTick = repeat->tick + (to - repeat->utick);

        for (const Measure* measure : repeat->measureList()) {
            if (measure->endTick().ticks() <= segment.tick || measure->tick().ticks() >= segment.endTick) {
                continue;
            }

            segment.measures.push_back(measure);
        }

        if (segment.measures.empty()) {
            continue;
        }

        result.push_back(std::move(segment));
    }
}

std::vector<PlaybackTimelineSegment> mu::engraving::buildPlaybackTimeline(const RepeatList& repeats, const PlaybackLoopExpansion& loop)
{
    std::vector<PlaybackTimelineSegment> result;

    if (!loop.isActive()) {
        for (const RepeatSegment* repeat : repeats) {
            PlaybackTimelineSegment segment;
            segment.utick = repeat->utick;
            segment.tick = repeat->tick;
            segment.endTick = repeat->endTick();
            segment.measures = repeat->measureList();
            result.push_back(std::move(segment));
        }

        return result;
    }

    const int length = loop.loopLength();

    appendTimelineRange(result, repeats, 0, loop.loopInUtick, 0 /*utickShift*/);

    for (int pass = 0; pass < loop.passes; ++pass) {
        appendTimelineRange(result, repeats, loop.loopInUtick, loop.loopOutUtick, pass * length);
    }

    appendTimelineRange(result, repeats, loop.loopOutUtick, loop.baseTicks, (loop.passes - 1) * length);

    return result;
}

PlaybackLoopExpansion mu::engraving::makePlaybackLoopExpansion(const RepeatList& repeats, int loopInRawTick, int loopOutRawTick,
                                                               double loopSeconds, bool enabled)
{
    PlaybackLoopExpansion expansion;
    expansion.enabled = enabled;
    expansion.baseTicks = repeats.ticks();

    if (!enabled || loopOutRawTick <= loopInRawTick) {
        return expansion;
    }

    const int loopInUtick = repeats.tick2utick(loopInRawTick);
    const int loopOutUtick = repeats.tick2utick(loopOutRawTick);
    if (loopOutUtick <= loopInUtick) {
        return expansion;
    }

    int passes = PlaybackLoopExpansion::EXPANSION_MAX_PASSES;
    if (loopSeconds > 0.0) {
        passes = int(std::ceil(PlaybackLoopExpansion::EXPANSION_BUDGET_SECS / loopSeconds));
    }

    passes = std::clamp(passes, PlaybackLoopExpansion::MIN_PASSES_WHEN_ACTIVE, PlaybackLoopExpansion::EXPANSION_MAX_PASSES);

    expansion.loopInUtick = loopInUtick;
    expansion.loopOutUtick = loopOutUtick;
    expansion.passes = passes;

    return expansion;
}
