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

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

//! ⚠️ The full path, like every other test in this tree: the include root is `src/`, not the module
//! directory - a bare `"scoresave.h"` resolves only for files that sit next to it.
#include "agentharness/qml/MuseScore/AgentHarness/scoresave.h"

using namespace muse::agentharness;

//! ── Saving to the file the score came from (第 146 条) ────────────────────────────────────────────
//!
//! ⛔ WHAT IS TESTABLE HERE IS THE TWO DECISIONS, NOT THE SAVE ITSELF. Writing the score is the app's job
//! (`INotationProject::save`) and needs a project, a score and a window; the parts this subsystem owns are
//! (a) WHEN to refuse - and a tool that opens a dialog it cannot wait for is the failure being refused -
//! and (b) HOW to tell that the file was written, which is a question about a file and nothing else.
//!
//! ⚠️ The third part - that a save reaches the user's actual file - is judged on the real program, because
//! it is a claim about the program (see 交接文档 §4.16).

TEST(AgentHarness_ScoreSave, RefusesAScoreThatHasNoFileYet)
{
    //! ⛔ THE REFUSAL IS THE RULE, and the reason is not squeamishness: with no path, the app's save asks
    //! the user for a filename (`ProjectActionsController::saveProject`), and a tool cannot wait for that
    //! dialog - it would report "saved" while the user is still looking at a file picker.
    const QString refused = saveRefusal(true, false, QString());

    EXPECT_FALSE(refused.isEmpty());
    EXPECT_TRUE(refused.contains(QStringLiteral("has not been saved yet"))) << refused.toStdString();
    //! ⚠️ And it NAMES THE WAY OUT: "I cannot do this" without "here is how it happens" is the shape of
    //! answer 第 137 条 measured the model drawing the wrong conclusion from.
    EXPECT_TRUE(refused.contains(QStringLiteral("Ctrl+Shift+S"))) << refused.toStdString();
}

TEST(AgentHarness_ScoreSave, AnImportedFileIsRefusedWithItsOwnReason)
{
    //! ⛔⛔ THIS IS THE TEST THE FIRST VERSION OF THE FUNCTION WOULD HAVE FAILED, and the real program is
    //! what taught us: `isNewlyCreated()` is TRUE for a file in an older format (`NotationProject::load`
    //! sets it when `mscVersion() < 400`), so the tool refused a score that HAD a file - and explained it as
    //! "this score has never been saved", which was simply not true. The reason and the instruction differ
    //! per case, so each case is pinned here.
    const QString refused = saveRefusal(true, true, QStringLiteral("C:/somewhere/old-score.mscx"));

    EXPECT_FALSE(refused.isEmpty());
    EXPECT_TRUE(refused.contains(QStringLiteral("imported"))) << refused.toStdString();
    EXPECT_TRUE(refused.contains(QStringLiteral("older format"))) << refused.toStdString();
    //! ⚠️ The path is named: "which file are you talking about" is the first question a reader has.
    EXPECT_TRUE(refused.contains(QStringLiteral("old-score.mscx"))) << refused.toStdString();
    //! ⛔ And it must NOT say "never been saved" - that sentence was the bug.
    EXPECT_FALSE(refused.contains(QStringLiteral("has not been saved yet"))) << refused.toStdString();
}

TEST(AgentHarness_ScoreSave, RefusesWhenThereIsNoScoreAtAll)
{
    const QString refused = saveRefusal(false, false, QString());

    EXPECT_FALSE(refused.isEmpty());
    EXPECT_TRUE(refused.contains(QStringLiteral("no score is open"))) << refused.toStdString();
}

TEST(AgentHarness_ScoreSave, AllowsAScoreWithACurrentFormatFile)
{
    //! The happy path: a file, in the format this build writes, that is not a backup.
    EXPECT_TRUE(saveRefusal(true, false, QStringLiteral("C:/somewhere/score.mscx")).isEmpty());
}

TEST(AgentHarness_ScoreSave, TheJudgeIsTheFileNotTheAppsWord)
{
    //! ⛔ `save()` returning success says the app thinks it wrote; this says the file changed. They disagree
    //! exactly when the save went somewhere else than the caller believes - which would otherwise be
    //! reported as a successful save of the wrong file.
    const QString path = QDir::tempPath() + QStringLiteral("/agentharness-save-judge.mscx");
    QFile::remove(path);

    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("first");
    }

    const QFileInfo before(path);
    ASSERT_TRUE(before.exists());
    EXPECT_FALSE(fileWasRewritten(path, before.lastModified(), before.size()))
        << "nothing has written it yet";

    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        //! ⚠️ LONGER, so the size alone proves it even if the modification time lands in the same clock
        //! tick (a filesystem with coarse timestamps is not a hypothesis - it is why the size is compared).
        file.write("second, and longer");
    }

    EXPECT_TRUE(fileWasRewritten(path, before.lastModified(), before.size()));

    //! ⛔ A FILE THAT IS GONE IS NOT A SAVE. If the path vanished, "the file did not get written" is the
    //! honest reading - and `false` is what the caller needs to hear.
    ASSERT_TRUE(QFile::remove(path));
    EXPECT_FALSE(fileWasRewritten(path, before.lastModified(), before.size()));

    //! And an empty path never counts as written, whatever the filesystem says.
    EXPECT_FALSE(fileWasRewritten(QString(), before.lastModified(), before.size()));
}
