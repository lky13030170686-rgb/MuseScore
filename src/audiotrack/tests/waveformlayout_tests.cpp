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

// Verifies the part of the audio-track feature that lives in engraving: that a staff whose
// staff type is StaffTypes::WAVEFORM really is laid out by TLayout::layoutWaveformLane(), and
// that the peaks handed to the provider end up as vertical lines on the StaffLines element.
//
// This is the piece neither the renderer test (renderer only draws what layout produced) nor
// the provider unit test (it only checks peak lookup) could cover on its own, and the GUI
// import path cannot be driven from the command line, so a layout-level test is the only way
// to get automated evidence that a waveform reaches the score picture at all.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>

#include <cmath>
#include <iostream>
#include <utility>
#include <vector>

#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafflines.h"
#include "engraving/dom/system.h"
#include "engraving/dom/iaudiowaveformprovider.h"
#include "engraving/style/styledef.h"

#include "modularity/ioc.h"

#include "audiotrack/audiofilereader.h"
#include "audiotrack/audiowaveformprovider.h"
#include "audiotrack/iaudiowaveformservice.h"
#include "audiotrack/waveformcache.h"

#include "utils/scorerw.h"

#include "io/fileinfo.h"
#include "io/path.h"

#include "log.h"

using namespace mu::engraving;
using namespace muse;
// The decoding side (AudioFileReader / WaveformCache / AudioWaveformProvider) lives in
// muse::audiotrack, which is a different namespace from this test's mu::audiotrack::tests.
using namespace muse::audiotrack;

namespace mu::audiotrack::tests {
namespace {
// Hands back a fixed pattern instead of decoding a file, so the assertions below depend only
// on layout arithmetic and not on any WAV's contents.
class FakeWaveformProvider : public IAudioWaveformProvider
{
public:
    bool hasWaveform() const override { return m_hasWaveform; }

    double waveformDuration() const override { return m_hasWaveform ? 2.0 : 0.0; }

    void waveformPeaks(double fromSeconds, double toSeconds, int64_t count,
                       std::vector<AudioWaveformPeak>& out) const override
    {
        out.clear();
        if (!m_hasWaveform || count <= 0) {
            return;
        }

        m_lastFrom = fromSeconds;
        m_lastTo = toSeconds;
        m_lastCount = count;
        m_windows.emplace_back(fromSeconds, toSeconds);

        out.resize(static_cast<size_t>(count));
        for (size_t i = 0; i < out.size(); ++i) {
            out[i].max = m_peakMax;
            out[i].min = m_peakMin;
        }
    }

    void setHasWaveform(bool v) { m_hasWaveform = v; }
    void setFixedPeaks(float peakMax, float peakMin) { m_peakMax = peakMax; m_peakMin = peakMin; }

    //! Records the window the layout asked for, so the tempo-map mapping can be checked.
    mutable double m_lastFrom = -1.0;
    mutable double m_lastTo = -1.0;
    mutable int64_t m_lastCount = 0;
    mutable std::vector<std::pair<double, double> > m_windows;

private:
    bool m_hasWaveform = true;
    float m_peakMax = 0.8f;
    float m_peakMin = -0.2f;
};

// RAII: engraving resolves the provider through globalIoc(), so the mock has to be
// installed for the duration of a test and removed again afterwards.
class ScopedProvider
{
public:
    explicit ScopedProvider(std::shared_ptr<IAudioWaveformProvider> provider)
    {
        muse::modularity::globalIoc()->registerExport<IAudioWaveformProvider>("utests", provider);
    }

