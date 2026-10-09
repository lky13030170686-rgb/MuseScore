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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "async/async.h"
#include "engraving/dom/masterscore.h"
#include "engraving/editing/transaction/transaction.h"

#include "log.h"

#include "engraving/dom/score.h"
#include "engraving/dom/engravingitem.h"
//! `ScoreChanges::changedObjects` is keyed by `CommandType`, and `INotationUndoStack` only
//! forward-declares it - this include is what makes the histogram below compile.
#include "engraving/editing/transaction/undoablecommand.h"

#include "notation/inotation.h"
#include "notation/inotationelements.h"
#include "notation/inotationundostack.h"

#include "addressing.h"
#include "agentloop.h"
#include "llmtransport.h"
#include "semanticderive.h"
#include "scoredigest.h"
#include "systemprompt.h"
#include "tools.h"

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
//! The value itself lives on FieldController so `semanticderive.cpp` can use the same one.
static constexpr int TYPE_BUCKET_OFFSET = FieldController::TYPE_BUCKET_OFFSET;

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

void FieldController::applyDemoToolCallIfPending()
{
    //! ── Verification hooks ─────────────────────────────────────────────────────────────────
    //! Two independent switches, each gated on its OWN variable:
    //!   MUSE_AGENT_DEMO_TOOL="<tool>|<json args>"  runs one tool call directly
    //!   MUSE_AGENT_DEMO_AGENT="<message>"          runs one real turn through the agent loop
    //!
    //! ⚠️ They are checked SEPARATELY and neither returns early past the other. The first version put
    //! the agent hook at the end of the tool hook's body, behind its early return - so setting only
    //! the agent variable produced *nothing at all*, with no error anywhere. A verification switch
    //! that silently does nothing is worse than no switch, because it reads as "the feature is
    //! broken" rather than "the switch is not wired".
    if (qEnvironmentVariableIsSet("MUSE_AGENT_DEMO_TOOL")) {
        const QString spec = qEnvironmentVariable("MUSE_AGENT_DEMO_TOOL");
        const int bar = spec.indexOf(QLatin1Char('|'));
        const QString name = bar < 0 ? spec : spec.left(bar);
        const QString args = bar < 0 ? QString() : spec.mid(bar + 1);

        LOGW() << "[agent-demo] tool call before:" << name << args
               << "measures=" << measureCount()
               << "revision=" << revision()
               << "canUndo=" << (undoStackCanUndo());

        const QString text = runTool(name, args);

        LOGW() << "[agent-demo] tool call after :" << (m_lastToolOk ? "OK" : "FAILED")
               << "measures=" << measureCount()
               << "revision=" << revision()
               << "canUndo=" << (undoStackCanUndo())
               << "result=" << text.left(300);
    }

    //! ── Write-path verification ───────────────────────────────────────────────────────────────
    //! MUSE_AGENT_DEMO_WRITE=<mode> runs the checks that need a real score to be meaningful:
    //!
    //!   stale   a write fenced with a WRONG revision must be refused and change nothing
    //!   batch   three writes as one undo step: measures +3, ONE undo step, ONE timeline entry
    //!   partial a batch with a bad operation in it must roll back ENTIRELY
    //!
    //! ⚠️ Each mode is checked separately and none returns early past the others - see the note above
    //! about a verification switch that silently does nothing.
    if (qEnvironmentVariableIsSet("MUSE_AGENT_DEMO_WRITE")) {
        const QString mode = qEnvironmentVariable("MUSE_AGENT_DEMO_WRITE");

        if (mode == QLatin1String("stale")) {
            //! The fence's whole job. The revision passed is deliberately impossible, so a fence that
            //! does nothing at all is indistinguishable from one that works - UNLESS the measures are
            //! checked afterwards, which is why they are.
            const int before = measureCount();
            LOGW() << "[agent-write] STALE FENCE test: measures before =" << before
                   << "realRevision=" << scoreRevision();
            const QString text = runTool(QStringLiteral("command_dispatch"),
                                         QStringLiteral("{\"command\":\"command://notation/append-measures\","
                                                        "\"params\":{\"count\":2},\"expectRevision\":9999}"));
            LOGW() << "[agent-write] STALE FENCE result:" << (m_lastToolOk ? "OK (BAD!)" : "REFUSED")
                   << "measures after =" << measureCount()
                   << "(must equal" << before << ")"
                   << "text=" << text.left(200);
        } else if (mode == QLatin1String("batch")) {
            const int before = measureCount();
            LOGW() << "[agent-write] BATCH test: measures before =" << before
                   << "revision=" << scoreRevision();

            const QString text = runTool(QStringLiteral("patch_apply"),
                                         QStringLiteral("{\"actionName\":\"Add 3 measures\",\"ops\":["
                                                        "{\"command\":\"command://notation/append-measures\",\"params\":{\"count\":1}},"
                                                        "{\"command\":\"command://notation/append-measures\",\"params\":{\"count\":1}},"
                                                        "{\"command\":\"command://notation/append-measures\",\"params\":{\"count\":1}}]}"));
            LOGW() << "[agent-write] BATCH result:" << (m_lastToolOk ? "OK" : "FAILED")
                   << "measures after =" << measureCount()
                   << "(expect" << before + 3 << ")"
                   << "revision=" << scoreRevision()
                   << "text=" << text.left(300);
        } else if (mode == QLatin1String("partial")) {
            const int before = measureCount();
            LOGW() << "[agent-write] PARTIAL test: measures before =" << before;

            //! The third operation is a command URI that does not exist, so it is refused - and the
            //! whole batch must roll back, leaving the two good appends unapplied.
            const QString text = runTool(QStringLiteral("patch_apply"),
                                         QStringLiteral("{\"actionName\":\"Partial batch\",\"ops\":["
                                                        "{\"command\":\"command://notation/append-measures\",\"params\":{\"count\":1}},"
                                                        "{\"command\":\"command://notation/append-measures\",\"params\":{\"count\":1}},"
                                                        "{\"command\":\"command://notation/no-such-command\"}]}"));
            LOGW() << "[agent-write] PARTIAL result:" << (m_lastToolOk ? "OK (BAD!)" : "REFUSED")
                   << "measures after =" << measureCount()
                   << "(must equal" << before << ")"
                   << "text=" << text.left(400);
        }
    }

    //! ── The session log, exercised from the running program ───────────────────────────────────
    //! Printing the JSONL is what verifies the claim that matters - **the model's history is derived
    //! from the log**, so "what the model saw" is answerable from what was recorded.
    //! ⚠️ The user message is NOT appended here: `sendToAgent()` below appends it as part of the turn.
    //! Doing both put the same sentence in the request twice, which is exactly the kind of quiet
    //! duplication a reader would blame on the projection.
    LOGW() << "[agent-session] jsonl:\n" << sessionJsonLines();

    //! ── The full loop ─────────────────────────────────────────────────────────────────────────
    //! Pointed at the mock server (build/ah/mock-llm-server.ps1) this exercises the whole chain with
    //! no API key: request assembled from the log, streamed tool call, tool executed against the real
    //! score, second request carrying the result. Pointed at the real endpoint with a key it is the
    //! real thing.
    if (qEnvironmentVariableIsSet("MUSE_AGENT_DEMO_AGENT")) {
        const QString message = qEnvironmentVariable("MUSE_AGENT_DEMO_AGENT");
        LOGW() << "[agent-demo] submitting a real turn:" << message
               << "base=" << qEnvironmentVariable("MUSE_AGENT_API_BASE")
               << "configured=" << agentConfigured();
        sendToAgent(message);
    } else {
        seedSystemPrompt();
        appendUserMessage(QStringLiteral("what is in this score?"));
        LOGW() << "[agent-session] projection:\n" << sessionPreview();
    }
}

