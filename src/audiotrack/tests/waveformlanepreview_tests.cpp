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

//! Evidence for the drag preview of the audio lane.
//!
//! Two claims are worth automating here, and both are about what the user sees while lining
//! the backing track up by dragging the lane:
//!
//!   1. the lane is drawn where the pointer is taking it, *without* the score being laid out
//!      again -- re-laying out the score on every mouse move is what used to make the app
//!      unstable, so the preview must not depend on it;
//!   2. nothing else in the picture moves, and the shifted columns do not spill out of the
//!      lane's own system into the page margin.
//!
//! The check drives TDraw directly, so it sees exactly what the picture is made of: the lane's
//! line list, in the item's own coordinates, painted into an image. That also means it needs no
//! GUI and no window, which the drag itself cannot avoid.

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafflines.h"
#include "engraving/dom/system.h"
#include "engraving/dom/iaudiowaveformprovider.h"

#include "engraving/rendering/paintoptions.h"
#include "engraving/rendering/score/tdraw.h"

#include "modularity/ioc.h"

#include "draw/painter.h"

#include "utils/scorerw.h"

using namespace mu::engraving;
using namespace mu::engraving::rendering;
using namespace mu::engraving::rendering::score;
using namespace muse;

namespace mu::audiotrack::tests {
namespace {
//! Loud everywhere, so the lane lays out as one solid block of columns. That makes "where does
//! the lane start and end" a question about two pixels instead of about a waveform's shape.
class SolidWaveformProvider : public IAudioWaveformProvider
{
public:
    bool hasWaveform() const override { return true; }
    double waveformDuration() const override { return 8.0; }

    void setScoreOffsetSeconds(double seconds) override { m_scoreOffsetSeconds = seconds; }
    double scoreOffsetSeconds() const override { return m_scoreOffsetSeconds; }

    void waveformPeaks(double, double, int64_t count, std::vector<AudioWaveformPeak>& out) const override
    {
        out.clear();
        if (count <= 0) {
            return;
        }

        out.resize(static_cast<size_t>(count));
        for (AudioWaveformPeak& peak : out) {
            peak.max = 1.f;
            peak.min = -1.f;
        }
    }

private:
    double m_scoreOffsetSeconds = 0.0;
};

//! engraving resolves the provider through globalIoc(), so the stand-in has to be installed for
//! the duration of a test and taken out again afterwards.
class ScopedProvider
{
public:
    explicit ScopedProvider(std::shared_ptr<IAudioWaveformProvider> provider)
    {
        muse::modularity::globalIoc()->registerExport<IAudioWaveformProvider>("utests-preview", provider);
    }

    ~ScopedProvider()
    {
        muse::modularity::globalIoc()->unregister<IAudioWaveformProvider>("utests-preview");
    }
};

//! What the picture came out as: the image plus the horizontal extent of what was drawn in it.
struct Painted
{
    QImage image;
    int firstDark = -1;
    int lastDark = -1;
    int darkCount = 0;
};

//! The shift a drag is previewing. Whole score units, like the pointer delta it comes from.
constexpr double SHIFT = 40.0;

//! Room left on both sides of the lane. The lines are drawn with a pen, so what is painted
//! reaches half a line width beyond the first and last column; without the margin the left edge
//! of an unshifted lane would be cut off by the image and the measurement would be wrong.
constexpr int MARGIN = 200;

StaffLines* laneInMeasure(const Measure* measure, staff_idx_t staffIdx)
{
    return measure ? measure->staffLines(staffIdx) : nullptr;
}

//! Measures are a hand-rolled linked list, so they are walked with first()/nextMM() rather than
//! a range-for.
const Measure* firstMeasure(Score* score)
{
    for (const MeasureBase* mb = score->measures()->first(); mb; mb = mb->nextMM()) {
        if (mb->isMeasure()) {
            return toMeasure(mb);
        }
    }
    return nullptr;
}

const Measure* lastMeasure(Score* score)
{
    const Measure* last = nullptr;
    for (const MeasureBase* mb = score->measures()->first(); mb; mb = mb->nextMM()) {
        if (mb->isMeasure()) {
            last = toMeasure(mb);
        }
    }
    return last;
}

//! Paints one StaffLines item exactly as the score renderer would, at 1:1.
Painted paintLane(const StaffLines* lane)
{
    const RectF box = lane->ldata()->bbox();
    const int width = MARGIN + static_cast<int>(std::ceil(box.right())) + MARGIN;
    const int height = static_cast<int>(std::ceil(box.height())) + 4;

    Painted result;
    result.image = QImage(std::max(1, width), std::max(1, height), QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::white);

    {
        QPainter qtPainter(&result.image);
        qtPainter.setRenderHint(QPainter::Antialiasing, false);

        muse::draw::Painter painter(&qtPainter, "waveform-lane-preview-test");
        // The item's own coordinates start at the lane's top, which can be slightly negative
        // (the line width hangs over the top line).
        painter.translate(MARGIN, -box.top());

        PaintOptions opt;
        TDraw::drawItem(lane, &painter, opt);
        painter.endDraw();
    }

    for (int x = 0; x < result.image.width(); ++x) {
        for (int y = 0; y < result.image.height(); ++y) {
            if (result.image.pixelColor(x, y) != QColor(Qt::white)) {
                ++result.darkCount;
                if (result.firstDark < 0) {
                    result.firstDark = x;
                }
                result.lastDark = x;
                break;
            }
        }
    }

    return result;
}

//! How far past its last column the pen can paint. Used where the claim is "it stops at the end
//! of the system" rather than "it stops on an exact pixel".
int penSlack(const StaffLines* lane)
{
    return static_cast<int>(std::ceil(lane->lw() * 0.5)) + 2;
}

//! The picture is the evidence for a drawing change, so the same render can be written out for
//! a human to look at. Same convention as the other output tests: nothing is written unless
//! MUSE_WAVEFORM_PNG_DIR says where.
void dumpPng(const QString& name, const QImage& image)
{
    const char* env = std::getenv("MUSE_WAVEFORM_PNG_DIR");
    if (!env || !*env) {
        return;
    }

    const QString dir = QString::fromUtf8(env);
    QDir().mkpath(dir);
    image.save(dir + "/" + name, "png");
}
} // namespace

TEST(WaveformLanePreviewTests, ThePreviewMovesTheLaneWithoutLayingTheScoreOut)
{
    ScopedProvider provider(std::make_shared<SolidWaveformProvider>());

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);
    score->doLayout();

