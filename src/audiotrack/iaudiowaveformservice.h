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
#ifndef MUSE_AUDIOTRACK_IAUDIOWAVEFORMSERVICE_H
#define MUSE_AUDIOTRACK_IAUDIOWAVEFORMSERVICE_H

#include "modularity/imoduleinterface.h"
#include "io/path.h"
#include "types/ret.h"
#include "async/notification.h"

namespace muse::audiotrack {
//! Prepares the waveform for an audio file and makes it available to the engraving layer.
//!
//! Owns the decode-and-reduce step: opening the file, streaming it into a multi-level
//! peak cache, and installing that cache into the waveform provider that engraving reads.
//! Callers (currently the playback controller) only hand over a path; they do not deal
//! with cache levels or decoding.
//!
//! Generation must not block the UI: a 5-minute file is tens of millions of samples. The
//! implementation therefore runs the work off the calling thread and reports completion
//! through waveformChanged().
class IAudioWaveformService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IAudioWaveformService)

public:
    virtual ~IAudioWaveformService() = default;

    //! Decodes `path` and prepares its waveform. Returns immediately.
    //! Any previously prepared waveform is replaced once the new one is ready.
    virtual void loadWaveform(const muse::io::path_t& path) = 0;

    //! Drops the current waveform (e.g. the audio track was removed).
    virtual void clearWaveform() = 0;

    //! Whether a waveform is currently available for drawing.
    virtual bool hasWaveform() const = 0;

    //! Fired after the waveform becomes available or is cleared, so the score can be
    //! asked to re-layout. Delivered on the thread that finished the work; listeners
    //! that touch the UI must marshal to the main thread themselves.
    virtual muse::async::Notification waveformChanged() const = 0;
};
} // namespace muse::audiotrack

#endif // MUSE_AUDIOTRACK_IAUDIOWAVEFORMSERVICE_H