    ~ScopedProvider()
    {
        muse::modularity::globalIoc()->unregister<IAudioWaveformProvider>("utests");
    }
};

StaffLines* firstWaveformLane(Score* score)
{
    const MeasureBaseList* measures = score->measures();
    if (!measures) {
        return nullptr;
    }

    // MeasureBaseList is a hand-rolled linked list, not an STL container: walk it with
    // first()/nextMM() rather than a range-for.
    for (const MeasureBase* mb = measures->first(); mb; mb = mb->nextMM()) {
        if (!mb->isMeasure()) {
            continue;
        }

        const Measure* measure = toMeasure(mb);
        for (const Staff* staff : score->staves()) {
            if (!staff->isWaveformStaff(measure->tick())) {
                continue;
            }

            StaffLines* lines = measure->staffLines(staff->idx());
            if (lines) {
                return lines;
            }
        }
    }

    return nullptr;
}
} // namespace

class AudioTrackLayoutTests : public ::testing::Test
{
};

// The waveform staff type has to survive a score round trip, otherwise a reopened project
// would come back with the lane missing entirely.
TEST_F(AudioTrackLayoutTests, WaveformStaffTypeSurvivesScoreRead)
{
    const String relPath = u"data/audiotrack/waveform-staff.mscx";
    const String fullPath = ScoreRW::rootPath() + u"/" + relPath;

    // Assert the fixture is reachable before reading it: a wrong data root surfaces as a
    // null MasterScore, which is otherwise indistinguishable from an unparsable score.
    ASSERT_TRUE(muse::io::FileInfo::exists(muse::io::path_t(fullPath)))
        << "score fixture not found at " << fullPath.toStdString();

    MasterScore* score = ScoreRW::readScore(relPath);
    ASSERT_TRUE(score) << "failed to read " << fullPath.toStdString();

    bool found = false;
    for (const Staff* staff : score->staves()) {
        if (staff->isWaveformStaff(Fraction(0, 1))) {
            found = true;
            break;
        }
    }

    EXPECT_TRUE(found) << "no staff with StaffTypes::WAVEFORM after reading the score";

    delete score;
}

// A lane that lays out correctly still shows nothing unless it is actually painted, and
// StaffLines::collectForDrawing() gates painting on the staff being shown. Without this the
// waveform could be laid out perfectly and the user would still see an empty gap, which is
// the hardest kind of failure to diagnose from the code alone.
TEST_F(AudioTrackLayoutTests, WaveformLaneIsEligibleForDrawing)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    ScopedProvider scoped(provider);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);

    score->doLayout();

    StaffLines* lane = firstWaveformLane(score);
    ASSERT_TRUE(lane);

    // Staff::show() is part->show() && visible(); a part that defaults to hidden would make
    // the whole feature invisible in the GUI.
    EXPECT_TRUE(lane->staff()->show()) << "the waveform staff is not shown";
    EXPECT_TRUE(lane->score()->staff(lane->staffIdx())->show());

    const Part* part = lane->staff()->part();
    ASSERT_TRUE(part);
    EXPECT_TRUE(part->show())
        << "the audio part is hidden, so its lane would never be painted";

    EXPECT_TRUE(lane->collectForDrawing())
        << "StaffLines::collectForDrawing() is false: engraving will skip the lane entirely";

    delete score;
}

// With peaks available the lane must contain one line per pixel column of the measure.
TEST_F(AudioTrackLayoutTests, LaneDrawsOneLinePerColumn)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    ScopedProvider scoped(provider);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);

    score->doLayout();

    StaffLines* lane = firstWaveformLane(score);
    ASSERT_TRUE(lane) << "waveform staff produced no StaffLines to draw on";

    // Distinguish "the lane found no provider" from "the lane found a provider that drew
    // nothing": without this the failure below reads like a layout bug either way.
    ASSERT_NE(lane->waveformProvider(), nullptr)
        << "StaffLines::waveformProvider() is null: the IoC registration did not reach engraving";

    const double width = lane->ldata() ? lane->width() : 0.0;
    const std::vector<LineF>& lines = lane->lines();

    EXPECT_FALSE(lines.empty()) << "lane stayed empty even though the provider had peaks";

    // One line per pixel column, matching what WaveformRenderer produces.
    const size_t expected = static_cast<size_t>(std::max<int64_t>(1, static_cast<int64_t>(std::floor(width))));
    EXPECT_EQ(lines.size(), expected);

    delete score;
}