bool FieldController::undoStackCanUndo() const
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return false;
    }
    INotationUndoStackPtr stack = notation->undoStack();
    return stack ? stack->canUndo() : false;
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

    //! Verification hook, at the very end of the bind so the tool sees a fully wired field: the
    //! change channel is subscribed, the grid is built, and `revision` has a real value.
    applyDemoToolCallIfPending();
}
void FieldController::refreshScoreFacts()
{
    if (!m_score) {
        m_scoreName.clear();
        m_measureCount = 0;
        m_staffCount = 0;
        m_grid = MeasureGrid();
        return;
    }

    m_scoreName = m_score->name().toQString();
    m_measureCount = int(m_score->nmeasures());
    m_staffCount = int(m_score->nstaves());

    //! The grid is rebuilt from the score on every change. It is a flat copy of numbers, so the cost
    //! is one pass over the measures - cheap next to the layout work the same edit already triggered,
    //! and it keeps the addresses honest. Caching it across edits would be the one thing that could
    //! silently point at the wrong bar after a measure is inserted.
    m_grid = buildMeasureGrid(m_score);
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

    //! ⛔ WHICH NAME, AND WHY IT IS NOT ALWAYS `topMostUndoActionName()`.
    //!
    //! That call returns the transaction at the stack's *current* position - i.e. the one the next
    //! Ctrl+Z would take back. That is the right answer for an ordinary edit (the edit just pushed
    //! it) but the WRONG answer for an undo: after `undo()` the position has already moved back, so
    //! the "top" is the transaction *before* the one that was just undone. Reading it there labelled
    //! an undo of "添加2小节" as "迁移项目" - a plausible-looking name for an operation that did not
    //! happen, which is exactly the confidently-wrong label this field must never produce.
    //!
    //! For an undo the transaction that was taken back is the one sitting AT the current position,
    //! and `topMostRedoActionName()` is defined as exactly that (`UndoStack::next()` returns
    //! `m_transactions[m_currentIndex]`). So:
    //!
    //!     undo -> the name of what is now redoable  (the thing that was just undone)
    //!     edit -> the name of what is now undoable  (the thing that just committed)
    //!
    //! ⚠️ `lastActionNameAtIdx()` is NOT the tool for this: it is defined as
    //! `m_transactions[idx - 1]` (it answers "which transaction led to state idx"), so feeding it
    //! `currentStateIndex()` reads one transaction too early. That off-by-one was tried and is
    //! recorded here because both versions produce a *plausible* wrong name rather than an error.
    if (event.isUndo) {
        event.action = undoStack->topMostRedoActionName().translated().toQString();
    }

    if (event.action.isEmpty()) {
        //! Ordinary edit (or an undo whose name could not be resolved): the stack top is the
        //! transaction that just committed, which is the right answer here.
        //! `translated()` is the load-bearing call: a TranslatableString is a *deferred* translation,
        //! and this is the point where the user-facing name ("Insert note", "添加2小节") becomes a
        //! plain string we can store and show.
        event.action = undoStack->topMostUndoActionName().translated().toQString();
    }

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-field] name:"
               << (event.isUndo ? "UNDO" : "edit")
               << "stateIndex=" << int(undoStack->currentStateIndex())
               << "count=" << int(undoStack->undoRedoActionCount())
               << "top=\"" << undoStack->topMostUndoActionName().translated().toQString() << "\""
               << "chosen=\"" << event.action << "\"";
    }
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

