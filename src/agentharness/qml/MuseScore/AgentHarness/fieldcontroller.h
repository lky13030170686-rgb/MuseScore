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
#include <QString>
//! `QJsonObject` is a parameter type on the tool entry point below, so it must be a COMPLETE type
//! here, not a forward declaration. moc alone would accept the declaration; the compiler will not.
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlintegration.h>

#include <deque>
#include <functional>
#include <memory>

#include "async/asyncable.h"
#include "async/promise.h"
#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "interactive/iinteractive.h"
#include "rcommand/icommanddispatcher.h"
#include "rcommand/icommandsstate.h"
#include "rcommand/icommandsregister.h"

#include "addressing.h"
#include "scorerecipes.h"
#include "sessionlog.h"
//! `ToolResult` is a parameter type on the tool entry points below, so it must be a COMPLETE type here
//! - a forward declaration would satisfy the compiler for a pointer but not for a by-value callback
//! argument.
#include "tools.h"

namespace muse::agentharness {
class AgentLoop;
class LlmTransport;
struct RecipeResult;
}

namespace mu::engraving {
class Score;
}

namespace mu::engraving {
class Score;
struct ScoreChanges;
}

namespace muse::agentharness {
//! One recorded change to the score, exactly as the engraving layer reported it.
//!
//! This is the **raw layer** of the information field (see `Agent Harness/技术设计.md` §3.2).
//! It is deliberately faithful rather than readable: `changedObjects` is a map of live
//! `EngravingObject*`, so it can never be written to disk. The point of keeping this layer is
//! that the readable layer above it is *derived* - when a derivation rule turns out to be wrong,
//! the raw facts are still here to re-derive from. Storing only the readable layer would mean the
//! field silently lies whenever a rule is incomplete.
//!
//! SIZING: the raw layer is a bounded ring buffer. Once a record is written to the session log the
//! in-memory copy is only needed for the recent past (the panel, and re-derivation after a rule
//! change), so old records are dropped rather than growing without bound.
struct RawFieldEvent
{
    //! Monotonic, never reused. Parallels the `seq` of a SessionEvent (DSH keeps `seq === index`).
    quint64 seq = 0;
    //! Wall clock, for display and for correlating with log lines.
    QString wallClock;

    //! What caused it. Currently only user edits are observable; the agent's own writes will be
    //! tagged here too once the tool layer exists (that is the whole point of having provenance).
    enum class Source {
        User,
        Agent,
        Plugin,
        Unknown,
    };
    Source source = Source::Unknown;

    //! Undo-stack transaction name, e.g. "Insert note" / "Transpose harmony".
    //! Empty for undo/redo and for re-layout notifications.
    QString action;
    //! True for a redo entry, false for a normal write; undo is reported as its own event.
    bool isRedo = false;
    //! Set for an explicit undo (see the `undoRedoNotification` wiring) - the transaction name
    //! belongs to the command being *undone*, which is why it is carried separately from `action`.
    bool isUndo = false;

    //! The changed region, in ticks and staff indices. NOTE: these are the *only* parts of
    //! `ScoreChanges` that are safe to persist - see the pointer warning above.
    int tickFrom = -1;
    int tickTo = -1;
    int staffFrom = -1;
    int staffTo = -1;
    bool isTextEditing = false;
    //! True when the payload carried a usable boundary (tick+staff range).
    bool hasBoundary = false;

    //! How many distinct engraving objects the transaction touched.
    int objectCount = 0;
    //! Bucket -> how many changed objects were of that kind. A histogram rather than the objects
    //! themselves: it is what the readable layer summarizes, and it is stable.
    //! Command types use their own value; element types are offset by TYPE_BUCKET_OFFSET.
    QMap<int, int> typeCounts;
    //! PropertyId -> count, for property-only changes.
    QMap<int, int> propertyCounts;

