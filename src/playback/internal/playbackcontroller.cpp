/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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

#include "playbackcontroller.h"

#include "async/notifylist.h"
#include "containers.h"
#include "modularity/ioc.h"
#include "log.h"
#include "types/ret.h"
#include "io/fileinfo.h"
#include "io/path.h"

#include <QDir>
#include <QFile>
// qApp + QMetaObject::invokeMethod: marshalling the waveform-ready notification from the
// decode worker thread back onto the main thread.
#include <QCoreApplication>
#include <QMetaObject>

#include "audio/common/audioutils.h"
#include "audio/devtools/inputlag.h"

#include "engraving/dom/factory.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/utils.h"

#include "notation/iexcerptnotation.h" // IWYU pragma: keep
#include "notation/imasternotation.h"
#include "notation/inotationautomation.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationnoteinput.h" // IWYU pragma: keep
#include "notation/inotationparts.h"
#include "notation/inotationselection.h"

#include "project/inotationproject.h"

#include "../playbacktypes.h"
#include "onlinesoundscontroller.h"

using namespace muse;
using namespace muse::async;
using namespace muse::audio;
using namespace muse::midi;
using namespace mu::engraving;
using namespace mu::notation;
using namespace mu::playback;
using namespace mu::project;

//! TEMPORARY diagnostic (Jianpu build): records the playback position bookkeeping in
//! <home>/Documents/MuseScore4/jianpu-postrace.txt, to trace a measure number in the playback
//! toolbar that does not match the measure number shown in the status bar.
static void jianpuPositionTrace(const QString& line)
{
    static const QString path = QDir::homePath() + QStringLiteral("/Documents/MuseScore4/jianpu-postrace.txt");
    static bool started = false;
    QFile file(path);
    const QIODevice::OpenMode mode = started ? QIODevice::Append : QIODevice::Truncate;
    started = true;
    if (file.open(mode | QIODevice::Text)) {
        file.write(line.toUtf8());
        file.write("\n");
    }
}

static AudioOutputParams makeReverbOutputParams()
{
    AudioFxParams reverbParams;
    reverbParams.resourceMeta = makeReverbMeta();
    reverbParams.categories.insert(AudioFxCategory::FxReverb);
    reverbParams.chainOrder = 0;
    reverbParams.active = true;

    AudioOutputParams result;
    result.fxChain.emplace(reverbParams.chainOrder, std::move(reverbParams));

    return result;
}

static std::string resolveAuxTrackTitle(aux_channel_idx_t index, const AudioOutputParams& params, bool considerFx = true)
{
    if (considerFx && params.fxChain.size() == 1) {
        const AudioResourceMeta& meta = params.fxChain.cbegin()->second.resourceMeta;
        if (meta.id == MUSE_REVERB_ID) {
            return muse::trc("playback", "Reverb");
        }

        return meta.id;
    }

    return muse::mtrc("playback", "Aux %1").arg(index + 1).toStdString();
}

PlaybackController::PlaybackController(const muse::modularity::ContextPtr& iocCtx)
    : muse::Contextable(iocCtx), m_onlineSoundsController(std::make_unique<OnlineSoundsController>(iocCtx))
{
}

PlaybackController::~PlaybackController() = default;

void PlaybackController::init()
{
    m_onlineSoundsController->regActions();

    globalContext()->currentNotationChanged().onNotify(this, [this]() {
        onNotationChanged();
    });

    globalContext()->currentProjectChanged().onNotify(this, [this]() {
        if (m_isPlaybackInited) {
            resetPlayback();
        }

        if (!globalContext()->currentProject()) {
            return;
        }

        m_loadingProgress.start();

        playback()->init().onResolve(this, [this](const Ret& ret) {
            if (ret) {
                setupPlayback();
            }
        });
    });

    m_totalPlayTimeChanged.onNotify(this, [this]() {
        updateCurrentTempo();

        updateLoop();

        // Tempo/repeat/duration changes shift what secs_t a given tick maps to,
        // which invalidates any Volume/Pan automation envelope already sent to the engine
        resendAutomatedControlParams();
    });

    m_measureInputLag = configuration()->shouldMeasureInputLag();
}

void PlaybackController::updateCurrentTempo()
{
    if (!notationPlayback()) {
        return;
    }

    const Tempo& newTempo = notationPlayback()->multipliedTempo(m_currentTick);

    if (newTempo == m_currentTempo) {
        return;
    }

    m_currentTempo = newTempo;
    m_currentTempoChanged.notify();
}

bool PlaybackController::isPlayAllowed() const
{
    if (!m_isPlaybackInited) {
        return false;
    }

    if (!m_notation) {
        return false;
    }

    if (!m_masterNotation) {
        return false;
    }

    const MasterScore* masterScore = m_masterNotation->masterScore();
    if (!masterScore) {
        return false;
    }

    if (!masterScore->firstMeasure()) {
        return false;
    }

    if (!m_notation->hasVisibleParts()) {
        return false;
    }

    if (!isLoaded()) {
        return false;
    }

    return true;
}

Channel<bool> PlaybackController::isPlayAllowedChanged() const
{
    return m_isPlayAllowedChanged;
}

bool PlaybackController::isPlaying() const
{
    if (!currentPlayer()) {
        return false;
    }
    return currentPlayer()->playbackStatus() == PlaybackStatus::Running;
}

Channel<bool> PlaybackController::isPlayingChanged() const
{
    return m_isPlayingChanged;
}

bool PlaybackController::isPaused() const
{
    if (!currentPlayer()) {
        return false;
    }
    return currentPlayer()->playbackStatus() == PlaybackStatus::Paused;
}

bool PlaybackController::isLoaded() const
{
    return m_loadingTrackCount == 0;
}

bool PlaybackController::isLoopEnabled() const
{
    return notationPlayback() && notationPlayback()->isLoopEnabled();
}

Channel<bool> PlaybackController::loopEnabledChanged() const
{
    return m_loopEnabledChanged;
}

bool PlaybackController::loopBoundariesSet() const
{
    return notationPlayback() && !notationPlayback()->loopBoundaries().isNull();
}

void PlaybackController::seekRawTick(const midi::tick_t tick, const bool flushSound)
{
    if (m_currentTick == tick) {
        jianpuPositionTrace(QStringLiteral("seek skipped (same tick) tick=%1").arg(tick));
        return;
    }

    RetVal<midi::tick_t> playedTick = notationPlayback()->playPositionTickByRawTick(tick);
    if (!playedTick.ret) {
        jianpuPositionTrace(QStringLiteral("seek failed tick=%1").arg(tick));
        return;
    }

    const secs_t secs = playedTickToSecs(playedTick.val);

    // Update the position right away. The audio player reports the new position only after it has
    // processed the seek, and while it is stopped it may keep reporting the position it had, so
    // without this the measure/beat in the toolbar would not follow the selection.
    m_currentTick = tick;
    updateCurrentTempo();
    m_currentTickChanged.notify();

    jianpuPositionTrace(QStringLiteral("seek tick=%1 playedTick=%2 secs=%3").arg(tick).arg(playedTick.val).arg(QString::number(static_cast<double>(secs), 'f', 6)));

    seek(secs, flushSound);
}

muse::async::Notification PlaybackController::currentTickChanged() const
{
    return m_currentTickChanged;
}

void PlaybackController::seek(const audio::secs_t secs, const bool flushSound)
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    currentPlayer()->seek(secs, flushSound);
}

bool PlaybackController::isPlaybackInited() const
{
    return m_isPlaybackInited;
}

muse::async::Channel<bool> PlaybackController::playbackInitedChanged() const
{
    return m_playbackInited;
}

const IPlaybackController::InstrumentTrackIdMap& PlaybackController::instrumentTrackIdMap() const
{
    return m_instrumentTrackIdMap;
}

const IPlaybackController::AuxTrackIdMap& PlaybackController::auxTrackIdMap() const
{
    return m_auxTrackIdMap;
}

Channel<TrackId> PlaybackController::trackAdded() const
{
    return m_trackAdded;
}

Channel<TrackId> PlaybackController::trackRemoved() const
{
    return m_trackRemoved;
}

std::string PlaybackController::auxChannelName(aux_channel_idx_t index) const
{
    return resolveAuxTrackTitle(index, audioSettings()->auxOutputParams(index));
}

Channel<aux_channel_idx_t, std::string> PlaybackController::auxChannelNameChanged() const
{
    return m_auxChannelNameChanged;
}

Promise<SoundPresetList> PlaybackController::availableSoundPresets(const InstrumentTrackId& instrumentTrackId) const
{
    auto it = m_instrumentTrackIdMap.find(instrumentTrackId);
    if (it == m_instrumentTrackIdMap.end()) {
        return Promise<SoundPresetList>([](auto, auto reject) {
            return reject(static_cast<int>(Ret::Code::UnknownError), "invalid instrumentTrackId");
        });
    }

    const AudioInputParams& params = audioSettings()->trackInputParams(instrumentTrackId);
    return playback()->availableSoundPresets(params.resourceMeta);
}

const PlaybackController::SoloMuteState& PlaybackController::trackSoloMuteState(const InstrumentTrackId& trackId) const
{
    return m_notation->soloMuteState()->trackSoloMuteState(trackId);
}

void PlaybackController::setTrackSoloMuteState(const InstrumentTrackId& trackId, const SoloMuteState& state)
{
    if (trackId == notationPlayback()->metronomeTrackId()) {
        if (state.mute != notationConfiguration()->isMetronomeEnabled()) {
            return;
        }

        toggleMetronome();
        return;
    }

    m_notation->soloMuteState()->setTrackSoloMuteState(trackId, state);
}