// Each line has to span the peak's min..max, not a constant height: a layout that ignored the
// peak values would still emit the right *number* of lines.
TEST_F(AudioTrackLayoutTests, LinesFollowPeakAmplitudes)
{
    // Two runs with different, known peak pairs. Comparing the resulting line lengths
    // catches a layout that ignores the peak values (constant height) and one that halves
    // or doubles them; the absolute centre of the lane is deliberately not used, because
    // nothing in the public API pins it down.
    const auto lineLengthFor = [](float peakMax, float peakMin) {
        auto provider = std::make_shared<FakeWaveformProvider>();
        provider->setFixedPeaks(peakMax, peakMin);
        ScopedProvider scoped(provider);

        MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
        if (!score) {
            return -1.0;
        }

        score->doLayout();

        StaffLines* lane = firstWaveformLane(score);
        double length = -1.0;
        if (lane && !lane->lines().empty()) {
            const LineF& line = lane->lines().front();
            length = std::abs(line.p2().y() - line.p1().y());
        }

        delete score;
        return length;
    };

    // span = |max| + |min| drives the height, so 0.8/-0.2 (span 1.0) is twice 0.4/-0.1
    // (span 0.5), and both must be strictly positive.
    const double full = lineLengthFor(0.8f, -0.2f);
    const double half = lineLengthFor(0.4f, -0.1f);

    ASSERT_GT(full, 0.0) << "lane produced no line for the full-amplitude peaks";
    ASSERT_GT(half, 0.0) << "lane produced no line for the half-amplitude peaks";
    EXPECT_NEAR(full / half, 2.0, 0.02) << "line height does not scale with peak amplitude";

    // Swapping max and min must not change the height (the line spans min..max either way),
    // but a negative-vs-positive mix-up would, so pin the sign handling with one more pair.
    const double reversed = lineLengthFor(-0.2f, 0.8f);
    ASSERT_GT(reversed, 0.0);
    EXPECT_NEAR(reversed, full, 0.02) << "min/max order changed the drawn height";
}

// Without peaks the lane must fall back to the plain staff lines rather than drawing garbage.
TEST_F(AudioTrackLayoutTests, NoPeaksProducesNoLines)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    provider->setHasWaveform(false);
    ScopedProvider scoped(provider);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);

    score->doLayout();

    StaffLines* lane = firstWaveformLane(score);
    ASSERT_TRUE(lane);
    EXPECT_TRUE(lane->lines().empty()) << "lane drew lines from a provider that reported no waveform";

    delete score;
}

// A score whose waveform staff was saved with a line count of 0 still has to show its
// waveform. This is not hypothetical: the lane height is derived as (lines - 1) * distance,
// so a 0-line staff yields a negative height. The first version of this code treated that as
// "nothing to draw" and dropped the waveform silently, which is exactly the sort of failure
// nobody notices until they open a project and the audio has disappeared.
TEST_F(AudioTrackLayoutTests, ZeroLineStaffStillDrawsWaveform)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    ScopedProvider scoped(provider);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff-zerolines.mscx");
    ASSERT_TRUE(score);

    score->doLayout();

    StaffLines* lane = firstWaveformLane(score);
    ASSERT_TRUE(lane);
    EXPECT_FALSE(lane->lines().empty()) << "0-line waveform staff dropped its waveform";

    delete score;
}