    //! True when this event carried neither objects nor types - i.e. a pure re-layout ping.
    //! Emitted by playback control (`playbackcontroller.cpp`), NOT by a user edit. The field must
    //! skip these or it will record phantom operations (see 技术设计 §3.4).
    bool isLayoutOnly = false;
};

//! The information field: a time-ordered record of what happened to the score.
//!
//! This is the "read" half of the harness. It owns no score data - every field is a projection of
//! a signal the engraving and notation layers already emit. Two rules keep it honest:
//!
//!   1. **Subscribe to `Score::changesChannel()`, never to `notationChanged()`.** The latter is a
//!      repaint request for the notation view and would miss edits coming from anywhere else
//!      (`维护手册.md` §4.6 - this exact mistake was made once already on the MIDI page).
//!   2. **Never hold an `EngravingObject*` past the callback.** `Score` is owned and deleted by
//!      `EngravingProject`; a stale pointer here is a crash, and a pointer in the log is
//!      unreplayable.
//!
//! WHY THIS IS A `QML_ELEMENT` RATHER THAN A MODULE SERVICE: the only thing it needs is
//! `IGlobalContext::currentNotation()`, and `IGlobalContext` is a *context* interface - so the
//! field needs a context, which means it has to be created where a context is resolvable. The
//! project already answers this question for every per-window view model: declare it a
//! `QML_ELEMENT` with `ContextInject<IGlobalContext> = { this }` and let QML's own context resolve
//! it (see `UndoRedoToolbarModel`, `MidiEditorModel`). A `qmlRegisterSingletonInstance` would
//! *not* work here: a singleton has no QML parent, so `iocCtxForQmlObject` asserts and hands back
//! a null context.
class FieldController : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT
    QML_ELEMENT;

    //! How many raw events have been recorded since the field was created. Never decreases.
    Q_PROPERTY(int eventCount READ eventCount NOTIFY fieldChanged)
    //! Bumped whenever the score's contents change. Used as the optimistic-concurrency fence:
    //! a tool call that carries an older revision is refused rather than applied to stale data.
    Q_PROPERTY(int revision READ revision NOTIFY fieldChanged)
    Q_PROPERTY(QString scoreName READ scoreName NOTIFY fieldChanged)
    Q_PROPERTY(int measureCount READ measureCount NOTIFY fieldChanged)
    Q_PROPERTY(int staffCount READ staffCount NOTIFY fieldChanged)
    Q_PROPERTY(bool hasScore READ hasScore NOTIFY fieldChanged)
    //! The most recent events, newest first, as plain maps for QML.
    Q_PROPERTY(QVariantList recentEvents READ recentEvents NOTIFY fieldChanged)
    //! The most recent operations as a reader should see them (semantic layer), newest first.
    //! Derived from the raw events on demand - see semanticderive.h for why it is not stored.
    Q_PROPERTY(QVariantList recentOps READ recentOps NOTIFY fieldChanged)
    //! Human-readable one-line status, for the panel header.
    Q_PROPERTY(QString statusText READ statusText NOTIFY fieldChanged)

    //! NOTE the interface is `mu::context::IGlobalContext`, NOT `muse::context::...`: this class
    //! lives in `muse::agentharness`, where a bare `context::` would resolve to the (nonexistent)
    //! `muse::context`. The models this pattern is copied from sit in `mu::notation`, so there the
    //! short form happens to work - copying it verbatim here is a compile error, not a subtle bug.
    muse::ContextInject<mu::context::IGlobalContext> context = { this };
    //! Used for two things, both of them observation rather than behaviour: asking whether the
    //! notation page is open, and (under MUSE_AGENT_DEMO_NOTATION) opening it so the field can be
    //! exercised without a human at the keyboard.
    //! NOTE the interface is `muse::IInteractive`, NOT `muse::interactive::IInteractive` - the
    //! header lives in the `interactive/` directory but declares into namespace `muse`.
    muse::ContextInject<muse::IInteractive> interactive = { this };
    //! The write path goes through the notation command layer, so an agent's edit is the *same*
    //! action a user performs - same undo stack, same command state, same notifications. Anything
    //! that reaches around this into `Score::undoXxx` would be a second, divergent write path.
    muse::ContextInject<muse::rcommand::ICommandDispatcher> commandDispatcher = { this };
    muse::ContextInject<muse::rcommand::ICommandsState> commandsState = { this };
    //! Needed to *enumerate* commands: there is no global list of the 446 notation command constants,
    //! only the register that knows every command registered by every module. Asking it is what keeps
    //! `command_list` from being a hand-maintained copy that drifts the moment upstream adds one.
    //! ⚠️ GLOBAL, not contextual: `ICommandsRegister : MODULE_GLOBAL_INTERFACE` (the dispatcher and
    //! the state next to it are context interfaces). Using `ContextInject` here is a compile error -
    //! a `static_assert` inside the injector, not a subtle runtime surprise.
    muse::GlobalInject<muse::rcommand::ICommandsRegister> commandsRegister;

public:
    explicit FieldController(QObject* parent = nullptr);
    ~FieldController() override;

