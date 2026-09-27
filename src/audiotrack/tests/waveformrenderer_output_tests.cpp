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

//! Renders the waveform through the real draw path and writes PNGs, so the result can be
//! eyeballed without launching the GUI.
//!
//! This is evidence generation rather than a test: it asserts only that the files were
//! produced, and leaves judging the picture to a human. Set MUSE_WAVEFORM_PNG_DIR to
//! choose where the images land.

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QString>

#include <cstdlib>
#include <string>

#include "audiotrack/audiofilereader.h"
#include "audiotrack/waveformcache.h"
#include "audiotrack/waveformrenderer.h"

#include "draw/painter.h"

using namespace muse;
using namespace muse::audiotrack;

namespace {
String testFile(const char* name)
{
    return String::fromUtf8(std::string(audiotrack_tests_DATA_ROOT) + "/" + name);
}

QString outDir()
{
    const char* env = std::getenv("MUSE_WAVEFORM_PNG_DIR");
    if (env && *env) {
        return QString::fromUtf8(env);
    }
    return QString();   // empty -> skip writing
}

//! Renders one request into a PNG and returns the path written (empty on skip/failure).
QString renderToPng(const WaveformCache& cache, const WaveformRenderRequest& req, int width, int height,
                    const QString& fileName)
{
    const QString dir = outDir();
    if (dir.isEmpty()) {
        return QString();
    }

    QDir().mkpath(dir);

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);

    {
        muse::draw::Painter painter(&image, "waveform_png");
        WaveformRenderer::paint(painter, cache, req);
    }

    const QString path = dir + "/" + fileName;
    EXPECT_TRUE(image.save(path, "png")) << "failed to write " << path.toStdString();
    return path;
}
}

TEST(WaveformRendererOutputTests, WritesLookAtMePngs)
{
    if (outDir().isEmpty()) {
        GTEST_SKIP() << "set MUSE_WAVEFORM_PNG_DIR to emit waveform PNGs";
    }

    // Metronome: clicks with clear gaps, so the picture should show distinct spikes.
    WaveformCache metro;
    {
        AudioFileReader r;
        ASSERT_TRUE(r.open(testFile("data/metronome-120bpm-48000.wav")));
        ASSERT_TRUE(metro.generate(r, 0));
    }

    {
        WaveformRenderRequest req;
        req.x = 20.0;
        req.y = 20.0;
        req.width = 1160.0;
        req.height = 120.0;
        req.fromSeconds = 0.0;
        req.toSeconds = metro.duration();
        const QString p = renderToPng(metro, req, 1200, 160, "waveform-metronome-full.png");
        EXPECT_FALSE(p.isEmpty());
    }

    {
        // Zoomed into ~2 seconds, where individual clicks should be obvious.
        WaveformRenderRequest req;
        req.x = 20.0;
        req.y = 20.0;
        req.width = 1160.0;
        req.height = 120.0;
        req.fromSeconds = 0.0;
        req.toSeconds = 2.0;
        const QString p = renderToPng(metro, req, 1200, 160, "waveform-metronome-zoom.png");
        EXPECT_FALSE(p.isEmpty());
    }

    // Sweep: continuous tone, so the picture should be a filled band, not spikes.
    WaveformCache sweep;
    {
        AudioFileReader r;
        ASSERT_TRUE(r.open(testFile("data/sweep-stereo-44100.wav")));
        ASSERT_TRUE(sweep.generate(r, 0));
    }
    {
        WaveformRenderRequest req;
        req.x = 20.0;
        req.y = 20.0;
        req.width = 1160.0;
        req.height = 120.0;
        req.fromSeconds = 0.0;
        req.toSeconds = sweep.duration();
        const QString p = renderToPng(sweep, req, 1200, 160, "waveform-sweep-full.png");
        EXPECT_FALSE(p.isEmpty());
    }
}
