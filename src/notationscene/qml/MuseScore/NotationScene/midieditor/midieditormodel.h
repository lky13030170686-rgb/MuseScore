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

#include <QObject>
#include <QVariantList>
#include <qqmlintegration.h>

#include "async/asyncable.h"

#include "modularity/ioc.h"
#include "context/iglobalcontext.h"

#include "notation/inotation.h"

#include "midieditornotes.h"

namespace mu::engraving {
class Score;
class Note;
}

namespace mu::notation {
//! NOTE: The "MIDI" page (musescore://midi) shows the very same score as the notation page,
//!       but as a piano roll. This model is the only thing between that view and the engraving
//!       model: it flattens the score into a list of note rectangles and writes edits back
//!       through the regular property/undo machinery.
class MidiEditorModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT
    QML_ELEMENT;

    Q_PROPERTY(bool hasScore READ hasScore NOTIFY scoreChanged)
    Q_PROPERTY(QVariantList notes READ notes NOTIFY scoreChanged)
    Q_PROPERTY(QVariantList measures READ measures NOTIFY scoreChanged)
    Q_PROPERTY(int lowestPitch READ lowestPitch NOTIFY scoreChanged)
    Q_PROPERTY(int highestPitch READ highestPitch NOTIFY scoreChanged)
    Q_PROPERTY(int totalTicks READ totalTicks NOTIFY scoreChanged)
    Q_PROPERTY(int staffCount READ staffCount NOTIFY scoreChanged)
    Q_PROPERTY(QStringList staffNames READ staffNames NOTIFY scoreChanged)
    Q_PROPERTY(QString scoreName READ scoreName NOTIFY scoreChanged)
    Q_PROPERTY(double playbackTick READ playbackTick NOTIFY playbackTickChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playbackTickChanged)

    muse::ContextInject<context::IGlobalContext> context = { this };

public:
    explicit MidiEditorModel(QObject* parent = nullptr);
    ~MidiEditorModel() override;

    bool hasScore() const { return m_hasScore; }
    QVariantList notes() const { return m_notes; }
    QVariantList measures() const { return m_measures; }
    int lowestPitch() const { return m_lowestPitch; }
    int highestPitch() const { return m_highestPitch; }
    int totalTicks() const { return m_totalTicks; }
    int staffCount() const { return int(m_staffNames.size()); }
    QStringList staffNames() const { return m_staffNames; }
    QString scoreName() const { return m_scoreName; }
    double playbackTick() const { return m_playbackTick; }
    bool isPlaying() const { return m_isPlaying; }

    Q_INVOKABLE void init();

    //! NOTE: `row` is an index into notes(), and is only stable until the score changes.
    //!       The view is expected to submit one edit when the mouse is released (not on every
    //!       mouse move), which is what the audio lane drag does as well.
    Q_INVOKABLE void setNotePitch(int row, int pitch);
    Q_INVOKABLE void setNoteVelocity(int row, int velocity);

signals:
    void scoreChanged();
    void playbackTickChanged();

private:
    void reload();
    void updatePlaybackState();

    void connectToCurrentScore();
    void disconnectFromCurrentScore();

    engraving::Score* currentScore() const;
    engraving::Note* noteAt(int row) const;

    std::vector<MidiNoteItem> m_entries;
    QVariantList m_notes;
    QVariantList m_measures;

    QStringList m_staffNames;
    QString m_scoreName;
    bool m_hasScore = false;

    int m_lowestPitch = 60;
    int m_highestPitch = 72;
    int m_totalTicks = 0;

    double m_playbackTick = 0.0;
    bool m_isPlaying = false;

    INotationPtr m_notation;
};
}