    //! Start observing the current notation, and keep following it across score changes.
    //! Called from QML's Component.onCompleted. Safe to call twice; the second call only rebinds.
    Q_INVOKABLE void init();

    //! Snapshot layer, on demand. Both are pure projections of the score as it is *right now*, so
    //! neither can go stale - see scoredigest.h for why nothing here is cached.
    //! These are the read surface the agent's read tools will be built on; exposing them to QML now
    //! also makes them verifiable from the running program without an agent loop.
    Q_INVOKABLE QString digestOverview() const;
    Q_INVOKABLE QString digestWindow(int firstMeasure, int lastMeasure) const;

    //! ── The tool surface ─────────────────────────────────────────────────────────────────────
    //! One entry point for every tool call, so "the agent can only do what the table exposes" is
    //! enforced in a single place rather than at each call site. Returns the model-facing text;
    //! `ok` is reported through `lastToolOk()` (kept out of the return value because QML has no
    //! cheap way to return a pair).
    Q_INVOKABLE QString runTool(const QString& name, const QString& argsJson);
    //! Run a tool that may finish LATER, reporting through `done`.
    //!
    //! ⛔ A tool CAN be asynchronous, and `patch_apply` is: it has to queue its transaction and its
    //! dispatches into the same FIFO to make a batch one undo step (scoreactiongateway.h). `runTool`
    //! cannot express that - it must return a string - so a tool that finishes later returns an empty
    //! success and reports through this channel instead. Exactly one of the two paths fires.
    void runToolAsync(const QString& name, const QString& argsJson, std::function<void(const ToolResult&)> done);
    Q_INVOKABLE bool lastToolOk() const { return m_lastToolOk; }
    //! Tool names, for diagnostics and (later) the prompt's tool list.
    Q_INVOKABLE QStringList availableTools() const;

    //! ── The agent loop's log ──────────────────────────────────────────────────────────────────
    //! The session log is the single source of truth for a conversation: the model's history is
    //! *derived* from it (see sessionlog.h). Exposed here so the panel can show a transcript and so
    //! the log can be inspected from the running program before the loop itself exists.
    Q_INVOKABLE QString sessionJsonLines() const;
    //! Replay the log through the projection and return what the model would be sent, one line per
    //! message. This is the check that "the model sees only what was recorded" holds in practice.
    Q_INVOKABLE QString sessionPreview() const;
    //! Append a user message to the log (the loop will do this itself once it exists).
    Q_INVOKABLE void appendUserMessage(const QString& text);
    //! Seed the log with the system prompt. Idempotent per call, like the real loop's first step.
    Q_INVOKABLE void seedSystemPrompt();
    //! Number of events in the session log.
    Q_INVOKABLE int sessionEventCount() const;

    //! ── The agent loop, as QML sees it ────────────────────────────────────────────────────────
    //! ⛔ These are Q_PROPERTIES, not Q_INVOKABLE getters, and that is load-bearing: a QML binding to
    //! an invokable function is evaluated ONCE and never re-evaluated, because QML has nothing to
    //! connect to. The first version exposed them as `Q_INVOKABLE QString agentTranscript() const`
    //! and the panel stayed empty forever while the loop ran perfectly - the log showed twelve
    //! events and the transcript showed none. A read-only view of changing state must be a property.
    Q_PROPERTY(QVariantList agentTranscript READ agentTranscript NOTIFY fieldChanged)
    Q_PROPERTY(QString agentStreamingText READ agentStreamingText NOTIFY fieldChanged)
    Q_PROPERTY(QString agentStreamingReasoning READ agentStreamingReasoning NOTIFY fieldChanged)
    Q_PROPERTY(bool agentRunning READ agentRunning NOTIFY fieldChanged)
    Q_PROPERTY(bool agentConfigured READ agentConfigured NOTIFY fieldChanged)
    Q_PROPERTY(QString agentLastError READ agentLastError NOTIFY fieldChanged)
    Q_PROPERTY(QString agentApiKeySource READ agentApiKeySource NOTIFY fieldChanged)

    //! Send a user message and run a turn. Returns immediately: the loop is asynchronous, and
    //! blocking here would freeze the editor for the length of a model response.
    Q_INVOKABLE void sendToAgent(const QString& text);
    Q_INVOKABLE void abortAgent();
    Q_INVOKABLE QString agentModel() const;
    Q_INVOKABLE void setAgentModel(const QString& model);
    //! Override the API base URL (a gateway, or a local mock for verification).
    Q_INVOKABLE void setAgentBaseUrl(const QString& url);
    //! Store an API key for this run. Held in memory only - see `agentApiKeySource()` for why it is
    //! not written to settings yet.
    Q_INVOKABLE void setAgentApiKey(const QString& key);