void PlaybackController::playElements(const std::vector<const notation::EngravingItem*>& elements, const PlayParams& params, bool isMidi)
{
    IF_ASSERT_FAILED(notationPlayback()) {
        return;
    }

    if ((!configuration()->playNotesWhenEditing()) || (isMidi && !configuration()->playNotesOnMidiInput())) {
        return;
    }

    if (m_measureInputLag) {
        START_INPUT_LAG_TIMER;
    }

    std::vector<const notation::EngravingItem*> elementsForPlaying;
    elementsForPlaying.reserve(elements.size());

    bool playChordWhenEditing = configuration()->playChordWhenEditing();
    bool playHarmonyWhenEditing = configuration()->playHarmonyWhenEditing();

    for (const EngravingItem* element : elements) {
        IF_ASSERT_FAILED(element) {
            continue;
        }

        if (!element->isPlayable()) {
            continue;
        }

        if (element->isChord() && !playChordWhenEditing) {
            continue;
        }

        if (element->isHarmony() && !playHarmonyWhenEditing) {
            continue;
        }

        elementsForPlaying.push_back(element);
    }

    const mpe::duration_t duration = params.duration.has_value() ? params.duration.value()
                                     : notationConfiguration()->notePlayDurationMilliseconds() * 1000;

    notationPlayback()->triggerEventsForItems(elementsForPlaying, duration, params.flushSound);
}

void PlaybackController::playNotes(const NoteValList& notes, staff_idx_t staffIdx, const Segment* segment, const PlayParams& params)
{
    Segment* seg = const_cast<Segment*>(segment);
    Chord* chord = engraving::Factory::createChord(seg);
    chord->setOwnershipParent(seg);

    std::vector<const EngravingItem*> elements;
    elements.reserve(notes.size());

    for (const NoteVal& nval : notes) {
        Note* note = engraving::Factory::createNote(chord);
        note->setOwnershipParent(chord);
        note->setStaffIdx(staffIdx);
        note->setNval(nval);
        elements.push_back(note);
    }

    playElements(elements, params);

    delete chord;
    DeleteAll(elements);
}

void PlaybackController::playMetronome(int tick)
{
    notationPlayback()->triggerMetronome(tick);
}

void PlaybackController::triggerControllers(const muse::mpe::ControllerChangeEventList& list, staff_idx_t staffIdx, int tick)
{
    notationPlayback()->triggerControllers(list, staffIdx, tick);
}

void PlaybackController::seekElement(const notation::EngravingItem* element, bool flushSound)
{
    IF_ASSERT_FAILED(notationPlayback()) {
        return;
    }

    if (!element) {
        return;
    }

    // Go through seekRawTick(), so that the position is updated and the toolbar notified right away,
    // and so that the element tick is mapped through the repeat list in the same way everywhere.
    seekRawTick(element->tick().ticks(), flushSound);
}

void PlaybackController::seekBeat(int measureIndex, int beatIndex, bool flushSound)
{
    if (!notationPlayback()) {
        return;
    }

    // The measure/beat fields of the playback toolbar go through the same path as the other seeks, so
    // that the position is updated even while the audio player is not running.
    seekRawTick(notationPlayback()->beatToRawTick(measureIndex, beatIndex), flushSound);
}

void PlaybackController::seekRangeSelection()
{
    if (!selection()->isRange()) {
        return;
    }

    midi::tick_t startTick = selectionRange()->startTick().ticks();

    seekRawTick(startTick);
}

void PlaybackController::onAudioResourceChanged(const TrackId trackId, const InstrumentTrackId& instrumentTrackId,
                                                const AudioResourceMeta& oldMeta, const AudioResourceMeta& newMeta)
{
    INotationPlaybackPtr notationPlayback = this->notationPlayback();
    if (!notationPlayback) {
        return;
    }

    if (oldMeta.type == newMeta.type && oldMeta.id == newMeta.id) {
        return;
    }

    if (shouldLoadDrumset(instrumentTrackId, oldMeta, newMeta)) {
        m_drumsetLoader.loadDrumset(m_notation, instrumentTrackId, newMeta);
    }

    notationPlayback->removeSoundFlags({ instrumentTrackId });

    if (audio::isOnlineAudioResource(newMeta)) {
        m_onlineSoundsController->addOnlineTrack(trackId, newMeta);
        tours()->onEvent(u"online_sounds_added");
        notationPlayback->setSendEventsOnScoreChange(instrumentTrackId, true);
    } else if (audio::isOnlineAudioResource(oldMeta)) {
        m_onlineSoundsController->removeOnlineTrack(trackId);
        notationPlayback->setSendEventsOnScoreChange(instrumentTrackId, false);
    }
}

bool PlaybackController::shouldLoadDrumset(const engraving::InstrumentTrackId& instrumentTrackId,
                                           const AudioResourceMeta& oldMeta, const AudioResourceMeta& newMeta) const
{
    if (oldMeta.type == newMeta.type && oldMeta.id == newMeta.id) {
        return false;
    }

    const Part* part = masterNotationParts()->part(instrumentTrackId.partId);
    const Instrument* instrument = part ? part->instrumentById(instrumentTrackId.instrumentId) : nullptr;
    if (!instrument || !instrument->useDrumset()) {
        return false;
    }

    return isResourceType(oldMeta, AudioResourceType::MuseSamplerSoundPack)
           || isResourceType(newMeta, AudioResourceType::MuseSamplerSoundPack);
}

void PlaybackController::addSoundFlagsIfNeed(const std::vector<EngravingItem*>& selection)
{
    if (selection.empty()) {
        return;
    }

    std::vector<StaffText*> staffTextList;

    for (EngravingItem* item : selection) {
        if (!item || !item->isStaffText()) {
            continue;
        }

        InstrumentTrackId trackId = mu::engraving::makeInstrumentTrackId(item);
        const AudioInputParams& params = audioSettings()->trackInputParams(trackId);

        bool supportsSoundFlags = params.type() == AudioSourceType::MuseSampler;
        if (supportsSoundFlags) {
            staffTextList.push_back(toStaffText(item));
        }
    }

    if (!staffTextList.empty()) {
        notationPlayback()->addSoundFlags(staffTextList);
    }
}

muse::audio::IPlayerPtr PlaybackController::currentPlayer() const
{
    return m_player;
}

INotationPlaybackPtr PlaybackController::notationPlayback() const
{
    return m_masterNotation ? m_masterNotation->playback() : nullptr;
}

INotationPartsPtr PlaybackController::masterNotationParts() const
{
    return m_masterNotation ? m_masterNotation->parts() : nullptr;
}

INotationSelectionPtr PlaybackController::selection() const
{
    return m_notation ? m_notation->interaction()->selection() : nullptr;
}

INotationSelectionRangePtr PlaybackController::selectionRange() const
{
    INotationSelectionPtr selection = this->selection();
    return selection ? selection->range() : nullptr;
}

INotationInteractionPtr PlaybackController::interaction() const
{
    return m_notation ? m_notation->interaction() : nullptr;
}

uint64_t PlaybackController::notationPlaybackKey() const
{
    return reinterpret_cast<uint64_t>(notationPlayback().get());
}

void PlaybackController::onNotationChanged()
{
    setNotation(globalContext()->currentNotation());
}

void PlaybackController::onPartChanged(const Part* part)
{
    if (!m_notation->hasVisibleParts()) {
        doPause();
    }
    m_isPlayAllowedChanged.send(isPlayAllowed());

    if (!configuration()->muteHiddenInstruments()) {
        return;
    }

    for (const InstrumentTrackId& instrumentTrackId : part->instrumentTrackIdList()) {
        auto soloMuteState = trackSoloMuteState(instrumentTrackId);
        soloMuteState.mute = !part->show();
        setTrackSoloMuteState(instrumentTrackId, soloMuteState);
    }

    if (part->hasChordSymbol()) {
        InstrumentTrackId chordSymbolsTrackId = notationPlayback()->chordSymbolsTrackId(part->id());
        auto chordsSoloMuteState = trackSoloMuteState(chordSymbolsTrackId);
        chordsSoloMuteState.mute = !part->show();
        setTrackSoloMuteState(chordSymbolsTrackId, chordsSoloMuteState);
    }
}

void PlaybackController::onSelectionChanged()
{
    const INotationSelectionPtr selection = this->selection();
    if (!selection || !m_player) {
        return;
    }

    bool selectionTypeChanged = m_isRangeSelection && !selection->isRange();
    m_isRangeSelection = selection->isRange();

    if (!m_isRangeSelection) {
        if (selectionTypeChanged) {
            updateLoop();
            updateSoloMuteStates();
        }

        addSoundFlagsIfNeed(selection->elements());

        // Move the position to the selected element as well. The cursor drawn in the score follows
        // the selection, so without this the measure/beat in the playback toolbar would keep the
        // position of the previous playback and would not match the note that was just clicked
        // (clicking an element seeks only while playing, see NotationViewInputController).
        if (!isPlaying() && selection->element()) {
            seekElement(selection->element(), false /*flushSound*/);
        }
        return;
    }

    m_player->resetLoop();

    jianpuPositionTrace(QStringLiteral("selection range: startTick=%1 endTick=%2 currentTick=%3")
                        .arg(selectionRange()->startTick().ticks())
                        .arg(selectionRange()->endTick().ticks())
                        .arg(m_currentTick));

    seekRangeSelection();
    updateSoloMuteStates();
}

muse::Ret PlaybackController::togglePlay()
{
    if (isPlaying()) {
        return pause();
    }

    if (isPaused()) {
        doResume();
        return make_ok();
    }

    return play();
}

