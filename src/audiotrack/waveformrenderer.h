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
#ifndef MUSE_AUDIOTRACK_WAVEFORMRENDERER_H
#define MUSE_AUDIOTRACK_WAVEFORMRENDERER_H

#include <cstdint>
#include <vector>

// LineF is a template alias (LineX<double>), which cannot be forward-declared, so the
// full definition is required here.
#include "draw/types/geometry.h"

#include "waveformcache.h"

namespace muse::draw {
class Painter;
}

namespace muse::audiotrack {
//! Turns a WaveformCache into the vertical lines that make up a waveform picture.
//!
//! Deliberately split from the drawing step: the geometry is pure arithmetic and can be
//! unit-tested without a Painter or a Qt widget, while the drawing itself is a thin loop.
//! This is the same split BBC audiowaveform uses (compute peaks, then render).
//!
//! Why lines and not a path: a waveform is one vertical segment per pixel column. Building
//! a QPainterPath per pixel is a well-known performance trap (documented in
//! 音频轨/docs/技术调研报告.md §4), so callers get a flat line array to hand to
//! Painter::drawLines() in ONE call rather than looping over drawLine().
struct WaveformRenderRequest
{
    //! Pixel rectangle to draw into, in the painter's current coordinate system.
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    //! Time window of the cache to show, in seconds.
    double fromSeconds = 0.0;
    double toSeconds = 0.0;

    //! Vertical centre of each drawn line inside the rect, 0 = top, 1 = bottom.
    double centreRatio = 0.5;
    //! Fraction of the half-height actually used, so the trace does not touch the edges.
    double amplitudeRatio = 0.45;
};

class WaveformRenderer
{
public:
    //! Number of vertical lines that would be produced for the given request.
    //! Equals the number of pixel columns, clamped to something sane for huge zooms-out.
    static int64_t lineCount(const WaveformRenderRequest& req);

    //! Builds the vertical lines for the request. Returns a flat array of segments
    //! (x, y1, x, y2) laid out as consecutive muse::LineF entries, ready for drawLines().
    //! \param minValue/maxValue optional normalisation range; when both are 0 the full
    //!        -1..1 range is used (i.e. no normalisation), which is what a waveform view
    //!        usually wants so that relative loudness stays visible.
    static std::vector<muse::LineF> buildLines(const WaveformCache& cache,
                                               const WaveformRenderRequest& req,
                                               float minValue = 0.f,
                                               float maxValue = 0.f);

    //! Convenience: compute and draw in one call.
    static void paint(muse::draw::Painter& painter,
                      const WaveformCache& cache,
                      const WaveformRenderRequest& req,
                      float minValue = 0.f,
                      float maxValue = 0.f);
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_WAVEFORMRENDERER_H
