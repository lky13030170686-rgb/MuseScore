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
#include <gtest/gtest.h>

#include "agentharness/qml/MuseScore/AgentHarness/fieldcontroller.h"
#include "agentharness/qml/MuseScore/AgentHarness/semanticderive.h"

using namespace muse::agentharness;

namespace {
//! A raw event with the fields the derivation actually reads. Kept as a helper rather than a
//! fixture so each test states only the fields it cares about.
RawFieldEvent makeEvent(quint64 seq, const QString& action, int tickFrom, int tickTo, int objectCount = 1)
{
    RawFieldEvent e;
    e.seq = seq;
    e.wallClock = QStringLiteral("2026-10-09T00:00:00.000");
    e.source = RawFieldEvent::Source::User;
    e.action = action;
    e.tickFrom = tickFrom;
    e.tickTo = tickTo;
    e.staffFrom = 0;
    e.staffTo = 0;
    e.objectCount = objectCount;
    return e;
}

//! 4/4, four bars of 1920 ticks, quarter-note beats - built by hand so the derivation tests do not
//! depend on a score file. The grid/score agreement is asserted separately in addressing_tests.
MeasureGrid fourFourGrid(int measures = 4)
{
    MeasureGrid grid;
    for (int i = 0; i < measures; ++i) {
        grid.starts.append(i * 1920);
        grid.lengths.append(1920);
        grid.beatTicks.append(480);
        grid.measureTicks.append(1920);
    }
    return grid;
}
} // namespace

//! ── The op stream mirrors the raw stream ──────────────────────────────────────────────────────

TEST(AgentHarness_SemanticDerive, OneRawEventBecomesOneOp)
{
    //! One-to-one on purpose: the raw layer is already one-event-per-transaction, and the op stream
    //! has to line up with the undo stack because the undo stack is what the user can act on. An op
    //! stream that merged events would no longer explain what Ctrl+Z will take back.
    QVector<RawFieldEvent> raw;
    raw.append(makeEvent(1, QStringLiteral("Insert note"), 0, 0));
    raw.append(makeEvent(2, QStringLiteral("Insert note"), 480, 480));
    raw.append(makeEvent(3, QStringLiteral("Transpose harmony"), 0, 1920));

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, fourFourGrid());

    ASSERT_EQ(ops.size(), 3);
    EXPECT_EQ(ops[0].seq, 1);
    EXPECT_EQ(ops[1].seq, 2);
    EXPECT_EQ(ops[2].seq, 3);
    EXPECT_EQ(ops[2].action, QStringLiteral("Transpose harmony"));
    EXPECT_EQ(ops[2].source, QStringLiteral("user"));
}

TEST(AgentHarness_SemanticDerive, OrderIsPreservedAndSeqIsTheJoinKey)
{
    //! The raw layer hands events oldest-first; the semantic layer must not reorder them, because
    //! `seq` is the coordinate the two layers are joined on (and the panel reverses for display).
    QVector<RawFieldEvent> raw;
    for (int i = 1; i <= 5; ++i) {
        raw.append(makeEvent(quint64(i), QStringLiteral("op%1").arg(i), i * 480, i * 480));
    }

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, fourFourGrid());
    ASSERT_EQ(ops.size(), 5);
    for (int i = 0; i < ops.size(); ++i) {
        EXPECT_EQ(ops[i].seq, quint64(i + 1));
    }
}

//! ── Addresses ─────────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SemanticDerive, OpCarriesAReadablePosition)
{
    const MeasureGrid grid = fourFourGrid();

    QVector<RawFieldEvent> raw;
    raw.append(makeEvent(1, QStringLiteral("Insert note"), 0, 0));            // m1 b1
    raw.append(makeEvent(2, QStringLiteral("Insert note"), 480, 480));        // m1 b2
    raw.append(makeEvent(3, QStringLiteral("Insert note"), 1920 + 960, 0));   // m2 b3

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, grid);
    ASSERT_EQ(ops.size(), 3);
    EXPECT_EQ(ops[0].where, QStringLiteral("m1b1"));
    EXPECT_EQ(ops[1].where, QStringLiteral("m1b2"));
    EXPECT_EQ(ops[2].where, QStringLiteral("m2b3"));
}