muse::Ret PlaybackController::play(bool showErrors)
{
    if (!isPlayAllowed()) {
        LOGW() << "playback not allowed";
        return make_ret(Ret::Code::NotSupported);
    }

    IF_ASSERT_FAILED(currentPlayer()) {
        return make_ret(Ret::Code::InternalError);
    }

    if (showErrors && m_onlineSoundsController->shouldShowOnlineSoundsProcessingError(isPlaying())) {
        m_onlineSoundsController->showOnlineSoundsProcessingError([this]() { play(false /*showErrors*/); });
        return make_ret(Ret::Code::NotSupported);
    }

    interaction()->endEditElement();
    interaction()->noteInput()->endNoteInput();

    if (isPaused()) {
        notationPlayback()->sendEventsForChangedTracks();

        secs_t pos = currentPlayer()->playbackPosition();
        secs_t endSecs = totalPlayTime();
        if (pos == endSecs) {
            secs_t startSecs = playbackStartSecs();
            seek(startSecs);
        }

        doResume();
    } else {
        notationPlayback()->sendEventsForChangedTracks();

        doPlay();
    }

    return make_ok();
}

muse::Ret PlaybackController::pause(bool select)
{
    if (isPlaying()) {
        doPause(select);
    }

    return make_ok();
}

muse::Ret PlaybackController::stop()
{
    doStop();
    return make_ok();
}

muse::Ret PlaybackController::rewind(muse::secs_t secs)
{
    if (!isPlayAllowed()) {
        LOGW() << "playback not allowed";
        return make_ret(Ret::Code::NotSupported);
    }

    doRewind(secs);
    return make_ok();
}

muse::Ret PlaybackController::playFromSelection(bool showErrors)
{
    if (!isPlayAllowed()) {
        LOGW() << "playback not allowed";
        return make_ret(Ret::Code::NotSupported);
    }

    if (showErrors && m_onlineSoundsController->shouldShowOnlineSoundsProcessingError(isPlaying())) {
        m_onlineSoundsController->showOnlineSoundsProcessingError([this]() { playFromSelection(false /*showErrors*/); });
        return make_ret(Ret::Code::NotSupported);
    }

    int startTick = 0;

    if (!selection()->isNone()) {
        startTick = INT_MAX;
        for (const EngravingItem* item : selection()->elements()) {
            startTick = std::min(startTick, item->tick().ticks());
        }
    } else {
        // Selection is none - fall back to last element hit...
        if (const EngravingItem* lastElementHit = selection()->lastElementHit()) {
            startTick = lastElementHit->tick().ticks();
        }
    }

    const LoopBoundaries& loop = notationPlayback()->loopBoundaries();
    if (loop.enabled) {
        if (startTick < loop.loopInTick.ticks() || startTick > loop.loopOutTick.ticks()) {
            startTick = loop.loopInTick.ticks();
        }
    }

    const RetVal<midi::tick_t> retval = notationPlayback()->playPositionTickByRawTick(startTick);
    if (!retval.ret) {
        return retval.ret;
    }

    seek(playedTickToSecs(retval.val));

    if (isPaused()) {
        doResume();
    } else if (!isPlaying()) {
        doPlay();
    }

    return make_ok();
}

void PlaybackController::doPlay()
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    if (isLoopEnabled()) {
        secs_t startSecs = playbackStartSecs();
        seek(startSecs);
    }

    currentPlayer()->prepareToPlay().onResolve(this, [this](const Ret& ret) {
        if (!currentPlayer()) {
            return;
        }

        if (!ret) {
            LOGE() << ret.toString();
        }

        secs_t delay = 0.;
        if (notationConfiguration()->isCountInEnabled()) {
            notationPlayback()->triggerCountIn(m_currentTick, delay);
        }

        currentPlayer()->play(delay);
    });
}

void PlaybackController::doRewind(secs_t newPosition)
{
    secs_t startSecs = playbackStartSecs();
    secs_t endSecs = totalPlayTime();
    newPosition = std::clamp(newPosition, startSecs, endSecs);

    seek(newPosition);
}

void PlaybackController::doPause(bool select)
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    if (isPaused()) {
        return;
    }

    currentPlayer()->pause();

    if (select && m_notation) {
        const Fraction playPositionFrac = Fraction::fromTicks(m_currentTick);
        interaction()->findAndSelectChordRest(playPositionFrac);
    }
}

void PlaybackController::doStop()
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    currentPlayer()->stop();
}

void PlaybackController::doResume()
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    currentPlayer()->prepareToPlay().onResolve(this, [this](const Ret& ret) {
        if (!currentPlayer()) {
            return;
        }

        if (!ret) {
            LOGE() << ret.toString();
        }

        secs_t delay = 0.;
        if (notationConfiguration()->isCountInEnabled()) {
            notationPlayback()->triggerCountIn(m_currentTick, delay);
        }

        currentPlayer()->resume(delay);
    });
}

void PlaybackController::onPlaybackStatusChanged()
{
    if (!notationPlayback()) {
        return;
    }

    bool playing = isPlaying();
    const auto& onlineSounds = m_onlineSoundsController->onlineSounds();

    for (const auto& pair : m_instrumentTrackIdMap) {
        bool shouldSendOnScoreChange = playing || muse::contains(onlineSounds, pair.second);
        notationPlayback()->setSendEventsOnScoreChange(pair.first, shouldSendOnScoreChange);
    }
}

secs_t PlaybackController::playbackStartSecs() const
{
    if (!m_notation) {
        return 0;
    }

    const LoopBoundaries& loop = notationPlayback()->loopBoundaries();
    if (loop.enabled) {
        // Convert from raw ticks (visual tick != playback tick due to repeats etc)
        RetVal<tick_t> startTick = notationPlayback()->playPositionTickByRawTick(loop.loopInTick.ticks());
        if (!startTick.ret) {
            return 0;
        }
        return playedTickToSecs(startTick.val);
    }

    return 0;
}

InstrumentTrackIdSet PlaybackController::instrumentTrackIdSetForRangePlayback() const
{
    std::vector<const Part*> selectedParts = selectionRange()->selectedParts();
    Fraction startTick = selectionRange()->startTick();
    int startTicks = startTick.ticks();

    InstrumentTrackIdSet result;

    for (const Part* part : selectedParts) {
        if (const Instrument* startInstrument = part->instrument(startTick)) {
            result.insert({ part->id(), startInstrument->id() });
        }

        for (auto [tick, instrument] : part->instruments()) {
            if (tick > startTicks) {
                result.insert({ part->id(), instrument->id() });
            }
        }

        if (part->hasChordSymbol()) {
            result.insert(notationPlayback()->chordSymbolsTrackId(part->id()));
        }
    }

    return result;
}

muse::Ret PlaybackController::togglePlayRepeats()
{
    bool playRepeatsEnabled = notationConfiguration()->isPlayRepeatsEnabled();
    notationConfiguration()->setIsPlayRepeatsEnabled(!playRepeatsEnabled);
    return make_ok();
}

muse::Ret PlaybackController::togglePlayChordSymbols()
{
    bool playChordSymbolsEnabled = notationConfiguration()->isPlayChordSymbolsEnabled();
    notationConfiguration()->setIsPlayChordSymbolsEnabled(!playChordSymbolsEnabled);

    for (auto it = m_instrumentTrackIdMap.cbegin(); it != m_instrumentTrackIdMap.cend(); ++it) {
        if (notationPlayback()->isChordSymbolsTrack(it->first)) {
            setTrackActivity(it->first, !playChordSymbolsEnabled);
        }
    }
    return make_ok();
}

muse::Ret PlaybackController::toggleAutomaticallyPan()
{
    bool panEnabled = notationConfiguration()->isAutomaticallyPanEnabled();
    notationConfiguration()->setIsAutomaticallyPanEnabled(!panEnabled);
    return make_ok();
}

muse::Ret PlaybackController::toggleMetronome()
{
    bool metronomeEnabled = notationConfiguration()->isMetronomeEnabled();
    bool countInEnabled = notationConfiguration()->isCountInEnabled();

    notationConfiguration()->setIsMetronomeEnabled(!metronomeEnabled);

    setTrackActivity(notationPlayback()->metronomeTrackId(), !metronomeEnabled || countInEnabled);

    return make_ok();
}

muse::Ret PlaybackController::toggleCountIn()
{
    bool metronomeEnabled = notationConfiguration()->isMetronomeEnabled();
    bool countInEnabled = notationConfiguration()->isCountInEnabled();

    notationConfiguration()->setIsCountInEnabled(!countInEnabled);

    setTrackActivity(notationPlayback()->metronomeTrackId(), metronomeEnabled || !countInEnabled);

    return make_ok();
}

muse::Ret PlaybackController::toggleMidiInput()
{
    bool wasMidiInputEnabled = notationConfiguration()->isMidiInputEnabled();
    notationConfiguration()->setIsMidiInputEnabled(!wasMidiInputEnabled);
    return make_ok();
}

muse::Ret PlaybackController::setMidiUseWrittenPitch(bool useWrittenPitch)
{
    notationConfiguration()->setMidiUseWrittenPitch(useWrittenPitch);
    return make_ok();
}

muse::Ret PlaybackController::toggleLoopPlayback()
{
    if (isLoopEnabled()) {
        disableLoop();
        return make_ok();
    }

    if (loopBoundariesSet() && !selection()->isRange()) {
        enableLoop();
        return make_ok();
    }

    int loopInTick = 0;
    int loopOutTick = 0;

    if (!selection()->isNone()) {
        loopInTick = selectionRange()->startTick().ticks();
        loopOutTick = selectionRange()->endTick().ticks();
    }

    if (loopInTick <= 0) {
        loopInTick = INotationPlayback::FirstScoreTick;
    }

    if (loopOutTick <= 0) {
        loopOutTick = INotationPlayback::LastScoreTick;
    }

    addLoopBoundaryToTick(LoopBoundaryType::LoopIn, loopInTick);
    addLoopBoundaryToTick(LoopBoundaryType::LoopOut, loopOutTick);

    return make_ok();
}

