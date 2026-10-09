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
#include "fieldcontroller.h"

#include <QDateTime>

#include "log.h"

#include "engraving/dom/score.h"
#include "engraving/dom/engravingitem.h"
//! `ScoreChanges::changedObjects` is keyed by `CommandType`, and `INotationUndoStack` only
//! forward-declares it - this include is what makes the histogram below compile.
#include "engraving/editing/transaction/undoablecommand.h"

#include "notation/inotation.h"
#include "notation/inotationelements.h"
#include "notation/inotationundostack.h"

using namespace muse;
using namespace muse::agentharness;
//! NOTE `INotationPtr` / `INotationUndoStackPtr` live in `mu::notation`, not `muse::notation` -
//! `using namespace muse` alone does not reach them.
using namespace mu::engraving;
using namespace mu::notation;

//! Set MUSE_AGENT_FIELD_TRACE=1 to log one line per recorded event.
//! This is the observation point for "does the field actually see what the user did" - the
//! question the M1 acceptance criterion asks. It uses LOGW because plain info/debug lines do not
//! reach the log file in this project (维护手册.md §7.1), and it doubles as the reverse-verification
//! switch: with it off, nothing is logged and the panel is the only evidence.
static bool fieldTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet("MUSE_AGENT_FIELD_TRACE");
    return enabled;
}

//! Command types and element types share one histogram; this keeps them from colliding.
//! Sized well above the number of `CommandType` values (see undoablecommand.h).
static constexpr int TYPE_BUCKET_OFFSET = 1000;

FieldController::FieldController(QObject* parent)
    //! `Contextable` has NO default constructor: it must be handed the context it will resolve
    //! injections from. `iocCtxForQmlObject(this)` is this project's canonical answer for a
    //! QML-declared element - it walks up to the object's QQmlContext, so the field picks up the
    //! context of the window whose panel created it. Omitting the argument is a compile error,
    //! not a silent misconfiguration.
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

FieldController::~FieldController()
{
    //! Asyncable's own destructor drops the subscriptions, but the score pointer is ours to
    //! forget: `Score` may already be gone by the time this object is destroyed (it belongs to
    //! EngravingProject), so nothing here may touch it.
    m_score = nullptr;
}

void FieldController::init()
{
    if (m_inited) {
        unbind();
        bindToCurrentNotation();
        return;
    }

    m_inited = true;

    context()->currentNotationChanged().onNotify(this, [this]() {
        //! Switching project: the previous score is gone (and `Score` is deleted with its
        //! EngravingProject), so the field must let go of it before touching anything else.
        unbind();
        bindToCurrentNotation();
    });

    bindToCurrentNotation();
}

void FieldController::unbind()
{
    if (m_score) {
        m_score->changesChannel().disconnect(this);
    }

    if (INotationPtr notation = context()->currentNotation()) {
        if (INotationUndoStackPtr undoStack = notation->undoStack()) {
            undoStack->stackChanged().disconnect(this);
            undoStack->undoRedoNotification().disconnect(this);
        }
    }

    m_score = nullptr;
}

void FieldController::bindToCurrentNotation()
{
    //! Announce every bind attempt. Without this the log only shows what happened at the very
    //! first call, which is *before* a score is loaded - and that reads as "the field never saw
    //! the score" even when the later rebind worked. A trace that answers the wrong question is
    //! worse than no trace.
    if (fieldTraceEnabled()) {
        const bool notationOpen = interactive()->isOpened(muse::Uri("musescore://notation")).val;
        LOGW() << "[agent-field] bind: notationPageOpen=" << notationOpen
               << "currentNotation=" << (context()->currentNotation() ? "yes" : "none");
    }

    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        refreshScoreFacts();
        emit fieldChanged();
        return;
    }

    Score* score = notation->elements()->msScore();
    if (!score) {
        refreshScoreFacts();
        emit fieldChanged();
        return;
    }

    m_score = score;

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-field] bound: score=" << score->name().toQString()
               << "measures=" << score->nmeasures()
               << "staves=" << score->nstaves();
    }

    //! ── The primary channel ────────────────────────────────────────────────────────────
    //! Every committed transaction sends this exactly once, and an empty transaction sends
    //! nothing at all (TransactionManager::commitTransaction). So "one transaction = one raw
    //! event" holds by upstream construction - no debouncing needed here.
    m_score->changesChannel().onReceive(this, [this](const ScoreChanges& changes) {
        onScoreChanges(changes);
    });

    if (INotationUndoStackPtr undoStack = notation->undoStack()) {
        //! Transaction committed. The name of what just happened is read in onScoreChanges (the
        //! change payload arrives *after* the stack was updated); this only refreshes the panel.
        undoStack->stackChanged().onNotify(this, [this]() {
            onStackChanged();
        });

        undoStack->undoRedoNotification().onNotify(this, [this]() {
            onUndoRedo();
        });
    }

    refreshScoreFacts();
    emit fieldChanged();
}