// The window handed to the provider has to come from the score's tempo timeline: each
// measure of a 4/4 score at the default 120 BPM spans exactly two seconds, so the windows
// must be 2 s long and tile [0, 2), [2, 4), ... A layout that used a hand-rolled BPM
// calculation would drift once a tempo change exists, which is why the mapping goes through
// Score::utick2utime() and is pinned here.
TEST_F(AudioTrackLayoutTests, WindowFollowsTempoTimeline)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    ScopedProvider scoped(provider);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);

    score->doLayout();

    StaffLines* lane = firstWaveformLane(score);
    ASSERT_TRUE(lane);
    ASSERT_FALSE(lane->lines().empty());

    // Every request the layout made, in order. Note the layout runs more than once (the
    // score loader lays out, and the test lays out again), so the sequence restarts at 0
    // partway through; what matters is that every window is one measure long and that each
    // run starts at the beginning of the score and advances without gaps.
    ASSERT_FALSE(provider->m_windows.empty()) << "the layout never asked for peaks";

    double previousTo = -1.0;
    size_t runCount = 0;
    for (const auto& w : provider->m_windows) {
        EXPECT_NEAR(w.second - w.first, 2.0, 1e-6)
            << "measure window is not two seconds long";

        if (previousTo < 0.0 || w.first < previousTo) {
            // A new layout run: it has to start at the beginning of the score.
            EXPECT_NEAR(w.first, 0.0, 1e-6) << "a layout run did not start at tick 0";
            ++runCount;
        } else {
            EXPECT_NEAR(w.first, previousTo, 1e-6) << "measure windows do not tile the timeline";
        }
        previousTo = w.second;
    }

    EXPECT_GT(runCount, 0u);

    EXPECT_GT(provider->m_lastCount, 0);

    delete score;
}

// End-to-end shape check with real audio: decodes the checked-in metronome fixture, feeds
// the resulting peak cache to the real AudioWaveformProvider (not the fake above), lays the
// score out and inspects what engraving produced. The fake provider proves the layout maths;
// this proves the real decode -> cache -> provider -> lane chain produces clicks where the
// audio actually has them.
TEST_F(AudioTrackLayoutTests, RealAudioReachesTheLane)
{
    WaveformCache cache;
    {
        AudioFileReader reader;
        ASSERT_TRUE(reader.open(String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT)
                                                + "/data/metronome-120bpm-48000.wav")))
            << "could not open the metronome fixture";

        ASSERT_TRUE(cache.generate(reader, 0)) << "could not build a peak cache";
    }

    auto provider = std::make_shared<AudioWaveformProvider>();
    provider->setCache(std::make_shared<const WaveformCache>(std::move(cache)));
    ASSERT_TRUE(provider->hasWaveform());
    ScopedProvider scoped(provider);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);

    score->doLayout();

    StaffLines* lane = firstWaveformLane(score);
    ASSERT_TRUE(lane);
    const std::vector<LineF>& lines = lane->lines();
    ASSERT_FALSE(lines.empty()) << "real audio produced no waveform lines";

    // The first measure spans 0..2 s of a 120 BPM 4/4 metronome: four beats, so four clicks.
    // Beats in this fixture are not uniform (the downbeat is louder), so the tallest line in
    // each beat window has to be separated from its neighbours by silent columns.
    //
    // Rather than guessing pixel positions, count how many lines are near-silent: with four
    // clicks in two seconds there must be silent stretches between them.
    size_t silent = 0;
    for (const LineF& line : lines) {
        if (std::abs(line.p2().y() - line.p1().y()) < 1.0) {
            ++silent;
        }
    }

    EXPECT_GT(silent, 0u) << "a click track produced no silent columns";
    EXPECT_GT(lines.size() - silent, 0u) << "no audible columns were drawn";

    delete score;
}

