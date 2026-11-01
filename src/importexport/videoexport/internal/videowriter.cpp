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
#include "videowriter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include <QPainter>
#include <QThread>

#include "global/concurrency/concurrent.h"
#include "global/io/filestream.h"

#include "draw/fontmetrics.h"
#include "draw/painter.h"
#include "draw/types/drawtypes.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/page.h"
#include "engraving/dom/repeatlist.h"

#include "notation/imasternotation.h"
#include "notation/inotation.h"
#include "notation/inotationelements.h" // IWYU pragma: keep
#include "notation/inotationpainting.h"
#include "notation/inotationplayback.h"
#include "notation/notationtypes.h"

#include "notationscene/qml/MuseScore/NotationScene/playbackcursor.h"

#include "defer.h"
#include "log.h"

using namespace mu::iex::videoexport;
using namespace mu::project;
using namespace mu::notation;
using namespace muse::draw;
using namespace muse::midi;

static muse::String notationTitle(const INotationPtr notation)
{
    muse::String title;
    mu::engraving::Score* score = notation->elements()->msScore();
    mu::engraving::MasterScore* masterScore = notation->masterNotation()->masterScore();

    if (const mu::engraving::Text* text = score->getText(mu::engraving::TextStyleType::TITLE)) {
        title = text->plainText();
    }

    if (title.isEmpty()) {
        if (const mu::engraving::Text* text = masterScore->getText(mu::engraving::TextStyleType::TITLE)) {
            title = text->plainText();
        }
    }

    if (title.isEmpty()) {
        title = masterScore->metaTag(u"workTitle");
    }

    return title;
}

static muse::String notationSubtitle(const INotationPtr notation)
{
    if (notation->isMaster()) {
        return muse::String();
    }

    return notation->name();
}

std::vector<INotationWriter::UnitType> VideoWriter::supportedUnitTypes() const
{
    return { UnitType::PER_PART };
}

bool VideoWriter::supportsUnitType(UnitType unitType) const
{
    std::vector<UnitType> unitTypes = supportedUnitTypes();
    return std::find(unitTypes.cbegin(), unitTypes.cend(), unitType) != unitTypes.cend();
}

muse::Ret VideoWriter::write(INotationPtr notation, muse::io::IODevice& device, const Options& options)
{
    std::string filePath = device.meta("file_path");
    IF_ASSERT_FAILED(!filePath.empty()) {
        return make_ret(muse::Ret::Code::InternalError);
    }

    bool withAudio = muse::value(options, OptionKey::WITH_AUDIO, muse::Val(true)).toBool();

    Config cfg = makeConfig();

    muse::io::path_t finalPath(filePath);
    muse::io::path_t tempAudioPath = finalPath + ".tmp_audio.aac";

    auto encoder = videoEncodeResolver()->currentVideoEncoder();

    //! NOTE LKY fork: without FFmpeg loaded there is no encoder. Upstream dereferenced it and
    //! crashed with SIGSEGV; a plain error is more useful (the console run hits this).
    IF_ASSERT_FAILED(encoder) {
        LOGE() << "no video encoder available (FFmpeg is not loaded)";
        return make_ret(muse::Ret::Code::InternalError);
    }

    muse::media::IVideoEncoder::Options encoderOptions;
    encoderOptions.format = "mp4";
    encoderOptions.width = cfg.width;
    encoderOptions.height = cfg.height;
    encoderOptions.bitrate = cfg.bitrate;
    encoderOptions.gop = cfg.fps / 2;
    encoderOptions.fps = cfg.fps;

    if (!encoder->open(finalPath, encoderOptions)) {
        LOGE() << "failed to open video encoder";
        return make_ret(muse::Ret::Code::UnknownError);
    }

    m_isCompleted = false;
    m_audioCompleted = false;
    m_abort = false;
    m_writeRet = muse::Ret();
    m_audioRet = muse::Ret();

    startVideoExport(encoder, notation, cfg);

    if (withAudio) {
        startAudioExport(notation, tempAudioPath, cfg);
    } else {
        m_audioCompleted = true;
        m_audioRet = muse::make_ok();
    }

    while (!m_isCompleted || !m_audioCompleted) {
        application()->processEvents();
        QThread::yieldCurrentThread();
    }

    if (m_audioWriter) {
        m_audioWriter->progress()->finished().disconnect(this);
        m_audioWriter = nullptr;
    }

    if (m_audioFile) {
        m_audioFile->close();
        m_audioFile.reset();
    }

    muse::Ret result = m_writeRet;

    encoder->finishEncode();

    // Release the device's file handle before add audio replaces the file
    device.close();

    if (withAudio) {
        if (result && m_audioRet) {
            if (!encoder->addAudio(tempAudioPath)) {
                result = make_ret(muse::Ret::Code::UnknownError);
            }
        } else if (!result) {
            // keep video error
        } else {
            result = m_audioRet;
        }

        fileSystem()->remove(tempAudioPath);
    }

    encoder->close();

    return result;
}

