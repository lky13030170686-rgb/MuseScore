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

//! Draws exactly what engraving laid out for the waveform lane, so the result can be
//! eyeballed without launching the GUI.
//!
//! This is evidence generation rather than a test: it asserts only that the files were
//! produced, and leaves judging the picture to a human. Set MUSE_WAVEFORM_PNG_DIR to choose
//! where the images land.

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QString>

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafflines.h"

#include "modularity/ioc.h"

#include "audiotrack/audiofilereader.h"
#include "audiotrack/audiowaveformprovider.h"
#include "audiotrack/waveformcache.h"

#include "utils/scorerw.h"

using namespace mu::engraving;
using namespace muse;
using namespace muse::audiotrack;

namespace mu::audiotrack::tests {
namespace {
QString outDir()
{
    const char* env = std::getenv("MUSE_WAVEFORM_PNG_DIR");
    if (env && *env) {
        return QString::fromUtf8(env);
    }
    return QString();   // empty -> skip writing
}

//! Paints the line list of `lane` at 1:1 so the picture is literally the layout output.
QImage paintLane(const StaffLines* lane, int height, double& widthOut)
{
    const std::vector<LineF>& lines = lane->lines();

    double maxX = 0.0;
    for (const LineF& l : lines) {
        maxX = std::max(maxX, std::max(l.p1().x(), l.p2().x()));
    }
    widthOut = maxX;

    const int w = std::max(1, static_cast<int>(std::ceil(maxX)) + 2);
    QImage image(w, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(Qt::black, 1.0));

    // Lines are in staff coordinates where y grows downwards; the lane's own origin is
    // whatever layout chose, so shift by the minimum y so nothing falls outside the image.
    double minY = lines.empty() ? 0.0 : lines.front().p1().y();
    for (const LineF& l : lines) {
        minY = std::min(minY, std::min(l.p1().y(), l.p2().y()));
    }

    for (const LineF& l : lines) {
        p.drawLine(QPointF(l.p1().x(), l.p1().y() - minY),
                   QPointF(l.p2().x(), l.p2().y() - minY));
    }

    return image;
}

StaffLines* firstWaveformLane(Score* score)
{
    const MeasureBaseList* measures = score->measures();
    if (!measures) {
        return nullptr;
    }

    for (const MeasureBase* mb = measures->first(); mb; mb = mb->nextMM()) {
        if (!mb->isMeasure()) {
            continue;
        }

        const Measure* measure = toMeasure(mb);
        for (const Staff* staff : score->staves()) {
            if (staff->isWaveformStaff(measure->tick())) {
                StaffLines* lines = measure->staffLines(staff->idx());
                if (lines) {
                    return lines;
                }
            }
        }
    }

    return nullptr;
}
} // namespace

TEST(WaveformLaneOutputTests, WritesLanePngs)
{
    if (outDir().isEmpty()) {
        GTEST_SKIP() << "set MUSE_WAVEFORM_PNG_DIR to emit lane PNGs";
    }

    const QString dir = outDir();
    QDir().mkpath(dir);

    // One image per fixture, so the click track (spikes) and the sweep (solid band) can be
    // compared side by side against what the renderer tests produce.
    const std::pair<const char*, const char*> fixtures[] = {
        { "data/metronome-120bpm-48000.wav", "lane-metronome.png" },
        { "data/sweep-stereo-44100.wav", "lane-sweep.png" },
    };

    for (const auto& fixture : fixtures) {
        WaveformCache cache;
        {
            AudioFileReader reader;
            ASSERT_TRUE(reader.open(String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT) + "/" + fixture.first)))
                << "could not open " << fixture.first;
            ASSERT_TRUE(cache.generate(reader, 0));
        }

        auto provider = std::make_shared<AudioWaveformProvider>();
        provider->setCache(std::make_shared<const WaveformCache>(std::move(cache)));

        muse::modularity::globalIoc()->registerExport<IAudioWaveformProvider>("utests-output", provider);

        MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
        ASSERT_TRUE(score);
        score->doLayout();

        StaffLines* lane = firstWaveformLane(score);
        ASSERT_TRUE(lane);
        ASSERT_FALSE(lane->lines().empty());

        double width = 0.0;
        const QImage image = paintLane(lane, 140, width);
        const QString path = dir + "/" + QString::fromUtf8(fixture.second);
        EXPECT_TRUE(image.save(path, "png")) << "failed to write " << path.toStdString();

        delete score;
        muse::modularity::globalIoc()->unregister<IAudioWaveformProvider>("utests-output");
    }
}
} // namespace mu::audiotrack::tests