// The objective is a lane that scrolls with the system, so it has to survive the score's
// "hide empty staves" style. The lane holds no notes, which makes it look empty to the
// system layout, and the default global setting is off -- so this only breaks for users who
// turn that option on.
//
// This drives the lane through the same code the app uses (SystemLayout::hideEmptyStaves
// via doLayout) but only asserts that turning the style on does not remove the lane from
// the systems that were showing it. It does not reproduce the "normal staff plus empty
// lane" arrangement, because building a printable second part inside a unit test needs more
// of the score machinery than is reasonable here; that arrangement is covered by hand in
// the GUI acceptance steps.
TEST_F(AudioTrackLayoutTests, LaneSurvivesHideEmptyStaves)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    ScopedProvider scoped(provider);

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/waveform-staff.mscx");
    ASSERT_TRUE(score);

    // Baseline: which music systems show the lane with the default style.
    score->doLayout();
    size_t shownByDefault = 0;
    for (System* system : score->systems()) {
        if (!system->staves().empty() && system->staff(0)->show()) {
            ++shownByDefault;
        }
    }
    ASSERT_GT(shownByDefault, 0u) << "the lane is not shown even with hide-empty-staves off";

    score->style().set(Sid::hideEmptyStaves, true);
    score->doLayout();

    size_t shownWithStyle = 0;
    for (System* system : score->systems()) {
        if (!system->staves().empty() && system->staff(0)->show()) {
            ++shownWithStyle;
        }
    }

    EXPECT_EQ(shownWithStyle, shownByDefault)
        << "turning on hide-empty-staves removed the waveform lane from systems that showed it";

    delete score;
}

// The whole feature in one test: take an ordinary score, add the Audio track the way a user
// does, and check that a waveform actually gets drawn on the new lane.
//
// The other tests each cover one link (the instrument resolves to a waveform staff, the lane
// draws peaks, the lane is eligible for painting). This one covers the seam between them --
// instrument added -> part and staff created -> layout finds the lane -> peaks reach it --
// which is where a mistake would produce a lane that exists but stays blank.
TEST_F(AudioTrackLayoutTests, AddingTheAudioTrackInstrumentDrawsAWaveform)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    ScopedProvider scoped(provider);

    const InstrumentTemplate* templ = searchTemplate(u"audio-track");
    ASSERT_TRUE(templ) << "the Audio track instrument is missing from instruments.xml";

    MasterScore* score = ScoreRW::readScore(u"data/audiotrack/plain-staff.mscx");
    ASSERT_TRUE(score);
    ASSERT_EQ(score->nstaves(), 1u);

    // Exactly what adding the instrument from the instrument dialog ends up doing: a part
    // built from the template.
    score->appendPart(templ);
    ASSERT_EQ(score->nstaves(), 2u) << "adding the Audio track did not add a staff";

    score->doLayout();

    StaffLines* lane = firstWaveformLane(score);
    ASSERT_TRUE(lane) << "the added audio staff has no staff lines to draw the waveform on";

    EXPECT_TRUE(lane->staff()->isWaveformStaff(Fraction(0, 1)))
        << "the added staff is not the waveform lane";

    EXPECT_FALSE(lane->lines().empty())
        << "the waveform lane came out blank: peaks never reached a lane created by adding "
           "the instrument";

    // A blank lane would also pass a bare "not empty" check, so confirm the columns really
    // carry the peak heights rather than a fallback hairline.
    size_t fullHeight = 0;
    for (const LineF& line : lane->lines()) {
        if (std::abs(line.p2().y() - line.p1().y()) > 1.0) {
            ++fullHeight;
        }
    }
    EXPECT_GT(fullHeight, 0u) << "every column was drawn as a flat hairline";

    delete score;
}

