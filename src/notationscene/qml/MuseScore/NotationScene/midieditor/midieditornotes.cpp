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

#include "midieditornotes.h"

#include <algorithm>
#include <cmath>

#include "translation.h"

#include "engraving/automation/automationdata.h"
#include "engraving/automation/automationtypes.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/noteevent.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/editing/editnote.h"

using namespace mu::engraving;
using namespace muse;

namespace mu::notation {
//! NOTE: Same meaning as in the Properties panel (see NotePlaybackModel): 0 means "the user never
//!       set a velocity", and is shown as 64 so that the lane does not look empty.
static constexpr int DEFAULT_VELOCITY = 64;

//! `NoteEvent::ontime` / `len` are thousandths of the nominal note length (see noteevent.h), which
//! is exactly the unit the legacy MIDI renderer uses: on = tick + (ticks * ontime) / 1000.
static constexpr int NOTE_EVENT_UNIT = 1000;

int midiDisplayVelocity(int userVelocity)
{
    return userVelocity == 0 ? DEFAULT_VELOCITY : userVelocity;
}

//! Reads the "played" layer of a note. Without an override the played values are the notated ones.
static void readPlayOverride(const Note* note, int nominalTicks, bool& hasOverride,
                             int& playTick, int& playDurationTicks, int& playVelocityPercent)
{
    const int noteTick = note->tick().ticks();

    hasOverride = false;
    playTick = noteTick;
    playDurationTicks = nominalTicks;
    playVelocityPercent = 100;

    const NoteEventList& events = note->playEvents();
    if (events.empty()) {
        return;
    }

    const NoteEvent& event = events.front();

    const int shiftTicks = (nominalTicks * event.ontime()) / NOTE_EVENT_UNIT;
    const int lenTicks = std::max(1, (nominalTicks * event.len()) / NOTE_EVENT_UNIT);
    const int velocityPercent = int(std::lround(event.velocityMultiplier() * 100.0));

    playTick = noteTick + shiftTicks;
    playDurationTicks = lenTicks;
    playVelocityPercent = velocityPercent;

    //! NOTE: the score reader hands us a neutral event for every note, so "has an event" is not the
    //!       same as "the user changed something". Only a real difference is worth drawing.
    hasOverride = shiftTicks != 0
                  || lenTicks != nominalTicks
                  || velocityPercent != 100;
}

std::vector<MidiNoteItem> collectMidiNotes(const Score* score)
{
    std::vector<MidiNoteItem> result;
    if (!score) {
        return result;
    }

    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (const Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            for (track_idx_t track = 0; track < score->ntracks(); ++track) {
                EngravingItem* item = segment->element(track);
                if (!item || !item->isChord()) {
                    continue;
                }

                const Chord* chord = toChord(item);
                if (chord->isGrace()) {
                    //! NOTE: grace notes have no independent time position in a roll.
                    continue;
                }

                const int durationTicks = chord->actualTicks().ticks();
                if (durationTicks <= 0) {
                    continue;
                }

                for (const Note* note : chord->notes()) {
                    MidiNoteItem entry;
                    entry.note = const_cast<Note*>(note);
                    entry.tick = note->tick().ticks();
                    entry.durationTicks = durationTicks;
                    entry.pitch = note->pitch();
                    entry.velocity = midiDisplayVelocity(note->userVelocity());
                    entry.hasVelocityOverride = note->userVelocity() != 0;
                    entry.staffIndex = int(note->staffIdx());
                    entry.voice = int(note->voice());

                    readPlayOverride(note, durationTicks, entry.hasPlayOverride,
                                     entry.playTick, entry.playDurationTicks, entry.playVelocityPercent);

                    result.push_back(entry);
                }
            }
        }
    }

    return result;
}

std::vector<MidiMeasureItem> collectMidiMeasures(const Score* score)
{
    std::vector<MidiMeasureItem> result;
    if (!score) {
        return result;
    }

    for (const Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        MidiMeasureItem item;
        item.tick = measure->tick().ticks();
        item.endTick = measure->endTick().ticks();
        result.push_back(item);
    }

    return result;
}

bool applyNotePitch(Score* score, Note* note, int pitch)
{
    if (!score || !note) {
        return false;
    }

    pitch = std::clamp(pitch, 0, 127);
    if (pitch == note->pitch()) {
        return false;
    }

    score->startCmd(TranslatableString("midieditor", "Change pitch"));
    EditNote::undoChangePitch(score, note, pitch, note->tpc1default(pitch), note->tpc2default(pitch));
    score->endCmd();

    return true;
}

