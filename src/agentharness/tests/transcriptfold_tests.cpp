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

#include "agentharness/qml/MuseScore/AgentHarness/transcriptfold.h"

using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS. The fold decides what the user SEES of a turn, and every way it can be wrong is
//! invisible from the outside: a reordered row still renders, a dropped row still renders, and a group
//! that swallowed an error still renders - it just renders the wrong story. So the properties that must
//! hold are pinned here rather than eyeballed on a screenshot:
//!   - nothing is reordered, nothing is dropped;
//!   - a run of one is folded too (one rule, no special case);
//!   - an error is flagged, so the view can refuse to hide it.

namespace {
QVariantMap row(const QString& kind, const QString& text, quint64 seq, bool isError = false)
{
    QVariantMap m;
    m.insert(QStringLiteral("kind"), kind);
    m.insert(QStringLiteral("role"), kind == QStringLiteral("user") ? QStringLiteral("you")
                                                                    : QStringLiteral("tool"));
    m.insert(QStringLiteral("text"), text);
    m.insert(QStringLiteral("seq"), QVariant::fromValue(seq));
    if (isError) {
        m.insert(QStringLiteral("isError"), true);
    }
    return m;
}

QVariantMap call(const QString& name, quint64 seq)
{
    return row(QStringLiteral("toolCall"), name + QStringLiteral(" {}"), seq);
}

QVariantMap result(const QString& text, quint64 seq, bool isError = false)
{
    return row(QStringLiteral("toolResult"), text, seq, isError);
}

QString kindOf(const QVariantList& rows, int index)
{
    return rows[index].toMap().value(QStringLiteral("kind")).toString();
}
} // namespace

TEST(AgentHarness_TranscriptFold, ARunOfToolRowsBecomesOneGroup)
{
    //! The measured case: four tools in one turn rendered as eight rows between the question and the
    //! answer. One group line is the whole point of this file.
    const QVariantList rows {
        row(QStringLiteral("user"), QStringLiteral("how many measures?"), 1),
        call(QStringLiteral("score_overview"), 2),
        call(QStringLiteral("score_window"), 3),
        result(QStringLiteral("score: …"), 4),
        result(QStringLiteral("measures 1..2 of 2"), 5),
        row(QStringLiteral("assistant"), QStringLiteral("2 measures."), 6),
    };

    const QVariantList folded = foldToolRows(rows);

    ASSERT_EQ(folded.size(), 3);
    EXPECT_EQ(kindOf(folded, 0), QStringLiteral("user"));
    EXPECT_EQ(kindOf(folded, 1), QStringLiteral("toolGroup"));
    EXPECT_EQ(kindOf(folded, 2), QStringLiteral("assistant"));

    const QVariantMap group = folded[1].toMap();
    EXPECT_EQ(group.value(QStringLiteral("callCount")).toInt(), 2);
    EXPECT_EQ(group.value(QStringLiteral("resultCount")).toInt(), 2);
    EXPECT_EQ(group.value(QStringLiteral("names")).toString(), QStringLiteral("score_overview, score_window"));
    EXPECT_EQ(group.value(QStringLiteral("items")).toList().size(), 4);
}

TEST(AgentHarness_TranscriptFold, NothingIsReorderedAndNothingIsDropped)
{
    //! ⛔ The two failures that would still "work": a fold that sorts (results before calls) and a fold
    //! that summarises (throws the rows away, so expanding the group has nothing to show).
    const QVariantList rows {
        call(QStringLiteral("a"), 1),
        result(QStringLiteral("ra"), 2),
        call(QStringLiteral("b"), 3),
        result(QStringLiteral("rb"), 4),
    };

    const QVariantList folded = foldToolRows(rows);
    ASSERT_EQ(folded.size(), 1);

    const QVariantList items = folded[0].toMap().value(QStringLiteral("items")).toList();
    ASSERT_EQ(items.size(), rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        //! The folded row is the ORIGINAL map, field for field - the fold may not be the reason a field
        //! disappears.
        EXPECT_EQ(items[i].toMap(), rows[i].toMap()) << "row " << i << " changed";
    }
}

TEST(AgentHarness_TranscriptFold, AnAnswerBetweenTwoToolRunsMakesTwoGroups)
{
    //! A turn with two tool-calling steps: the model's own words sit between them, and pulling both runs
    //! into one group would move those words out of the order they were said in.
    const QVariantList rows {
        call(QStringLiteral("a"), 1),
        result(QStringLiteral("ra"), 2),
        row(QStringLiteral("assistant"), QStringLiteral("I will look at the notes."), 3),
        call(QStringLiteral("b"), 4),
        result(QStringLiteral("rb"), 5),
    };

    const QVariantList folded = foldToolRows(rows);

    ASSERT_EQ(folded.size(), 3);
    EXPECT_EQ(kindOf(folded, 0), QStringLiteral("toolGroup"));
    EXPECT_EQ(kindOf(folded, 1), QStringLiteral("assistant"));
    EXPECT_EQ(kindOf(folded, 2), QStringLiteral("toolGroup"));
    EXPECT_NE(folded[0].toMap().value(QStringLiteral("groupId")).toString(),
              folded[2].toMap().value(QStringLiteral("groupId")).toString());
}