QVariantList FieldController::recentOps() const
{
    //! Derived on demand rather than kept in sync: `deriveSemanticOps` is pure, the raw ring is at
    //! most a few hundred entries, and a stored copy would be one more thing that can disagree with
    //! the facts it came from. The panel asks for 30 rows, so the derivation is truncated after the
    //! fact - not before, because truncating the input would change what the newest ops are.
    QVector<RawFieldEvent> raw;
    raw.reserve(int(m_events.size()));
    for (const RawFieldEvent& e : m_events) {
        raw.append(e);
    }

    const QVector<SemanticOp> ops = deriveSemanticOps(raw, m_grid);

    QVariantList out;
    int taken = 0;
    for (auto it = ops.crbegin(); it != ops.crend() && taken < PANEL_EVENT_LIMIT; ++it, ++taken) {
        const SemanticOp& op = *it;
        QVariantMap item;
        item.insert(QStringLiteral("seq"), QVariant::fromValue(qulonglong(op.seq)));
        item.insert(QStringLiteral("time"), op.wallClock);
        item.insert(QStringLiteral("source"), op.source);
        item.insert(QStringLiteral("action"), op.action);
        item.insert(QStringLiteral("isUndo"), op.isUndo);
        item.insert(QStringLiteral("isRedo"), op.isRedo);
        item.insert(QStringLiteral("where"), op.where);
        item.insert(QStringLiteral("tickFrom"), op.tickFrom);
        item.insert(QStringLiteral("tickTo"), op.tickTo);
        item.insert(QStringLiteral("objectCount"), op.objectCount);
        item.insert(QStringLiteral("kinds"), op.kinds.join(QLatin1Char('+')));
        item.insert(QStringLiteral("line"), op.toString());
        out.append(item);
    }

    return out;
}

QString FieldController::digestOverview() const
{
    if (!m_score) {
        return QStringLiteral("(no score open)");
    }

    const QString text = buildScoreOverview(m_score);

    //! MUSE_AGENT_FIELD_TRACE also dumps the digest, because "what would the agent be told" is the
    //! question the read side exists to answer - and the only way to answer it without an agent
    //! loop in place yet is to print it.
    if (fieldTraceEnabled()) {
        LOGW() << "[agent-field] digest overview:\n" << text;
    }

    return text;
}

QString FieldController::digestWindow(int firstMeasure, int lastMeasure) const
{
    if (!m_score) {
        return QStringLiteral("(no score open)");
    }

    const QString text = buildMeasureWindow(m_score, firstMeasure, lastMeasure);

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-field] digest window" << firstMeasure << ".." << lastMeasure << ":\n" << text;
    }

    return text;
}

