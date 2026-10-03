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

#include <functional>

#include "async/asyncable.h"

#include "modularity/ioc.h"
#include "context/iglobalcontext.h"

#include "notation/inotation.h"

#include "engraving/automation/automationtypes.h"

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

    //! The same edit for many notes at once, which is what a brush stroke needs. Setting them one by
    //! one made the model rebuild its whole note list - twice - per note, so sweeping over a phrase
    //! cost dozens of rebuilds. Here the rebuild happens once, at the end.
    Q_INVOKABLE void setNoteVelocities(const QVariantList& rows, const QVariantList& velocities);

    //! The Dynamics AUTOMATION curve of one staff - where the loudness should be over time. This is
    //! what a crescendo, a diminuendo or an fp inside a single note is, and it is the same curve the
    //! notation page draws next to the mixer, so the two stay one thing rather than two.
    //!
    //! Returns a list of { tick, value } with value in 0..1.
    Q_INVOKABLE QVariantList automationPoints(int staffIndex) const;

    //! Writes one point of that curve. Goes through the score's own undoable automation command, so
    //! it undoes and saves exactly like the same edit made on the notation page.
    Q_INVOKABLE void setAutomationPoint(int staffIndex, int tick, double value);

    //! Writes a whole stroke at once, from a list of { tick, value }. One command for the batch, for
    //! the same reason the velocity brush needs one: every command notifies the whole score.
    Q_INVOKABLE void setAutomationPoints(int staffIndex, const QVariantList& points);

    //! Removes the point at that tick, if there is one.
    Q_INVOKABLE void removeAutomationPoint(int staffIndex, int tick);

    //! The "played" layer: where the note actually sounds (tick) and for how long, plus the velocity
    //! multiplier in percent. Absolute ticks, so the view does not need to know about thousandths.
    Q_INVOKABLE void setNotePlayOverride(int row, int startTick, int durationTicks, int velocityPercent);

signals:
    void scoreChanged();
    void playbackTickChanged();

private:
    void reload();
    void updatePlaybackState();

    //! The real work of setNoteVelocities(), run one event-loop turn later (see the .cpp).
    void applyVelocityBatch(const QVariantList& rows, const QVariantList& velocities);

    //! Runs `mutate` with rebuilds suppressed, then rebuilds once. Writing through the engraving
    //! model notifies the score, and the notification handler rebuilds the note list - so a single
    //! edit otherwise rebuilds twice, and a batch of N rebuilds 2N times.
    void mutateOnce(const std::function<void()>& mutate);

    void connectToCurrentScore();
    void disconnectFromCurrentScore();

    engraving::Score* currentScore() const;
    engraving::Note* noteAt(int row) const;

    //! The automation curve key for one staff's Dynamics curve, or an invalid key when there is no
    //! such staff.
    engraving::AutomationCurveKey dynamicsKey(int staffIndex) const;

    std::vector<MidiNoteItem> m_entries;
    QVariantList m_notes;
    QVariantList m_measures;

    QStringList m_staffNames;
    QString m_scoreName;
    bool m_hasScore = false;

    //! Set while an edit of our own is in flight, so the score's change notification does not
    //! rebuild the list underneath us.
    bool m_rebuildSuppressed = false;

    int m_lowestPitch = 60;
    int m_highestPitch = 72;
    int m_totalTicks = 0;

    double m_playbackTick = 0.0;
    bool m_isPlaying = false;

    INotationPtr m_notation;
};
}