muse::Ret PlaybackController::toggleHearPlaybackWhenEditing()
{
    bool wasPlayNotesWhenEditing = configuration()->playNotesWhenEditing();
    configuration()->setPlayNotesWhenEditing(!wasPlayNotesWhenEditing);
    return make_ok();
}

muse::Ret PlaybackController::reloadPlaybackCache()
{
    INotationPlaybackPtr nPlayback = notationPlayback();
    if (nPlayback) {
        nPlayback->reload();
    }

    return make_ok();
}

muse::Ret PlaybackController::addLoopBoundary(LoopBoundaryType type)
{
    if (isPlaying()) {
        addLoopBoundaryToTick(type, m_currentTick);
    } else {
        addLoopBoundaryToTick(type, INotationPlayback::SelectedNoteTick);
    }

    return make_ok();
}

void PlaybackController::addLoopBoundaryToTick(LoopBoundaryType type, int tick)
{
    if (notationPlayback()) {
        notationPlayback()->addLoopBoundary(type, tick);
        enableLoop();
    }
}

void PlaybackController::updateLoop()
{
    if (!notationPlayback() || !currentPlayer()) {
        return;
    }

    const LoopBoundaries& boundaries = notationPlayback()->loopBoundaries();

    if (!boundaries.enabled) {
        disableLoop();
        return;
    }

    // Convert from raw ticks (visual tick != playback tick due to repeats etc)
    RetVal<tick_t> playbackTickFrom = notationPlayback()->playPositionTickByRawTick(boundaries.loopInTick.ticks());
    RetVal<tick_t> playbackTickTo = notationPlayback()->playPositionTickByRawTick(boundaries.loopOutTick.ticks());
    if (!playbackTickFrom.ret || !playbackTickTo.ret) {
        return;
    }

    secs_t fromSecs = playedTickToSecs(playbackTickFrom.val);
    secs_t toSecs = playedTickToSecs(playbackTickTo.val);
    currentPlayer()->setLoop(fromSecs, toSecs);

    enableLoop();
}

void PlaybackController::enableLoop()
{
    if (notationPlayback()) {
        notationPlayback()->setLoopBoundariesEnabled(true);
    }
}

void PlaybackController::disableLoop()
{
    IF_ASSERT_FAILED(notationPlayback() && currentPlayer()) {
        return;
    }

    currentPlayer()->resetLoop();
    notationPlayback()->setLoopBoundariesEnabled(false);
}

mu::project::IProjectAudioSettingsPtr PlaybackController::audioSettings() const
{
    INotationProjectPtr project = globalContext()->currentProject();
    IF_ASSERT_FAILED(project) {
        return nullptr;
    }

    return project->audioSettings();
}

void PlaybackController::resetPlayback()
{
    if (currentPlayer()) {
        currentPlayer()->playbackPositionChanged().disconnect(this);
        currentPlayer()->playbackStatusChanged().disconnect(this);
    }

    playback()->clearSources();
    playback()->sourceParamsChanged().disconnect(this);
    playback()->fxChainParamsChanged().disconnect(this);
    playback()->clearAllFx();
    playback()->masterFxChainParamsChanged().disconnect(this);
    playback()->clearMasterOutputParams();

    m_seqAsyncReceiver.async_disconnectAll();

    m_currentTick = 0;

    playback()->deinit();

    m_instrumentTrackIdMap.clear();
    m_auxTrackIdMap.clear();

    m_isRangeSelection = false;

    m_isPlaybackInited = false;
    m_playbackInited.send(m_isPlaybackInited);

    m_player = nullptr;
    globalContext()->setCurrentPlayer(nullptr);

    m_onlineSoundsController->reset();
}

void PlaybackController::addTrack(const InstrumentTrackId& instrumentTrackId, const TrackAddFinished& onFinished)
{
    if (notationPlayback()->metronomeTrackId() == instrumentTrackId) {
        doAddTrack(instrumentTrackId, muse::trc("playback", "Metronome"), onFinished);
        return;
    }

    const Part* part = masterNotationParts()->part(instrumentTrackId.partId);
    if (!part) {
        return;
    }

    if (notationPlayback()->isChordSymbolsTrack(instrumentTrackId)) {
        const std::string trackName = muse::trc("playback", "Chords") + "." + part->partName().toStdString();
        doAddTrack(instrumentTrackId, trackName, onFinished);
        return;
    }

    const muse::String primaryInstrId = part->instrument()->id();
    if (instrumentTrackId.instrumentId == primaryInstrId) {
        const std::string trackName = part->partName().toStdString();
        doAddTrack(instrumentTrackId, trackName, onFinished);
        return;
    }

    const Instrument* instrument = part->instrumentById(instrumentTrackId.instrumentId);
    if (instrument != nullptr) {
        std::string trackName = "(" + instrument->trackName().toStdString() + ")";
        doAddTrack(instrumentTrackId, trackName, onFinished);
    }
}

void PlaybackController::doAddTrack(const InstrumentTrackId& instrumentTrackId, const std::string& title,
                                    const TrackAddFinished& onFinished)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    if (!instrumentTrackId.isValid()) {
        return;
    }

    mpe::PlaybackData playbackData = notationPlayback()->trackPlaybackData(instrumentTrackId);
    if (!playbackData.isValid()) {
        return;
    }

    AudioInputParams inParams = audioSettings()->trackInputParams(instrumentTrackId);
    AudioOutputParams originParams = trackOutputParams(instrumentTrackId);
    AudioResourceMeta originMeta = inParams.resourceMeta;

    bool isMetronome = notationPlayback()->metronomeTrackId() == instrumentTrackId;

    if (!inParams.isValid()) {
        if (isMetronome) {
            const SoundProfile& profile = profilesRepo()->profile(configuration()->basicSoundProfileName());
            inParams = { profile.findResource(playbackData.setupData), {} };
        } else {
            const SoundProfile& profile = profilesRepo()->profile(audioSettings()->activeSoundProfile());
            inParams = { profile.findResource(playbackData.setupData), {} };
        }
    }

    if (!isMetronome && originParams.auxSends.empty()) {
        const muse::String& instrumentSoundId = inParams.resourceMeta.attributeVal(PLAYBACK_SETUP_DATA_ATTRIBUTE);
        AudioSourceType sourceType = inParams.isValid() ? inParams.type() : AudioSourceType::Fluid;

        for (aux_channel_idx_t idx = 0; idx < AUX_CHANNEL_NUM; ++idx) {
            gain_t signalAmount = configuration()->defaultAuxSendValue(idx, sourceType, instrumentSoundId);
            originParams.auxSends.emplace_back(AuxSendParams { signalAmount, true });
        }
    }

    uint64_t playbackKey = notationPlaybackKey();

    TrackParams trackParams;
    trackParams.source = inParams;
    trackParams.fxChain = originParams.fxChain;
    trackParams.auxSends = originParams.auxSends;
    trackParams.control = trackControlParams(instrumentTrackId, originParams);

    playback()->addTrack(title, std::move(playbackData), trackParams)
    .onResolve(this, [this, title, instrumentTrackId, playbackKey, onFinished, originMeta, originParams](const TrackId trackId,
                                                                                                         const TrackParams& appliedParams) {
        //! NOTE It may be that while we were adding a track, the notation was already closed (or opened another)
        //! This situation can be if the notation was opened and immediately closed.
        if (notationPlaybackKey() != playbackKey) {
            return;
        }

        m_instrumentTrackIdMap.insert({ instrumentTrackId, trackId });

        const bool trackNewlyAdded = !audioSettings()->trackHasExistingOutputParams(instrumentTrackId);

        auto appliedOutParams = originParams;
        appliedOutParams.fxChain = appliedParams.fxChain;

        audioSettings()->setTrackInputParams(instrumentTrackId, appliedParams.source);
        audioSettings()->setTrackOutputParams(instrumentTrackId, appliedOutParams);

        updateSoloMuteStates();

        onFinished();

        m_trackAdded.send(trackId);

        if (trackNewlyAdded) {
            onTrackNewlyAdded(instrumentTrackId);
        }

        if (shouldLoadDrumset(instrumentTrackId, originMeta, appliedParams.source.resourceMeta)) {
            m_drumsetLoader.loadDrumset(m_notation, instrumentTrackId, appliedParams.source.resourceMeta);
        }

        if (muse::audio::isOnlineAudioResource(appliedParams.source.resourceMeta)) {
            m_onlineSoundsController->addOnlineTrack(trackId, appliedParams.source.resourceMeta);

            if (notationPlayback()) {
                notationPlayback()->setSendEventsOnScoreChange(instrumentTrackId, true);
            }
        }
    })
    .onReject(this, [instrumentTrackId, onFinished](int code, const std::string& msg) {
        LOGE() << "can't add a new track, code: [" << code << "] " << msg;

        onFinished();
    });

    m_loadingTrackCount++;
}