VideoWriter::Config VideoWriter::makeConfig() const
{
    Config cfg;

    cfg.fps = configuration()->fps();

    std::string resolution = configuration()->resolution();
    if (resolution == "2160p") {
        cfg.width = 3840;
        cfg.height = 2160;
    } else if (resolution == "1440p") {
        cfg.width = 2560;
        cfg.height = 1440;
    } else if (resolution == "1080p") {
        cfg.width = 1920;
        cfg.height = 1080;
    } else if (resolution == "720p") {
        cfg.width = 1280;
        cfg.height = 720;
    } else if (resolution == "480p") {
        cfg.width = 854;
        cfg.height = 480;
    } else if (resolution == "360p") {
        cfg.width = 640;
        cfg.height = 360;
    } else {
        cfg.width = 1920;
        cfg.height = 1080;
    }

    // compute bitrate according to Google recommended settings
    // https://support.google.com/youtube/answer/1722171?hl=en
    float br = 8;
    if (cfg.height == 2160) {
        br = cfg.fps < 35 ? 40 : 60;
    } else if (cfg.height == 1440) {
        br = cfg.fps < 35 ? 16 : 24;
    } else if (cfg.height == 1080) {
        br = cfg.fps < 35 ? 10 : 15;
    } else if (cfg.height == 720) {
        br = cfg.fps < 35 ? 5 : 7.5;
    } else if (cfg.height == 480) {
        br = cfg.fps < 35 ? 2.5 : 4;
    } else if (cfg.height == 360) {
        br = cfg.fps < 35 ? 1 : 1.5;
    }
    cfg.bitrate = int(br * 1000000);

    cfg.leadingSec = configuration()->leadingSec();
    cfg.trailingSec = configuration()->trailingSec();

    cfg.viewMode = configuration()->viewMode();

    //! LKY fork: multi-page frames + optional title/logo segments.
    //! Disabling a segment zeroes its length here (and not in `VideoWriter::write()`), because the
    //! audio export reads `leadingSec`/`trailingSec` from the very same Config for its silence.
    cfg.pagesPerScreen = configuration()->pagesPerScreen();
    cfg.showTitlePage = configuration()->showTitlePage();
    cfg.showEndLogo = configuration()->showEndLogo();

    if (!cfg.showTitlePage) {
        cfg.leadingSec = 0.0f;
    }

    if (!cfg.showEndLogo) {
        cfg.trailingSec = 0.0f;
    }

    return cfg;
}

void VideoWriter::startVideoExport(muse::media::IVideoEncoderPtr encoder, INotationPtr notation, const Config& cfg)
{
    muse::Concurrent::run([this, encoder, notation, cfg]() {
        doGenerate(encoder, notation, cfg);
    });
}

