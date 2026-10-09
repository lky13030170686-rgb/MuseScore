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

#pragma once

#include <QString>
#include <QVector>

#include <algorithm>
#include <vector>

namespace mu::engraving {
class Score;
}

namespace muse::agentharness {
struct MeasureGrid;

//! ── The one function in this file that touches engraving ─────────────────────────────────────
//! Declared here so callers get it from the same header; defined in the .cpp next to the arithmetic
//! it feeds. Everything else in this header is pure.
MeasureGrid buildMeasureGrid(const mu::engraving::Score* score);

//! A position on the score, expressed the way a musician says it: part, staff, measure, beat.
//!
//! WHY NOT TICKS. Ticks are the only correct internal coordinate - every upstream API takes them -
//! but they are the wrong thing to put in front of a model or a user. "Measure 3, beat 2" needs no
//! conversion on their side, survives a time-signature change being discussed, and cannot be
//! silently off by one the way a hand-computed tick can. The rule this project follows is:
//! **ticks inside, measure/beat at the boundary.**
//!
//! `beat` is 1-based (musicians count from 1) and `measure` is 1-based (so do measures).
//! `tickInBeat` is 0-based and only present when a position is *between* beats.
struct ScoreAddress
{
    int part = 0;         //!< Index into the score's part list; 0 = the score itself.
    int staff = 0;        //!< 0-based staff index (the UI shows staff 1 for this).
    int measure = 1;      //!< 1-based measure number.
    int beat = 1;         //!< 1-based beat within the measure.
    int tickInBeat = 0;   //!< 0-based offset inside the beat.

    //! "3:2" - compact, and what the panel and the model both show.
    QString toString() const;
};

//! The measure/time-signature grid of one score, flattened once so address lookups are cheap.
//!
//! Built from a live `Score` by `buildMeasureGrid()`, which is the only function in this file that
//! touches engraving. Everything else here is arithmetic on this snapshot - deliberately, because
//! the arithmetic is the part that has to be right and the part that can be unit-tested without a
//! score, an application, or an IoC container.
//!
//! WHY A FLATTENED COPY: the same reason the field never stores an `EngravingObject*`. A grid that
//! held `Measure*` would go stale the moment a measure is inserted, and stale here means "the model
//! edits the wrong bar", which is the most expensive class of bug this subsystem can have.
struct MeasureGrid
{
    //! Absolute tick where each measure starts; `starts[i]` is measure i (0-based).
    QVector<int> starts;
    //! Length of each measure in ticks. Irregular measures are why this is not derived from a
    //! single time signature.
    QVector<int> lengths;
    //! Ticks per beat, per measure. Carried per measure rather than globally: a time-signature
    //! change mid-score means the beat length is a property of the measure, not of the score.
    QVector<int> beatTicks;
    //! Ticks per whole measure (numerator x beat length), per measure.
    QVector<int> measureTicks;

    bool isEmpty() const { return starts.isEmpty(); }
    int measureCount() const { return starts.size(); }
    //! Total ticks covered by the grid.
    int totalTicks() const;

    //! Measure index (0-based) containing `tick`, or -1 when out of range.
    int measureIndexForTick(int tick) const;
};

//! ── Pure arithmetic (unit-testable without a score) ─────────────────────────────────────────

//! Address for a tick, given the grid. Falls back to measure 1 / beat 1 for an empty grid rather
//! than inventing a measure that does not exist.
ScoreAddress addressForTick(const MeasureGrid& grid, int tick, int staff = 0, int part = 0);

//! First tick of a measure (0-based measure index). Returns -1 when out of range - callers must
//! check, because "measure 99 of a 4-bar score" is a thing a model will ask for.
int tickForMeasure(const MeasureGrid& grid, int measureIndex);

//! Tick of a beat inside a measure. `beat` is 1-based. Returns -1 when out of range.
int tickForBeat(const MeasureGrid& grid, int measureIndex, int beat);

//! "m3b2" style, for compact tool arguments and logs.
QString formatAddress(const ScoreAddress& address);

} // namespace muse::agentharness