void PlaybackController::addAuxTrack(aux_channel_idx_t index, const TrackAddFinished& onFinished)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    AudioOutputParams originParams;

    if (audioSettings()->containsAuxOutputParams(index)) {
        originParams = audioSettings()->auxOutputParams(index);
    } else if (index == REVERB_CHANNEL_IDX) {
        originParams = makeReverbOutputParams();
    }

    TrackParams trackParams;
    trackParams.source = {};
    trackParams.fxChain = originParams.fxChain;
    trackParams.auxSends = originParams.auxSends;
    trackParams.control = originParams.control();

    std::string title = resolveAuxTrackTitle(index, originParams, false);
    uint64_t playbackKey = notationPlaybackKey();

    playback()->addAuxTrack(title, trackParams)
    .onResolve(this, [this, playbackKey, index, onFinished, originParams](const TrackId trackId, const TrackParams& appliedParams) {
        //! NOTE It may be that while we were adding a track, the notation was already closed (or opened another)
        //! This situation can be if the notation was opened and immediately closed.
        if (notationPlaybackKey() != playbackKey) {
            return;
        }

        m_auxTrackIdMap.insert({ index, trackId });

        auto appliedOutParams = originParams;
        appliedOutParams.fxChain = appliedParams.fxChain;

        audioSettings()->setAuxOutputParams(index, appliedOutParams);

        updateSoloMuteStates();
        onFinished();

        m_trackAdded.send(trackId);
    })
    .onReject(this, [onFinished](int code, const std::string& msg) {
        LOGE() << "can't add a new aux track, code: [" << code << "] " << msg;

        onFinished();
    });

    m_loadingTrackCount++;
}

void PlaybackController::setTrackActivity(const engraving::InstrumentTrackId& instrumentTrackId, const bool isActive)
{
    IF_ASSERT_FAILED(audioSettings() && playback()) {
        return;
    }

    AudioOutputParams outParams = audioSettings()->trackOutputParams(instrumentTrackId);
    ControlParams control = trackControlParams(instrumentTrackId, outParams, /*rebuildVolume*/ false, /*rebuildPan*/ false);

    control.muted = !isActive;

    audio::TrackId trackId = m_instrumentTrackIdMap[instrumentTrackId];
    playback()->setControlParams(trackId, control);
}

AudioOutputParams PlaybackController::trackOutputParams(const InstrumentTrackId& instrumentTrackId) const
{
    IF_ASSERT_FAILED(audioSettings() && notationConfiguration() && notationPlayback()) {
        return {};
    }

    AudioOutputParams result = audioSettings()->trackOutputParams(instrumentTrackId);

    if (instrumentTrackId == notationPlayback()->metronomeTrackId()) {
        result.muted = !notationConfiguration()->isMetronomeEnabled() && !notationConfiguration()->isCountInEnabled();
        return result;
    }

    if (notationPlayback()->isChordSymbolsTrack(instrumentTrackId)) {
        result.muted = !notationConfiguration()->isPlayChordSymbolsEnabled();
    }

    return result;
}

ControlParams PlaybackController::trackControlParams(const InstrumentTrackId& instrumentTrackId,
                                                     const AudioOutputParams& outParams,
                                                     bool rebuildVolume, bool rebuildPan)
{
    ControlParams control = outParams.control();
    const auto it = m_automatedControlParamsCache.find(instrumentTrackId);
    if (it != m_automatedControlParamsCache.end()) {
        control.volume = it->second.volume;
        control.balance = it->second.balance;
    }

    if (!rebuildVolume && !rebuildPan) {
        return control;
    }

    const INotationAutomationPtr automation = m_masterNotation ? m_masterNotation->automation() : nullptr;
    const AutomationDataConstPtr automationData = automation ? automation->automationData() : nullptr;
    if (!automationData || !instrumentTrackId.isValid()) {
        m_automatedControlParamsCache.erase(instrumentTrackId);
        return outParams.control();
    }

    auto buildAutomationEnvelope = [this](const AutomationCurve& curve) {
        muse::audio::AutomationEnvelope envelope;
        for (const auto& [tick, point] : curve) {
            envelope.insert({ playedTickToSecs(tick), point.value });
        }
        return envelope;
    };

    if (rebuildVolume) {
        const AutomationCurve& volumeCurve = automationData->curve(AutomationCurveKey::instrument(AutomationType::Volume,
                                                                                                  instrumentTrackId));
        control.volume = !volumeCurve.empty() ? AutomatableValue<volume_db_t>(buildAutomationEnvelope(volumeCurve))
                         : AutomatableValue<volume_db_t>(outParams.volume);
    }

    if (rebuildPan) {
        const AutomationCurve& panCurve = automationData->curve(AutomationCurveKey::instrument(AutomationType::Pan, instrumentTrackId));
        control.balance = !panCurve.empty() ? AutomatableValue<balance_t>(buildAutomationEnvelope(panCurve))
                          : AutomatableValue<balance_t>(outParams.balance);
    }

    m_automatedControlParamsCache[instrumentTrackId] = control;

    return control;
}

void PlaybackController::onAutomationDataChanged(const AutomationChanges& changes)
{
    if (changes.isFullReset) {
        resendAutomatedControlParams();
        return;
    }

    InstrumentTrackIdSet volumeTrackIds;
    InstrumentTrackIdSet panTrackIds;

    for (const AutomationCurveKey& key : changes.affectedKeys) {
        const std::optional<InstrumentTrackId> trackId = key.trackId();
        if (!trackId) {
            continue;
        }

        if (key.type == AutomationType::Volume) {
            volumeTrackIds.insert(*trackId);
        } else if (key.type == AutomationType::Pan) {
            panTrackIds.insert(*trackId);
        }
    }

    if (volumeTrackIds.empty() && panTrackIds.empty()) {
        return;
    }

    resendAutomatedControlParams(volumeTrackIds, panTrackIds);
}

void PlaybackController::resendAutomatedControlParams(std::optional<InstrumentTrackIdSet> volumeTrackIds,
                                                      std::optional<InstrumentTrackIdSet> panTrackIds)
{
    if (!playback()) {
        return;
    }

    for (const auto& pair : m_instrumentTrackIdMap) {
        const bool rebuildVolume = !volumeTrackIds || muse::contains(*volumeTrackIds, pair.first);
        const bool rebuildPan = !panTrackIds || muse::contains(*panTrackIds, pair.first);

        if (!rebuildVolume && !rebuildPan) {
            continue;
        }

        const AudioOutputParams outParams = trackOutputParams(pair.first);
        playback()->setControlParams(pair.second, trackControlParams(pair.first, outParams, rebuildVolume, rebuildPan));
    }
}

void PlaybackController::removeTrack(const InstrumentTrackId& instrumentTrackId)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    auto search = m_instrumentTrackIdMap.find(instrumentTrackId);

    if (search == m_instrumentTrackIdMap.end()) {
        return;
    }

    playback()->removeTrack(search->second);
    audioSettings()->removeTrackParams(instrumentTrackId);

    m_masterNotation->notation()->soloMuteState()->removeTrackSoloMuteState(instrumentTrackId);
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        if (const INotationPtr& notation = excerpt->notation()) {
            notation->soloMuteState()->removeTrackSoloMuteState(instrumentTrackId);
        }
    }

    m_onlineSoundsController->removeOnlineTrack(search->second);

    m_trackRemoved.send(search->second);
    m_instrumentTrackIdMap.erase(instrumentTrackId);
    m_automatedControlParamsCache.erase(instrumentTrackId);
}

void PlaybackController::onTrackNewlyAdded(const InstrumentTrackId& instrumentTrackId)
{
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        if (const INotationPtr& notation = excerpt->notation()) {
            if (notation == m_notation || notation->soloMuteState()->trackSoloMuteStateExists(instrumentTrackId)) {
                continue;
            }

            const Part* part = notation->parts()->part(instrumentTrackId.partId);
            const bool shouldMute = !part || !part->show();

            const INotationSoloMuteState::SoloMuteState soloMuteState = { shouldMute, /*solo*/ false };
            notation->soloMuteState()->setTrackSoloMuteState(instrumentTrackId, soloMuteState);
        }
    }
}

void PlaybackController::setupPlayback()
{
    playback()->removeAllTracks();

    m_player = playback()->player();
    globalContext()->setCurrentPlayer(m_player);

    if (!notationPlayback()) {
        return;
    }

    const AudioOutputParams& masterOutputParams = audioSettings()->masterAudioOutputParams();
    playback()->setMasterFxChainParams(masterOutputParams.fxChain);
    playback()->setMasterAuxSendsParams(masterOutputParams.auxSends);
    playback()->setMasterControlParams(masterOutputParams.control());

    subscribeOnAudioParamsChanges();
    setupTracks();
    setupPlayer();

    // Re-add the backing audio track recorded in the project, if any. Done after
    // setupTracks() so the mixer exists; the waveform lane and its peaks follow.
    restoreAudioTrack();

    m_isPlaybackInited = true;
    m_playbackInited.send(m_isPlaybackInited);
}

