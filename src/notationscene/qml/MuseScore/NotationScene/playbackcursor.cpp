/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include "playbackcursor.h"

#include "draw/painter.h"

#include "engraving/dom/measure.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/system.h"

#include "notation/inotation.h"
#include "notation/inotationelements.h" // IWYU pragma: keep

using namespace mu::engraving;
using namespace mu::notation;
using namespace muse;

static double systemBottomY(const Score* score, const System* system)
{
    double systemBottomY = 0.0;

    for (size_t i = 0; i < score->nstaves(); ++i) {
        const SysStaff* ss = system->staff(i);
        //! `staff(i)` returns nullptr for an index the system does not carry; dereferencing it
        //! here used to be one of the ways this path died (see resolveCursorRectByTick).
        if (!ss || !score->staff(i) || !ss->show() || !score->staff(i)->show()) {
            continue;
        }

        systemBottomY = ss->bbox().bottom();
    }

    return systemBottomY;
}

static RectF calculateRect(double x, const Score* score, const System* system, double systemBottomY)
{
    const double spatium = score->style().spatium();

    return RectF {
        x - spatium,
        //! `staffCanvasYpage(0)` walks `m_staves[0]` and then `page()->canvasPos()`; it is only
        //! safe on a system that belongs to the *current* layout (checked by the caller).
        system->staffCanvasYpage(0) - 3.0 * spatium,
        0.4 * spatium,
        systemBottomY + 6.0 * spatium
    };
}

void PlaybackCursor::paint(muse::draw::Painter* painter)
{
    if (!m_visible) {
        return;
    }

    painter->fillRect(m_rect, color());
}

void PlaybackCursor::setNotation(INotationPtr notation)
{
    if (m_notation == notation) {
        return;
    }

    //! No cache invalidation to do any more (see resolveCursorRectByTick): the cursor resolves
    //! Measure/System/Segment from the score on every move, so re-layouts cannot leave it
    //! holding freed objects. The old `changesChannel` / `viewModeChanged` subscriptions existed
    //! only to clear that cache, and they were never enough — a dock resize (mixer) re-layouts
    //! the score without emitting either of them.
    m_notation = notation;
}

void PlaybackCursor::move(muse::midi::tick_t tick)
{
    m_rect = resolveCursorRectByTick(tick);
}

muse::RectF PlaybackCursor::resolveCursorRectByTick(int _tick) const
{
    const Score* score = m_notation ? m_notation->elements()->msScore() : nullptr;
    if (!score) {
        return RectF();
    }

    auto interpolateXByTicks = [](int tick, int startTick, int endTick,
                                  double segmentStartX, double segmentEndX) {
        const int durationTicks = endTick - startTick;
        return durationTicks > 0
               ? segmentStartX + (segmentEndX - segmentStartX) * double(tick - startTick) / double(durationTicks)
               : segmentStartX;
    };

    const Fraction tick = Fraction::fromTicks(_tick);

    //! ⛔⛔ **No cross-call caching here, on purpose** (2026-11-06, 进度快照 第 176 条).
    //!
    //! This used to keep `const System*/Measure*/Segment*` between calls. A re-layout —
    //! opening/closing a dock panel (the mixer changes the central widget's width), a
    //! zoom-to-fit, a page/continuous mode change — **destroys and rebuilds** those objects,
    //! and the score does *not* announce it through `changesChannel()` (that channel only
    //! fires on edits). The cached pointers then pointed into freed memory and the 23 ms
    //! playback-cursor timer walked them into `System::staffCanvasYpage()` →
    //! `page()->canvasPos()` → a **virtual call through a dead object** → `execute of
    //! address 0x0` (5+ crashes on 2026-11-06, all while playing).
    //!
    //! Re-resolving on every call costs a measure-map lookup plus one scan of the measure's
    //! chord-rest segments — at the cursor's 43 Hz that is nothing, and it cannot dangle.
    const Measure* measure = score->tick2measureMM(tick);
    if (!measure) {
        return RectF();
    }

    const System* system = measure->system();
    if (!system || !system->page() || system->staves().empty()) {
        return RectF();
    }

    const double systemBottom = systemBottomY(score, system);

    for (const Segment* s = measure->first(SegmentType::ChordRest); s;) {
        const Fraction segmentStartTick = s->tick();
        const double segmentStartX = s->canvasPos().x();

        const Segment* nextSegment = s->next(SegmentType::ChordRest);
        while (nextSegment && !nextSegment->visible()) {
            nextSegment = nextSegment->next(SegmentType::ChordRest);
        }

        Fraction segmentEndTick;
        double segmentEndX = 0.0;

        if (nextSegment) {
            segmentEndTick = nextSegment->tick();
            segmentEndX = nextSegment->canvasPos().x();
        } else {
            segmentEndTick = measure->endTick();

            const Segment* endBar = measure->findSegment(SegmentType::EndBarLine, segmentEndTick);

            segmentEndX = endBar ? endBar->canvasPos().x()
                          : measure->canvasPos().x() + measure->width();
        }

        if (tick < segmentStartTick || tick >= segmentEndTick) {
            s = nextSegment;
            continue;
        }

        const double x = interpolateXByTicks(_tick, segmentStartTick.ticks(), segmentEndTick.ticks(),
                                             segmentStartX, segmentEndX);
        return calculateRect(x, score, system, systemBottom);
    }

    return RectF();
}

bool PlaybackCursor::visible() const
{
    return m_visible;
}

void PlaybackCursor::setVisible(bool arg)
{
    m_visible = arg;
}

const muse::RectF& PlaybackCursor::rect() const
{
    return m_rect;
}

QColor PlaybackCursor::color() const
{
    return configuration()->playbackCursorColor();
}