void FieldController::refreshScoreFacts()
{
    if (!m_score) {
        m_scoreName.clear();
        m_measureCount = 0;
        m_staffCount = 0;
        return;
    }

    m_scoreName = m_score->name().toQString();
    m_measureCount = int(m_score->nmeasures());
    m_staffCount = int(m_score->nstaves());
}

void FieldController::noteActionFromUndoStack(RawFieldEvent& event) const
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return;
    }

    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return;
    }

    //! `translated()` is the load-bearing call: a TranslatableString is a *deferred* translation,
    //! and this is the point where the user-facing name ("Insert note", "Transpose harmony")
    //! becomes a plain string we can store and show.
    event.action = undoStack->topMostUndoActionName().translated().toQString();
}

void FieldController::onScoreChanges(const ScoreChanges& changes)
{
    //! ── Undo / redo direction ──────────────────────────────────────────────────────────
    //! ⛔ NOT from `undoRedoNotification`: `NotationUndoStack::undo()` calls
    //! `transactionManager()->undoRedo()` - which is what sends the change payload we are
    //! handling right now - and only *afterwards* notifies undo/redo and stack-changed. So a flag
    //! set by that notification is always one event too late, and an earlier version of this code
    //! therefore never marked a single undo.
    //!
    //! A DECREASE in the state index means undo. That is the whole rule, and it is deliberately
    //! the only rule: `currentStateIndex()` counts every action ever committed (it kept climbing
    //! 1, 2, 3 while notes were being typed), so "index went up" does NOT mean redo - it means
    //! "a new action". Marking that as redo made every note after the first look like a redo,
    //! which is exactly the kind of confidently-wrong label this field must never produce.
    //! Redo is therefore left unmarked until it can be identified by a signal that actually means
    //! it; an absent label is honest, a wrong one is not.
    int stateIndex = -1;
    if (INotationPtr notation = context()->currentNotation()) {
        if (INotationUndoStackPtr undoStack = notation->undoStack()) {
            stateIndex = int(undoStack->currentStateIndex());
        }
    }

    const bool isUndo = m_lastStateIndex >= 0 && stateIndex >= 0 && stateIndex < m_lastStateIndex;
    const bool isRedo = false;
    m_lastStateIndex = stateIndex;

    RawFieldEvent event;
    event.wallClock = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    event.isUndo = isUndo;
    event.isRedo = isRedo;
    event.tickFrom = changes.tickFrom;    event.tickTo = changes.tickTo;
    event.staffFrom = changes.staffIdxFrom == muse::nidx ? -1 : int(changes.staffIdxFrom);
    event.staffTo = changes.staffIdxTo == muse::nidx ? -1 : int(changes.staffIdxTo);
    event.isTextEditing = changes.isTextEditing;
    event.hasBoundary = changes.isValidBoundary();

    event.objectCount = int(changes.changedObjects.size());
    for (const auto& pair : changes.changedObjects) {
        for (CommandType type : pair.second) {
            event.typeCounts[int(type)] += 1;
        }
    }
    for (const ElementType type : changes.changedTypes) {
        //! Element types get their own bucket in the same histogram, offset so a type and a
        //! command can never collide. Kept because a transaction can change an element type
        //! without any object of ours landing in `changedObjects`.
        event.typeCounts[int(type) + TYPE_BUCKET_OFFSET] += 1;
    }
    for (const Pid pid : changes.changedPropertyIdSet) {
        event.propertyCounts[int(pid)] += 1;
    }

    //! ── Filter: NOT every send on this channel is a user edit ─────────────────────────
    //! Playback control sends a hand-made, boundary-only payload whose whole meaning is
    //! "the layout was rebuilt, drop your caches" (playbackcontroller.cpp). It carries no
    //! objects and no types. Recording it would put a phantom operation in the field for
    //! every play/seek, which is exactly the kind of quiet lie the field must not tell.
    event.isLayoutOnly = changes.changedObjects.empty() && changes.changedTypes.empty();

    if (event.isLayoutOnly) {
        if (fieldTraceEnabled()) {
            LOGW() << "[agent-field] skip layout-only changes"
                   << "ticks=" << event.tickFrom << ".." << event.tickTo;
        }
        return;
    }

    noteActionFromUndoStack(event);

    if (event.action.isEmpty()) {
        //! No named transaction on top: a write that bypassed the notation undo stack. Worth
        //! seeing, so record it with a placeholder rather than dropping it - a gap in the field
        //! is worse than an ugly label.
        event.action = QStringLiteral("(unnamed change)");
    }

    event.source = RawFieldEvent::Source::User;

    record(event);

    refreshScoreFacts();
    ++m_revision;
    emit fieldChanged();
}

