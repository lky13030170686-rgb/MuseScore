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
#include "semanticderive.h"

#include <QMap>

#include "addressing.h"
#include "fieldcontroller.h"

using namespace muse::agentharness;

//! ── Command types ────────────────────────────────────────────────────────────────────────────
//! Hand-written, and deliberately so. `CommandType` is a small, closed, semantic enum whose values
//! are already the right vocabulary ("ChangePitch", "AddElement", "InsertMeasures"); what is missing
//! upstream is only a name table. Note the `UNDO_NAME` macro in undoablecommand.h attaches a name to
//! *command classes*, not to the enum, and reaching those needs instances - so a table here is
//! cheaper and cannot drift into "instantiating commands just to read their names".
//!
//! ⚠️ This table must stay in sync with the enum. It is checked by a unit test that walks the enum's
//! own range, so a value added upstream without a name here fails the test rather than silently
//! rendering as "CommandType(52)".
static const QMap<int, QString>& commandTypeNames()
{
    static const QMap<int, QString> table = {
        { -1, QStringLiteral("Unknown") },

        // Parts
        { 0, QStringLiteral("InsertPart") },
        { 1, QStringLiteral("RemovePart") },
        { 2, QStringLiteral("AddPartToExcerpt") },
        { 3, QStringLiteral("SetSoloist") },
        { 4, QStringLiteral("ChangePart") },
        { 5, QStringLiteral("ConnectSharedPart") },
        { 6, QStringLiteral("DisconnectSharedPart") },

        // Staves
        { 7, QStringLiteral("InsertStaff") },
        { 8, QStringLiteral("RemoveStaff") },
        { 9, QStringLiteral("AddSystemObjectStaff") },
        { 10, QStringLiteral("RemoveSystemObjectStaff") },
        { 11, QStringLiteral("SortStaves") },
        { 12, QStringLiteral("ChangeStaff") },
        { 13, QStringLiteral("ChangeStaffType") },

        // MStaves
        { 14, QStringLiteral("InsertMStaff") },
        { 15, QStringLiteral("RemoveMStaff") },
        { 16, QStringLiteral("InsertStaves") },
        { 17, QStringLiteral("RemoveStaves") },
        { 18, QStringLiteral("ChangeMStaffProperties") },
        { 19, QStringLiteral("ChangeMStaffHideIfEmpty") },

        // Instruments
        { 20, QStringLiteral("ChangeInstrumentShort") },
        { 21, QStringLiteral("ChangeInstrumentLong") },
        { 22, QStringLiteral("ChangeInstrumentGroupOptions") },
        { 23, QStringLiteral("ChangeInstrumentNumber") },
        { 24, QStringLiteral("ChangeInstrument") },
        { 25, QStringLiteral("ChangeDrumset") },

        // Measures
        { 26, QStringLiteral("RemoveMeasures") },
        { 27, QStringLiteral("InsertMeasures") },
        { 28, QStringLiteral("ChangeMeasureLen") },
        { 29, QStringLiteral("ChangeMMRest") },
        { 30, QStringLiteral("ChangeMeasureRepeatCount") },

        // Elements
        { 31, QStringLiteral("AddElement") },
        { 32, QStringLiteral("RemoveElement") },
        { 33, QStringLiteral("Unlink") },
        { 34, QStringLiteral("Link") },
        { 35, QStringLiteral("ChangeElement") },
        { 36, QStringLiteral("ChangeParent") },

        // Notes
        { 37, QStringLiteral("ChangePitch") },
        { 38, QStringLiteral("ChangeFretting") },
        { 39, QStringLiteral("ChangeVelocity") },

        // ChordRest
        { 40, QStringLiteral("ChangeChordStaffMove") },
        { 41, QStringLiteral("SwapCR") },
        { 42, QStringLiteral("AddNoteParenthesesInfo") },
        { 43, QStringLiteral("RemoveNoteParenthesesInfo") },
        { 44, QStringLiteral("RemoveSingleNoteParentheses") },

        // Brackets
        { 45, QStringLiteral("RemoveBracket") },
        { 46, QStringLiteral("AddBracket") },

        // Fret
        { 47, QStringLiteral("FretDataChange") },
        { 48, QStringLiteral("FretDot") },
        { 49, QStringLiteral("FretMarker") },
        { 50, QStringLiteral("FretBarre") },
        { 51, QStringLiteral("FretClear") },
        { 52, QStringLiteral("AddFretDiagramToFretBox") },
        { 53, QStringLiteral("RemoveFretDiagramFromFretBox") },

        // Harmony
        { 54, QStringLiteral("TransposeHarmony") },

        // KeySig / Clef / Tremolo
        { 55, QStringLiteral("ChangeKeySig") },
        { 56, QStringLiteral("ChangeClefType") },
        { 57, QStringLiteral("MoveTremolo") },

        // Spanners / Ties
        { 58, QStringLiteral("ChangeSpannerElements") },
        { 59, QStringLiteral("InsertTimeUnmanagedSpanner") },
        { 60, QStringLiteral("ChangeStartEndSpanner") },
        { 61, QStringLiteral("ChangeTieEndPointActive") },

        // Style
        { 62, QStringLiteral("ChangeStyle") },
        { 63, QStringLiteral("ChangeStyleValues") },

        // Property / Voices / Excerpts / Meta / Text / Automation / Other
        { 64, QStringLiteral("ChangeProperty") },
        { 65, QStringLiteral("ExchangeVoice") },
        { 66, QStringLiteral("AddExcerpt") },
        { 67, QStringLiteral("RemoveExcerpt") },
        { 68, QStringLiteral("SwapExcerpt") },
        { 69, QStringLiteral("ChangeExcerptTitle") },
        { 70, QStringLiteral("ChangeMetaInfo") },
        { 71, QStringLiteral("TextEdit") },
        { 72, QStringLiteral("EditAutomationPoints") },
        { 73, QStringLiteral("InsertTime") },
        { 74, QStringLiteral("ChangeScoreOrder") },
    };
    return table;
}