void VideoWriter::startAudioExport(INotationPtr notation, const muse::io::path_t& audioPath, const Config& cfg)
{
    m_audioWriter = writers()->writer("aac");
    if (!m_audioWriter) {
        LOGE() << "aac writer not found";
        m_audioRet = make_ret(muse::Ret::Code::InternalError);
        m_audioCompleted = true;
        return;
    }

    m_audioWriter->progress()->finished().onReceive(this, [this](const muse::ProgressResult& res) {
        m_audioRet = res.ret;
        m_audioCompleted = true;
    });

    Options audioOpts;
    audioOpts[OptionKey::WAIT_FOR_COMPLETION] = muse::Val(false);
    audioOpts[OptionKey::LEADING_SILENCE_SEC] = muse::Val(static_cast<double>(cfg.leadingSec));
    audioOpts[OptionKey::TRAILING_SILENCE_SEC] = muse::Val(static_cast<double>(cfg.trailingSec));

    m_audioFile = std::make_unique<muse::io::FileStream>(audioPath);
    m_audioFile->setMeta("file_path", audioPath.toStdString());
    m_audioFile->open(muse::io::IODevice::WriteOnly);

    m_audioWriter->write(notation, *m_audioFile, audioOpts);
}

muse::Ret VideoWriter::writeList(const INotationPtrList&, muse::io::IODevice&, const Options&)
{
    NOT_SUPPORTED;
    return make_ret(muse::Ret::Code::NotSupported);
}

muse::Progress* VideoWriter::progress()
{
    return &m_progress;
}

void VideoWriter::abort()
{
    m_abort = true;

    if (m_audioWriter) {
        m_audioWriter->abort();
    }

    // Wait abort completion
    while (!m_isCompleted || !m_audioCompleted) {
        application()->processEvents();
        QThread::yieldCurrentThread();
    }
}

std::optional<VideoWriter::ScoreRestoreData> VideoWriter::prepareScore(INotationPtr notation, Config& config)
{
    ScoreRestoreData result;
    engraving::Score* score = notation->elements()->msScore();

    result.style = score->style();

    result.layoutMode = score->layoutMode();

    result.showFrames = score->showFrames();
    result.showInstrumentNames = score->showInstrumentNames();
    result.showInvisible = score->isShowInvisible();
    result.showPageborders = score->showPageborders();
    result.showUnprintable = score->showUnprintable();
    result.showVBox = score->layoutOptions().isShowVBox;

    score->setLayoutMode(engraving::LayoutMode::PAGE);

    score->setShowFrames(false);
    score->setShowInvisible(false);
    score->setShowPageborders(false);
    score->setShowUnprintable(false);

    if (config.viewMode == ViewMode::Flexible) {
        score->setShowInstrumentNames(false);
        score->setShowVBox(false);
    }

    score->doLayout();

    PageList pages = notation->elements()->pages();
    if (pages.empty()) {
        LOGE() << "No pages";
        restoreScore(notation, result);
        return std::nullopt;
    }

    const Page* page = pages.front();

    if (config.viewMode == ViewMode::PageFull) {
        double scaleX = config.width / page->width();
        double scaleY = config.height / page->height();
        double scale = std::min(scaleX, scaleY);
        config.canvasDpi = scale * engraving::DPI;
        config.moveToCenter = muse::PointF(0.5 * (config.width / scale - page->width()), 0.5 * (config.height / scale - page->height()));
        return result;
    }

    if (score->staves().size() > 3) {
        //! NOTE: Calculate the dpi to display all page elements
        double originalPageHeight = page->height() - score->style().styleD(engraving::Sid::pageOddTopMargin)
                                    - score->style().styleD(engraving::Sid::pageOddBottomMargin);
        double margin = 100.0;
        double ttboxHeight = originalPageHeight + margin * 2;
        double scale = config.height / ttboxHeight;
        config.canvasDpi = scale * engraving::DPI;
    }

    score->style().set(engraving::Sid::pageHeight, config.height / config.canvasDpi);
    score->style().set(engraving::Sid::pageWidth, config.width / config.canvasDpi);
    score->style().set(engraving::Sid::pagePrintableWidth, score->style().styleD(engraving::Sid::pageWidth)
                       - score->style().styleD(engraving::Sid::pageOddLeftMargin)
                       - score->style().styleD(engraving::Sid::pageEvenLeftMargin));

    score->style().set(engraving::Sid::pageEvenTopMargin, 0.0);
    score->style().set(engraving::Sid::pageEvenBottomMargin, 0.0);
    score->style().set(engraving::Sid::pageOddTopMargin, 0.0);
    score->style().set(engraving::Sid::pageOddBottomMargin, 0.0);
    score->style().set(engraving::Sid::pageTwosided, false);
    score->style().set(engraving::Sid::showHeader, false);
    score->style().set(engraving::Sid::showFooter, false);

    score->style().set(engraving::Sid::minSystemDistance, engraving::Spatium(10));
    score->style().set(engraving::Sid::maxSystemDistance, engraving::Spatium(10));
    score->style().set(engraving::Sid::staffLowerBorder, engraving::Spatium(5));
    score->style().set(engraving::Sid::staffUpperBorder, engraving::Spatium(7));

    score->setLayoutAll();
    score->doLayout();

    return result;
}

