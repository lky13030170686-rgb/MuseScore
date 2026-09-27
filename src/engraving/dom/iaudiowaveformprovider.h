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
#ifndef MU_ENGRAVING_IAUDIOWAVEFORMPROVIDER_H
#define MU_ENGRAVING_IAUDIOWAVEFORMPROVIDER_H

#include <cstdint>
#include <vector>

#include "global/modularity/imoduleinterface.h"

namespace mu::engraving {
//! One (min, max) amplitude pair covering a slice of audio, both in -1..1.
struct AudioWaveformPeak
{
    float min = 0.f;
    float max = 0.f;
};

//! Supplies waveform peaks to the engraving layer without engraving having to know
//! anything about audio decoding.
//!
//! Why an interface: engraving sits at the bottom of the dependency graph (it links only
//! muse::draw), so it cannot depend on src/audiotrack where the decoder and the peak
//! cache live. The audio track module registers an implementation at startup, and the
//! engraving renderer asks it for peaks in a time window. This mirrors how the audio
//! engine resolves app-provided sources through IAudioFileSourceProvider.
//!
//! Contract: implementations must be safe to call from the layout/paint thread and must
//! not allocate heavily per call (the renderer queries once per staff-lines layout).
class IAudioWaveformProvider : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IAudioWaveformProvider)

public:
    virtual ~IAudioWaveformProvider() = default;

    //! Whether any audio track is currently loaded and therefore worth drawing.
    virtual bool hasWaveform() const = 0;

    //! Total duration of the loaded audio, in seconds. 0 when none.
    virtual double waveformDuration() const = 0;

    //! Fills `out` with `count` peaks evenly spanning [fromSeconds, toSeconds).
    //! Out-of-range time yields silence rather than an error. `out` is resized to `count`.
    virtual void waveformPeaks(double fromSeconds, double toSeconds, int64_t count,
                               std::vector<AudioWaveformPeak>& out) const = 0;
};
} // namespace mu::engraving

#endif // MU_ENGRAVING_IAUDIOWAVEFORMPROVIDER_H