// Saving and reopening has to bring the lane back as a waveform lane.
//
// Reopening re-reads the part from the file rather than going through the instrument
// template, so the staff type has to be carried in the score itself. If it were not, the
// project would come back with an ordinary staff where the waveform used to be -- and the
// user's saved projects would quietly lose the lane.
TEST_F(AudioTrackLayoutTests, WaveformLaneSurvivesSaveAndReload)
{
    auto provider = std::make_shared<FakeWaveformProvider>();
    ScopedProvider scoped(provider);

    const InstrumentTemplate* templ = searchTemplate(u"audio-track");
    ASSERT_TRUE(templ);

    // Set MUSE_AUDIOTRACK_KEEP_SCORE to keep the saved score instead of deleting it, so it
    // can be rendered by the real application as an end-to-end check of the shipped binary.
    // Same idea as MUSE_WAVEFORM_PNG_DIR in the renderer output tests.
    const bool keepScore = qEnvironmentVariableIsSet("MUSE_AUDIOTRACK_KEEP_SCORE");
    const QString savedPath = keepScore
                              ? QDir::tempPath() + QStringLiteral("/dsh-audiotrack-score.mscx")
                              : QDir::tempPath() + QStringLiteral("/dsh-audiotrack-roundtrip.mscx");
    QFile::remove(savedPath);

    {
        MasterScore* score = ScoreRW::readScore(u"data/audiotrack/plain-staff.mscx");
        ASSERT_TRUE(score);

        score->appendPart(templ);
        ASSERT_EQ(score->nstaves(), 2u);
        ASSERT_TRUE(score->staff(1)->isWaveformStaff(Fraction(0, 1)))
            << "precondition failed: the lane was not a waveform staff before saving";

        ASSERT_TRUE(ScoreRW::saveScore(score, savedPath)) << "could not save the score";
        delete score;
    }

    ASSERT_TRUE(QFile::exists(savedPath)) << "the saved score is not on disk";

    MasterScore* reloaded = ScoreRW::readScore(savedPath, /*isAbsolutePath*/ true);
    ASSERT_TRUE(reloaded) << "could not reopen the saved score";
    ASSERT_EQ(reloaded->nstaves(), 2u) << "the audio lane did not come back";

    const Staff* laneStaff = reloaded->staff(1);
    ASSERT_TRUE(laneStaff);
    EXPECT_TRUE(laneStaff->isWaveformStaff(Fraction(0, 1)))
        << "reopening turned the waveform lane into a \""
        << laneStaff->staffType(Fraction(0, 1))->xmlName().toStdString() << "\" staff";

    // And it still draws, so the lane is not merely the right type but functional.
    reloaded->doLayout();

    StaffLines* lane = firstWaveformLane(reloaded);
    ASSERT_TRUE(lane) << "the reopened audio staff has no lines to draw the waveform on";
    EXPECT_FALSE(lane->lines().empty()) << "the reopened lane came out blank";

    // The lane carries one rest per measure, and every one of them must be a *gap* rest.
    //
    // They cannot simply be removed: Score::sanityCheck reports "Incomplete measure" when a
    // measure's first voice does not add up to the time signature, and NotationProject::load
    // then refuses to open the score (removing them for content lanes was tried, and produced
    // files the application could not load at all).
    //
    // But they must not be drawn either, because a measure rest is placed in the middle of the
    // staff -- exactly where the waveform is drawn. Gap rests are skipped by
    // RestLayout::layoutRest, so they satisfy the completeness check while staying invisible.
    int laneRests = 0;
    int laneGapRests = 0;
    for (const MeasureBase* mb = reloaded->measures()->first(); mb; mb = mb->nextMM()) {
        if (!mb->isMeasure()) {
            continue;
        }
        const Measure* measure = toMeasure(mb);
        for (const Segment* s = measure->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
            for (voice_idx_t voice = 0; voice < VOICES; ++voice) {
                EngravingItem* e = s->element(1 * VOICES + voice);
                if (!e) {
                    continue;
                }
                ++laneRests;
                if (e->isRest() && toRest(e)->isGap()) {
                    ++laneGapRests;
                }
            }
        }
    }

    EXPECT_GT(laneRests, 0)
        << "the reopened lane has no rests; a measure that adds up to nothing is reported as "
           "corrupted and the score will not open";

    EXPECT_EQ(laneGapRests, laneRests)
        << "the lane has " << (laneRests - laneGapRests)
        << " rest(s) that are not gap rests; they would be drawn on top of the waveform";

    delete reloaded;

    if (keepScore) {
        std::cout << "[keep] saved score: " << savedPath.toStdString() << std::endl;
    } else {
        QFile::remove(savedPath);
    }
}
} // namespace mu::audiotrack::tests
