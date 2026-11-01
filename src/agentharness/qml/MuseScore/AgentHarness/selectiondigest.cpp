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
#include "selectiondigest.h"

using namespace muse::agentharness;

namespace {
//! "m3b2" - the same compact form the tools take, so a reader can copy the address straight into a call.
QString positionOf(int measure, int beat)
{
    if (measure < 1) {
        return QString();
    }
    if (beat < 1) {
        return QStringLiteral("m%1").arg(measure);
    }
    return QStringLiteral("m%1b%2").arg(measure).arg(beat);
}
} // namespace

QString muse::agentharness::describeSelection(const SelectionFacts& facts)
{    switch (facts.kind) {
    case SelectionFacts::Kind::None:
        return QString();

    case SelectionFacts::Kind::Single: {
        const QString position = positionOf(facts.firstMeasure, facts.firstBeat);
        if (position.isEmpty()) {
            //! A selection exists but its position could not be resolved. Saying "selected" would leave the
            //! reader to guess WHERE, which is the one thing an address is for - so say the part that is
            //! true and stop.
            return facts.elementType.isEmpty() ? QString() : facts.elementType;
        }

        QString out = position;
        if (!facts.elementType.isEmpty()) {
            out += QStringLiteral(" · %1").arg(facts.elementType);
        }
        if (!facts.pitch.isEmpty()) {
            out += QLatin1Char(' ') + facts.pitch;
        }
        return out;
    }

    case SelectionFacts::Kind::Range: {
        const QString from = positionOf(facts.firstMeasure, facts.firstBeat);
        const QString to = positionOf(facts.lastMeasure, facts.lastBeat);
        if (from.isEmpty() || to.isEmpty()) {
            return QString();
        }

        //! `from..to` even when both ends are the same position: a range of one beat is still a range, and
        //! writing it as a single "m3b2" would lose the fact that there is a span (however small) rather
        //! than one clicked element.
        QString out = from == to ? from : QStringLiteral("%1..%2").arg(from, to);

        if (facts.firstStaff > 0) {
            out += facts.firstStaff == facts.lastStaff
                   ? QStringLiteral(" · staff %1").arg(facts.firstStaff)
                   : QStringLiteral(" · staves %1..%2").arg(facts.firstStaff).arg(facts.lastStaff);
        }

        //! "(s)" and not a plural rule: this line is read by a model, not printed in a sentence, and a
        //! model that has to infer a number from an inflected noun is a failure mode with no upside.
        out += QStringLiteral(" · %1 note(s)").arg(facts.noteCount);
        return out;
    }
    }

    return QString();
}

int muse::agentharness::rangeLastTick(int startTick, int endTickExclusive)
{
    //! A range whose end is not after its start is empty (or malformed). Answering `end - 1` would produce a
    //! tick BEFORE the selection started, and the caller would then address a measure that was never
    //! selected - so the start tick is the honest floor.
    return endTickExclusive > startTick ? endTickExclusive - 1 : startTick;
}

StaffSpan muse::agentharness::staffSpanForDisplay(int startStaffIndex, int endStaffIndexExclusive)
{
    StaffSpan span;
    if (startStaffIndex < 0 || endStaffIndexExclusive <= startStaffIndex) {
        return span;
    }

    //! `end` is EXCLUSIVE, so the last staff inside the range has index `end - 1`; its 1-BASED number is
    //! `end` itself. (Measured: a one-staff selection is start=0, end=1, and the first version's `end + 1`
    //! printed "staves 1..2" - the score has one staff.)
    span.first = startStaffIndex + 1;
    span.last = endStaffIndexExclusive;
    return span;
}