    //! Whether a turn is currently running.
    bool agentRunning() const;
    //! Whether an API key is available. The panel says "not configured" rather than letting the user
    //! send a message that will fail on the wire.
    bool agentConfigured() const;
    //! The assistant's text so far in the running turn - the panel renders this live.
    QString agentStreamingText() const;
    QString agentStreamingReasoning() const;
    QString agentLastError() const;
    //! Where the key came from, for the panel to display: "environment" / "set" / "none".
    //! Reported rather than guessed at, because "it works on my machine" is usually an environment
    //! variable someone forgot they set.
    QString agentApiKeySource() const;
    //! The conversation so far, oldest first, as `{ role, text, kind }` maps for the panel.
    //! Built from the LOG, so what the panel shows and what the model was sent cannot diverge.
    QVariantList agentTranscript() const;

    //! ── The write path ───────────────────────────────────────────────────────────────────────
    //! ⛔ Ask before writing. A disabled notation command is *silently skipped* by the controller's
    //! outer wrapper, so dispatching one produces "the call succeeded and nothing happened"
    //! (`维护手册.md` §4.8, 第 594 条 - this project has paid for that twice).
    bool isCommandEnabled(const QString& command) const;
    //! Names of the commands that are enabled right now.
    QStringList enabledCommandNames() const;
    //! Dispatch one notation command. Empty return means it was dispatched; otherwise the reason.
    //! NOTE this goes through the notation command layer, NOT through a bare `Score::undoXxx`, so
    //! the edit lands on the same undo stack as the user's own actions and Ctrl+Z takes it back.
    //! ⛔ `args` MUST be forwarded. Some handlers behave completely differently without their
    //! parameters: `append-measures` with no `count` opens a "how many measures?" dialog instead of
    //! appending anything, and a dialog in a scripted run never resolves. The command then reports
    //! success and nothing happens - the silent-no-op shape this project keeps running into.
    QString dispatchCommand(const QString& command, const QJsonObject& args = QJsonObject());

    //! ── Transaction control, for callers that must span asynchronous work ────────────────────
    //! ⛔ `dispatch()` only QUEUES the command handler, so a caller that wants several dispatches in
    //! one undo step has to keep the transaction open across their completions. These are that
    //! mechanism, split out because the promise chain that uses them lives in `ScoreActionGateway`.
    //! Prefer `runInTransaction` when the body does its work inline.
    void beginUndoTransaction(const QString& actionName);
    //! Close the transaction. Returns whether it COMMITTED - a rollback forced by the framework (a
    //! read-only score, or an error a command set) is reported as `false` rather than assumed.
    bool endUndoTransaction(bool commit);

    //! Fold every transaction committed since `startIdx` into the one AT `startIdx`, so a batch of
    //! commands becomes a single undo step under the first command's name.
    //!
    //! ⛔ WHY THIS IS NEEDED AT ALL, after `beginUndoTransaction` was supposed to do the job:
    //! not every notation command opens its transaction through `prepareChanges`. `append-measures`
    //! goes `NotationActionController::addBoxes` → `NotationInteraction::addBoxes` →
    //! `Score::insertMeasure` → `Score::startCmd`, which calls `beginTransaction` DIRECTLY on the
    //! transaction manager - bypassing the notation undo stack and therefore ignoring an already-open
    //! transaction. Measured: three appends produced three transactions (`stateIndex 1→2→3→4`) even
    //! though the batch's own transaction was open, and the batch's `commitChanges` then found no
    //! active transaction at all.
    //!
    //! So the transaction cannot be imposed from outside for these commands; it has to be
    //! RECONSTRUCTED afterwards. `UndoStack::mergeTransactions` exists for exactly this and is what the
    //! rest of the application uses (`NotationUndoStack::mergeTransactions`).
    void mergeTransactionsFrom(size_t startIdx);

    //! Undo back to `targetRevision`, undoing one transaction at a time.
    //!
    //! ⛔ WHY A ROLLBACK IS NOT ENOUGH ON ITS OWN: the commands in a batch commit THEMSELVES
    //! (`Score::startCmd` opens its own transaction), so by the time the batch discovers that one of
    //! them failed, the earlier ones are already on the undo stack. Closing the batch's transaction
    //! with `rollback` therefore rolls back nothing - measured: a three-operation batch with a bad
    //! third operation left the first two applied while reporting "rolled back".
    //!
    //! Undoing to the recorded revision is the only way to make "nothing was applied" true after the
    //! fact. Returns the number of transactions undone.
    int undoToRevision(int targetRevision);