void PlaybackController::addAudioTrack(const muse::io::path_t& filePath, const AudioTrackAddFinished& onFinished)
{
    const std::string path = filePath.toStdString();
    const std::string name = muse::io::filename(filePath).toStdString();

    // The engine opens the file itself (it cannot be handed a decoder object across the
    // RPC boundary), so only the path travels. Errors come back as a rejected promise.
    playback()->addTrack(name, path, TrackParams {})
    .onResolve(this, [this, path, onFinished](const TrackId trackId, const TrackParams&) {
        LOGI() << "audio track added, trackId: " << trackId << ", file: " << path;
        m_audioTrackIds.push_back(trackId);
        m_audioTrackAdded.send(trackId);

        // Create the waveform lane first, so the re-layout triggered when the peaks are
        // ready already has somewhere to draw them.
        ensureAudioWaveformStaff();

        // Remember the file in the project, so reopening the score restores this track.
        rememberAudioTrack(muse::io::path_t(path));

        // Line the audio up with the score. Done here rather than by the engine because the
        // offset is stored against the score (a tick), and only the notation side knows the
        // tempo map that turns it into the seconds the source works in.
        applyAudioTrackOffset();

        // Kick off waveform preparation. This is asynchronous on purpose: decoding a
        // multi-minute file takes hundreds of milliseconds and must not block the UI.
        // When it finishes, the score is asked to lay out again so the waveform staff
        // appears (or updates).
        if (audioWaveformService()) {
            audioWaveformService()->waveformChanged().onNotify(this, [this]() {
                onWaveformChanged();
            });
            audioWaveformService()->loadWaveform(muse::io::path_t(path));
        }

        if (onFinished) {
            onFinished(true);
        }
    })
    .onReject(this, [path, onFinished](int code, const std::string& msg) {
        LOGE() << "failed to add audio track for file: " << path
               << ", code: " << code << ", error: " << msg;
        if (onFinished) {
            onFinished(false);
        }
    });
}

Ret PlaybackController::importAudioTrack()
{
    LOGI() << "audiotrack: import requested, opening file dialog";

    const muse::io::path_t path = interactive()->selectOpeningFileSync(
        muse::trc("playback", "Import audio"), "", audioFileFilter());

    if (path.empty()) {
        LOGI() << "audiotrack: import cancelled by user";
        return muse::make_ret(Ret::Code::Cancel);   // user cancelled; not an error
    }

    return addAudioTrackFromPath(path);
}

std::vector<std::string> PlaybackController::audioFileFilter()
{
    // Only formats the audiotrack module can actually decode. libsndfile in this build has
    // ENABLE_MPEG=OFF, so MP3 is deliberately absent rather than offered and then failing.
    return {
        muse::trc("playback", "Audio files") + " (*.wav *.wave *.flac *.ogg *.oga *.opus *.aiff *.aif *.w64 *.caf)",
        muse::trc("playback", "WAV") + " (*.wav *.wave)",
        muse::trc("playback", "FLAC") + " (*.flac)",
        muse::trc("playback", "Ogg Vorbis") + " (*.ogg *.oga)",
        muse::trc("playback", "Opus") + " (*.opus)",
        muse::trc("playback", "All") + " (*)"
    };
}

void PlaybackController::rememberAudioTrack(const muse::io::path_t& filePath)
{
    if (!audioSettings()) {
        return;
    }

    // Read-modify-write rather than starting from a fresh struct: the alignment offset is
    // set separately from the file, and rebuilding the params here would silently reset it.
    project::AudioTrackParams params = audioSettings()->audioTrackParams();
    params.filePath = filePath;
    // io::filename() yields a path_t; the params field is a muse::String.
    params.name = muse::io::filename(filePath).toString();
    audioSettings()->setAudioTrackParams(params);

    LOGI() << "audiotrack: remembered audio track in project: " << filePath;
}

double PlaybackController::audioTrackOffsetSeconds() const
{
    if (!audioSettings()) {
        return 0.0;
    }

    const int tickOffset = audioSettings()->audioTrackParams().tickOffset;
    if (tickOffset == 0) {
        return 0.0;
    }

    // The offset is stored against the score, so it survives a tempo change: it means "the
    // audio's beginning lands here in the music", not "delay the audio by this many seconds".
    const engraving::Score* score = m_notation ? m_notation->score() : nullptr;
    if (!score) {
        return 0.0;
    }

    // A negative tick offset (the file carries material belonging before the score's start)
    // is expressed by the sign of the tick itself, which utick2utime handles.
    return score->utick2utime(tickOffset);
}

void PlaybackController::applyAudioTrackOffset()
{
    if (m_audioTrackIds.empty()) {
        return;
    }

    const double seconds = audioTrackOffsetSeconds();

    // The source is reachable only through the provider that created it -- see
    // AudioFileSourceProvider::createSource for why. Adjusting it takes effect at once, so
    // this doubles as the live path used while the user dials the offset in by ear.
    // Resolved as the engine interface and cast down. Registering the concrete class as well
    // would be simpler here, but the IoC container warns about the same object under two
    // types, and a noisy startup log is a cost paid by every run.
    std::shared_ptr<muse::audiotrack::AudioFileSourceProvider> provider
        = std::dynamic_pointer_cast<muse::audiotrack::AudioFileSourceProvider>(audioFileSourceProvider());
    if (!provider) {
        return;
    }

    muse::audiotrack::AudioTrackSourcePtr source = provider->lastCreatedSource();
    if (!source) {
        LOGW() << "audiotrack: audio source is gone, cannot apply offset";
        return;
    }

    source->setStartOffsetSeconds(seconds);
    LOGI() << "audiotrack: alignment offset set to " << seconds << " s";
}

void PlaybackController::setAudioTrackOffset(int tickOffset)
{
    if (!audioSettings()) {
        return;
    }

    project::AudioTrackParams params = audioSettings()->audioTrackParams();
    if (params.tickOffset == tickOffset) {
        return;
    }

    params.tickOffset = tickOffset;
    audioSettings()->setAudioTrackParams(params);

    applyAudioTrackOffset();
}

void PlaybackController::shiftAudioTrackOffset(int tickDelta)
{
    if (!audioSettings() || tickDelta == 0) {
        return;
    }

    setAudioTrackOffset(audioSettings()->audioTrackParams().tickOffset + tickDelta);
}

void PlaybackController::restoreAudioTrack()
{
    if (!audioSettings()) {
        return;
    }

    const project::AudioTrackParams params = audioSettings()->audioTrackParams();
    if (!params.isValid()) {
        return;
    }

    // The file may have moved or been deleted since the project was saved. Check before
    // trying to load it, so a missing file produces one clear warning instead of a decode
    // failure whose message is harder to act on.
    if (!muse::io::FileInfo::exists(params.filePath)) {
        LOGW() << "audiotrack: saved audio file is missing, skipping: " << params.filePath;
        return;
    }

    LOGI() << "audiotrack: restoring audio track from project: " << params.filePath;

    // Re-add the engine track and re-decode the waveform. addAudioTrack() also re-creates
    // the waveform lane if it is missing, which is what makes a reopened project show its
    // waveform without the user importing anything again.
    addAudioTrack(params.filePath, nullptr);
}

void PlaybackController::ensureAudioWaveformStaff()
{
    const notation::INotationPartsPtr parts = masterNotationParts();
    if (!parts) {
        return;
    }

    // One lane per score: importing a second file replaces the audio, it does not stack
    // lanes. (Multiple simultaneous backing tracks would need a per-track lane mapping,
    // which the first version deliberately does not attempt.)
    if (parts->hasAudioWaveformStaff()) {
        return;
    }

    const muse::ID partId = parts->appendAudioWaveformStaff();
    if (partId.isValid()) {
        LOGI() << "audiotrack: waveform staff created";
    } else {
        LOGE() << "audiotrack: failed to create waveform staff";
    }
}

bool PlaybackController::isAudioTrackPart(const Part* part)
{
    if (!part) {
        return false;
    }

    for (const Staff* staff : part->staves()) {
        if (staff && staff->isWaveformStaff(Fraction(0, 1))) {
            return true;
        }
    }

    return false;
}

void PlaybackController::onPartAdded(const Part* part)
{
    if (!isAudioTrackPart(part)) {
        return;
    }

    // The audio track is added through the instrument dialog, exactly like an instrument, so
    // at this point the lane exists but nothing has said which file it should play. Ask.
    //
    // Skipped while playback is still being set up: restoring a saved project also adds the
    // lane (setupPlayback calls restoreAudioTrack), and that path already knows the file.
    if (!m_isPlaybackInited) {
        LOGI() << "audiotrack: waveform lane added during setup, not prompting";
        return;
    }

    // An audio track is already playing (for example the user added the instrument a second
    // time): leave it alone rather than replacing what they are listening to.
    if (!m_audioTrackIds.empty()) {
        LOGI() << "audiotrack: waveform lane added but a track is already loaded, not prompting";
        return;
    }

    LOGI() << "audiotrack: waveform lane added, asking for the audio file";

    const muse::ID partId = part->id();

    // Deferred: adding a part happens inside the instrument dialog's own handling, and
    // opening a modal file dialog from within it would stack two dialogs in one event.
    QMetaObject::invokeMethod(qApp, [this, partId]() {
        const muse::Ret ret = importAudioTrack();

        if (ret.code() == static_cast<int>(muse::Ret::Code::Cancel)) {
            // The user backed out of the file chooser. The lane was created by the
            // instrument dialog before we could ask, and an audio track with no audio is
            // not something they can finish later, so drop it again: the score goes back to
            // how it was and adding the Audio track instrument can simply be repeated.
            LOGI() << "audiotrack: file selection cancelled, removing the empty lane";
            removeAudioTrackPart(partId);
        }
    }, Qt::QueuedConnection);
}

void PlaybackController::removeAudioTrackPart(const muse::ID& partId)
{
    const notation::INotationPartsPtr parts = masterNotationParts();
    if (!parts || !partId.isValid() || !parts->partExists(partId)) {
        return;
    }

    parts->removeParts(muse::IDList { partId });
}

void PlaybackController::onWaveformChanged()
{
    // The notification arrives on the worker thread that finished decoding; score layout
    // and painting must happen on the main thread, so hop over before touching anything.
    QMetaObject::invokeMethod(qApp, [this]() {
        // currentMasterNotation() hands back a shared_ptr, not a raw pointer.
        const notation::IMasterNotationPtr notation = globalContext()->currentMasterNotation();
        engraving::MasterScore* score = notation ? notation->masterScore() : nullptr;
        if (!score) {
            return;
        }

        // Force a re-layout so the waveform staff picks up the new peaks. Scoped to this
        // score: other open scores are unaffected.
        LOGI() << "audiotrack: waveform ready, relayouting score";
        score->setLayoutAll();
        score->update();
    }, Qt::QueuedConnection);
}

