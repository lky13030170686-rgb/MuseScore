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
#include "waveformrenderer.h"

#include <algorithm>
#include <cmath>

#include "draw/types/geometry.h"
#include "draw/types/pen.h"
#include "draw/painter.h"

using namespace muse;
using namespace muse::audiotrack;
using namespace muse::draw;

int64_t WaveformRenderer::lineCount(const WaveformRenderRequest& req)
{
    // One line per pixel column. Guard against a degenerate rect (zero or negative width)
    // and against absurd zooms-out producing millions of lines.
    static constexpr int64_t MAX_LINES = 20000;
    const int64_t n = static_cast<int64_t>(std::floor(req.width));
    return std::max<int64_t>(0, std::min<int64_t>(n, MAX_LINES));
}

std::vector<LineF> WaveformRenderer::buildLines(const WaveformCache& cache,
                                               const WaveformRenderRequest& req,
                                               float minValue,
                                               float maxValue)
{
    std::vector<LineF> lines;

    const int64_t count = lineCount(req);
    if (count <= 0 || cache.isEmpty() || req.height <= 0.0) {
        return lines;
    }

    const double secondsPerColumn = (req.toSeconds - req.fromSeconds) / static_cast<double>(count);
    if (!(secondsPerColumn > 0.0)) {
        return lines;
    }

    // How many source frames one column covers, which is what decides the cache level.
    const double framesPerColumn = secondsPerColumn * static_cast<double>(cache.sampleRate());
    const int level = cache.levelForSamplesPerPixel(static_cast<int64_t>(std::max(1.0, framesPerColumn)));

    // Peaks are stored at this many source frames per stored pixel; a column may therefore
    // span more than one stored peak and must be reduced across all of them.
    const int64_t spp = cache.samplesPerPixelAt(level);
    const int64_t totalPeaks = static_cast<int64_t>(cache.peakCountAt(level));
    if (totalPeaks <= 0) {
        return lines;
    }

    // Normalisation. A zero range means "use the raw -1..1 amplitude".
    const bool normalise = (maxValue > minValue);
    const float lo = normalise ? minValue : -1.f;
    const float hi = normalise ? maxValue : 1.f;
    const float span = std::max(1e-6f, hi - lo);

    const double centreY = req.y + req.height * req.centreRatio;
    const double halfHeight = req.height * 0.5 * req.amplitudeRatio;

    // Reuse one buffer across columns rather than allocating per column.
    std::vector<PeakPair> peaks;

    lines.reserve(static_cast<size_t>(count));

    for (int64_t col = 0; col < count; ++col) {
        const double colFromSec = req.fromSeconds + secondsPerColumn * static_cast<double>(col);
        const double colToSec = colFromSec + secondsPerColumn;

        // Source-frame range covered by this column, clamped to the cache.
        const int64_t frameFrom = static_cast<int64_t>(std::floor(colFromSec * cache.sampleRate()));
        const int64_t frameTo = static_cast<int64_t>(std::ceil(colToSec * cache.sampleRate()));

        // Convert to stored-peak indices at the chosen level.
        int64_t peakFrom = frameFrom / spp;
        int64_t peakTo = frameTo / spp;

        // Always cover at least one stored peak, otherwise a zoomed-in view would draw
        // nothing at all between two stored peaks.
        if (peakTo <= peakFrom) {
            peakTo = peakFrom + 1;
        }

        peakFrom = std::max<int64_t>(0, std::min<int64_t>(peakFrom, totalPeaks - 1));
        peakTo = std::max<int64_t>(peakFrom + 1, std::min<int64_t>(peakTo, totalPeaks));

        peaks.clear();
        cache.peaksAt(level, peakFrom, peakTo - peakFrom, peaks);
        if (peaks.empty()) {
            continue;
        }

        float mn = peaks[0].min;
        float mx = peaks[0].max;
        for (const PeakPair& p : peaks) {
            mn = std::min(mn, p.min);
            mx = std::max(mx, p.max);
        }

        // Map amplitude to a vertical segment, centred on centreY.
        auto toY = [&](float v) {
            const float norm = std::clamp((v - lo) / span, 0.f, 1.f);   // 0..1
            // 1.0 at the top of the range, so invert for screen Y.
            return centreY - (static_cast<double>(norm) * 2.0 - 1.0) * halfHeight;
        };

        const double cx = req.x + static_cast<double>(col) + 0.5;
        const double y1 = toY(mx);
        const double y2 = toY(mn);

        // A silent column collapses to a single point; draw a hairline so the trace stays
        // visible as a continuous centre line rather than looking like a gap.
        if (std::abs(y2 - y1) < 0.5) {
            lines.emplace_back(PointF(cx, centreY - 0.25), PointF(cx, centreY + 0.25));
        } else {
            lines.emplace_back(PointF(cx, y1), PointF(cx, y2));
        }
    }

    return lines;
}

void WaveformRenderer::paint(Painter& painter,
                             const WaveformCache& cache,
                             const WaveformRenderRequest& req,
                             float minValue,
                             float maxValue)
{
    const std::vector<LineF> lines = buildLines(cache, req, minValue, maxValue);
    if (lines.empty()) {
        return;
    }

    painter.save();
    // Antialiasing is a net loss for a dense field of vertical hairlines, and the
    // research report calls this out explicitly (音频轨/docs/技术调研报告.md §4).
    painter.setAntialiasing(false);
    painter.setPen(Pen(Color::BLACK, 1.0));
    painter.drawLines(lines.data(), lines.size());
    painter.restore();
}
