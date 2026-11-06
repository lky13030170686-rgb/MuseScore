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

#pragma once

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "notation/inotationconfiguration.h"
#include "draw/types/geometry.h"
#include "midi/miditypes.h"

#include "notation/inotation_fwd.h"

class QColor;

namespace mu::notation {
class PlaybackCursor : public muse::Contextable, public muse::async::Asyncable
{
    muse::GlobalInject<INotationConfiguration> configuration;

public:
    PlaybackCursor(const muse::modularity::ContextPtr& iocCtx)
        : muse::Contextable(iocCtx) {}

    void paint(muse::draw::Painter* painter);

    void setNotation(INotationPtr notation);
    void move(muse::midi::tick_t tick);

    bool visible() const;
    void setVisible(bool arg);

    const muse::RectF& rect() const;

private:
    QColor color() const;

    muse::RectF resolveCursorRectByTick(int tick) const;

    //! ⛔ There is deliberately **no cache of `System*` / `Measure*` / `Segment*` here**.
    //! A re-layout (opening the mixer changes the notation view's width; zoom-to-fit and
    //! page/continuous mode changes do the same kind of thing) destroys and rebuilds those
    //! objects, and the score only announces *edits* through `changesChannel()`. A raw
    //! pointer kept between calls therefore dangles and the 23 ms playback-cursor timer
    //! walks it into `System::staffCanvasYpage()` → `page()->canvasPos()` → a virtual call
    //! through freed memory → `execute of address 0x0` while playing (2026-11-06).
    //! Re-resolving per call is cheap at 43 Hz; see resolveCursorRectByTick().

    bool m_visible = false;
    muse::RectF m_rect;

    mu::notation::INotationPtr m_notation;
};
}
