/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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
#include "notationplayback.h"

#include <cmath>

#include "engraving/dom/chordrest.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/linkedobjects.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/page.h"
#include "engraving/dom/part.h"
#include "engraving/dom/repeatlist.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/soundflag.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/tempotimeline.h"
#include "engraving/dom/utils.h"

#include "notationerrors.h"

#include "log.h"

using namespace mu;
using namespace mu::notation;
using namespace mu::engraving;
using namespace muse;
using namespace muse::midi;
using namespace muse::async;

static constexpr double PLAYBACK_TAIL_SECS = 3;

NotationPlayback::NotationPlayback(IGetScore* getScore,
                                   muse::async::Channel<muse::RectF> notationChanged,
                                   const modularity::ContextPtr& iocCtx)
    : muse::Contextable(iocCtx), m_getScore(getScore), m_notationChanged(notationChanged), m_playbackModel(iocCtx)
{
    m_notationChanged.onReceive(this, [this](const muse::RectF&) {
        updateLoopBoundaries();
    });
}

mu::engraving::Score* NotationPlayback::score() const
{
    return m_getScore->score();
}

void NotationPlayback::init()
{
    IF_ASSERT_FAILED(score()) {
        return;
    }

    m_playbackModel.setPlayRepeats(configuration()->isPlayRepeatsEnabled());
    m_playbackModel.setPlayChordSymbols(configuration()->isPlayChordSymbolsEnabled());
    m_playbackModel.setUseScoreDynamicsForOffstreamPlayback(configuration()->playPreviewNotesWithScoreDynamics());
    m_playbackModel.setIsMetronomeEnabled(configuration()->isMetronomeEnabled());

    m_playbackModel.load(score());

    updateTotalPlayTime();
    updateLoopExpansion();
    m_playbackModel.tracksDataChanged().onReceive(this, [this](const InstrumentTrackIdSet&) {
        updateTotalPlayTime();
    });

    configuration()->isPlayRepeatsChanged().onNotify(this, [this]() {
        bool expandRepeats = configuration()->isPlayRepeatsEnabled();
        if (expandRepeats != m_playbackModel.isPlayRepeatsEnabled()) {
            m_playbackModel.setPlayRepeats(expandRepeats);

            //! NOTE: [our addition] the loop is only expanded on the repeat-expanded timeline, so
            //! this both applies and drops the expansion, and tells the player to loop again
            updateLoopExpansion();
            m_playbackModel.reload();
            m_loopBoundariesChanged.notify();
        }
    });

    configuration()->isPlayChordSymbolsChanged().onNotify(this, [this]() {
        bool playChordSymbols = configuration()->isPlayChordSymbolsEnabled();
        if (playChordSymbols != m_playbackModel.isPlayChordSymbolsEnabled()) {
            m_playbackModel.setPlayChordSymbols(playChordSymbols);
            m_playbackModel.reload();
        }
    });

    configuration()->playPreviewNotesWithScoreDynamicsChanged().onNotify(this, [this]() {
        bool useScoreDynamics = configuration()->playPreviewNotesWithScoreDynamics();
        if (useScoreDynamics != m_playbackModel.useScoreDynamicsForOffstreamPlayback()) {
            m_playbackModel.setUseScoreDynamicsForOffstreamPlayback(useScoreDynamics);
        }
    });

    configuration()->isMetronomeEnabledChanged().onNotify(this, [this]() {
        bool metronomeEnabled = configuration()->isMetronomeEnabled();
        if (metronomeEnabled != m_playbackModel.isMetronomeEnabled()) {
            m_playbackModel.setIsMetronomeEnabled(metronomeEnabled);
        }
    });

    score()->loopBoundaryTickChanged().onReceive(this, [this](LoopBoundaryType, unsigned) {
        updateLoopBoundaries();
    });
}

void NotationPlayback::reload()
{
    m_playbackModel.reload();
}

void NotationPlayback::setSendEventsOnScoreChange(const InstrumentTrackId& trackId, bool send)
{
    m_playbackModel.setSendEventsOnScoreChange(trackId, send);
}

void NotationPlayback::sendEventsForChangedTracks()
{
    m_playbackModel.sendEventsForChangedTracks();
}

muse::async::Channel<InstrumentTrackIdSet> NotationPlayback::tracksDataChanged() const
{
    return m_playbackModel.tracksDataChanged();
}