void VideoWriter::restoreScore(INotationPtr notation, const ScoreRestoreData& data)
{
    engraving::Score* score = notation->elements()->msScore();

    score->style() = data.style;

    score->setShowFrames(data.showFrames);
    score->setShowInstrumentNames(data.showInstrumentNames);
    score->setShowInvisible(data.showInvisible);
    score->setShowPageborders(data.showPageborders);
    score->setShowUnprintable(data.showUnprintable);
    score->setShowVBox(data.showVBox);

    score->setLayoutMode(data.layoutMode);

    score->setLayoutAll();
    score->update();
}

void VideoWriter::doGenerate(muse::media::IVideoEncoderPtr encoder, INotationPtr notation, const Config& config)
{
    Config actualConfig = config;
    auto restoreData = prepareScore(notation, actualConfig);
    if (!restoreData) {
        m_writeRet = make_ret(muse::Ret::Code::UnknownError);
        m_isCompleted = true;
        return;
    }

    DEFER {
        restoreScore(notation, restoreData.value());
        m_isCompleted = true;
    };

    // Setup painting
    QImage frame(actualConfig.width, actualConfig.height, QImage::Format_RGB32);
    frame.setDotsPerMeterX(std::lrint((actualConfig.canvasDpi * 1000) / engraving::INCH));
    frame.setDotsPerMeterY(std::lrint((actualConfig.canvasDpi * 1000) / engraving::INCH));

    QPainter qp(&frame);
    qp.setRenderHint(QPainter::Antialiasing, true);
    qp.setRenderHint(QPainter::TextAntialiasing, true);

    Painter painter(&qp, "video_writer");

    // Setup duration
    INotationPlaybackPtr playback = notation->masterNotation()->playback();
    float totalPlayTimeSec = playback->totalPlayTime();

    int leadingFrameCount = static_cast<int>(actualConfig.leadingSec * actualConfig.fps);
    int scoreFrameCount = static_cast<int>(totalPlayTimeSec * actualConfig.fps);
    int trailingFrameCount = static_cast<int>(actualConfig.trailingSec * actualConfig.fps);
    int totalFrameCount = leadingFrameCount + scoreFrameCount + trailingFrameCount;
    LOGI() << "totalPlayTime: " << totalPlayTimeSec << " sec" << " frame count " << totalFrameCount;

    m_progress.start();

    // Add score title
    if (!generateLeadingFrames(encoder, notation, painter, frame, actualConfig, totalFrameCount)) {
        return;
    }

    // Add score frames
    //! LKY fork: with more than one page per screen the frame is painted by our own renderer.
    const bool scoreFramesGenerated = actualConfig.pagesPerScreen > 1
                                      ? generateScoreFramesMultiPage(encoder, notation, qp, frame, actualConfig,
                                                                     totalPlayTimeSec, leadingFrameCount, totalFrameCount)
                                      : generateScoreFrames(encoder, notation, painter, frame, actualConfig,
                                                            totalPlayTimeSec, leadingFrameCount, totalFrameCount);
    if (!scoreFramesGenerated) {
        return;
    }

    // Add "Made with MuseScore"
    if (!generateTrailingFrames(encoder, actualConfig)) {
        return;
    }

    m_writeRet = muse::make_ok();
    m_progress.finish(muse::make_ok());
}