//! The write itself, with NO command of its own - so a batch can wrap many of them in one.
//! `velocity` may be 0 on purpose: that means "no own velocity", i.e. the note goes back to
//! following the dynamic marks. Clamping to 1 would make a per-note tweak impossible to undo.
static void writeNoteVelocity(Note* note, int velocity)
{
    //! NOTE: the same property the Properties panel writes (Pid::USER_VELOCITY), so the two stay
    //!       interchangeable. The propertyFlags handling is copied from
    //!       PropertiesPanelAbstractModel::setPropertyValue.
    PropertyFlags flags = note->propertyFlags(Pid::USER_VELOCITY);
    if (flags == PropertyFlags::STYLED) {
        flags = PropertyFlags::UNSTYLED;
    }
    note->undoChangeProperty(Pid::USER_VELOCITY, PropertyValue(std::clamp(velocity, 0, 127)), flags);
}

bool applyNoteVelocity(Score* score, Note* note, int velocity)
{
    if (!score || !note) {
        return false;
    }

    if (std::clamp(velocity, 0, 127) == note->userVelocity()) {
        return false;
    }

    score->startCmd(TranslatableString("midieditor", "Change velocity"));
    writeNoteVelocity(note, velocity);
    score->endCmd();

    return true;
}

int applyNoteVelocities(Score* score, const std::vector<std::pair<Note*, int> >& changes)
{
    if (!score) {
        return 0;
    }

    //! Only the notes that really change are worth touching - and if none do, the score must not be
    //! notified at all.
    std::vector<std::pair<Note*, int> > pending;
    pending.reserve(changes.size());
    for (const std::pair<Note*, int>& change : changes) {
        if (change.first && std::clamp(change.second, 0, 127) != change.first->userVelocity()) {
            pending.push_back(change);
        }
    }

    if (pending.empty()) {
        return 0;
    }

    //! ONE command for the whole batch. Every startCmd/endCmd pair notifies the score, and the
    //! subscribers to that notification rebuild things that cost O(score) - the notation view
    //! repaints, the playback events are rebuilt. One command per note therefore made a brush stroke
    //! over N notes cost N full-score rebuilds: that is the lag, and this is the fix.
    score->startCmd(TranslatableString("midieditor", "Draw velocities"));
    for (const std::pair<Note*, int>& change : pending) {
        writeNoteVelocity(change.first, change.second);
    }
    score->endCmd();

    return int(pending.size());
}

bool applyNotePlayOverride(Score* score, Note* note, int startTick, int durationTicks, int velocityPercent)
{
    if (!score || !note || !note->chord()) {
        return false;
    }

    const int nominalTicks = note->chord()->actualTicks().ticks();
    if (nominalTicks <= 0) {
        return false;
    }

    const int noteTick = note->tick().ticks();
    const int ontime = ((startTick - noteTick) * NOTE_EVENT_UNIT) / nominalTicks;
    const int len = std::max(1, (durationTicks * NOTE_EVENT_UNIT) / nominalTicks);
    const double velocityMultiplier = std::max(0.0, velocityPercent / 100.0);

    //! NOTE: copy - `ChangeNoteEventList` takes the new list by value, so the command owns it and
    //!       undo never depends on a pointer that a later reallocation could invalidate (that is why
    //!       this is preferred over `ChangeNoteEvent`).
    NoteEventList events = note->playEvents();
    if (events.empty()) {
        events.push_back(NoteEvent());
    }

    if (events.front().ontime() == ontime
        && events.front().len() == len
        && std::abs(events.front().velocityMultiplier() - velocityMultiplier) < 1e-9) {
        return false;
    }

    events.front().setOntime(ontime);
    events.front().setLen(len);
    events.front().setVelocityMultiplier(velocityMultiplier);

    score->startCmd(TranslatableString("midieditor", "Change played timing"));
    //! NOTE: reuse the engraving undo command instead of writing our own; it also switches the
    //!       chord's play event type to User, which is what makes the value survive saving.
    score->undo(new ChangeNoteEventList(note, events));
    score->endCmd();

    return true;
}

//! The Dynamics curve key of one staff, built the way the notation page builds it
//! (NotationAutomationController::curveKeyFor -> ScoreAutomationController::resolveKeys), so that the
//! two pages address ONE curve. A staff index outside the score yields an invalid key, and every entry
//! point below treats that as "do nothing".
static AutomationCurveKey dynamicsKey(const Score* score, int staffIndex)
{
    if (!score || staffIndex < 0 || size_t(staffIndex) >= score->nstaves()) {
        return {};
    }

    const Staff* staff = score->staff(size_t(staffIndex));
    if (!staff) {
        return {};
    }

    return AutomationCurveKey::staff(AutomationType::Dynamics, staff->id());
}