const engraving::InstrumentTrackId& NotationPlayback::metronomeTrackId() const
{
    return m_playbackModel.metronomeTrackId();
}

engraving::InstrumentTrackId NotationPlayback::chordSymbolsTrackId(const ID& partId) const
{
    return m_playbackModel.chordSymbolsTrackId(partId);
}

bool NotationPlayback::isChordSymbolsTrack(const engraving::InstrumentTrackId& trackId) const
{
    return m_playbackModel.isChordSymbolsTrack(trackId);
}

const muse::mpe::PlaybackData& NotationPlayback::trackPlaybackData(const engraving::InstrumentTrackId& trackId) const
{
    return m_playbackModel.resolveTrackPlaybackData(trackId);
}

void NotationPlayback::triggerEventsForItems(const std::vector<const EngravingItem*>& items, muse::mpe::duration_t duration,
                                             bool flushSound)
{
    m_playbackModel.triggerEventsForItems(items, duration, flushSound);
}

void NotationPlayback::triggerMetronome(muse::midi::tick_t tick)
{
    m_playbackModel.triggerMetronome(tick);
}

void NotationPlayback::triggerCountIn(muse::midi::tick_t tick, muse::secs_t& countInDuration)
{
    muse::mpe::duration_t durationInMicrosecs = 0;
    m_playbackModel.triggerCountIn(tick, durationInMicrosecs);
    countInDuration = muse::usecs_to_secs(durationInMicrosecs);
}

void NotationPlayback::triggerControllers(const muse::mpe::ControllerChangeEventList& list, staff_idx_t staffIdx, int tick)
{
    if (list.empty()) {
        return;
    }

    const Staff* staff = score()->staff(staffIdx);
    if (!staff) {
        return;
    }

    const Part* part = staff->part();
    const InstrumentTrackId trackId {
        part->id(),
        part->instrumentId(Fraction::fromTicks(tick))
    };

    const mpe::PlaybackEventsMap events {
        { 0, mpe::PlaybackEventList(list.begin(), list.end()) }
    };

    mpe::PlaybackData& data = m_playbackModel.resolveTrackPlaybackData(trackId);
    data.offStream.send(events, false /*flushOffstream*/);
}

InstrumentTrackIdSet NotationPlayback::existingTrackIdSet() const
{
    return m_playbackModel.existingTrackIdSet();
}

muse::async::Channel<InstrumentTrackId> NotationPlayback::trackAdded() const
{
    return m_playbackModel.trackAdded();
}

muse::async::Channel<InstrumentTrackId> NotationPlayback::trackRemoved() const
{
    return m_playbackModel.trackRemoved();
}

void NotationPlayback::updateLoopBoundaries()
{
    LoopBoundaries newBoundaries;
    newBoundaries.loopInTick = score()->loopInTick();
    newBoundaries.loopOutTick = score()->loopOutTick();
    newBoundaries.enabled = m_loopBoundaries.enabled;

    if (m_loopBoundaries != newBoundaries) {
        m_loopBoundaries = newBoundaries;

        //! NOTE: [our addition] applied *before* anyone reacts to the change: whoever handles the
        //! notification (the player, deciding whether it has to loop) has to see the new state,
        //! otherwise a new loop range would be handed to the player while the old expansion is
        //! still in the timeline - and the loop would happen twice.
        updateLoopExpansion();

        m_loopBoundariesChanged.notify();
        return;
    }

    //! NOTE: [our addition] recomputed on every score change: the boundaries themselves stay the
    //! same in raw ticks, but the uticks they map to move when measures are inserted or removed.
    //! It does nothing (and does not rebuild anything) when the expansion is unchanged.
    updateLoopExpansion();
}