bool VideoWriter::generateLeadingFrames(muse::media::IVideoEncoderPtr encoder, INotationPtr notation,
                                        Painter& painter, QImage& frame,
                                        const Config& config, int totalFrameCount)
{
    int leadingFrameCount = static_cast<int>(config.leadingSec * config.fps);
    if (leadingFrameCount <= 0) {
        return true;
    }

    muse::String title = notationTitle(notation);
    muse::String subtitle = notationSubtitle(notation);

    auto scaledFontPointSize = [&config](double basePixelSize) {
        double pixelSize = basePixelSize * config.height / 1080.0;
        return pixelSize * 72.0 / engraving::DPI;
    };

    Font titleFont(Font::FontFamily(u"Muse Sans"), Font::Type::Text);
    titleFont.setPointSizeF(scaledFontPointSize(128.0));
    titleFont.setWeight(Font::Weight::Medium);

    Font subtitleFont(titleFont);
    subtitleFont.setPointSizeF(scaledFontPointSize(48.0));

    muse::RectF frameRect = muse::RectF::fromQRectF(QRectF(frame.rect()));

    const double maxTextWidth = frameRect.width() * 0.9;
    const double textLeft = (frameRect.width() - maxTextWidth) / 2.0;

    auto lineCount = [&](const FontMetrics& fm, const muse::String& text) {
        if (text.isEmpty()) {
            return 0;
        }
        double textWidth = fm.horizontalAdvance(text);
        return std::max(1, static_cast<int>(std::ceil(textWidth / maxTextWidth)));
    };

    FontMetrics titleFontMetrics(titleFont);
    const int titleLines = lineCount(titleFontMetrics, title);
    const double titleHeight = titleLines * titleFontMetrics.lineSpacing();

    FontMetrics subtitleFontMetrics(subtitleFont);
    const int subtitleLines = lineCount(subtitleFontMetrics, subtitle);
    const double subtitleHeight = subtitleLines * subtitleFontMetrics.lineSpacing();

    double centerY = frameRect.center().y();
    double titleTop = centerY - titleHeight / 2.0;
    muse::RectF titleRect(textLeft, titleTop, maxTextWidth, titleHeight);

    const double subtitleOffset = config.height / 20.0;
    double subtitleTop = titleRect.bottom() + subtitleOffset;
    muse::RectF subtitleRect(textLeft, subtitleTop, maxTextWidth, subtitleHeight);

    for (int f = 0; f < leadingFrameCount; f++) {
        if (m_abort) {
            m_writeRet = make_ret(muse::Ret::Code::Cancel);
            m_progress.finish(m_writeRet);
            return false;
        }

        m_progress.progress(f, totalFrameCount);

        painter.fillRect(frameRect, Color::BLACK);
        painter.setPen(Color::WHITE);
        painter.setFont(titleFont);
        painter.drawText(titleRect, AlignCenter, TextWordWrap, title);

        if (!subtitle.isEmpty()) {
            painter.setFont(subtitleFont);
            painter.drawText(subtitleRect, AlignCenter, TextWordWrap, subtitle);
        }

        encoder->encodeImage(frame);
    }

    return true;
}

bool VideoWriter::generateTrailingFrames(muse::media::IVideoEncoderPtr encoder, const Config& config)
{
    int trailingFrameCount = static_cast<int>(config.trailingSec * config.fps);
    if (trailingFrameCount <= 0) {
        return true;
    }

    static const muse::io::path_t RESOURCE_PATH = ":/videoexport/internal/resources/video_made_with.mp4";
    muse::ByteArray videoData = fileSystem()->readFile(RESOURCE_PATH).val;

    encoder->encodeVideo(videoData, trailingFrameCount);

    return true;
}