void FieldController::runToolAsync(const QString& name, const QString& argsJson,
                                   std::function<void(const ToolResult&)> done)
{
    const ToolSpec* spec = findTool(name);
    if (!spec) {
        ToolResult r = ToolResult::failure(QStringLiteral("no such tool: %1 (available: %2)")
                                           .arg(name, toolNames().join(QStringLiteral(", "))));
        m_lastToolOk = false;
        done(r);
        return;
    }

    //! Parse, and refuse malformed JSON *before* the body runs. A model that emits broken arguments
    //! should be told so plainly and get to try again; feeding `{}` to the body instead would make
    //! it look like the arguments were accepted and silently ignored (DSH keeps the raw string and
    //! reports INVALID_ARGS for the same reason).
    QJsonObject args;
    if (!argsJson.trimmed().isEmpty()) {
        QJsonParseError err {};
        const QJsonDocument doc = QJsonDocument::fromJson(argsJson.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            ToolResult r = ToolResult::failure(QStringLiteral("arguments are not valid JSON: %1")
                                               .arg(err.errorString()));
            m_lastToolOk = false;
            done(r);
            return;
        }
        if (!doc.isObject()) {
            ToolResult r = ToolResult::failure(QStringLiteral("arguments must be a JSON object"));
            m_lastToolOk = false;
            done(r);
            return;
        }
        args = doc.object();
    }

    //! Which path fired. A tool either returns a result or reports one later - and a tool that did
    //! NEITHER would hang the loop forever waiting for a turn that never finishes, so the empty-text
    //! case is treated as "this tool went asynchronous" and the completion is what ends the wait.
    auto answered = std::make_shared<bool>(false);

    ToolContext ctx;
    ctx.field = this;
    ctx.complete = [this, name, argsJson, done, answered](const ToolResult& r) {
        if (*answered) {
            //! A tool that reports twice would append two results for one call, and the wire format
            //! pairs results to calls one-to-one - so the second one is dropped rather than sent.
            LOGW() << "[agent-tool]" << name << "reported a result twice; ignoring the second";
            return;
        }
        *answered = true;
        m_lastToolOk = r.ok;

        if (fieldTraceEnabled()) {
            LOGW() << "[agent-tool]" << name
                   << (r.ok ? "OK" : "FAILED")
                   << "(async) args=" << argsJson
                   << "->" << r.text.left(200);
        }

        done(r);
    };

    const ToolResult result = spec->execute(args, ctx);

    if (*answered) {
        //! The tool finished synchronously and used the channel. Nothing more to do.
        return;
    }

    if (!result.text.isEmpty() || !result.ok) {
        //! It returned a real result instead.
        m_lastToolOk = result.ok;

        if (fieldTraceEnabled()) {
            LOGW() << "[agent-tool]" << name
                   << (result.ok ? "OK" : "FAILED")
                   << "args=" << argsJson
                   << "->" << result.text.left(200);
        }

        *answered = true;
        done(result);
        return;
    }

    //! Empty success: the tool has taken responsibility for reporting later through `ctx.complete`.
    //! Nothing to do here - `done` will be called from the event loop.
}

QString FieldController::runTool(const QString& name, const QString& argsJson)
{
    //! The synchronous face of `runToolAsync`, for QML and for the verification hooks. An
    //! asynchronous tool cannot report through it, so its acknowledgement ("the work is queued") is
    //! what comes back - the real result goes to the session log.
    //!
    //! ⛔ THE STATE IS ON THE HEAP, and that is not tidiness. The completion may be called from a
    //! QUEUED callback - after this function has returned - so anything it touches must outlive the
    //! stack frame. The first version captured `QString text` and `bool answered` by reference and
    //! SIGSEGVed the application the moment a tool went asynchronous: the callback wrote through a
    //! pointer into a frame that no longer existed.
    auto text = std::make_shared<QString>();
    auto answered = std::make_shared<bool>(false);

    runToolAsync(name, argsJson, [text, answered](const ToolResult& r) {
        *text = r.text;
        *answered = true;
    });

    if (!*answered) {
        return QStringLiteral("(queued; the result will appear in the conversation when it completes)");
    }

    return *text;
}

QStringList FieldController::availableTools() const
{
    return toolNames();
}

bool FieldController::isCommandEnabled(const QString& command) const
{
    if (command.isEmpty()) {
        return false;
    }

    const muse::rcommand::Command cmd(muse::Uri(command.toStdString()));
    return commandsState()->commandState(cmd).enabled;
}