//! Element-type buckets the field can actually produce, named only where the name earns its keep.
//!
//! WHY NOT ALL ~156: `ElementType` has no name table upstream, and writing 156 literals by hand
//! would be a large, silently-drifting duplicate of an enum we do not own. More importantly, most of
//! those values are engraving-internal taxonomy (a specific kind of bracket, a specific notehead)
//! that adds noise rather than meaning. What a reader needs from this field is "was it notes, or
//! text, or a time signature" - so the common cases are named and everything else renders as
//! `ElementType(<n>)`, which is honest and greppable.
static const QMap<int, QString>& elementTypeNames()
{
    static const QMap<int, QString> table = {
        { 0, QStringLiteral("INVALID") },
    };
    return table;
}

QString muse::agentharness::commandTypeName(int value)
{
    const auto& table = commandTypeNames();
    const auto it = table.constFind(value);
    if (it != table.constEnd()) {
        return it.value();
    }

    //! Not "unknown" - the number, so it can be looked up. An unnamed kind is still evidence that
    //! something happened, and dropping it would make the op stream look cleaner than reality.
    return QStringLiteral("CommandType(%1)").arg(value);
}

bool muse::agentharness::isElementTypeBucket(int value)
{
    //! Reads FieldController's constant rather than repeating 1000: the packing is done on that
    //! side, so the unpacking rule has to come from the same place or the two can drift.
    return value >= FieldController::TYPE_BUCKET_OFFSET;
}

QString muse::agentharness::elementTypeName(int value)
{
    if (!isElementTypeBucket(value)) {
        return QString();
    }

    const int raw = value - FieldController::TYPE_BUCKET_OFFSET;
    const auto& table = elementTypeNames();
    const auto it = table.constFind(raw);
    if (it != table.constEnd()) {
        return it.value();
    }

    return QStringLiteral("ElementType(%1)").arg(raw);
}

QString SemanticOp::toString() const
{
    QString out = QStringLiteral("#%1 ").arg(seq);

    if (isUndo) {
        out += QStringLiteral("[undo] ");
    } else if (isRedo) {
        out += QStringLiteral("[redo] ");
    }

    out += action;

    if (!where.isEmpty()) {
        out += QStringLiteral(" @") + where;
    }

    if (!kinds.isEmpty()) {
        out += QStringLiteral(" (") + kinds.join(QLatin1Char('+')) + QLatin1Char(')');
    }

    return out;
}

QVector<SemanticOp> muse::agentharness::deriveSemanticOps(const QVector<RawFieldEvent>& raw, const MeasureGrid& grid, int part)
{
    QVector<SemanticOp> ops;
    ops.reserve(raw.size());

    for (const RawFieldEvent& e : raw) {
        SemanticOp op;
        op.seq = e.seq;
        op.wallClock = e.wallClock;
        op.action = e.action;
        op.isUndo = e.isUndo;
        op.isRedo = e.isRedo;
        op.tickFrom = e.tickFrom;
        op.tickTo = e.tickTo;
        op.staffFrom = e.staffFrom;
        op.staffTo = e.staffTo;
        op.objectCount = e.objectCount;

        switch (e.source) {
        case RawFieldEvent::Source::User:    op.source = QStringLiteral("user"); break;
        case RawFieldEvent::Source::Agent:   op.source = QStringLiteral("agent"); break;
        case RawFieldEvent::Source::Plugin:  op.source = QStringLiteral("plugin"); break;
        case RawFieldEvent::Source::Unknown: op.source = QStringLiteral("unknown"); break;
        }

        //! Kinds, sorted and de-duplicated. Sorted because "derive the same ops from the same raw
        //! log" has to be a byte-for-byte claim to be testable, and a QMap iterates in key order.
        for (auto it = e.typeCounts.constBegin(); it != e.typeCounts.constEnd(); ++it) {
            const QString name = isElementTypeBucket(it.key()) ? elementTypeName(it.key()) : commandTypeName(it.key());
            if (!name.isEmpty() && !op.kinds.contains(name)) {
                op.kinds.append(name);
            }
        }
        op.kinds.sort();

        //! The readable position. Built from the START of the changed range: "which bar did this
        //! touch" is the question being asked, and a range spanning several bars is still reported
        //! by where it begins plus the tick range in the fields above.
        if (op.tickFrom >= 0 && !grid.isEmpty()) {
            const ScoreAddress from = addressForTick(grid, op.tickFrom, op.staffFrom, part);
            op.where = formatAddress(from);

            if (op.tickTo > op.tickFrom) {
                const ScoreAddress to = addressForTick(grid, op.tickTo, op.staffTo, part);
                if (to.measure != from.measure) {
                    op.where += QStringLiteral("..") + formatAddress(to);
                }
            }
        }

        ops.append(op);
    }

    return ops;
}