bool VideoWriter::generateScoreFrames(muse::media::IVideoEncoderPtr encoder, INotationPtr notation,
                                      Painter& painter, QImage& frame,
                                      const Config& config, float totalPlayTimeSec,
                                      int leadingFrameCount, int totalFrameCount)
{
    int scoreFrameCount = static_cast<int>(totalPlayTimeSec * config.fps);
    if (scoreFrameCount <= 0) {
        return true;
    }

    PageList pages = notation->elements()->pages();
    auto painting = notation->painting();
    INotationPlaybackPtr playback = notation->masterNotation()->playback();
    muse::RectF frameRect = muse::RectF::fromQRectF(QRectF(frame.rect()));

    auto pageByTick = [](const PageList& pages, tick_t tick) -> const Page* {
        for (const Page* p : pages) {
            if (tick < static_cast<tick_t>(p->endTick().ticks())) {
                return p;
            }
        }
        return nullptr;
    };

    const Color CURSOR_COLOR = Color(2, 109, 203, 127);

    PlaybackCursor cursor(iocContext());
    cursor.setNotation(notation);

    for (int f = 0; f < scoreFrameCount; f++) {
        if (m_abort) {
            m_writeRet = make_ret(muse::Ret::Code::Cancel);
            m_progress.finish(m_writeRet);
            return false;
        }

        m_progress.progress(leadingFrameCount + f, totalFrameCount);

        float currentTimeSec = static_cast<float>(f) / config.fps;
        if (currentTimeSec > totalPlayTimeSec) {
            currentTimeSec = totalPlayTimeSec;
        }

        tick_t tick = playback->secToTick(currentTimeSec);

        const Page* page = pageByTick(pages, tick);
        if (!page) {
            break;
        }

        if (!page->firstMeasure()) {
            // Skip pages with no notation
            continue;
        }

        INotationPainting::Options opt;
        opt.fromPage = static_cast<int>(page->pageNumber());
        opt.toPage = opt.fromPage;
        opt.deviceDpi = config.canvasDpi;

        painter.fillRect(frameRect, Color::BLACK);

        painter.save();
        painter.translate(config.moveToCenter);

        painting->paintPrint(&painter, opt);

        cursor.move(tick);

        muse::RectF cursorRect = cursor.rect();
        muse::PointF pagePos = page->pos();
        muse::RectF cursorAbsRect = cursorRect.translated(-pagePos);

        painter.fillRect(cursorAbsRect, CURSOR_COLOR);

        painter.restore();

        encoder->encodeImage(frame);
    }

    return true;
}

// ============================================================================
// LKY fork: several pages on screen at the same time ("连排多页")
//
// Why it is a separate renderer instead of a tweak of `generateScoreFrames()`:
// `INotationPainting::paintPrint()` always paints ONE page, and every page is painted at its own
// origin (paintItem() uses `item->pagePos()`, which is page-relative - see Paint::paintScore()).
// So "show N pages" cannot be done by asking the paint code for a range: it would stack all pages
// at the same place. We paint each page into its own QImage and compose them instead, which also
// gives us a place to cache a page (a page is redrawn for every frame it appears in, and painting
// it from scratch costs milliseconds).
// ============================================================================

void VideoWriter::pageGridOf(int pagesPerScreen, double pageAspect, double frameAspect, int& columns, int& rows)
{
    const int pages = std::max(1, pagesPerScreen);

    columns = 1;
    rows = pages;

    if (pageAspect <= 0.0 || frameAspect <= 0.0) {
        return;
    }

    //! NOTE Page height and frame height are both normalised to 1.
    //! A grid may have MORE slots than pages (5 pages -> 3x2, 7 pages -> 4x2): the last row is
    //! simply not full, and the caller centres the pages that are there. Restricting this to exact
    //! factorisations would leave prime page counts (5, 7) with nothing but a single row/column.
    //! Tie-break matters: 7 pages fit 4x2 and 5x2 with the SAME page size, while 5x2 wastes three
    //! slots instead of one - so among equal scales prefer the arrangement with fewer empty slots,
    //! and only then the wider one ("连排").
    double bestScale = 0.0;
    int bestWaste = std::numeric_limits<int>::max();

    for (int c = 1; c <= pages; ++c) {
        const int r = (pages + c - 1) / c;
        const double scale = std::min(frameAspect / (c * pageAspect), 1.0 / r);
        const int waste = c * r - pages;

        const bool better = scale > bestScale + 1e-9
                            || (std::abs(scale - bestScale) <= 1e-9
                                && (waste < bestWaste || (waste == bestWaste && c > columns)));

        if (better) {
            bestScale = scale;
            bestWaste = waste;
            columns = c;
            rows = r;
        }
    }
}