QStringList FieldController::enabledCommandNames() const
{
    //! Enumerating the register and asking each command's state is the only way to answer "what can I
    //! do right now" without hardcoding a list - and a hardcoded list would drift from the 446
    //! commands the moment upstream adds one. This is the same reason the write side asks upstream
    //! `toDurationList()` instead of keeping a table of writable durations.
    QStringList names;
    for (const muse::rcommand::CommandInfo& info : commandsRegister()->commandInfoList()) {
        if (!info.isValid()) {
            continue;
        }
        if (commandsState()->commandState(info.command).enabled) {
            names.append(QString::fromStdString(info.command.toString()));
        }
    }
    return names;
}

namespace {
//! Translate a tool's JSON arguments into the command layer's `Params`.
//!
//! Only the scalar types a notation command actually declares are mapped; anything else is dropped
//! rather than coerced, because silently turning an object into a string would make the handler
//! misread it.
//!
//! NOTE `Val`'s constructors are all `explicit`, so `params[key] = true` does not compile - the value
//! has to be named before it goes in.
muse::rcommand::Params toCommandParams(const QJsonObject& args)
{
    muse::rcommand::Params params;
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        const QJsonValue v = it.value();
        const std::string key = it.key().toStdString();
        if (v.isBool()) {
            params.insert({ key, muse::Val(v.toBool()) });
        } else if (v.isDouble()) {
            //! JSON has no integer type: `2` and `2.0` both arrive as double. Send the integral form
            //! when the value is integral, because command handlers read `count` as an int and
            //! `Val::toInt()` on a stored double is a different path.
            const double d = v.toDouble();
            if (d == double(int(d))) {
                params.insert({ key, muse::Val(int(d)) });
            } else {
                params.insert({ key, muse::Val(d) });
            }
        } else if (v.isString()) {
            params.insert({ key, muse::Val(v.toString().toStdString()) });
        }
    }
    return params;
}
} // namespace

QString FieldController::dispatchCommand(const QString& command, const QJsonObject& args)
{    if (command.isEmpty()) {
        return QStringLiteral("empty command");
    }

    const muse::rcommand::Command cmd(muse::Uri(command.toStdString()));

    //! Translate the tool's JSON arguments into the command layer's `Params`.
    const muse::rcommand::Params params = toCommandParams(args);

    muse::async::Promise<muse::rcommand::Response> promise
        = params.empty() ? commandDispatcher()->dispatch(cmd) : commandDispatcher()->dispatch(cmd, params);

    //! The dispatch is asynchronous, but every notation command's handler runs synchronously inside
    //! it (they are plain C++ handlers, not queued work), so by the time the promise settles the
    //! edit has already happened. Reporting a failure here means the handler itself refused.
    QString error;
    promise.onResolve(this, [&error](const muse::rcommand::Response& res) {
        if (!res.ret) {
            error = QString::fromStdString(res.ret.toString());
        }
    });

    return error;
}

QString FieldController::sessionJsonLines() const
{
    return QString::fromUtf8(m_session.toJsonLines());
}

QString FieldController::sessionPreview() const
{
    const QVector<WireMessage> messages = m_session.deriveMessages();
    if (messages.isEmpty()) {
        return QStringLiteral("(the session log projects to no messages)");
    }

    QStringList lines;
    lines.append(QStringLiteral("%1 event(s) in the log project to %2 model message(s):")
                 .arg(m_session.size()).arg(messages.size()));

    for (const WireMessage& m : messages) {
        QString detail = m.content;
        if (!m.toolCalls.isEmpty()) {
            QStringList names;
            for (const QJsonObject& call : m.toolCalls) {
                names.append(call.value(QStringLiteral("function")).toObject()
                             .value(QStringLiteral("name")).toString());
            }
            detail += QStringLiteral(" [tool_calls: %1]").arg(names.join(QStringLiteral(", ")));
        }
        if (!m.toolCallId.isEmpty()) {
            detail = QStringLiteral("(answers %1) %2").arg(m.toolCallId, detail);
        }

        //! Truncated per message: the preview exists to show the SHAPE of the request (who said what,
        //! in what order), and a long tool result would bury it.
        if (detail.size() > 120) {
            detail = detail.left(120) + QStringLiteral(" ...");
        }
        lines.append(QStringLiteral("  %1: %2").arg(m.role, detail));
    }

    return lines.join(QLatin1Char('\n'));
}

void FieldController::appendUserMessage(const QString& text)
{
    QJsonObject data;
    data.insert(QStringLiteral("content"), text);
    m_session.append(SessionEvent::USER_MESSAGE, data);

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-session] user/message seq=" << qulonglong(m_session.size() - 1)
               << "content=" << text.left(120);
    }

    emit fieldChanged();
}