//! NOTE: [our addition] The loop is played the way the native repeats are played: the region is
//! laid out on the playback timeline several times in place, so the player never has to seek back
//! (no flush, no audible seam, and the loop is exactly as long as the notation says).
//! See engraving/playback/playbackloopexpansion.h
void NotationPlayback::updateLoopExpansion()
{
    engraving::Score* sc = score();
    if (!sc) {
        return;
    }

    PlaybackLoopExpansion expansion;

    if (m_loopBoundaries.enabled && !m_loopBoundaries.isNull()) {
        //! NOTE: only the repeat-expanded timeline can carry the loop passes; with the repeats
        //! flattened the loop is still looped by the player itself
        if (m_playbackModel.isPlayRepeatsEnabled()) {
            const RepeatList& repeats = sc->repeatList(true);

            const int loopInRawTick = m_loopBoundaries.loopInTick.ticks();
            const int loopOutRawTick = m_loopBoundaries.loopOutTick.ticks();

            const int loopInUtick = repeats.tick2utick(loopInRawTick);
            const int loopOutUtick = repeats.tick2utick(loopOutRawTick);

            if (loopOutUtick > loopInUtick) {
                const TempoTimeline& base = sc->tempoTimeline(true);
                const double loopSeconds = base.utick2utime(loopOutUtick) - base.utick2utime(loopInUtick);

                expansion = makePlaybackLoopExpansion(repeats, loopInRawTick, loopOutRawTick, loopSeconds, true);
            }
        }
    }

    //! NOTE: compared against what this notation applied rather than against the score, because the
    //! score is shared: a notation that does not drive the loop playback must not clear it
    if (expansion == m_loopExpansion) {
        return;
    }

    m_loopExpansion = expansion;
    sc->setPlaybackLoopExpansion(m_loopExpansion);

    m_playbackModel.reload();
    updateTotalPlayTime();
}

const PlaybackLoopExpansion& NotationPlayback::loopExpansion() const
{
    const mu::engraving::Score* sc = score();
    if (!sc) {
        static const PlaybackLoopExpansion empty;
        return empty;
    }

    return sc->playbackLoopExpansion();
}

void NotationPlayback::updateTotalPlayTime()
{
    const mu::engraving::Score* score = m_getScore->score();
    if (!score) {
        return;
    }

    const bool expandRepeats = m_playbackModel.isPlayRepeatsEnabled();
    const PlaybackLoopExpansion& expansion = loopExpansion();
    const int lastTick = expansion.isActive() ? expansion.expandedTicks() : score->repeatList(expandRepeats).ticks();
    const TempoTimeline& timeline = score->tempoTimeline(expandRepeats);
    audio::secs_t newPlayTime = timeline.utick2utime(lastTick);
    newPlayTime += PLAYBACK_TAIL_SECS;

    if (m_totalPlayTime == newPlayTime) {
        return;
    }

    m_totalPlayTime = newPlayTime;
    m_totalPlayTimeChanged.send(m_totalPlayTime);
}

//! NOTE: [our addition] Raw ticks reach the playback timeline through the native repeat expansion
//! first, and through the loop passes second. Playing from here wants the first occurrence; the
//! position reported by the player is mapped back the other way in secToTick().
int NotationPlayback::playbackUtickByRawTick(muse::midi::tick_t tick) const
{
    const mu::engraving::Score* sc = score();
    if (!sc) {
        return 0;
    }

    const int baseUtick = sc->repeatList(m_playbackModel.isPlayRepeatsEnabled()).tick2utick(tick);

    return loopExpansion().fromBaseUtick(baseUtick);
}

muse::audio::secs_t NotationPlayback::totalPlayTime() const
{
    return m_totalPlayTime;
}

muse::async::Channel<muse::audio::secs_t> NotationPlayback::totalPlayTimeChanged() const
{
    return m_totalPlayTimeChanged;
}

muse::audio::secs_t NotationPlayback::playedTickToSec(tick_t tick) const
{
    const mu::engraving::Score* sc = score();
    if (!sc) {
        return 0;
    }

    return sc->tempoTimeline(m_playbackModel.isPlayRepeatsEnabled()).utick2utime(tick);
}

tick_t NotationPlayback::secToPlayedTick(muse::audio::secs_t sec) const
{
    const mu::engraving::Score* sc = score();
    if (!sc) {
        return 0;
    }

    return sc->tempoTimeline(m_playbackModel.isPlayRepeatsEnabled()).utime2utick(sec);
}

tick_t NotationPlayback::secToTick(muse::audio::secs_t sec) const
{
    const mu::engraving::Score* sc = score();
    if (!sc) {
        return 0;
    }

    const bool expandRepeats = m_playbackModel.isPlayRepeatsEnabled();
    const tick_t utick = sc->tempoTimeline(expandRepeats).utime2utick(sec);

    // The flattened timeline's domain is already raw tick - only the expanded one needs converting back
    if (!expandRepeats) {
        return utick;
    }

    //! NOTE: [our addition] With a loop the expanded timeline plays the region several times, so
    //! the position has to be mapped back through the loop expansion first
    return sc->expandedRepeatList().utick2tick(loopExpansion().toBaseUtick(utick));
}

