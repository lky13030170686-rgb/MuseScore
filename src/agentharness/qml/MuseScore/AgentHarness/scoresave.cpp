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
#include "scoresave.h"

#include <QFileInfo>

using namespace muse::agentharness;

QString muse::agentharness::saveRefusal(bool hasProject, bool newlyCreated, const QString& path)
{
    if (!hasProject) {
        return QStringLiteral("no score is open, so there is nothing to save");
    }

    if (path.isEmpty()) {
        //! ⛔ THE REFUSAL NAMES THE WAY OUT, and the way out is the user's own Save As: the tool cannot ask
        //! for a filename (it would have to wait for a dialog), and inventing one would put a file on the
        //! user's disk that they never chose a place for.
        return QStringLiteral("this score has not been saved yet, so there is no file to save it to. Choosing "
                              "a name and a folder is the user's decision - ask them to save it once "
                              "(Ctrl+Shift+S), and this tool will keep that file up to date from then on.");
    }

    if (newlyCreated) {
        //! ⛔⛔ MEASURED 2026-10-17 (第 146 条), AND THE FIRST VERSION OF THIS MESSAGE WAS WRONG ABOUT IT.
        //! `isNewlyCreated()` does NOT mean "never saved": `NotationProject::load` sets it for a file whose
        //! MSC version is older than the current format, and for a backup - "this was IMPORTED, so the
        //! editor would rather not write it in place". The app's own save asks the user WHERE for exactly
        //! these files (`ProjectActionsController::saveProject`: `isExistingSave = !isNewlyCreated()`), and a
        //! tool must not do what the app deliberately does not: overwriting the user's old-format original
        //! with a converted one, in a place they did not choose.
        return QStringLiteral("the editor treats %1 as an imported file: it is in an older format (or a "
                              "backup), and the editor does not save those in place - it asks where to save "
                              "instead, which is a dialog a tool cannot wait for. Ask the user to save it once "
                              "by hand (Ctrl+S, then pick a location); the file they pick can be saved by this "
                              "tool from then on.").arg(path);
    }

    return QString();
}

bool muse::agentharness::fileWasRewritten(const QString& path, const QDateTime& before, qint64 sizeBefore)
{
    if (path.isEmpty()) {
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        //! ⚠️ A save that DELETED the file is not a save. `false` here reads as "the file did not get
        //! written", which is what the caller needs to hear.
        return false;
    }

    return info.lastModified() != before || info.size() != sizeBefore;
}