void FieldController::seedSystemPrompt()
{
    QJsonObject data;
    data.insert(QStringLiteral("content"), buildSystemPrompt());
    m_session.append(SessionEvent::SYSTEM_MESSAGE, data);

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-session] system/message recorded;"
               << "tools=" << toolNames().join(QStringLiteral(","));
    }

    emit fieldChanged();
}

int FieldController::sessionEventCount() const
{
    return m_session.size();
}


void FieldController::beginUndoTransaction(const QString& actionName)
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return;
    }
    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return;
    }

    undoStack->prepareChanges(muse::TranslatableString::untranslatable(muse::String(actionName)));
}

bool FieldController::endUndoTransaction(bool commit)
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return false;
    }
    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return false;
    }

    const size_t before = undoStack->currentStateIndex();

    if (commit) {
        undoStack->commitChanges();
    } else {
        //! ⛔ Rolled back, not committed with the successful prefix. A half-applied instruction is the
        //! worst available outcome: the model believes it did the whole thing, the user sees part of
        //! it, and neither can tell which part.
        undoStack->rollbackChanges();
    }

    const size_t after = undoStack->currentStateIndex();

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-write] transaction closed: stateIndex" << int(before) << "->" << int(after)
               << "committed=" << (after != before);
    }

    //! The stack is the only authority on whether the work survived: `commitChanges` is void, and the
    //! framework forces a rollback for a read-only score or a command that set an error.
    return after != before;
}

void FieldController::mergeTransactionsFrom(size_t startIdx)
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return;
    }
    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return;
    }

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-write] merging transactions from index" << int(startIdx)
               << "(current" << int(undoStack->currentStateIndex()) << ")";
    }

    undoStack->mergeTransactions(startIdx);
}

mu::engraving::Score* FieldController::currentScore() const
{
    INotationPtr notation = context()->currentNotation();
    return notation ? notation->score() : nullptr;
}

RecipeResult FieldController::runNoteRecipe(const QString& actionName,
                                            const std::function<RecipeResult(mu::engraving::Score*)>& recipe)
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return RecipeResult::failure(QStringLiteral("no score is open"));
    }

    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return RecipeResult::failure(QStringLiteral("the score has no undo stack, so nothing can be "
                                                    "edited safely"));
    }

    mu::engraving::Score* score = notation->score();
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score is open"));
    }

    RecipeResult outcome;

    //! `prepareChanges`/`commitChanges` rather than `transaction()`: the recipe is a plain function, and
    //! the pair is the shape that lets the outcome be read before deciding to commit. A recipe that
    //! failed must ROLL BACK rather than commit an empty transaction - an empty commit still moves the
    //! revision, which would make a no-op look like an edit to anything watching the fence.
    undoStack->prepareChanges(muse::TranslatableString::untranslatable(muse::String(actionName)));
    outcome = recipe(score);

    if (outcome.ok) {
        undoStack->commitChanges();
    } else {
        undoStack->rollbackChanges();
    }

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-write] recipe" << actionName
               << (outcome.ok ? "OK" : "FAILED")
               << "revision=" << scoreRevision()
               << (outcome.ok ? outcome.detail : outcome.problem);
    }

    return outcome;
}
int FieldController::undoToRevision(int targetRevision){
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return 0;
    }
    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return 0;
    }

    int undone = 0;
    //! Bounded by the number of transactions available, so a stack that refuses to move cannot spin.
    while (int(undoStack->currentStateIndex()) > targetRevision && undoStack->canUndo()) {
        undoStack->undo(nullptr);
        ++undone;
    }

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-write] undoToRevision" << targetRevision
               << "undid" << undone
               << "now at" << int(undoStack->currentStateIndex());
    }

    return undone;
}

muse::async::Promise<muse::rcommand::Response> FieldController::dispatchCommandPromise(const QString& command,
                                                                                       const QJsonObject& args)
{
    const muse::rcommand::Command cmd(muse::Uri(command.toStdString()));
    const muse::rcommand::Params params = toCommandParams(args);
    return params.empty() ? commandDispatcher()->dispatch(cmd) : commandDispatcher()->dispatch(cmd, params);
}