//! Whether the lane may edit this point at all. The score's own answer is "not if I generated it, and
//! not if it belongs to an engraving item" - see NotationAutomationController::requestRemovePoint.
static bool isAuthoredPoint(const AutomationPoint& point)
{
    return !point.generated && !point.itemId.has_value();
}

std::vector<MidiAutomationPoint> collectAutomationPoints(const Score* score, int staffIndex)
{
    std::vector<MidiAutomationPoint> result;

    const AutomationCurveKey key = dynamicsKey(score, staffIndex);
    if (!score || !key.isValid()) {
        return result;
    }

    const AutomationDataConstPtr data = score->automationData();
    if (!data) {
        return result;
    }

    const AutomationCurve& curve = data->curve(key);
    result.reserve(curve.size());

    for (const auto& [tick, point] : curve) {
        MidiAutomationPoint item;
        item.tick = tick;
        item.value = double(point.value.outValue);
        item.authored = isAuthoredPoint(point);
        result.push_back(item);
    }

    return result;
}

int applyAutomationPoints(Score* score, int staffIndex, const std::vector<MidiAutomationPoint>& points)
{
    const AutomationCurveKey key = dynamicsKey(score, staffIndex);
    if (!score || !key.isValid() || points.empty()) {
        return 0;
    }

    //! The curve as it stands, so that a point which already holds the value can be left alone. Reading
    //! it is what keeps a stroke that changes nothing from pushing an undo step that does nothing - and
    //! from clobbering an arrival shape the point may carry.
    const AutomationDataConstPtr data = score->automationData();
    const AutomationCurve* curve = data ? &data->curve(key) : nullptr;

    AutomationPointEdits edits;
    edits.reserve(points.size());

    for (const MidiAutomationPoint& point : points) {
        if (point.tick < 0) {
            continue;
        }

        const real_t value = real_t::make(std::clamp(point.value, 0.0, 1.0));

        if (curve) {
            const AutomationCurve::const_iterator it = curve->find(point.tick);
            if (it != curve->end() && it->second.value.outValue == value) {
                continue;
            }
        }

        AutomationPoint written;
        //! NOTE: engraving's AutomationPoint wraps the mpe one - the value lives one level down.
        written.value.outValue = value;

        //! `inValue` is left at ArrivalFromPrevious (the default), which is what makes a run of points
        //! read as one continuous move - the shape of a crescendo - rather than as steps.
        //!
        //! `generated` stays false and no `itemId` is set: this is the user's own point now, exactly like
        //! a point added on the notation page (NotationAutomationController::requestAddPoint). Being
        //! authored is also what makes it win over a generated point at the same tick - the score's
        //! generation step leaves an authored point alone.

        edits.push_back({ point.tick, AutomationPointEdit::SetPoint { written } });
    }

    if (edits.empty()) {
        return 0;
    }

    //! ONE command for the whole stroke - see applyNoteVelocities for why that matters. It goes through
    //! the score's own undoable automation command, so it undoes and saves like the same edit made on
    //! the notation page.
    //!
    //! ⚠️ The command has to be OPEN when that call is made: `Score::undo()` (cmd.cpp) applies a command
    //! immediately and then DISCARDS it when no transaction is open, so an edit made outside one changes
    //! the curve and leaves nothing to undo - the stroke would be un-undoable. The notation page wraps
    //! this very call in a transaction for the same reason (NotationAutomation::editPoints).
    score->startCmd(TranslatableString("midieditor", "Draw dynamics curve"));
    score->editAutomationPoints(key, edits);
    score->endCmd();

    return int(edits.size());
}

bool eraseAutomationPoint(Score* score, int staffIndex, int tick)
{
    const AutomationCurveKey key = dynamicsKey(score, staffIndex);
    if (!score || !key.isValid() || tick < 0) {
        return false;
    }

    const AutomationDataConstPtr data = score->automationData();
    if (!data) {
        return false;
    }

    const AutomationCurve& curve = data->curve(key);
    const AutomationCurve::const_iterator it = curve.find(tick);
    if (it == curve.end()) {
        return false;
    }

    //! The mark's point, not the lane's - see the note on the declaration.
    if (!isAuthoredPoint(it->second)) {
        return false;
    }

    AutomationPointEdits edits { { tick, AutomationPointEdit::ErasePoint {} } };

    //! In a command, for the reason spelled out in applyAutomationPoints.
    score->startCmd(TranslatableString("midieditor", "Remove dynamics point"));
    score->editAutomationPoints(key, edits);
    score->endCmd();

    return true;
}
}