void FieldController::onStackChanged()
{
    //! ⛔ Do NOT clear the undo/redo flags here. `NotationUndoStack::undo()` fires
    //! `undoRedoNotification` and then `stackChanged` (notationundostack.cpp: notifyAboutUndoRedo
    //! then notifyAboutStateChanged), and the change payload that describes the undo arrives
    //! *inside* `transactionManager()->undoRedo()` - i.e. BEFORE both. So a clear here would be
    //! harmless today, but the flags are consumed by the next `onScoreChanges` regardless, and
    //! keeping exactly one place that resets them is what makes the ordering auditable. The
    //! earlier version cleared them here and the `[undo]` marker never appeared.
    emit fieldChanged();
}

void FieldController::onUndoRedo()
{
    //! The undo/redo itself is already described by the change payload handled in
    //! `onScoreChanges` (see the state-index note there); this only refreshes the panel, which
    //! wants to redraw as soon as the stack moves rather than waiting for the next edit.
    emit fieldChanged();
}

void FieldController::record(RawFieldEvent event)
{
    event.seq = m_nextSeq++;
    if (m_events.size() >= MAX_EVENTS) {
        m_events.pop_front();
        ++m_droppedEvents;
    }
    m_events.push_back(event);

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-field] seq=" << qulonglong(event.seq)
               << "action=\"" << event.action << "\""
               << "ticks=" << event.tickFrom << ".." << event.tickTo
               << "staves=" << event.staffFrom << ".." << event.staffTo
               << "objects=" << event.objectCount
               << "state=" << m_lastStateIndex
               << (event.isUndo ? "UNDO" : "")
               << (event.isRedo ? "REDO" : "");
    }

    emit fieldChanged();
}

QVariantList FieldController::recentEvents() const
{
    QVariantList out;

    int taken = 0;
    for (auto it = m_events.rbegin(); it != m_events.rend() && taken < PANEL_EVENT_LIMIT; ++it, ++taken) {
        const RawFieldEvent& e = *it;
        QVariantMap item;
        item.insert(QStringLiteral("seq"), QVariant::fromValue(qulonglong(e.seq)));
        item.insert(QStringLiteral("time"), e.wallClock);
        item.insert(QStringLiteral("action"), e.action);
        item.insert(QStringLiteral("isUndo"), e.isUndo);
        item.insert(QStringLiteral("isRedo"), e.isRedo);
        item.insert(QStringLiteral("tickFrom"), e.tickFrom);
        item.insert(QStringLiteral("tickTo"), e.tickTo);
        item.insert(QStringLiteral("staffFrom"), e.staffFrom);
        item.insert(QStringLiteral("staffTo"), e.staffTo);
        item.insert(QStringLiteral("objectCount"), e.objectCount);
        item.insert(QStringLiteral("kindCount"), int(e.typeCounts.size()));
        out.append(item);
    }

    return out;
}

QString FieldController::statusText() const
{
    if (!m_score) {
        return QStringLiteral("no score");
    }

    return QStringLiteral("%1 · %2 measures · %3 staves · %4 events · rev %5")
           .arg(m_scoreName.isEmpty() ? QStringLiteral("(untitled)") : m_scoreName)
           .arg(m_measureCount)
           .arg(m_staffCount)
           .arg(eventCount())
           .arg(m_revision);
}