TEST(AgentHarness_SemanticDerive, ARangeSpanningMeasuresShowsBothEnds)
{
    const MeasureGrid grid = fourFourGrid();

    QVector<RawFieldEvent> raw;
    raw.append(makeEvent(1, QStringLiteral("Transpose harmony"), 0, 1920 + 480));

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, grid);
    ASSERT_EQ(ops.size(), 1);
    //! A range inside one bar must NOT get a "..": "m1b1..m1b1" is noise, and noise in the field is
    //! what makes a reader stop reading it.
    EXPECT_EQ(ops[0].where, QStringLiteral("m1b1..m2b2"));
}

TEST(AgentHarness_SemanticDerive, SameMeasureRangeIsNotRepeated)
{
    const MeasureGrid grid = fourFourGrid();

    QVector<RawFieldEvent> raw;
    raw.append(makeEvent(1, QStringLiteral("Change velocity"), 0, 1440));

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, grid);
    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0].where, QStringLiteral("m1b1")) << "both ends are in measure 1";
}

TEST(AgentHarness_SemanticDerive, NoGridMeansNoPositionButTheOpSurvives)
{
    //! With no score open (or a grid that could not be built) the operation must still be recorded.
    //! Dropping it would put a hole in the history exactly when something odd is happening - which
    //! is the moment the history matters most.
    QVector<RawFieldEvent> raw;
    raw.append(makeEvent(1, QStringLiteral("Insert note"), 0, 0));

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, MeasureGrid());
    ASSERT_EQ(ops.size(), 1);
    EXPECT_TRUE(ops[0].where.isEmpty());
    EXPECT_EQ(ops[0].action, QStringLiteral("Insert note"));
    EXPECT_EQ(ops[0].tickFrom, 0) << "the raw coordinate is still there";
}

TEST(AgentHarness_SemanticDerive, NegativeTicksAreNotAddressed)
{
    //! `tickFrom == -1` is the raw layer's "no boundary in this payload". It must not be turned into
    //! "measure 1 beat 1", which would be a fabricated position.
    QVector<RawFieldEvent> raw;
    RawFieldEvent e = makeEvent(1, QStringLiteral("Insert note"), -1, -1);
    raw.append(e);

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, fourFourGrid());
    ASSERT_EQ(ops.size(), 1);
    EXPECT_TRUE(ops[0].where.isEmpty());
}

//! ── Undo / redo marking ───────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SemanticDerive, UndoKeepsTheActionNameButIsMarked)
{
    //! The action name describes the operation being *undone*, so a reader must never take the name
    //! alone as "what happened" - that is why `isUndo` exists and why `toString()` prefixes it.
    QVector<RawFieldEvent> raw;
    raw.append(makeEvent(1, QStringLiteral("Insert note"), 0, 0));

    RawFieldEvent undo = makeEvent(2, QStringLiteral("Insert note"), 0, 0);
    undo.isUndo = true;
    raw.append(undo);

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, fourFourGrid());
    ASSERT_EQ(ops.size(), 2);
    EXPECT_FALSE(ops[0].isUndo);
    EXPECT_TRUE(ops[1].isUndo);
    EXPECT_EQ(ops[1].action, QStringLiteral("Insert note")) << "the name is the undone operation";
    EXPECT_TRUE(ops[1].toString().contains(QStringLiteral("[undo]")));
}

TEST(AgentHarness_SemanticDerive, RedoIsOnlyMarkedWhenItIsActuallyAKnownRedo)
{
    //! Regression guard for the M0 bug: "the state index went up" was once treated as redo, which
    //! marked every new note after the first as a redo. The derivation must report exactly what the
    //! raw layer says and nothing more.
    QVector<RawFieldEvent> raw;
    for (int i = 1; i <= 3; ++i) {
        raw.append(makeEvent(quint64(i), QStringLiteral("Insert note"), i * 480, i * 480));
    }

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, fourFourGrid());
    ASSERT_EQ(ops.size(), 3);
    for (const SemanticOp& op : ops) {
        EXPECT_FALSE(op.isRedo) << "a fresh edit is never a redo";
        EXPECT_FALSE(op.isUndo);
    }
}

