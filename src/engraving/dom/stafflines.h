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

#ifndef MU_ENGRAVING_STAFFLINES_H
#define MU_ENGRAVING_STAFFLINES_H

#include <vector>

#include "engravingitem.h"
#include "iaudiowaveformprovider.h"

namespace mu::engraving {
//-------------------------------------------------------------------
//   @@ StaffLines
///    The StaffLines class is the graphic representation of a staff,
///    it draws the horizontal staff lines.
//-------------------------------------------------------------------

class StaffLines final : public EngravingItem
{
    OBJECT_ALLOCATOR(engraving, StaffLines)
    DECLARE_CLASSOF(ElementType::STAFF_LINES)

    //! Optional: registered by the audio track module. When present and a waveform staff
    //! is being laid out, the lane is filled with the audio waveform instead of staff
    //! lines. Absent (as in unit-test or stub builds) the lane simply stays empty.
    muse::GlobalInject<IAudioWaveformProvider> audioWaveformProvider;

public:

    StaffLines* clone() const override { return new StaffLines(*this); }

    PointF pagePos() const override;      ///< position in page coordinates
    PointF canvasPos() const override;    ///< position in page coordinates

    const std::vector<LineF>& lines() const { return m_lines; }
    void setLines(const std::vector<LineF>& l) { m_lines = l; }

    Measure* measure() const { return (Measure*)ownershipParent(); }
    double y1() const;

    double lw() const { return m_lw; }
    void setLw(double w) { m_lw = w; }

    RectF hitBBox() const override;
    Shape hitShape() const override;

    bool collectForDrawing() const override;

    //! The waveform source, or nullptr when the audio track module is not loaded.
    //! GlobalInject::operator() yields a shared_ptr; hand out the raw pointer because the
    //! provider is owned by the IoC registry for the lifetime of the app.
    IAudioWaveformProvider* waveformProvider() const { return audioWaveformProvider().get(); }

private:
    friend class Factory;
    StaffLines(Measure* parent);

    double m_lw = 0.0;
    std::vector<LineF> m_lines;
};
}

#endif // MU_LIBMSCORE_STAFFLINES_H