Ret PlaybackController::addAudioTrackFromPath(const muse::io::path_t& path)
{
    if (!m_isPlaybackInited) {
        return make_ret(Ret::Code::InternalError, std::string("playback not initialized"));
    }

    if (path.empty()) {
        return muse::make_ret(Ret::Code::Cancel);
    }

    LOGI() << "audio track import requested: " << path;

    // Decoding happens on the engine side, so the outcome is asynchronous. Tell the user
    // when the file could not be used instead of leaving a silently missing track.
    addAudioTrack(path, [this, path](bool success) {
        if (success) {
            return;
        }

        std::string text = muse::trc("playback", "Could not read this audio file:");
        text += "\n" + path.toStdString() + "\n\n";
        text += muse::trc("playback", "Supported formats: WAV, FLAC, Ogg Vorbis, Opus.");

        interactive()->error(muse::trc("playback", "Import audio"), text);
    });

    return muse::make_ok();
}

const std::vector<TrackId>& PlaybackController::audioTrackIds() const
{
    return m_audioTrackIds;
}

async::Channel<TrackId> PlaybackController::audioTrackAdded() const
{
    return m_audioTrackAdded;
}

async::Channel<TrackId> PlaybackController::audioTrackRemoved() const
{
    return m_audioTrackRemoved;
}