bool VideoWriter::generateScoreFramesMultiPage(muse::media::IVideoEncoderPtr encoder, INotationPtr notation,
                                               QPainter& qp, QImage& frame, const Config& config,
                                               float totalPlayTimeSec, int leadingFrameCount, int totalFrameCount)
{
    int scoreFrameCount = static_cast<int>(totalPlayTimeSec * config.fps);
    if (scoreFrameCount <= 0) {
        return true;
    }

    PageList pages = notation->elements()->pages();
    if (pages.empty()) {
        return true;
    }

    INotationPaintingPtr painting = notation->painting();
    INotationPlaybackPtr playback = notation->masterNotation()->playback();

    //! NOTE Pages of one score share a page style, and `Paint::paintScore()` uses the FIRST page
    //! size for its viewport as well - so the slot geometry is derived from that page only.
    INotationPainting::Options sizeOpt;
    sizeOpt.fromPage = 0;
    sizeOpt.toPage = 0;
    const muse::SizeF pageSizeInch = painting->pageSizeInch(sizeOpt);
    if (pageSizeInch.width() <= 0.0 || pageSizeInch.height() <= 0.0) {
        LOGE() << "invalid page size, cannot lay pages out";
        return true;
    }

    int columns = 1;
    int rows = 1;
    pageGridOf(config.pagesPerScreen,
               pageSizeInch.width() / pageSizeInch.height(),
               static_cast<double>(config.width) / config.height,
               columns, rows);
    const int pagesPerScreen = columns * rows;

    const int gap = std::max(1, static_cast<int>(std::lrint(config.width * 0.004)));
    const double availableWidth = config.width - gap * (columns - 1);
    const double availableHeight = config.height - gap * (rows - 1);
    const double dpi = std::min(availableWidth / (columns * pageSizeInch.width()),
                                availableHeight / (rows * pageSizeInch.height()));
    const int pageWidth = std::max(1, static_cast<int>(std::lrint(pageSizeInch.width() * dpi)));
    const int pageHeight = std::max(1, static_cast<int>(std::lrint(pageSizeInch.height() * dpi)));
    const double gridWidth = columns * pageWidth + gap * (columns - 1);
    const double gridHeight = rows * pageHeight + gap * (rows - 1);
    const double originX = (config.width - gridWidth) / 2.0;
    const double originY = (config.height - gridHeight) / 2.0;

    const int pageDpi = std::max(1, static_cast<int>(std::lrint(dpi)));
    //! NOTE Device pixels per engraving unit. `paintPrint()` maps the page onto `pageSizeInch * pageDpi`.
    const double scale = static_cast<double>(pageDpi) / engraving::DPI;

    LOGI() << "pages per screen: " << config.pagesPerScreen << " (" << columns << "x" << rows << ")"
           << ", page image: " << pageWidth << "x" << pageHeight << ", dpi: " << pageDpi;

    std::map<int, QImage> pageCache;

    auto renderPage = [&](int pageIndex) {
        QImage image(pageWidth, pageHeight, QImage::Format_RGB32);
        image.setDotsPerMeterX(std::lrint((pageDpi * 1000) / engraving::INCH));
        image.setDotsPerMeterY(std::lrint((pageDpi * 1000) / engraving::INCH));
        image.fill(Qt::white);

        INotationPainting::Options opt;
        opt.fromPage = pageIndex;
        opt.toPage = pageIndex;
        opt.deviceDpi = pageDpi;

        {
            QPainter pagePainter(&image);
            pagePainter.setRenderHint(QPainter::Antialiasing, true);
            pagePainter.setRenderHint(QPainter::TextAntialiasing, true);

            {
                Painter painter(&pagePainter, "video_writer");
                painting->paintPrint(&painter, opt);
            }
        }

        return image;
    };

    auto pageIndexByTick = [&pages](tick_t tick) {
        for (size_t i = 0; i < pages.size(); ++i) {
            if (tick < static_cast<tick_t>(pages.at(i)->endTick().ticks())) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(pages.size()) - 1;
    };

    PlaybackCursor cursor(iocContext());
    cursor.setNotation(notation);

    const QColor CURSOR_COLOR(2, 109, 203, 127);

    //! NOTE Pages are shown in fixed groups (0..n-1, n..2n-1, ...): the picture then changes once
    //! per group instead of once per page, which is the point of showing several pages at once.
    int currentGroupStart = -1;

    //! NOTE A group can fill fewer slots than the grid has: 5 pages in a 3x2 grid, 7 in a 4x2,
    //! or a short last group (15 pages with 4 per screen leaves 3). Slots are never resized, but
    //! the pages that ARE there are centred inside their row, so the last row does not lean left.
    int groupPageCount = 0;

    auto slotLeft = [&](int slot) {
        const int row = slot / columns;
        const int usedInRow = std::clamp(groupPageCount - row * columns, 0, columns);
        const double rowOffset = (columns - usedInRow) * (pageWidth + gap) / 2.0;
        return originX + rowOffset + (slot % columns) * (pageWidth + gap);
    };

    auto slotTop = [&](int slot) {
        return originY + (slot / columns) * (pageHeight + gap);
    };

    qp.save();
    qp.resetTransform();

    for (int f = 0; f < scoreFrameCount; f++) {
        if (m_abort) {
            qp.restore();
            m_writeRet = make_ret(muse::Ret::Code::Cancel);
            m_progress.finish(m_writeRet);
            return false;
        }

        m_progress.progress(leadingFrameCount + f, totalFrameCount);

        float currentTimeSec = static_cast<float>(f) / config.fps;
        if (currentTimeSec > totalPlayTimeSec) {
            currentTimeSec = totalPlayTimeSec;
        }

        const tick_t tick = playback->secToTick(currentTimeSec);
        const int currentPage = pageIndexByTick(tick);
        const int groupStart = (currentPage / pagesPerScreen) * pagesPerScreen;

        qp.fillRect(frame.rect(), Qt::black);

        if (groupStart != currentGroupStart) {
            currentGroupStart = groupStart;
            pageCache.clear();
            groupPageCount = std::min(pagesPerScreen, static_cast<int>(pages.size()) - groupStart);
        }

        for (int slot = 0; slot < pagesPerScreen; ++slot) {
            const int pageIndex = groupStart + slot;
            if (pageIndex >= static_cast<int>(pages.size())) {
                break;
            }

            if (!pages.at(pageIndex)->firstMeasure()) {
                //! NOTE A page without notation (e.g. a title page) keeps an empty slot.
                continue;
            }

            auto it = pageCache.find(pageIndex);
            if (it == pageCache.end()) {
                it = pageCache.emplace(pageIndex, renderPage(pageIndex)).first;
            }

            const QPoint slotOrigin(static_cast<int>(std::lrint(slotLeft(slot))),
                                    static_cast<int>(std::lrint(slotTop(slot))));

            qp.drawImage(slotOrigin, it->second);
        }

        //! NOTE The playback cursor is reported in canvas coordinates (it uses `canvasPos()`),
        //! while a page is painted in page coordinates - hence the `- page->pos()`.
        cursor.move(tick);
        const muse::RectF cursorRect = cursor.rect();
        const int cursorSlot = currentPage - groupStart;

        if (cursorSlot >= 0 && cursorSlot < pagesPerScreen
            && cursorRect.width() > 0.0 && cursorRect.height() > 0.0) {
            const double slotX = slotLeft(cursorSlot);
            const double slotY = slotTop(cursorSlot);
            const muse::PointF pagePos = pages.at(currentPage)->pos();

            const QRectF cursorDeviceRect(slotX + (cursorRect.x() - pagePos.x()) * scale,
                                          slotY + (cursorRect.y() - pagePos.y()) * scale,
                                          cursorRect.width() * scale,
                                          cursorRect.height() * scale);

            qp.fillRect(cursorDeviceRect, CURSOR_COLOR);
        }

        encoder->encodeImage(frame);
    }

    qp.restore();

    return true;
}
