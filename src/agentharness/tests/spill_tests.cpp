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

#include <QFile>
#include <QFileInfo>

#include "agentharness/qml/MuseScore/AgentHarness/spill.h"

using namespace muse::agentharness;

//! WHY THIS SUITE IS SMALL AND POINTED: the spill has one job - keep a huge result out of the conversation
//! without losing it - and exactly two ways to fail at it, both of which look fine from the outside:
//! firing on results that did not need it (turning a readable answer into a file path), and losing the
//! content it claims to have stored. So the tests are: it stays quiet below the threshold, and what it
//! says it wrote is what is on disk.

namespace {
QString bigText(const QString& marker)
{
    //! Comfortably past the threshold, and built from a marker so the head and the tail are identifiable.
    QString text = QStringLiteral("HEAD-%1\n").arg(marker);
    while (text.toUtf8().size() < SPILL_THRESHOLD_BYTES + 5000) {
        text += QStringLiteral("filler line that exists only to take up space\n");
    }
    text += QStringLiteral("TAIL-%1").arg(marker);
    return text;
}

//! The path the preview names, or an empty string when it names none.
QString pathFromPreview(const QString& preview)
{
    const QString marker = QStringLiteral("The whole result is in ");
    const int start = preview.indexOf(marker);
    if (start < 0) {
        return QString();
    }
    const int from = start + marker.size();
    const int end = preview.indexOf(QStringLiteral(" - read it"), from);
    return end < 0 ? QString() : preview.mid(from, end - from);
}
} // namespace

TEST(AgentHarness_Spill, AShortResultComesBackUntouched)
{
    const QString text = QStringLiteral("m1 4/4: [C4]/480 | [D4]/480");

    //! ⛔ THE MOST IMPORTANT CASE IS THE QUIET ONE, exactly as with the sanity gate. A spill that fires on
    //! an ordinary result replaces a perfectly readable answer with a file path - the caller asked what is
    //! in bar 1 and is told where a file is. Byte-for-byte equality is the assertion, because "it looks
    //! about the same" would pass against a preview that happens to be the whole text.
    EXPECT_EQ(spillIfLarge(QStringLiteral("score_window"), text), text);
}

TEST(AgentHarness_Spill, ALargeResultIsReplacedByAPreviewAndTheFileHoldsTheRest)
{
    const QString text = bigText(QStringLiteral("A"));

    const QString preview = spillIfLarge(QStringLiteral("score_window"), text);

    ASSERT_NE(preview, text) << "a result past the threshold must be replaced";
    EXPECT_LT(preview.toUtf8().size(), text.toUtf8().size()) << "the preview must be smaller";

    //! ⛔ AND THE FILE MUST HOLD WHAT THE PREVIEW CLAIMS. This is the half that fails silently: a spill that
    //! writes nothing, or writes a truncated buffer, still returns a preview naming a path - and the caller
    //! only finds out when it goes looking. Comparing the whole payload is the only assertion that catches
    //! "it said it saved it".
    const QString path = pathFromPreview(preview);
    ASSERT_FALSE(path.isEmpty()) << "the preview must name the file: " << preview.left(300).toStdString();

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    EXPECT_EQ(QString::fromUtf8(file.readAll()), text) << "the file must hold the WHOLE result";
    file.close();

    QFile::remove(path);
}

TEST(AgentHarness_Spill, ThePreviewKeepsBothEnds)
{
    const QString text = bigText(QStringLiteral("B"));

    const QString preview = spillIfLarge(QStringLiteral("command_list"), text);

    //! ⛔ BOTH ENDS, NOT JUST THE HEAD. The head says what the result IS; the tail is where a list ends up
    //! saying how many there were. Keeping only the head would hide exactly the part a caller checks to
    //! decide whether the result is complete - and a truncated list that still starts correctly is the
    //! most convincing kind of wrong.
    EXPECT_TRUE(preview.contains(QStringLiteral("HEAD-B"))) << "the preview must start with the result";
    EXPECT_TRUE(preview.contains(QStringLiteral("TAIL-B"))) << "and it must end with the result";
}

TEST(AgentHarness_Spill, ThePreviewSaysHowMuchWasLeftOut)
{
    const QString text = bigText(QStringLiteral("C"));

    const QString preview = spillIfLarge(QStringLiteral("score_window"), text);

    //! The count is what tells a caller whether going to the file is worth it. Without it the preview looks
    //! like a complete - if oddly short - result.
    EXPECT_TRUE(preview.contains(QStringLiteral("bytes omitted")))
        << preview.left(300).toStdString();

    const QString path = pathFromPreview(preview);
    if (!path.isEmpty()) {
        QFile::remove(path);
    }
}

TEST(AgentHarness_Spill, AToolNameWithPathCharactersDoesNotEscapeTheDirectory)
{
    //! ⚠️ Tool names are ours and contain no separators today. This pins the sanitiser anyway, because the
    //! consequence of it being missing is a write OUTSIDE the spill directory - a category of bug that is
    //! cheap to prevent and expensive to discover.
    const QString text = bigText(QStringLiteral("D"));

    const QString preview = spillIfLarge(QStringLiteral("../../evil/name"), text);

    const QString path = pathFromPreview(preview);
    ASSERT_FALSE(path.isEmpty());
    EXPECT_FALSE(path.contains(QStringLiteral(".."))) << path.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path)) << "the file must be where the preview says it is";
    QFile::remove(path);
}