void PlaybackController::subscribeOnAudioParamsChanges()
{
    playback()->masterFxChainParamsChanged().onReceive(this, [this](const AudioFxChain& params) {
        AudioOutputParams outParams = audioSettings()->masterAudioOutputParams();
        outParams.fxChain = params;
        audioSettings()->setMasterAudioOutputParams(outParams);
    });

    playback()->sourceParamsChanged().onReceive(this, [this](const TrackId trackId, const AudioInputParams& params) {
        auto search = std::find_if(m_instrumentTrackIdMap.begin(), m_instrumentTrackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (search != m_instrumentTrackIdMap.end()) {
            const AudioResourceMeta& oldMeta = audioSettings()->trackInputParams(search->first).resourceMeta;
            onAudioResourceChanged(trackId, search->first, oldMeta, params.resourceMeta);

            audioSettings()->setTrackInputParams(search->first, params);
        }
    });

    playback()->fxChainParamsChanged().onReceive(this, [this](const TrackId trackId, const AudioFxChain& params) {
        auto instrumentIt = std::find_if(m_instrumentTrackIdMap.begin(), m_instrumentTrackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (instrumentIt != m_instrumentTrackIdMap.end()) {
            AudioOutputParams outParams = audioSettings()->trackOutputParams(instrumentIt->first);
            outParams.fxChain = params;
            audioSettings()->setTrackOutputParams(instrumentIt->first, outParams);
            return;
        }

        auto auxIt = std::find_if(m_auxTrackIdMap.begin(), m_auxTrackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (auxIt != m_auxTrackIdMap.end()) {
            aux_channel_idx_t auxIdx = auxIt->first;
            std::string oldName = resolveAuxTrackTitle(auxIdx, audioSettings()->auxOutputParams(auxIdx));
            AudioOutputParams outParams = audioSettings()->auxOutputParams(auxIdx);
            outParams.fxChain = params;
            std::string newName = resolveAuxTrackTitle(auxIdx, outParams);

            audioSettings()->setAuxOutputParams(auxIdx, outParams);

            if (oldName != newName) {
                m_auxChannelNameChanged.send(auxIdx, newName);
            }
        }
    });
}

void PlaybackController::setupTracks()
{
    m_instrumentTrackIdMap.clear();

    if (!masterNotationParts()) {
        return;
    }

    m_loadingTrackCount = 0;

    InstrumentTrackIdSet trackIdSet = notationPlayback()->existingTrackIdSet();
    size_t trackCount = trackIdSet.size() + AUX_CHANNEL_NUM;
    std::string title = muse::trc("playback", "Loading audio samples");

    auto onAddFinished = [this, trackCount, title]() {
        m_loadingTrackCount--;

        size_t current = trackCount - m_loadingTrackCount;
        m_loadingProgress.progress(current, trackCount, title);

        if (m_loadingTrackCount == 0) {
            m_loadingProgress.finish(muse::make_ok());
            m_isPlayAllowedChanged.send(isPlayAllowed());
        }
    };

    for (const InstrumentTrackId& trackId : trackIdSet) {
        addTrack(trackId, onAddFinished);
    }

    for (aux_channel_idx_t idx = 0; idx < AUX_CHANNEL_NUM; ++idx) {
        addAuxTrack(idx, onAddFinished);
    }

    m_loadingProgress.progress(0, trackCount, title);

    notationPlayback()->trackAdded().onReceive(this, [this, onAddFinished](const InstrumentTrackId& instrumentTrackId) {
        addTrack(instrumentTrackId, onAddFinished);
    });

    notationPlayback()->trackRemoved().onReceive(this, [this](const InstrumentTrackId& instrumentTrackId) {
        removeTrack(instrumentTrackId);
    });

    NotifyList<const Part*> partList = masterNotationParts()->partList();

    //! HACK - ideally we would use "this" (PlaybackController) instead of m_seqAsyncReceiver for the following
    //! subscription, but we've already subscribed to onItemChanged for a different reason in setNotation...
    partList.onItemChanged(&m_seqAsyncReceiver, [this](const Part*) {
        updateSoloMuteStates();
    });

    audioSettings()->auxSoloMuteStateChanged().onReceive(
        this, [this](aux_channel_idx_t, const notation::INotationSoloMuteState::SoloMuteState&) {
        updateSoloMuteStates();
    });

    m_isPlayAllowedChanged.send(isPlayAllowed());
}

void PlaybackController::setupPlayer()
{
    currentPlayer()->playbackPositionChanged().onReceive(this, [this](const audio::secs_t pos) {
        const midi::tick_t previousTick = m_currentTick;
        m_currentTick = notationPlayback()->secToTick(pos);

        jianpuPositionTrace(QStringLiteral("player pos=%1 tick=%2 (was %3) status=%4")
                            .arg(QString::number(static_cast<double>(pos), 'f', 6)).arg(m_currentTick).arg(previousTick)
                            .arg(static_cast<int>(currentPlayer()->playbackStatus())));

        updateCurrentTempo();

        secs_t endSecs = totalPlayTime();
        if (pos + muse::msecs_to_secs(1) >= endSecs) {
            doStop();
        }
    });

    currentPlayer()->playbackStatusChanged().onReceive(this, [this](PlaybackStatus) {
        onPlaybackStatusChanged();
    });

    currentPlayer()->setDuration(notationPlayback()->totalPlayTime());

    notationPlayback()->totalPlayTimeChanged().onReceive(this, [this](const audio::secs_t totalPlaybackTime) {
        currentPlayer()->setDuration(totalPlaybackTime);
        m_totalPlayTimeChanged.notify();
    });
}

void PlaybackController::updateSoloMuteStates()
{
    if (!audioSettings() || !playback() || !m_notation) {
        return;
    }

    TRACEFUNC;

    InstrumentTrackIdSet existingTrackIdSet = notationPlayback()->existingTrackIdSet();
    bool hasSolo = false;

    for (const InstrumentTrackId& instrumentTrackId : existingTrackIdSet) {
        if (instrumentTrackId == notationPlayback()->metronomeTrackId()) {
            continue;
        }
        if (m_notation->soloMuteState()->trackSoloMuteState(instrumentTrackId).solo) {
            hasSolo = true;
            break;
        }
    }

    InstrumentTrackIdSet allowedInstrumentTrackIdSet = instrumentTrackIdSetForRangePlayback();
    bool isRangePlaybackMode = !m_isExportingAudio && selection()->isRange() && !allowedInstrumentTrackIdSet.empty();

    for (const InstrumentTrackId& instrumentTrackId : existingTrackIdSet) {
        if (!muse::contains(m_instrumentTrackIdMap, instrumentTrackId)) {
            continue;
        }

        if (instrumentTrackId == notationPlayback()->metronomeTrackId()) {
            continue;
        }

        // 1. Recall the solo-mute state for this notation
        const auto& soloMuteState = m_notation->soloMuteState()->trackSoloMuteState(instrumentTrackId);

        // 2. Evaluate "force mute" (disabling the mute button)
        bool shouldForceMute = hasSolo && !soloMuteState.solo;
        if (notationPlayback()->isChordSymbolsTrack(instrumentTrackId) && !shouldForceMute) {
            shouldForceMute = !notationConfiguration()->isPlayChordSymbolsEnabled();
        }

        if (isRangePlaybackMode && !shouldForceMute) {
            shouldForceMute = !muse::contains(allowedInstrumentTrackIdSet, instrumentTrackId);
        }

        // 3. Update params for playback / mixer
        AudioOutputParams params = trackOutputParams(instrumentTrackId);
        params.solo = soloMuteState.solo;
        params.muted = soloMuteState.mute || shouldForceMute;
        params.forceMute = shouldForceMute;

        audio::TrackId trackId = m_instrumentTrackIdMap.at(instrumentTrackId);
        playback()->setControlParams(trackId, trackControlParams(instrumentTrackId, params, /*rebuildVolume*/ false, /*rebuildPan*/ false));
    }

    updateAuxMuteStates();
}

void PlaybackController::updateAuxMuteStates()
{
    for (const auto& pair : m_auxTrackIdMap) {
        auto soloMuteState = audioSettings()->auxSoloMuteState(pair.first);

        AudioOutputParams params = audioSettings()->auxOutputParams(pair.first);
        if (params.muted == soloMuteState.mute) {
            continue;
        }

        params.muted = soloMuteState.mute;
        playback()->setControlParams(pair.second, params.control());
    }
}

secs_t PlaybackController::totalPlayTime() const
{
    auto np = notationPlayback();
    return np ? np->totalPlayTime() : secs_t { 0.0 };
}

Notification PlaybackController::totalPlayTimeChanged() const
{
    return m_totalPlayTimeChanged;
}

const Tempo& PlaybackController::currentTempo() const
{
    return m_currentTempo;
}

Notification PlaybackController::currentTempoChanged() const
{
    return m_currentTempoChanged;
}

MeasureBeat PlaybackController::currentBeat() const
{
    if (!notationPlayback()) {
        return MeasureBeat();
    }

    MeasureBeat beat = notationPlayback()->beat(m_currentTick);

    // The playback model always belongs to the master score, but the measure number shown in the
    // toolbar has to be the one printed in the notation that is displayed: a part numbers its own
    // measures, and it can differ from the master (a part's measure list can have diverged from the
    // master's). Then the master's number would match neither the measure the user sees on screen
    // nor the number in the status bar, which reads the displayed score as well.
    const Score* displayedScore = m_notation ? m_notation->score() : nullptr;
    if (displayedScore) {
        const MeasureBeat displayedBeat = findBeat(displayedScore, m_currentTick);
        beat.measureNumber = displayedBeat.measureNumber;
        beat.maxMeasureNumber = displayedBeat.maxMeasureNumber;
    }

    {
        static int64_t lastLoggedTick = -1;
        if (lastLoggedTick != static_cast<int64_t>(m_currentTick)) {
            lastLoggedTick = static_cast<int64_t>(m_currentTick);
            const Score* masterScore = m_masterNotation ? m_masterNotation->masterScore() : nullptr;
            const Measure* measure = masterScore ? masterScore->tick2measure(Fraction::fromTicks(m_currentTick)) : nullptr;
            const int shownBeat = static_cast<int>(beat.beat) + 1;
            jianpuPositionTrace(QStringLiteral("currentBeat tick=%1 barIndex=%2 measureNumber=%3 measureTick=%4 measureEndTick=%5 "
                                               "nMeasures=%6 src=%7 shownMeasure=%8 shownBeat=%9 beat=%10 beatIndex=%11 "
                                               "maxBeat=%12 selectionStart=%13")
                                .arg(m_currentTick)
                                .arg(beat.measureIndex)
                                .arg(measure ? measure->measureNumber() : -1)
                                .arg(measure ? measure->tick().ticks() : -1)
                                .arg(measure ? measure->endTick().ticks() : -1)
                                .arg(masterScore ? static_cast<int>(const_cast<Score*>(masterScore)->measures()->size()) : -1)
                                .arg(displayedScore == masterScore ? QStringLiteral("master") : QStringLiteral("other"))
                                .arg(beat.measureNumber)
                                .arg(shownBeat)
                                .arg(QString::number(static_cast<double>(beat.beat), 'f', 3))
                                .arg(static_cast<int>(beat.beat))
                                .arg(beat.maxBeatIndex + 1)
                                .arg(selection() && selection()->isRange() ? selectionRange()->startTick().ticks() : -1));
        }
    }

    return beat;
}

secs_t PlaybackController::beatToSecs(int measureIndex, int beatIndex) const
{
    if (!notationPlayback()) {
        return 0;
    }

    muse::midi::tick_t rawTick = notationPlayback()->beatToRawTick(measureIndex, beatIndex);
    muse::midi::tick_t playedTick = notationPlayback()->playPositionTickByRawTick(rawTick).val;

    return playedTickToSecs(playedTick);
}

double PlaybackController::tempoMultiplier() const
{
    return notationPlayback() ? notationPlayback()->tempoMultiplier() : 1.0;
}

void PlaybackController::setTempoMultiplier(double multiplier)
{
    if (!notationPlayback()) {
        return;
    }

    tick_t tick = m_currentTick;
    bool playing = isPlaying();

    if (playing) {
        doPause();
    }

    notationPlayback()->setTempoMultiplier(multiplier);
    seekRawTick(tick);
    updateLoop();

    if (playing) {
        doResume();
    }
}

muse::Progress PlaybackController::loadingProgress() const
{
    return m_loadingProgress;
}

void PlaybackController::applyProfile(const SoundProfileName& profileName)
{
    project::IProjectAudioSettingsPtr audioSettingsPtr = audioSettings();

    IF_ASSERT_FAILED(audioSettingsPtr) {
        return;
    }

    const SoundProfile& profile = profilesRepo()->profile(profileName);
    if (!profile.isValid()) {
        return;
    }

    notationPlayback()->removeSoundFlags(notationPlayback()->existingTrackIdSet());

    const InstrumentTrackId& metronomeTrackId = notationPlayback()->metronomeTrackId();

    for (const auto& pair : m_instrumentTrackIdMap) {
        if (pair.first == metronomeTrackId) {
            continue;
        }

        const mpe::PlaybackData& playbackData = notationPlayback()->trackPlaybackData(pair.first);
        AudioSourceParams newSourceParams { profile.findResource(playbackData.setupData), {} };

        playback()->setSourceParams(pair.second, newSourceParams);
    }

    audioSettingsPtr->setActiveSoundProfile(profileName);
}

void PlaybackController::setNotation(notation::INotationPtr notation)
{
    if (m_notation == notation) {
        return;
    }

    if (m_notation) {
        INotationPartsPtr notationParts = m_notation->parts();
        NotifyList<const Part*> partList = notationParts->partList();
        partList.disconnect(this);

        m_notation->interaction()->selectionChanged().disconnect(this);
        m_notation->interaction()->textEditingEnded().disconnect(this);
        m_notation->soloMuteState()->trackSoloMuteStateChanged().disconnect(this);
    }

    m_notation = notation;

    m_isPlayAllowedChanged.send(isPlayAllowed());

    if (!m_notation) {
        setMasterNotation(nullptr);
        return;
    }

    setMasterNotation(m_notation->masterNotation());

    if (!m_notation->hasVisibleParts()) {
        doPause();
    }

    updateSoloMuteStates();

    NotifyList<const Part*> partList = m_notation->parts()->partList();

    partList.onItemAdded(this, [this](const Part* part) {
        onPartChanged(part);
        onPartAdded(part);
    });

    partList.onItemChanged(this, [this](const Part* part) {
        onPartChanged(part);
    });

    m_notation->interaction()->selectionChanged().onNotify(this, [this]() {
        onSelectionChanged();
    });

    m_notation->interaction()->textEditingEnded().onReceive(this, [this](engraving::TextBase* text) {
        if (text && text->isHarmony()) {
            playElements({ text });
        }
    });

    m_notation->soloMuteState()->trackSoloMuteStateChanged().onReceive(
        this, [this](const InstrumentTrackId&, const notation::INotationSoloMuteState::SoloMuteState&) {
        updateSoloMuteStates();
    });
}

void PlaybackController::setMasterNotation(notation::IMasterNotationPtr masterNotation)
{
    if (m_masterNotation == masterNotation) {
        return;
    }

    if (m_masterNotation) {
        m_masterNotation->hasPartsChanged().disconnect(this);
        m_masterNotation->playback()->loopBoundariesChanged().disconnect(this);
        m_masterNotation->playback()->loopEnabledChanged().disconnect(this);

        if (const AutomationDataConstPtr automationData = m_masterNotation->automation()->automationData()) {
            automationData->changed().disconnect(this);
        }
    }

    m_masterNotation = masterNotation;

    m_totalPlayTimeChanged.notify();

    if (!m_masterNotation) {
        return;
    }

    m_masterNotation->hasPartsChanged().onNotify(this, [this]() {
        m_isPlayAllowedChanged.send(isPlayAllowed());
    });

    m_masterNotation->playback()->loopBoundariesChanged().onNotify(this, [this]() {
        updateLoop();
    });

    m_masterNotation->playback()->loopEnabledChanged().onReceive(this, [this](bool value) {
        m_loopEnabledChanged.send(value);
    });

    if (const AutomationDataConstPtr automationData = m_masterNotation->automation()->automationData()) {
        automationData->changed().onReceive(this, [this](const AutomationChanges& changes) {
            onAutomationDataChanged(changes);
        });
    }
}

void PlaybackController::setIsExportingAudio(bool exporting)
{
    if (m_isExportingAudio == exporting) {
        return;
    }

    m_isExportingAudio = exporting;
    updateSoloMuteStates();

    if (exporting && notationPlayback()) {
        notationPlayback()->sendEventsForChangedTracks();
    }
}

bool PlaybackController::canReceiveAction(const muse::actions::ActionCode&) const
{
    if (!m_masterNotation || !m_masterNotation->hasParts()) {
        return false;
    }

    return true;
}

const std::map<TrackId, AudioResourceMeta>& PlaybackController::onlineSounds() const
{
    return m_onlineSoundsController->onlineSounds();
}

muse::async::Notification PlaybackController::onlineSoundsChanged() const
{
    return m_onlineSoundsController->onlineSoundsChanged();
}

muse::Progress PlaybackController::onlineSoundsProcessingProgress() const
{
    return m_onlineSoundsController->onlineSoundsProcessingProgress();
}

muse::audio::secs_t PlaybackController::playedTickToSecs(int tick) const
{
    return secs_t(notationPlayback()->playedTickToSec(tick));
}