    const Measure* measure = firstMeasure(score);
    ASSERT_TRUE(measure);

    StaffLines* lane = laneInMeasure(measure, 0);
    ASSERT_TRUE(lane);
    ASSERT_FALSE(lane->lines().empty());

    const Painted before = paintLane(lane);
    ASSERT_GT(before.darkCount, 0) << "the lane drew nothing at all";
    ASSERT_GE(before.lastDark, before.firstDark);

    // Exactly what a drag does: a transient shift on the score. Note that the score is NOT laid
    // out again anywhere in this test -- that is the whole point of the preview, and the reason
    // it cannot bring back the crash that per-move re-layouts caused.
    score->setAudioLanePreviewShift(SHIFT);

    const Painted after = paintLane(lane);
    EXPECT_EQ(after.firstDark - before.firstDark, static_cast<int>(SHIFT))
        << "the lane was not drawn shifted, so the drag would still be blind";
    EXPECT_EQ(after.lastDark - before.lastDark, static_cast<int>(SHIFT))
        << "the far end of the lane did not move with the near end";
    EXPECT_EQ(after.darkCount, before.darkCount) << "the shift changed how much is drawn";

    dumpPng("lane-drag-preview-before.png", before.image);
    dumpPng("lane-drag-preview-shifted.png", after.image);

    // Releasing clears the preview, and the picture has to go back to what the layout says.
    score->setAudioLanePreviewShift(0.0);

    const Painted restored = paintLane(lane);
    EXPECT_EQ(restored.firstDark, before.firstDark);
    EXPECT_EQ(restored.lastDark, before.lastDark);
    EXPECT_EQ(restored.darkCount, before.darkCount);

    delete score;
}

TEST(WaveformLanePreviewTests, ShiftedColumnsLeaveTheSystemInsteadOfThePage)
{
    ScopedProvider provider(std::make_shared<SolidWaveformProvider>());

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);
    score->doLayout();

    // The last measure ends where its system ends, so the shift there has to be cut off: a lane
    // drawn past the end of its system would sit in the page margin.
    const Measure* measure = lastMeasure(score);
    ASSERT_TRUE(measure);

    StaffLines* lane = laneInMeasure(measure, 0);
    ASSERT_TRUE(lane);
    ASSERT_FALSE(lane->lines().empty());

    const Painted before = paintLane(lane);
    ASSERT_GT(before.darkCount, 0);

    score->setAudioLanePreviewShift(SHIFT);

    const Painted after = paintLane(lane);
    EXPECT_EQ(after.firstDark - before.firstDark, static_cast<int>(SHIFT));

    // The far end barely moves: what would have gone past the system was dropped instead. The
    // test above shows the same lane moving a full SHIFT when there is room, so a small move
    // here can only mean the columns were dropped.
    const int farEndMove = after.lastDark - before.lastDark;
    EXPECT_LE(farEndMove, penSlack(lane))
        << "the shifted lane was drawn past the end of its system, into the page margin";
    EXPECT_LT(farEndMove, static_cast<int>(SHIFT));
    EXPECT_LT(after.darkCount, before.darkCount) << "nothing was dropped, so nothing was cut off";

    delete score;
}

TEST(WaveformLanePreviewTests, OrdinaryStaffLinesAreNotShiftedByThePreview)
{
    ScopedProvider provider(std::make_shared<SolidWaveformProvider>());

    // A score whose staff is an ordinary one: a drag on the audio lane is not allowed to move
    // any part of the score itself, and a score with no lane in it must be untouched.
    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/plain-staff.mscx");
    ASSERT_TRUE(score);
    score->doLayout();

    const Measure* measure = firstMeasure(score);
    ASSERT_TRUE(measure);

    StaffLines* staffLines = laneInMeasure(measure, 0);
    ASSERT_TRUE(staffLines);
    ASSERT_FALSE(staffLines->lines().empty());

    const Painted before = paintLane(staffLines);
    ASSERT_GT(before.darkCount, 0);

    score->setAudioLanePreviewShift(SHIFT);

    const Painted after = paintLane(staffLines);
    EXPECT_EQ(after.firstDark, before.firstDark);
    EXPECT_EQ(after.lastDark, before.lastDark);
    EXPECT_EQ(after.darkCount, before.darkCount);

    delete score;
}
} // namespace mu::audiotrack::tests