RetVal<muse::midi::tick_t> NotationPlayback::playPositionTickByRawTick(muse::midi::tick_t tick) const
{
    if (!score()) {
        return make_ret(Err::Undefined);
    }

    muse::midi::tick_t playbackTick = playbackUtickByRawTick(tick);

    return RetVal<muse::midi::tick_t>::make_ok(std::move(playbackTick));
}

RetVal<muse::midi::tick_t> NotationPlayback::playPositionTickByElement(const EngravingItem* element) const
{
    IF_ASSERT_FAILED(element) {
        return make_ret(Err::Undefined);
    }

    if (!score()) {
        return make_ret(Err::Undefined);
    }

    return playPositionTickByRawTick(element->tick().ticks());
}

void NotationPlayback::addLoopBoundary(LoopBoundaryType boundaryType, tick_t tick)
{
    const Measure* first = score()->firstMeasure();
    const Measure* last = score()->lastMeasure();
    IF_ASSERT_FAILED(first && last) {
        return;
    }

    if (tick == BoundaryTick::FirstScoreTick) {
        tick = first->tick().ticks();
    } else if (tick == BoundaryTick::LastScoreTick) {
        tick = last->endTick().ticks();
    }

    switch (boundaryType) {
    case LoopBoundaryType::LoopIn:
        addLoopIn(tick);
        break;
    case LoopBoundaryType::LoopOut:
        addLoopOut(tick);
        break;
    case LoopBoundaryType::Unknown:
        break;
    }
}

void NotationPlayback::addLoopIn(int _tick)
{
    Fraction tick = Fraction::fromTicks(_tick);

    if (_tick == BoundaryTick::SelectedNoteTick) {
        tick = score()->pos();
    }

    if (tick >= score()->loopOutTick()) { // If In pos >= Out pos, reset Out pos to end of score
        score()->setLoopOutTick(score()->lastMeasure()->endTick());
    }

    score()->setLoopInTick(tick);
}

void NotationPlayback::addLoopOut(int _tick)
{
    Fraction tick = Fraction::fromTicks(_tick);

    if (_tick == BoundaryTick::SelectedNoteTick) {
        tick = score()->pos() + score()->inputState().ticks();
    }

    if (tick <= score()->loopInTick()) { // If Out pos <= In pos, reset In pos to beginning of score
        score()->setLoopInTick(Fraction(0, 1));
    } else {
        if (tick > score()->lastMeasure()->endTick()) {
            tick = score()->lastMeasure()->endTick();
        }
    }

    score()->setLoopOutTick(tick);
}

void NotationPlayback::setLoopBoundariesEnabled(bool enabled)
{
    if (m_loopBoundaries.enabled == enabled) {
        return;
    }

    m_loopBoundaries.enabled = enabled;

    //! NOTE: [our addition] the expansion is applied first, so the player sees it when it decides
    //! whether it has to loop by itself (see updateLoopBoundaries)
    updateLoopExpansion();

    m_loopBoundariesChanged.notify();
    m_loopEnabledChanged.send(enabled);
}

bool NotationPlayback::isLoopEnabled() const
{
    return !m_loopBoundaries.isNull() && m_loopBoundaries.enabled;
}

bool NotationPlayback::isLoopExpanded() const
{
    return loopExpansion().isActive();
}

Channel<bool> NotationPlayback::loopEnabledChanged() const
{
    return m_loopEnabledChanged;
}

const LoopBoundaries& NotationPlayback::loopBoundaries() const
{
    return m_loopBoundaries;
}

Notification NotationPlayback::loopBoundariesChanged() const
{
    return m_loopBoundariesChanged;
}

const Tempo& NotationPlayback::multipliedTempo(tick_t tick) const
{
    if (!score()) {
        static Tempo empty;
        return empty;
    }

    m_currentTempo.valueBpm = static_cast<int>(std::round(score()->multipliedTempo(Fraction::fromTicks(tick)).toBPM().val));

    return m_currentTempo;
}

MeasureBeat NotationPlayback::beat(tick_t tick) const
{
    return mu::engraving::findBeat(m_getScore->score(), tick);
}