    //! Run a note recipe against the current score, inside a transaction.
    //!
    //! ⛔ The transaction is opened HERE and not by the caller. A recipe pushes `UndoableCommand`s, and
    //! outside a transaction `currentOrDummyTransaction()` hands back a dummy that DISCARDS them - the
    //! change would appear and then vanish. Making the transaction the controller's job means a recipe
    //! cannot be called without one.
    //!
    //! `recipe` receives the score and returns what happened. It must not be stored: the score pointer
    //! is only valid for the duration of the call (the "never cache an EngravingObject*" invariant).
    RecipeResult runNoteRecipe(const QString& actionName, const std::function<RecipeResult(mu::engraving::Score*)>& recipe);

    //! The score behind the current notation, or null. For recipes - everything else should go through
    //! the notation interface.
    mu::engraving::Score* currentScore() const;

    //! Dispatch and hand back the promise, so the caller can continue when the handler has run.
    //! `dispatchCommand` is the synchronous-looking face of this; it cannot tell you when the work
    //! actually happened.
    muse::async::Promise<muse::rcommand::Response> dispatchCommandPromise(const QString& command,
                                                                         const QJsonObject& args);

    //! Run `body` inside ONE undo transaction, so everything it dispatches becomes a single undo step.
    //!
    //! ⛔⛔ **`dispatch()` IS ASYNCHRONOUS, SO THIS CANNOT BE A SYNCHRONOUS WRAPPER.** `make_promise`
    //! goes through `Async::call`, which puts the command handler on the thread's queue - the handler
    //! does not run at the point of the call. A version that opened the transaction, dispatched, and
    //! committed synchronously therefore committed BEFORE any handler ran, and the handlers each
    //! opened their own transaction afterwards. The measured symptom: three commands produced three
    //! undo steps, and the transaction reported `stateIndex 1 -> 1, committed=0` while the dispatches
    //! reported `txActive=1` - both true, because nothing had run yet.
    //!
    //! So the transaction has to be opened and closed around the *completion* of the dispatches, which
    //! is what `runTransactionAsync` does. This synchronous form remains for callers whose body does
    //! its own work inline.
    //!
    //! Returns false when the transaction was ROLLED BACK, which happens when the score went read-only
    //! or a command set an error (`TransactionManager::endTransaction` forces a rollback in both
    //! cases). Callers must treat `false` as "nothing in this block took effect" - not as "some of it
    //! did".
    bool runInTransaction(const QString& actionName, const std::function<void()>& body);

    //! Open a transaction, run `body` (which may dispatch commands that complete later), and commit
    //! when `done` says the work has finished.
    //!
    //! `done` is called while the transaction is still open and returns whether to keep the work:
    //! `true` commits, `false` rolls back. The transaction stays open for the whole of it - including
    //! the deferred command handlers - which is the only way a batch of dispatches becomes one undo
    //! step. See the note above for why that is not the synchronous shape.
    void runTransactionAsync(const QString& actionName, const std::function<void()>& body,
                             const std::function<bool()>& done);

    //! The score's revision: the number of committed transactions on its undo stack.
    //!
    //! ⛔ THIS IS THE FENCE, and it is read from the undo stack rather than counted here. A counter of
    //! our own would be a second truth about "has the score changed", and the first thing it would
    //! miss is a change made by the user with the mouse - which is exactly the change an optimistic
    //! write needs to notice. `currentStateIndex()` moves on every commit, whoever made it.
    int scoreRevision() const;

    //! Whether an undo transaction is currently open on this score.
    //!
    //! Exposed for one reason: `ICommandDispatcher::dispatch` is ASYNCHRONOUS (`make_promise` uses
    //! `Async::call`), so a command's handler - and therefore its `Score::undo()` - does not run at the
    //! point of the call. Whether a batch of dispatches lands inside one transaction is a question
    //! about WHEN the handlers run, and this is the only way to observe that from outside.
    bool hasActiveTransaction() const;