bool FieldController::runInTransaction(const QString& actionName, const std::function<void()>& body)
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return false;
    }

    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return false;
    }

    //! The revision before and after tells us whether the transaction actually committed. Reading the
    //! stack is the only reliable way to know: `transaction()` returns void, and
    //! `TransactionManager::endTransaction` can force a rollback (read-only score, or a command that
    //! set an error) without telling the caller.
    const size_t before = undoStack->currentStateIndex();

    //! `untranslatable` takes a `const char*` or a `muse::String`, NOT a `std::string` - passing one
    //! is a compile error, not a silent conversion. The `String` overload is the right one here: the
    //! action name is built at runtime (it can include a count), so there is no literal to hand over.
    const muse::TranslatableString name = muse::TranslatableString::untranslatable(muse::String(actionName));

    undoStack->transaction(name, [&body](mu::engraving::Transaction&) {
        body();
    });

    const size_t after = undoStack->currentStateIndex();

    if (fieldTraceEnabled()) {
        LOGW() << "[agent-write] transaction" << actionName
               << "stateIndex" << int(before) << "->" << int(after)
               << "committed=" << (after != before);
    }

    return after != before;
}

int FieldController::scoreRevision() const
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return 0;
    }

    INotationUndoStackPtr undoStack = notation->undoStack();
    if (!undoStack) {
        return 0;
    }

    //! `currentStateIndex()` is the position in the undo stack's state list, which advances on every
    //! committed transaction - including the ones the user made with the mouse, which is the point.
    //! Clamped to int because it is exposed to the model as a number and a size_t would be printed as
    //! an unsigned value that never looks "before" anything.
    return int(undoStack->currentStateIndex());
}

void FieldController::postToEventLoop(std::function<void()> body)
{
    //! ⛔ NOT `muse::async::Async::call`. That was tried twice and SIGSEGVed both times - once with a
    //! null receiver, once with `this` - and the queue machinery is not worth debugging for a
    //! one-shot "run this after the current stack unwinds". `QMetaObject::invokeMethod` with
    //! `Qt::QueuedConnection` is Qt's own answer to exactly that question, it is what the agent loop
    //! already uses for its step continuation, and it ties the callback's lifetime to this object:
    //! if the field dies first, the invocation is dropped instead of calling into freed memory.
    QMetaObject::invokeMethod(this, std::move(body), Qt::QueuedConnection);
}

bool FieldController::hasActiveTransaction() const
{
    INotationPtr notation = context()->currentNotation();
    if (!notation) {
        return false;
    }

    //! ⚠️ `INotationUndoStack` does NOT expose this - the question "is a transaction open" is only
    //! answerable at the engraving layer, so this reaches for the score and asks its transaction
    //! manager. That is a deliberate exception to "everything goes through the notation interface":
    //! the alternative was adding a method to a public notation header, which is an 8-16 minute
    //! rebuild for a diagnostic (维护手册.md §9.3 build traps).
    mu::engraving::Score* score = notation->score();
    if (!score) {
        return false;
    }

    mu::engraving::TransactionManager* manager = score->masterScore()->transactionManager();
    return manager && manager->currentTransaction() != nullptr;
}

void FieldController::ensureAgentLoop()
{
    if (m_loop) {
        return;
    }

    m_transport = std::make_unique<LlmTransport>();

    //! The base URL is overridable so the whole chain can be pointed at a local mock. That is what
    //! makes the transport verifiable without a real key - and it is the same switch a user behind a
    //! gateway needs.
    const QString override = qEnvironmentVariable("MUSE_AGENT_API_BASE");
    m_transport->setBaseUrl(override.isEmpty() ? QStringLiteral("https://api.deepseek.com") : override);

    m_loop = std::make_unique<AgentLoop>(&m_session, m_transport.get(), this);

    connect(m_loop.get(), &AgentLoop::assistantText, this, [this](const QString&) {
        emit fieldChanged();
    });
    connect(m_loop.get(), &AgentLoop::assistantReasoning, this, [this](const QString&) {
        emit fieldChanged();
    });
    connect(m_loop.get(), &AgentLoop::runningChanged, this, [this]() {
        emit fieldChanged();
    });
    connect(m_loop.get(), &AgentLoop::turnFinished, this, [this](bool ok, const QString& error) {
        m_agentLastError = ok ? QString() : error;
        LOGW() << "[agent-loop] turn finished" << (ok ? "OK" : "FAILED")
               << "error=" << error
               << "sessionEvents=" << m_session.size();
        LOGW() << "[agent-loop] projection after the turn:\n" << sessionPreview();
        emit fieldChanged();
    });
}

void FieldController::sendToAgent(const QString& text)
{
    ensureAgentLoop();
    m_agentLastError.clear();
    m_loop->submitUserMessage(text);
    emit fieldChanged();
}

void FieldController::abortAgent()
{
    if (m_loop) {
        m_loop->abort();
    }
}

bool FieldController::agentRunning() const
{
    return m_loop && m_loop->isRunning();
}