tick_t NotationPlayback::beatToRawTick(int measureIndex, int beatIndex) const
{
    return score() ? score()->sigmap()->bar2tick(measureIndex, beatIndex) : 0;
}

double NotationPlayback::tempoMultiplier() const
{
    return score() ? score()->tempoTimeline().tempoMultiplier().val : 1.0;
}

void NotationPlayback::setTempoMultiplier(double multiplier)
{
    Score* score = this->score();
    if (!score) {
        return;
    }

    if (!score->masterScore()->setTempoMultiplier(multiplier)) {
        return;
    }

    score->masterScore()->updateRepeatListTempo();

    m_playbackModel.reload();
}

void NotationPlayback::addSoundFlags(const std::vector<StaffText*>& staffTextList)
{
    TRACEFUNC;

    if (staffTextList.empty()) {
        return;
    }

    bool added = false;

    for (StaffText* staffText : staffTextList) {
        added |= doAddSoundFlag(staffText);
    }

    if (added) {
        score()->update();
        m_notationChanged.send(muse::RectF());
    }
}

bool NotationPlayback::doAddSoundFlag(StaffText* staffText)
{
    IF_ASSERT_FAILED(staffText) {
        return false;
    }

    if (staffText->hasSoundFlag()) {
        return false;
    }

    SoundFlag* soundFlag = Factory::createSoundFlag(staffText);
    staffText->add(soundFlag);

    const LinkedObjects* links = staffText->links();
    if (!links) {
        return true;
    }

    for (EngravingObject* obj : *links) {
        if (obj && obj->isStaffText() && obj != staffText) {
            toStaffText(obj)->add(soundFlag->linkedClone());
        }
    }

    return true;
}

void NotationPlayback::removeSoundFlags(const InstrumentTrackIdSet& trackIdSet)
{
    TRACEFUNC;

    std::vector<StaffText*> staffTextList = collectStaffText(trackIdSet, true /*withSoundFlags*/);
    if (staffTextList.empty()) {
        return;
    }

    for (StaffText* staffText : staffTextList) {
        if (!staffText->hasSoundFlag()) {
            continue;
        }

        staffText->remove(staffText->soundFlag());

        const LinkedObjects* links = staffText->links();
        if (!links) {
            continue;
        }

        for (EngravingObject* obj : *links) {
            if (obj && obj->isStaffText() && obj != staffText) {
                StaffText* linkedStaffText = toStaffText(obj);
                if (!linkedStaffText->hasSoundFlag()) {
                    continue;
                }

                linkedStaffText->remove(linkedStaffText->soundFlag());
            }
        }
    }

    score()->update();

    m_playbackModel.reload();
    m_notationChanged.send(muse::RectF());
}

bool NotationPlayback::hasSoundFlags(const engraving::InstrumentTrackIdSet& trackIdSet)
{
    TRACEFUNC;

    for (const InstrumentTrackId& trackId : trackIdSet) {
        if (m_playbackModel.hasSoundFlags(trackId)) {
            return true;
        }
    }

    return false;
}

std::vector<StaffText*> NotationPlayback::collectStaffText(const InstrumentTrackIdSet& trackIdSet, bool withSoundFlags) const
{
    TRACEFUNC;

    std::vector<StaffText*> result;

    if (trackIdSet.empty()) {
        return result;
    }

    const Score* score = this->score();
    IF_ASSERT_FAILED(score) {
        return result;
    }

    const Measure* fm = score->firstMeasure();
    if (!fm) {
        return result;
    }

    for (const Segment* seg = fm->first(SegmentType::ChordRest); seg; seg = seg->next1(SegmentType::ChordRest)) {
        for (EngravingItem* annotation : seg->annotations()) {
            if (!annotation || !annotation->isStaffText()) {
                continue;
            }

            StaffText* staffText = toStaffText(annotation);
            bool hasSoundFlag = staffText->hasSoundFlag();

            if (withSoundFlags && !hasSoundFlag) {
                continue;
            }

            if (!withSoundFlags && hasSoundFlag) {
                continue;
            }

            InstrumentTrackId trackId = mu::engraving::makeInstrumentTrackId(annotation);
            if (muse::contains(trackIdSet, trackId)) {
                result.push_back(staffText);
            }
        }
    }

    return result;
}