    //! Run `body` on this object's own event queue, i.e. after the current call stack unwinds.
    //!
    //! WHY THIS EXISTS: `ICommandDispatcher::dispatch` is asynchronous - `make_promise` goes through
    //! `Async::call`, which puts the handler on the thread's queue rather than running it. A batch of
    //! dispatches therefore has to be QUEUED AS A WHOLE for its transaction to still be open when the
    //! handlers run (scoreactiongateway.h).
    //!
    //! ⛔ THE CALLER MUST BE A REAL `Asyncable`. `muse::async::Async::call(nullptr, ...)` looks like
    //! "no receiver needed" and is not: the queued message carries the receiver, and dispatching it
    //! with a null one SIGSEGVs the application the next time the queue is pumped. Measured, not
    //! guessed - it crashed twice before this was written.
    //!
    //! \b Lifetime: passing `this` also means the queued work is DROPPED if the field is destroyed
    //! first, which is the behaviour we want - a callback into a dead object is the other way this
    //! could crash.
    void postToEventLoop(std::function<void()> body);

    int eventCount() const { return int(m_events.size() + m_droppedEvents); }
    int revision() const { return m_revision; }
    QString scoreName() const { return m_scoreName; }
    int measureCount() const { return m_measureCount; }
    int staffCount() const { return m_staffCount; }
    bool hasScore() const { return m_score != nullptr; }
    QVariantList recentEvents() const;
    //! Semantic layer, newest first. Pure derivation from `events()` + the measure grid.
    QVariantList recentOps() const;
    QString statusText() const;

    //! Test/diagnostic seam: the raw events themselves, oldest first.
    const std::deque<RawFieldEvent>& events() const { return m_events; }
    //! Test/diagnostic seam: the measure grid the semantic layer addresses against.
    const MeasureGrid& measureGrid() const { return m_grid; }

    //! The field this many raw events deep is plenty; older ones are dropped (and counted).
    static constexpr size_t MAX_EVENTS = 512;
    //! How many events the QML panel is shown at once.
    static constexpr int PANEL_EVENT_LIMIT = 30;
    //! Command types and element types share one histogram; this keeps them from colliding.
    //! Sized well above the number of `CommandType` values (see undoablecommand.h).
    //! PUBLIC because the packing is done here and the unpacking is done in `semanticderive.cpp`:
    //! both sides must read the same number, and a private constant would force the other side to
    //! repeat the literal - which is how two halves of one rule drift apart.
    static constexpr int TYPE_BUCKET_OFFSET = 1000;

signals:
    void fieldChanged();

private:
    void bindToCurrentNotation();
    void unbind();
    void onScoreChanges(const mu::engraving::ScoreChanges& changes);
    void onStackChanged();
    void onUndoRedo();
    void refreshScoreFacts();

    void record(RawFieldEvent event);
    void noteActionFromUndoStack(RawFieldEvent& event) const;
    //! Verification hook: run one tool call once a score is bound (MUSE_AGENT_DEMO_TOOL).
    void applyDemoToolCallIfPending();
    //! Whether the notation undo stack currently has something to undo. Used by the demo hook to
    //! show that a tool's write landed on the *user's* undo stack, not a private one.
    bool undoStackCanUndo() const;

    mu::engraving::Score* m_score = nullptr;

    //! Flattened measure grid, rebuilt whenever the score changes. Kept as a value (never pointers)
    //! for the same reason nothing else here caches engraving objects: a stale grid means the model
    //! is told the wrong bar, which is the most expensive failure this subsystem can have.
    MeasureGrid m_grid;

    std::deque<RawFieldEvent> m_events;
    quint64 m_nextSeq = 1;
    quint64 m_droppedEvents = 0;

    int m_revision = 0;
    QString m_scoreName;
    int m_measureCount = 0;
    int m_staffCount = 0;

    bool m_inited = false;
    //! The undo stack's state index as of the last recorded event. Undo/redo is derived by
    //! comparing it, NOT from `undoRedoNotification` - see the note in onScoreChanges.
    int m_lastStateIndex = -1;
    //! Whether the last `runTool()` call succeeded. See the note on `runTool`.
    bool m_lastToolOk = true;

    //! The conversation log. Owned here for now; the agent loop will drive it, but keeping it alive
    //! from the field means the transcript and the derived history are inspectable before the loop
    //! exists - which is how the projection gets verified against the running program.
    SessionLog m_session;

    //! The loop and its transport, created on first use. Lazily, so a session that never talks to a
    //! model never builds a network stack - and so the field keeps working with no key configured.
    std::unique_ptr<LlmTransport> m_transport;
    std::unique_ptr<AgentLoop> m_loop;
    QString m_agentLastError;

    void ensureAgentLoop();
};
} // namespace muse::agentharness