bool FieldController::agentConfigured() const
{
    //! Constructing the transport is cheap and does not open a socket; it is the only way to ask the
    //! key question before a turn starts.
    const_cast<FieldController*>(this)->ensureAgentLoop();
    return m_transport && m_transport->isConfigured();
}

QString FieldController::agentStreamingText() const
{
    return m_loop ? m_loop->streamingText() : QString();
}

QString FieldController::agentStreamingReasoning() const
{
    return m_loop ? m_loop->streamingReasoning() : QString();
}

QString FieldController::agentLastError() const
{
    return m_agentLastError;
}

QString FieldController::agentModel() const
{
    return m_loop ? m_loop->model() : QStringLiteral("deepseek-chat");
}

void FieldController::setAgentModel(const QString& model)
{
    ensureAgentLoop();
    m_loop->setModel(model);
}

void FieldController::setAgentBaseUrl(const QString& url)
{
    ensureAgentLoop();
    m_transport->setBaseUrl(url);
}

void FieldController::setAgentApiKey(const QString& key)
{
    ensureAgentLoop();
    m_transport->setApiKey(key);

    if (fieldTraceEnabled()) {
        //! Never log the key itself, not even a prefix. A log line is the easiest way for a secret to
        //! end up somewhere it should not be.
        LOGW() << "[agent-llm] api key set by the user (length" << key.size() << ")";
    }
}

QString FieldController::agentApiKeySource() const
{
    FieldController* self = const_cast<FieldController*>(this);
    self->ensureAgentLoop();

    if (!m_transport->apiKey().isEmpty()) {
        //! `LlmTransport::apiKey()` resolves the environment on first ask, so by this point a
        //! non-empty key may have come from either place. The distinction is reported by asking the
        //! environment directly - the transport does not need to remember where it got it.
        return qEnvironmentVariableIsEmpty("DEEPSEEK_API_KEY")
               ? QStringLiteral("set")
               : QStringLiteral("environment");
    }

    return QStringLiteral("none");
}

QVariantList FieldController::agentTranscript() const
{
    //! Derived from the log, in the same spirit as the model history: the panel's transcript and the
    //! model's messages are two projections of one record, so they cannot disagree about what was
    //! said. Tool calls and results are shown because a user watching an agent work needs to see
    //! *what it did*, not just what it said.
    QVariantList out;

    for (const SessionEvent& e : m_session.events()) {
        if (e.type == SessionEvent::USER_MESSAGE) {
            QVariantMap m;
            m.insert(QStringLiteral("kind"), QStringLiteral("user"));
            m.insert(QStringLiteral("role"), QStringLiteral("you"));
            m.insert(QStringLiteral("text"), e.data.value(QStringLiteral("content")).toString());
            out.append(m);
            continue;
        }

        if (e.type == SessionEvent::ASSISTANT_MESSAGE) {
            const QString text = e.data.value(QStringLiteral("content")).toString();
            if (!text.isEmpty()) {
                QVariantMap m;
                m.insert(QStringLiteral("kind"), QStringLiteral("assistant"));
                m.insert(QStringLiteral("role"), QStringLiteral("agent"));
                m.insert(QStringLiteral("text"), text);
                out.append(m);
            }

            const QJsonArray calls = e.data.value(QStringLiteral("toolCalls")).toArray();
            for (const QJsonValue& v : calls) {
                const QJsonObject fn = v.toObject().value(QStringLiteral("function")).toObject();
                QVariantMap m;
                m.insert(QStringLiteral("kind"), QStringLiteral("toolCall"));
                m.insert(QStringLiteral("role"), QStringLiteral("tool"));
                m.insert(QStringLiteral("text"), QStringLiteral("%1 %2")
                         .arg(fn.value(QStringLiteral("name")).toString(),
                              fn.value(QStringLiteral("arguments")).toString()));
                out.append(m);
            }
            continue;
        }

        if (e.type == SessionEvent::TOOL_RESULT) {
            QVariantMap m;
            m.insert(QStringLiteral("kind"), QStringLiteral("toolResult"));
            m.insert(QStringLiteral("role"), QStringLiteral("tool"));
            m.insert(QStringLiteral("text"), e.data.value(QStringLiteral("content")).toString());
            m.insert(QStringLiteral("isError"), e.data.value(QStringLiteral("isError")).toBool());
            out.append(m);
            continue;
        }

        if (e.type == SessionEvent::ASSISTANT_ATTEMPT) {
            QVariantMap m;
            m.insert(QStringLiteral("kind"), QStringLiteral("attempt"));
            m.insert(QStringLiteral("role"), QStringLiteral("error"));
            m.insert(QStringLiteral("text"), e.data.value(QStringLiteral("problem")).toString());
            out.append(m);
            continue;
        }
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