//! ── Kinds ─────────────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SemanticDerive, KindsAreNamedSortedAndDeduplicated)
{
    QVector<RawFieldEvent> raw;
    RawFieldEvent e = makeEvent(1, QStringLiteral("Insert note"), 0, 0);
    //! Deliberately out of order, and with a duplicate, so sorting and de-duplication are both
    //! exercised. Sorted output is what makes "same log, same ops" a byte-for-byte claim.
    e.typeCounts[31] = 2;   // AddElement
    e.typeCounts[37] = 1;   // ChangePitch
    e.typeCounts[31] = 3;   // AddElement again (same key: still one entry)
    raw.append(e);

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, fourFourGrid());
    ASSERT_EQ(ops.size(), 1);
    ASSERT_EQ(ops[0].kinds.size(), 2);
    EXPECT_EQ(ops[0].kinds[0], QStringLiteral("AddElement"));
    EXPECT_EQ(ops[0].kinds[1], QStringLiteral("ChangePitch"));
}

TEST(AgentHarness_SemanticDerive, AnUnnamedKindIsReportedByNumberNotDropped)
{
    //! An unnamed kind is still evidence that something happened. Rendering it as "CommandType(999)"
    //! keeps it greppable; dropping it would make the op stream look cleaner than reality.
    EXPECT_EQ(commandTypeName(999), QStringLiteral("CommandType(999)"));
    EXPECT_EQ(commandTypeName(31), QStringLiteral("AddElement"));
    EXPECT_EQ(commandTypeName(37), QStringLiteral("ChangePitch"));
}

TEST(AgentHarness_SemanticDerive, ElementTypeBucketsAreSeparatedFromCommandTypes)
{
    //! The raw layer packs both into one histogram with an offset. If the unpacking rule drifted from
    //! the packing rule, element types would be reported as command types - a wrong label, which is
    //! worse than no label.
    EXPECT_FALSE(isElementTypeBucket(0));
    EXPECT_FALSE(isElementTypeBucket(FieldController::TYPE_BUCKET_OFFSET - 1));
    EXPECT_TRUE(isElementTypeBucket(FieldController::TYPE_BUCKET_OFFSET));
    EXPECT_TRUE(isElementTypeBucket(FieldController::TYPE_BUCKET_OFFSET + 5));

    EXPECT_TRUE(elementTypeName(5).isEmpty()) << "not an element bucket";

    QVector<RawFieldEvent> raw;
    RawFieldEvent e = makeEvent(1, QStringLiteral("Insert note"), 0, 0);
    e.typeCounts[FieldController::TYPE_BUCKET_OFFSET + 7] = 1;
    raw.append(e);

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, fourFourGrid());
    ASSERT_EQ(ops.size(), 1);
    ASSERT_EQ(ops[0].kinds.size(), 1);
    EXPECT_TRUE(ops[0].kinds[0].startsWith(QStringLiteral("ElementType(")))
        << "an unnamed element type must not be reported as a command type";
}

//! ── Determinism ───────────────────────────────────────────────────────────────────────────────

TEST(AgentHarness_SemanticDerive, DerivingTwiceGivesTheSameAnswer)
{
    //! The claim that makes this layer safe to be opinionated: it is a pure function of the raw
    //! facts, so a wrong rule costs a re-run rather than the history.
    QVector<RawFieldEvent> raw;
    for (int i = 1; i <= 6; ++i) {
        RawFieldEvent e = makeEvent(quint64(i), QStringLiteral("Insert note"), i * 240, i * 240 + 120);
        e.typeCounts[31] = 1;
        e.typeCounts[37] = 1;
        raw.append(e);
    }

    const MeasureGrid grid = fourFourGrid();
    const QVector<SemanticOp> a = deriveSemanticOps(raw, grid);
    const QVector<SemanticOp> b = deriveSemanticOps(raw, grid);

    ASSERT_EQ(a.size(), b.size());
    for (int i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].toString(), b[i].toString());
        EXPECT_EQ(a[i].where, b[i].where);
        EXPECT_EQ(a[i].kinds, b[i].kinds);
    }
}

TEST(AgentHarness_SemanticDerive, EmptyInputGivesEmptyOutput)
{
    const QVector<SemanticOp> ops = deriveSemanticOps(QVector<RawFieldEvent>(), fourFourGrid());
    EXPECT_TRUE(ops.isEmpty());
}