TEST(AgentHarness_TranscriptFold, ASingleToolCallIsFoldedToo)
{
    //! One rule, no special case: "fold runs of two or more" would leave the most common small turn
    //! (one read) exactly as noisy as before.
    const QVariantList folded = foldToolRows({ call(QStringLiteral("score_overview"), 1),
                                               result(QStringLiteral("score: …"), 2) });

    ASSERT_EQ(folded.size(), 1);
    EXPECT_EQ(kindOf(folded, 0), QStringLiteral("toolGroup"));
    EXPECT_EQ(folded[0].toMap().value(QStringLiteral("callCount")).toInt(), 1);
}

TEST(AgentHarness_TranscriptFold, AnErrorIsNeverHiddenInsideAGroup)
{
    //! A failed tool call explains everything that follows it. If the group did not carry the flag, the
    //! view would collapse the one row that says "this did not work".
    const QVariantList folded = foldToolRows({ call(QStringLiteral("note_set_pitch"), 1),
                                               result(QStringLiteral("no note at m1b1"), 2, true),
                                               call(QStringLiteral("score_window"), 3),
                                               result(QStringLiteral("measures 1..2 of 2"), 4) });

    ASSERT_EQ(folded.size(), 1);
    EXPECT_TRUE(folded[0].toMap().value(QStringLiteral("hasError")).toBool());
}

TEST(AgentHarness_TranscriptFold, RowsWithoutASequenceStillGetDistinctIds)
{
    //! The view keys its expand/collapse state by `groupId`. Two groups sharing an id would open and
    //! close together - and the caller that passes rows without a `seq` is exactly the one that would
    //! never notice.
    QVariantMap first;
    first.insert(QStringLiteral("kind"), QStringLiteral("toolCall"));
    first.insert(QStringLiteral("text"), QStringLiteral("a {}"));

    QVariantMap said;
    said.insert(QStringLiteral("kind"), QStringLiteral("assistant"));
    said.insert(QStringLiteral("text"), QStringLiteral("said"));

    QVariantMap second;
    second.insert(QStringLiteral("kind"), QStringLiteral("toolCall"));
    second.insert(QStringLiteral("text"), QStringLiteral("b {}"));

    const QVariantList folded = foldToolRows({ first, said, second });

    ASSERT_EQ(folded.size(), 3);
    EXPECT_NE(folded[0].toMap().value(QStringLiteral("groupId")).toString(),
              folded[2].toMap().value(QStringLiteral("groupId")).toString());
}

TEST(AgentHarness_TranscriptFold, TheFirstGroupsIdSurvivesNewRowsArriving)
{
    //! The transcript is rebuilt on every streamed token, and the view remembers which groups are open.
    //! An id derived from the group's POSITION would move when a new turn adds a group before it... but a
    //! seq-derived id does not, and this is the property that keeps a group the user opened open.
    const QVariantList first { call(QStringLiteral("a"), 7), result(QStringLiteral("ra"), 8) };
    const QVariantList later { call(QStringLiteral("a"), 7), result(QStringLiteral("ra"), 8),
                               row(QStringLiteral("assistant"), QStringLiteral("done"), 9),
                               call(QStringLiteral("b"), 10), result(QStringLiteral("rb"), 11) };

    const QString before = foldToolRows(first)[0].toMap().value(QStringLiteral("groupId")).toString();
    const QString after = foldToolRows(later)[0].toMap().value(QStringLiteral("groupId")).toString();

    //! ⚠️ Non-empty is asserted as well, and that is not decoration: the reverse verification for this
    //! file (making `foldToolRows` a no-op) left THIS test green, because two missing ids are equal to
    //! each other. A test that a broken build also passes is not evidence.
    EXPECT_FALSE(before.isEmpty());
    EXPECT_TRUE(before.startsWith(QStringLiteral("tg")));
    EXPECT_EQ(before, after);
}

TEST(AgentHarness_TranscriptFold, EmptyAndToolFreeTranscriptsAreUnchanged)
{
    EXPECT_TRUE(foldToolRows({}).isEmpty());

    const QVariantList rows { row(QStringLiteral("user"), QStringLiteral("hi"), 1),
                              row(QStringLiteral("assistant"), QStringLiteral("hello"), 2) };
    EXPECT_EQ(foldToolRows(rows), rows);
}
