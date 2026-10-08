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
#include <string>

#include "translation.h"
#include "log.h"

#include "engraving/automation/automationdata.h"
#include "engraving/automation/automationtypes.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/input.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/noteevent.h"
#include "engraving/dom/noteval.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/editing/editnote.h"
#include "engraving/editing/noteinput.h"
#include "engraving/editing/transaction/transaction.h"

using namespace mu::engraving;
using namespace muse;

namespace mu::notation {
//! NOTE: Same meaning as in the Properties panel (see NotePlaybackModel): 0 means "the user never
//!       set a velocity", and is shown as 64 so that the lane does not look empty.
static constexpr int DEFAULT_VELOCITY = 64;

//! `NoteEvent::ontime` / `len` are thousandths of the nominal note length (see noteevent.h), which
//! is exactly the unit the legacy MIDI renderer uses: on = tick + (ticks * ontime) / 1000.
static constexpr int NOTE_EVENT_UNIT = 1000;

//! 力度车道（"演奏力度"通道）能写到的最大百分比。上限只是**防呆**：`velocityMultiplier` 是乘在
//! 动态级别上的，写太大就削顶；界面上真正的范围比这个小（见 QML 里的夹取）。
static constexpr int MAX_PLAY_VELOCITY_PERCENT = 400;

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

//! The write itself, with NO command of its own - so a drag of a whole selection can wrap many of them
//! in one. Reuses the very command the notation editor uses, so linked notes stay in sync (see the
//! note on `applyNotePitch`).
static void writeNotePitch(Score* score, Note* note, int pitch)
{
    EditNote::undoChangePitch(score, note, pitch, note->tpc1default(pitch), note->tpc2default(pitch));
}

bool applyNotePitch(Score* score, Note* note, int pitch, bool openCommand)
{
    if (!score || !note) {
        return false;
    }

    pitch = std::clamp(pitch, 0, 127);
    if (pitch == note->pitch()) {
        return false;
    }

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Change pitch"));
    }
    writeNotePitch(score, note, pitch);
    if (openCommand) {
        score->endCmd();
    }

    return true;
}

int applyNotePitches(Score* score, const std::vector<std::pair<Note*, int> >& changes, bool openCommand)
{
    if (!score) {
        return 0;
    }

    //! Same rule as the velocity batch: only what really changes is worth touching, and a batch that
    //! changes nothing must not notify the score at all.
    std::vector<std::pair<Note*, int> > pending;
    pending.reserve(changes.size());
    for (const std::pair<Note*, int>& change : changes) {
        if (change.first && std::clamp(change.second, 0, 127) != change.first->pitch()) {
            pending.push_back(change);
        }
    }

    if (pending.empty()) {
        return 0;
    }

    //! ONE command for the whole batch - see applyNoteVelocities for the reason.
    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Change pitches"));
    }
    for (const std::pair<Note*, int>& change : pending) {
        writeNotePitch(score, change.first, std::clamp(change.second, 0, 127));
    }
    if (openCommand) {
        score->endCmd();
    }

    return int(pending.size());
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

bool applyNoteVelocity(Score* score, Note* note, int velocity, bool openCommand)
{
    if (!score || !note) {
        return false;
    }

    if (std::clamp(velocity, 0, 127) == note->userVelocity()) {
        return false;
    }

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Change velocity"));
    }
    writeNoteVelocity(note, velocity);
    if (openCommand) {
        score->endCmd();
    }

    return true;
}

int applyNoteVelocities(Score* score, const std::vector<std::pair<Note*, int> >& changes, bool openCommand)
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
    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Draw velocities"));
    }
    for (const std::pair<Note*, int>& change : pending) {
        writeNoteVelocity(change.first, change.second);
    }
    if (openCommand) {
        score->endCmd();
    }

    return int(pending.size());
}

bool applyNotePlayOverride(Score* score, Note* note, int startTick, int durationTicks, int velocityPercent, bool openCommand)
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

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Change played timing"));
    }
    //! NOTE: reuse the engraving undo command instead of writing our own; it also switches the
    //!       chord's play event type to User, which is what makes the value survive saving.
    score->undo(new ChangeNoteEventList(note, events));
    if (openCommand) {
        score->endCmd();
    }

    return true;
}

//! ⚠️ `NoteEvent` 的力度乘子**只改这一个字段**：`ontime` / `len` 原样保留 —— 写一次"演奏力度"
//! 绝不该顺手把演奏时值搬走（那是另一条手势、另一个通道）。
static bool writeNotePlayVelocity(Score* score, Note* note, int velocityPercent)
{
    const double multiplier = std::max(0.0, velocityPercent / 100.0);

    //! NOTE: copy - `ChangeNoteEventList` takes the new list by value, so the command owns it and undo
    //!       never depends on a pointer a later reallocation could invalidate (same reason as in
    //!       `applyNotePlayOverride`).
    NoteEventList events = note->playEvents();
    if (events.empty()) {
        //! 读谱器会给每个音填一个中性事件，所以这里基本不会走到；真没有就补一个中性的，
        //! 免得"只改力度"变成"改了一个时值全 0 的事件"（那会把音压成 0 长度）。
        events.push_back(NoteEvent());
    }

    if (std::abs(events.front().velocityMultiplier() - multiplier) < 1e-9) {
        return false;
    }

    events.front().setVelocityMultiplier(multiplier);
    score->undo(new ChangeNoteEventList(note, events));
    return true;
}

int applyNotePlayVelocities(Score* score, const std::vector<std::pair<Note*, int> >& changes, bool openCommand)
{
    if (!score) {
        return 0;
    }

    std::vector<std::pair<Note*, int> > pending;
    pending.reserve(changes.size());
    for (const std::pair<Note*, int>& change : changes) {
        if (!change.first) {
            continue;
        }

        const int percent = std::clamp(change.second, 0, MAX_PLAY_VELOCITY_PERCENT);
        const double multiplier = percent / 100.0;
        const NoteEventList& events = change.first->playEvents();
        const double current = events.empty() ? NoteEvent::DEFAULT_VELOCITY_MULTIPLIER : events.front().velocityMultiplier();

        if (std::abs(current - multiplier) > 1e-9) {
            pending.emplace_back(change.first, percent);
        }
    }

    if (pending.empty()) {
        return 0;
    }

    //! ONE command for the whole stroke - see applyNoteVelocities for the reason.
    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Draw played velocities"));
    }
    for (const std::pair<Note*, int>& change : pending) {
        writeNotePlayVelocity(score, change.first, change.second);
    }
    if (openCommand) {
        score->endCmd();
    }

    return int(pending.size());
}

//! 造一个"只用来发声"的临时音符，音高可以**不是**谱面上那个。
//!
//! 为什么要临时元素：卷帘窗拖动改音高时**只预览、不写谱**（松手才提交一次，见 §4.8），
//! 而播放层是从 engraving 模型渲染事件的 —— 拿原来的 `Note*` 去播，响的永远是**拖动前**的音高
//! （用户 2026-10-07 报的「上下拖动响的是同一个音」）。记谱页拖动时之所以响的是拖到的音，
//! 是因为 `viewInteraction()->drag()` **实时改谱**；这一页不能那么做，所以改"造一个临时音符"。
//!
//! 记谱页点音符走的是 `IPlaybackController::playElements()`，这里给的仍然是**同一个接口、
//! 同一种元素**（一个 `Note`），所以"编辑时播放音符"这个设置照旧管着它 —— 只是元素是临时的。
//!
//! 三个字段必须从**原音符**抄过来，否则播放层认不出这条轨道：
//! `track`（→ `part()`，`makeInstrumentTrackId()` 用它找轨道）、`staffIdx`、`voice`；
//! 位置（`tick`）由父元素（和弦/段）带出来，所以临时元素挂在**原音符所在的段**上。
//!
//! ⚠️ `chordOut` 归调用方删除，**而删除和弦就会删掉它自己的音符**（`Chord::~Chord()` 里
//! `DeleteAll(m_notes)`）—— 所以**不要**再单独 delete `note`，否则是二次释放。
MidiAuditionNote midiNoteToAudition(engraving::Score* score, engraving::Note* note, int pitch)
{
    MidiAuditionNote result;
    if (!score || !note) {
        return result;
    }

    const Chord* sourceChord = note->chord();
    Segment* segment = sourceChord ? sourceChord->segment() : nullptr;

    //! NOTE: 父元素给的是"能解析出 score、能带出 tick"的归属链。
    //! ⚠️ `Factory::createChord(segment)` **只把段记成构造参数**，并不建立**归属**关系 ——
    //! 而 `EngravingItem::tick()` 是沿 `ownershipParent()` 往上找段/小节的，`ownershipParent()`
    //! 又只在**显式设置过**时才返回父对象 ⇒ 不显式设的话临时音符的 tick 恒为 0
    //! （试听就会响在曲子开头，而不是这个音所在的位置）。单元测试量到的就是这一点。
    Chord* chord = Factory::createChord(segment);
    if (!chord) {
        return result;
    }

    if (segment) {
        chord->setOwnershipParent(segment);
    }

    chord->setTrack(note->track());
    chord->setStaffIdx(note->staffIdx());
    chord->setVoice(note->voice());

    Note* temp = Factory::createNote(chord);
    temp->setOwnershipParent(chord);
    temp->setTrack(note->track());
    temp->setStaffIdx(note->staffIdx());
    temp->setVoice(note->voice());

    NoteVal nval;
    nval.pitch = std::clamp(pitch, 0, 127);
    temp->setNval(nval);

    result.note = temp;
    result.chord = chord;
    return result;
}

// ── 实时录制的写回（见 midirecorder.h 里的数据流图）──────────────────────────────────────────────

//! tick 之前**最后一个** ChordRest 段（同一个小节内）。
//!
//! ⚠️ 不能用 `Score::tick2segment()` 代替：它只返回"正好落在 tick 上"的段，
//! 而这里要的恰恰是"包含这个 tick 的那一段"（空谱表整小节只有一个小节休止符，
//! 网格位置上一个段都没有）。
static Segment* chordRestSegmentAtOrBefore(Measure* measure, const Fraction& tick)
{
    Segment* found = nullptr;
    for (Segment* segment = measure->first(SegmentType::ChordRest); segment;
         segment = segment->next(SegmentType::ChordRest)) {
        if (segment->tick() > tick) {
            break;
        }
        found = segment;
    }

    return found;
}

//! 让 `tick` 处**存在**一个 ChordRest 段，并返回它。
//!
//! 为什么需要：`Score::setNoteRest()` 要求一个"段"，而记谱模型里的段只在 ChordRest 的**起点**上
//! （空谱表一个小节只有一个小节休止符，1/16 网格上的位置一个段都没有）。所以录制写回的第一步
//! 是"把既有那一段从 tick 处切开"。
//!
//! 切开**不是抹掉**：前半按原样重建（音高 + 力度；连音线/记号不在复刻之列），
//! 后半由 `makeGap()` 自己克隆（`addClone(cr, ...)`，见 `维护手册.md` §4.8.4 的边界那一节）。
//! 于是"在某音中间落一个新的录制音"结果是那个音被切成两半，而不是消失。
//!
//! `scratch` 是一个**临时**的 `InputState`：`setNoteRest()` 末尾会往它里面写段/轨，
//! 不传它的话写的是**用户的**输入光标，而且走 `select()` 分支改掉用户在当前谱面的选中。
//!
//! ⚠️⚠️ **`Segment*` 不能跨 `setNoteRest()` 复用**：它内部的 `makeGap()` 会把正在被替换的
//! ChordRest **移除**，而空掉的段会被**一并删掉**（`Score::undoRemoveElement()` 末尾那句
//! `if (s->empty()) doUndoRemoveElement(s);`）⇒ 新音符落在**另一个**段对象上，而手里那个
//! 已经是悬空的（表现为 `segment->element(track) == nullptr`，静默跳过，一个音都写不进去）。
//! 所以本文件每一步都**从乐谱里按 tick 重新找段**，绝不缓存段指针。
static Segment* recordSegmentAt(Score* score, const Fraction& tick, track_idx_t track, InputState& scratch)
{
    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return nullptr;
    }

    if (Segment* existing = measure->findSegment(SegmentType::ChordRest, tick)) {
        return existing;
    }

    Segment* previous = chordRestSegmentAtOrBefore(measure, tick);
    if (!previous || previous->tick() >= tick) {
        return nullptr;
    }

    const Fraction previousTick = previous->tick();

    EngravingItem* item = previous->element(track);
    ChordRest* cr = (item && item->isChordRest()) ? toChordRest(item) : nullptr;
    if (!cr) {
        //! 这一声部在这里是空的（voice 1..3 的空隙）。不处理是对的：段是**各声部共用**的，
        //! 同一个 tick 上的 0 声部先写（见 applyRecordedChords 的排序），段已经由它建出来了。
        return nullptr;
    }

    const Fraction length = tick - previousTick;
    if (length <= Fraction(0, 1) || length >= cr->ticks()) {
        return nullptr;
    }

    if (cr->isRest()) {
        //! 休止符：直接切。`makeGap()` 会把余下的部分重新铺成休止符。
        score->setNoteRest(previous, track, NoteVal(), length, DirectionV::AUTO, false, {}, false, &scratch);
    } else {
        //! 和弦：前半按原样重建，否则那个音被覆盖到的部分会变成一个休止符（等于被抹掉）。
        const Chord* chord = toChord(cr);
        std::vector<NoteVal> values;
        for (const Note* note : chord->notes()) {
            NoteVal value(note->pitch());
            value.velocityOverride = note->userVelocity();
            values.push_back(value);
        }

        if (values.empty()) {
            return nullptr;
        }

        score->setNoteRest(previous, track, values.front(), length, DirectionV::AUTO, false, {}, false, &scratch);

        //! 前半所在的段**重新找**（`previous` 可能已经在上一步里被删掉，见函数头那段说明）。
        Segment* headSegment = measure->findSegment(SegmentType::ChordRest, previousTick);
        EngravingItem* headItem = headSegment ? headSegment->element(track) : nullptr;
        Chord* head = (headItem && headItem->isChord()) ? toChord(headItem) : nullptr;
        if (head) {
            Transaction& tx = score->transactionManager()->currentOrDummyTransaction();
            for (size_t i = 1; i < values.size(); ++i) {
                NoteInput::addPitchToChord(tx, score, values[i], head, &scratch, false);
            }

            for (Note* note : head->notes()) {
                for (const NoteVal& value : values) {
                    if (value.pitch == note->pitch()) {
                        writeNoteVelocity(note, value.velocityOverride);
                        break;
                    }
                }
            }
        }
    }

    return measure->findSegment(SegmentType::ChordRest, tick);
}

//! 这个和弦的所有音（`setNoteRest` 会按 request 的时值把音拆成连音线，所以建出来的可能不止一个和弦）。
static std::vector<Chord*> chordChainAt(Segment* segment, track_idx_t track)
{
    std::vector<Chord*> chords;

    for (Segment* current = segment; current; current = current->next(SegmentType::ChordRest)) {
        EngravingItem* item = current->element(track);
        if (!item || !item->isChord()) {
            break;
        }

        Chord* chord = toChord(item);
        if (chord->isGrace()) {
            break;
        }

        chords.push_back(chord);

        //! 连音线的下一半一定紧接着这一段；没有 tieFor 就说明这个音写完了。
        if (chord->notes().empty() || !chord->notes().front()->tieFor()) {
            break;
        }
    }

    return chords;
}

MidiRecordedWriteResult applyRecordedChords(Score* score, int staffIndex, int baseVoice,
                                            const std::vector<MidiRecordedChord>& chords, bool openCommand)
{
    MidiRecordedWriteResult result;

    if (!score || chords.empty() || staffIndex < 0 || size_t(staffIndex) >= score->nstaves()) {
        return result;
    }

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Record MIDI"));
    }

    InputState scratch;
    const Fraction scoreEnd = score->endTick();
    const track_idx_t staffTrack = staff2track(staff_idx_t(staffIndex));

    //! 同一个 tick 上**先写 0 声部**：段是各声部共用的，0 声部在 tick 处把段切出来之后，
    //! 1/2/3 声部才有位置可写（`buildRecordedChords` 按"时值短的先落 0 声部"分声部）。
    std::vector<const MidiRecordedChord*> ordered;
    ordered.reserve(chords.size());
    for (const MidiRecordedChord& chord : chords) {
        ordered.push_back(&chord);
    }

    std::stable_sort(ordered.begin(), ordered.end(), [](const MidiRecordedChord* a, const MidiRecordedChord* b) {
        if (a->tick != b->tick) {
            return a->tick < b->tick;
        }
        return a->voice < b->voice;
    });

    for (const MidiRecordedChord* entry : ordered) {
        if (entry->notes.empty()) {
            ++result.chordsSkipped;
            continue;
        }

        const Fraction tick = Fraction::fromTicks(std::max(0, entry->tick));
        if (tick >= scoreEnd) {
            //! 谱面之外不写：`setNoteRest` 其实会自己加小节，但那会让"跟着伴奏录一段"变成
            //! "莫名其妙多出几十小节"。跳过的数量会报给用户（见 MidiRecordedWriteResult）。
            ++result.chordsSkipped;
            continue;
        }

        const int voice = std::clamp(baseVoice + entry->voice, 0, int(VOICES) - 1);
        const track_idx_t track = staffTrack + track_idx_t(voice);
        const Fraction duration = Fraction::fromTicks(std::max(1, entry->durationTicks));

        Segment* segment = recordSegmentAt(score, tick, track, scratch);
        if (!segment || segment->tick() != tick) {
            ++result.chordsSkipped;
            continue;
        }

        //! 第一个音建出和弦（跨小节自动变连音线），其余音加到这个和弦上。
        score->setNoteRest(segment, track, NoteVal(entry->notes.front().pitch), duration,
                           DirectionV::AUTO, false, {}, false, &scratch);

        //! ⚠️ 段**重新找**，不要用传进去那一个：`setNoteRest()` 里的 `makeGap()` 会移除它替换掉的
        //! ChordRest，而空掉的段会被一并删掉 —— 新和弦因此落在**另一个**段对象上，
        //! 手里那个已经悬空（`element(track)` 是 null）。见 recordSegmentAt() 头部的说明。
        Measure* measure = score->tick2measure(tick);
        Segment* created = measure ? measure->findSegment(SegmentType::ChordRest, tick) : nullptr;
        EngravingItem* item = created ? created->element(track) : nullptr;
        Chord* chord = (item && item->isChord()) ? toChord(item) : nullptr;
        if (!chord || chord->tick() != tick) {
            ++result.chordsSkipped;
            continue;
        }

        Transaction& tx = score->transactionManager()->currentOrDummyTransaction();
        for (size_t i = 1; i < entry->notes.size(); ++i) {
            NoteVal value(entry->notes[i].pitch);
            NoteInput::addPitchToChord(tx, score, value, chord, &scratch, false);
        }

        //! 力度写成 `Pid::USER_VELOCITY`（力度车道与 Properties 面板写的**同一个**属性）。
        //! ⚠️ 不能直接 `note->setUserVelocity()`：那一步不进事务，撤销再重做之后力度会回到 0
        //! （重做用的是入栈那一刻的克隆）。`writeNoteVelocity()` 走的是 `undoChangeProperty`。
        //! 连音线的每一段都要写 —— 它们是不同的音符。
        for (Chord* part : chordChainAt(created, track)) {
            for (Note* note : part->notes()) {
                for (const MidiRecordedChord::Note& source : entry->notes) {
                    if (source.pitch == note->pitch()) {
                        writeNoteVelocity(note, source.velocity);
                        break;
                    }
                }
            }
        }

        ++result.chordsWritten;
        result.notesWritten += int(entry->notes.size());
        result.lastTick = entry->tick;
    }

    if (openCommand) {
        score->endCmd();
    }

    return result;
}

// ── 结构性编辑：增 / 删 / 移 / 改记谱时长 / 复制粘贴（见头文件那一节）────────────────────────────
//!
//! 这一组与前四个写入函数的**根本差别**：前四个只改属性，音符对象不动；这一组改的是**谱面结构**，
//! 音符对象会被整个换掉。所以贯穿全组的两条纪律：
//!  ① 每一步都**从乐谱里按 tick 重新找段/和弦**（`setNoteRest()` 内 `makeGap()` 会移除它替换掉的
//!     ChordRest，空掉的段会被一并删掉 —— `维护手册.md` §4.8.4 第 1 条）；
//!  ② 凡是"先删再写"，**先把要保留的数据抄成值**（`MidiNoteData`），写完之后旧 `Note*` 一律作废。

//! 一个"要写进和弦的音"：音高 + 它自己的力度（0 = 没有自己的力度）。
struct MidiChordTone {
    int pitch = 0;
    int velocity = 0;
};

static bool chordHasPitch(const Chord* chord, int pitch)
{
    if (!chord) {
        return false;
    }

    for (const Note* note : chord->notes()) {
        if (note->pitch() == pitch) {
            return true;
        }
    }

    return false;
}

//! 把一个音符读成"与对象解绑的数据"。`false` = 这个音没法参与结构编辑（没和弦 / 时值为 0）。
static bool readNoteData(const Note* note, MidiNoteData& out)
{
    if (!note || !note->chord()) {
        return false;
    }

    const int nominalTicks = note->chord()->actualTicks().ticks();
    if (nominalTicks <= 0) {
        return false;
    }

    out.tick = note->tick().ticks();
    out.durationTicks = nominalTicks;
    out.pitch = note->pitch();
    out.velocity = note->userVelocity();

    readPlayOverride(note, nominalTicks, out.hasPlayOverride,
                     out.playTick, out.playDurationTicks, out.playVelocityPercent);
    return true;
}

//! 力度写进**整条连音线**（连音线的每一段是不同的音符对象）。`tones` 里没有的音高不动。
static void writeChordToneVelocities(Segment* segment, track_idx_t track, const std::vector<MidiChordTone>& tones)
{
    for (Chord* part : chordChainAt(segment, track)) {
        for (Note* note : part->notes()) {
            for (const MidiChordTone& tone : tones) {
                if (tone.pitch == note->pitch()) {
                    if (std::clamp(tone.velocity, 0, 127) != note->userVelocity()) {
                        writeNoteVelocity(note, tone.velocity);
                    }
                    break;
                }
            }
        }
    }
}

//! 在 (staff, voice, tick) 处写一个和弦，返回**真正落进乐谱的音**（没写成就是空）。
//!
//! 三条语义：
//!  * 那个 tick 上**已经有和弦** → 音**加到这个和弦上**（记谱页"再点一个音"就是加和弦音），
//!    时值参数被忽略 —— 一个和弦只有一个时值；已经在和弦里的音高直接跳过（不写重复音）。
//!  * 那个 tick 上**只有休止符或空隙** → `setNoteRest()`：它会切开前面的音符、把余下的铺成休止符。
//!  * **谱面之外一律不写**（`setNoteRest()` 其实会自己加小节，但那会让"点错一下"变成"多出几十小节"）。
static std::vector<Note*> writeChordAt(Score* score, int staffIndex, int voice, int tick, int durationTicks,
                                       const std::vector<MidiChordTone>& tones, InputState& scratch)
{
    std::vector<Note*> written;

    if (!score || tones.empty() || staffIndex < 0 || size_t(staffIndex) >= score->nstaves()) {
        return written;
    }
    if (tick < 0 || durationTicks <= 0 || voice < 0 || voice >= int(VOICES)) {
        return written;
    }

    const Fraction at = Fraction::fromTicks(tick);
    if (at >= score->endTick()) {
        return written;
    }

    Measure* measure = score->tick2measure(at);
    if (!measure) {
        return written;
    }

    const track_idx_t track = staff2track(staff_idx_t(staffIndex)) + track_idx_t(voice);
    Segment* existing = measure->findSegment(SegmentType::ChordRest, at);
    EngravingItem* existingItem = existing ? existing->element(track) : nullptr;

    if (existingItem && existingItem->isChord() && !toChord(existingItem)->isGrace()) {
        Chord* chord = toChord(existingItem);
        Transaction& tx = score->transactionManager()->currentOrDummyTransaction();

        std::vector<int> added;
        for (const MidiChordTone& tone : tones) {
            if (chordHasPitch(chord, tone.pitch)) {
                continue;
            }

            NoteVal value(tone.pitch);
            NoteInput::addPitchToChord(tx, score, value, chord, &scratch, false);
            added.push_back(tone.pitch);
        }

        //! 只写**刚加进来的**音：和弦里原有的音不是这一次编辑的目标。
        for (Note* note : chord->notes()) {
            if (std::find(added.begin(), added.end(), note->pitch()) == added.end()) {
                continue;
            }

            for (const MidiChordTone& tone : tones) {
                if (tone.pitch == note->pitch()) {
                    if (std::clamp(tone.velocity, 0, 127) != note->userVelocity()) {
                        writeNoteVelocity(note, tone.velocity);
                    }
                    break;
                }
            }

            written.push_back(note);
        }

        return written;
    }

    Segment* segment = recordSegmentAt(score, at, track, scratch);
    if (!segment || segment->tick() != at) {
        return written;
    }

    score->setNoteRest(segment, track, NoteVal(tones.front().pitch), Fraction::fromTicks(durationTicks),
                       DirectionV::AUTO, false, {}, false, &scratch);

    //! ⚠️ 段与和弦**重新找**：`setNoteRest()` 里的 `makeGap()` 会移除它替换掉的 ChordRest，而空掉的
    //! 段会被一并删掉 ⇒ 新和弦落在**另一个**段对象上，传进去那一个已经悬空（见函数头与 §4.8.4）。
    Measure* after = score->tick2measure(at);
    Segment* created = after ? after->findSegment(SegmentType::ChordRest, at) : nullptr;
    EngravingItem* createdItem = created ? created->element(track) : nullptr;
    Chord* chord = (createdItem && createdItem->isChord()) ? toChord(createdItem) : nullptr;
    if (!chord || chord->tick() != at) {
        return written;
    }

    Transaction& tx = score->transactionManager()->currentOrDummyTransaction();
    for (size_t i = 1; i < tones.size(); ++i) {
        if (chordHasPitch(chord, tones[i].pitch)) {
            continue;
        }

        NoteVal value(tones[i].pitch);
        NoteInput::addPitchToChord(tx, score, value, chord, &scratch, false);
    }

    //! 力度走 `Pid::USER_VELOCITY`（力度车道与 Properties 面板写的**同一个**属性）——
    //! 与 `applyRecordedChords()` 同一个理由：直接 `setUserVelocity()` 不进事务，撤销再重做就丢了。
    writeChordToneVelocities(created, track, tones);

    written.reserve(chord->notes().size());
    for (Note* note : chord->notes()) {
        written.push_back(note);
    }

    return written;
}

int deleteMidiNotes(Score* score, const std::vector<Note*>& notes, bool openCommand)
{
    if (!score || notes.empty()) {
        return 0;
    }

    //! 按**和弦**分组：删掉和弦的最后一个音 = 整个和弦变成同时值的休止符（小节永远是满的），
    //! 而和弦里还有别的音时只去掉被选中的那几个。这两种语义与记谱页按 Delete 完全一致，
    //! 也正是 `Score::deleteItem()` 的实现（这里分组只是为了不让"删和弦里一个音"变成"删整个和弦"）。
    std::vector<Chord*> chords;
    std::vector<std::vector<Note*> > selectedInChord;
    for (Note* note : notes) {
        Chord* chord = (note && note->chord()) ? note->chord() : nullptr;
        if (!chord || chord->isGrace()) {
            continue;
        }

        size_t index = 0;
        for (; index < chords.size(); ++index) {
            if (chords[index] == chord) {
                break;
            }
        }
        if (index == chords.size()) {
            chords.push_back(chord);
            selectedInChord.push_back({});
        }
        selectedInChord[index].push_back(note);
    }

    if (chords.empty()) {
        return 0;
    }

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Delete notes"));
    }

    int removed = 0;
    for (size_t i = 0; i < chords.size(); ++i) {
        Chord* chord = chords[i];
        const std::vector<Note*>& selected = selectedInChord[i];

        bool wholeChord = !chord->notes().empty() && chord->notes().size() == selected.size();
        if (wholeChord) {
            //! 整个和弦都不要了：换成一个同时值的休止符（`deleteItem` 的 CHORD 分支就是这么做的）。
            score->deleteItem(chord);
            removed += int(selected.size());
        } else {
            //! 只去掉这几个音：和弦留着（`deleteItem()` 的 NOTE 分支走的是同一个调用）。
            for (Note* note : selected) {
                score->undoRemoveElement(note);
                ++removed;
            }
        }
    }

    if (openCommand) {
        score->endCmd();
    }

    return removed;
}

bool insertMidiNote(Score* score, int staffIndex, int voice, int tick, int durationTicks, int pitch, bool openCommand,
                    std::vector<Note*>* insertedNotes)
{
    if (!score || pitch < 0 || pitch > 127 || durationTicks <= 0) {
        return false;
    }

    const int at = std::max(0, tick);
    const Fraction atFraction = Fraction::fromTicks(at);
    if (atFraction >= score->endTick() || !score->tick2measure(atFraction)) {
        return false;   // 谱面之外不写
    }

    //! 预判"这一下什么都不会发生"的情形，**免得压一个空的撤销步**：已经在这个和弦里 = 无操作。
    if (staffIndex >= 0 && size_t(staffIndex) < score->nstaves() && voice >= 0 && voice < int(VOICES)) {
        const Measure* measure = score->tick2measure(atFraction);
        const Segment* segment = measure ? measure->findSegment(SegmentType::ChordRest, atFraction) : nullptr;
        const track_idx_t track = staff2track(staff_idx_t(staffIndex)) + track_idx_t(voice);
        const EngravingItem* item = segment ? segment->element(track) : nullptr;
        if (item && item->isChord() && chordHasPitch(toChord(item), pitch)) {
            return false;
        }
    }

    std::vector<MidiChordTone> tones { { pitch, 0 } };

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Insert note"));
    }

    InputState scratch;
    const std::vector<Note*> written = writeChordAt(score, staffIndex, voice, at, durationTicks, tones, scratch);

    if (openCommand) {
        score->endCmd();
    }

    if (written.empty()) {
        return false;
    }

    if (insertedNotes) {
        *insertedNotes = written;
    }

    return true;
}

int moveMidiNotes(Score* score, const std::vector<MidiNoteMove>& moves, bool openCommand,
                  std::vector<Note*>* movedNotes)
{
    if (!score || moves.empty()) {
        return 0;
    }

    //! ① **先抄数据**：下面删完源音之后，手里这些 `Note*` 全部作废。
    struct PlannedMove {
        Note* source = nullptr;
        int staffIndex = 0;
        int voice = 0;
        int tick = 0;               //!< 目标 tick
        int durationTicks = 0;
        MidiNoteData data;
    };

    std::vector<PlannedMove> plans;
    plans.reserve(moves.size());

    const Fraction scoreEnd = score->endTick();   //! 删除不会缩短谱面，所以这一刻的末尾就是上限

    for (const MidiNoteMove& move : moves) {
        PlannedMove plan;
        if (!move.note || !readNoteData(move.note, plan.data)) {
            continue;
        }

        plan.source = move.note;
        plan.staffIndex = int(move.note->staffIdx());
        plan.voice = int(move.note->voice());
        plan.tick = std::max(0, move.tick);
        plan.durationTicks = move.durationTicks > 0 ? move.durationTicks : plan.data.durationTicks;

        if (Fraction::fromTicks(plan.tick) >= scoreEnd) {
            continue;   // 谱面之外不写
        }

        plans.push_back(plan);
    }

    if (plans.empty()) {
        return 0;
    }

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Move notes"));
    }

    //! ② 把**全部**源音先删掉，再在新位置重建 —— 这样"把 A 挪到 B 头上"这种批内重叠不会互相踩。
    std::vector<Note*> sources;
    sources.reserve(plans.size());
    for (const PlannedMove& plan : plans) {
        sources.push_back(plan.source);
    }
    deleteMidiNotes(score, sources, /*openCommand*/ false);

    //! ③ 重建。演奏层按**相对偏移**平移（"这个音比记谱起点晚 20 tick 出声"这件事跟着音走）。
    InputState scratch;
    int moved = 0;

    for (const PlannedMove& plan : plans) {
        const MidiNoteData& data = plan.data;
        const int playOffset = data.playTick - data.tick;
        const std::vector<MidiChordTone> tones { { data.pitch, data.velocity } };

        const std::vector<Note*> written = writeChordAt(score, plan.staffIndex, plan.voice, plan.tick,
                                                        plan.durationTicks, tones, scratch);
        if (written.empty()) {
            continue;
        }

        for (Note* note : written) {
            if (note->pitch() != data.pitch) {
                continue;
            }

            if (data.hasPlayOverride) {
                applyNotePlayOverride(score, note, plan.tick + playOffset, data.playDurationTicks,
                                      data.playVelocityPercent, /*openCommand*/ false);
            }

            if (movedNotes) {
                movedNotes->push_back(note);
            }
            ++moved;
        }
    }

    if (openCommand) {
        score->endCmd();
    }

    return moved;
}

int changeMidiNoteDurations(Score* score, const std::vector<std::pair<Note*, int> >& changes, bool openCommand)
{
    if (!score) {
        return 0;
    }

    //! 一个和弦只做一次：时值是 **ChordRest 级**的，和弦里的音不可能各自有时值（这也是唯一一处
    //! "改一个音会连带改同一个和弦里别的音"的编辑，界面上的说明写的就是这一点）。
    std::vector<std::pair<Chord*, int> > plan;
    for (const std::pair<Note*, int>& change : changes) {
        Note* note = change.first;
        if (!note || !note->chord() || note->chord()->isGrace()) {
            continue;
        }

        const int ticks = change.second;
        if (ticks <= 0) {
            continue;
        }

        Chord* chord = note->chord();
        if (chord->ticks() == Fraction::fromTicks(ticks)) {
            continue;   // 没变
        }

        bool seen = false;
        for (const std::pair<Chord*, int>& entry : plan) {
            if (entry.first == chord) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            plan.emplace_back(chord, ticks);
        }
    }

    if (plan.empty()) {
        return 0;
    }

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Change note length"));
    }

    int changed = 0;
    for (const std::pair<Chord*, int>& entry : plan) {
        //! 记谱页"选中音符换时值"用的就是这一个调用：变短自动补休止符、变长自动按小节切开并连音、
        //! 谱面不够长自动加小节 —— 全交给上游，我们不碰 segment。
        score->changeCRlen(entry.first, Fraction::fromTicks(entry.second));
        ++changed;
    }

    if (openCommand) {
        score->endCmd();
    }

    return changed;
}

std::vector<MidiClipboardNote> copyMidiNotes(const std::vector<Note*>& notes)
{
    std::vector<MidiNoteData> data;
    data.reserve(notes.size());

    for (Note* note : notes) {
        MidiNoteData item;
        if (readNoteData(note, item)) {
            data.push_back(item);
        }
    }

    std::vector<MidiClipboardNote> result;
    if (data.empty()) {
        return result;
    }

    int base = data.front().tick;
    for (const MidiNoteData& item : data) {
        base = std::min(base, item.tick);
    }

    //! 排序让粘贴时"同一个 tick 上的音"自然地挨在一起（它们要落成一个和弦）。
    std::stable_sort(data.begin(), data.end(), [](const MidiNoteData& a, const MidiNoteData& b) {
        if (a.tick != b.tick) {
            return a.tick < b.tick;
        }
        return a.pitch < b.pitch;
    });

    result.reserve(data.size());
    for (const MidiNoteData& item : data) {
        MidiClipboardNote entry;
        entry.tickOffset = item.tick - base;
        entry.durationTicks = item.durationTicks;
        entry.pitch = item.pitch;
        entry.velocity = item.velocity;
        entry.playOffsetTicks = item.playTick - item.tick;
        entry.playDurationTicks = item.playDurationTicks;
        entry.playVelocityPercent = item.playVelocityPercent;
        result.push_back(entry);
    }

    return result;
}

int pasteMidiNotes(Score* score, int staffIndex, int voice, int atTick, const std::vector<MidiClipboardNote>& notes,
                   bool openCommand, std::vector<Note*>* pastedNotes)
{
    if (!score || notes.empty() || staffIndex < 0 || size_t(staffIndex) >= score->nstaves()) {
        return 0;
    }
    if (voice < 0 || voice >= int(VOICES)) {
        return 0;
    }

    std::vector<MidiClipboardNote> ordered = notes;
    std::stable_sort(ordered.begin(), ordered.end(), [](const MidiClipboardNote& a, const MidiClipboardNote& b) {
        if (a.tickOffset != b.tickOffset) {
            return a.tickOffset < b.tickOffset;
        }
        return a.pitch < b.pitch;
    });

    const int base = std::max(0, atTick);
    const Fraction scoreEnd = score->endTick();

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Paste notes"));
    }

    InputState scratch;
    int written = 0;

    for (size_t i = 0; i < ordered.size();) {
        //! 同一个 tickOffset 的一组 = 一个和弦。
        size_t end = i;
        int durationTicks = 0;
        std::vector<MidiChordTone> tones;
        while (end < ordered.size() && ordered[end].tickOffset == ordered[i].tickOffset) {
            tones.push_back({ ordered[end].pitch, ordered[end].velocity });
            durationTicks = std::max(durationTicks, ordered[end].durationTicks);
            ++end;
        }

        const int tick = base + ordered[i].tickOffset;
        if (Fraction::fromTicks(tick) >= scoreEnd || durationTicks <= 0) {
            i = end;
            continue;
        }

        const std::vector<Note*> created = writeChordAt(score, staffIndex, voice, tick, durationTicks, tones, scratch);
        if (created.empty()) {
            i = end;
            continue;
        }

        //! 演奏层与力度一起粘回来：复制的是"这个音听起来是什么样"，不只是音高。
        for (Note* note : created) {
            if (pastedNotes) {
                pastedNotes->push_back(note);
            }
            ++written;

            for (size_t k = i; k < end; ++k) {
                if (ordered[k].pitch != note->pitch()) {
                    continue;
                }

                const int playTick = tick + ordered[k].playOffsetTicks;
                if (playTick != note->tick().ticks()
                    || ordered[k].playDurationTicks != note->chord()->actualTicks().ticks()
                    || ordered[k].playVelocityPercent != 100) {
                    applyNotePlayOverride(score, note, playTick, ordered[k].playDurationTicks,
                                          ordered[k].playVelocityPercent, /*openCommand*/ false);
                }
                break;
            }
        }

        i = end;
    }

    if (openCommand) {
        score->endCmd();
    }

    return written;
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

        //! 到达段的弯折控制（二次贝塞尔的弯折点）；`ArrivalFromPrevious` 的点没有它。
        if (const std::optional<AutomationPoint::Ease> bend = ease(point)) {
            item.hasEase = true;
            item.controlT = double(bend->t);
            item.controlValue = double(bend->value);
            item.arrival = double(std::get<AutomationPoint::ExplicitArrival>(point.value.inValue).value);
        } else {
            item.arrival = item.value;
        }

        result.push_back(item);
    }

    return result;
}

int applyAutomationPoints(Score* score, int staffIndex, const std::vector<MidiAutomationPoint>& points, bool openCommand)
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

        //! ⚠️ 到达值必须写 `ExplicitArrival { 自己的值, Ease::none() }`，**不能**留默认的
        //! `ArrivalFromPrevious`：后者的含义是"到达值 = 前一个点的值"，于是这一段是**平的**，
        //! 值在点处**跳变** —— 一串这样的点画出来是**阶梯**，不是渐强（2026-10-03 用户实测后
        //! 要求改成贝塞尔控制，根子就在这里）。写显式的到达值，这一段才是"从前一点的值斜到本点"
        //! ✓ 与记谱页新增点完全一致（NotationAutomationController::requestAddPoint 也是这么写的）。
        //!
        //! `Ease::none()` = 弯折点在段中点且不弯 = 直线；用户拖手柄才会换成别的弯折点。
        //! `generated` stays false and no `itemId` is set: this is the user's own point now, exactly like
        //! a point added on the notation page. Being authored is also what makes it win over a generated
        //! point at the same tick - the score's generation step leaves an authored point alone.
        written.value.inValue = AutomationPoint::ExplicitArrival { value, AutomationPoint::Ease::none() };

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
    //! the curve and leaves nothing to undo - the stroke would be un-undoable.
    //! `openCommand == false` 时事务由调用方开（MIDI 页用 notation 的 undo stack，理由见头文件）。
    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Draw dynamics curve"));
    }
    score->editAutomationPoints(key, edits);
    if (openCommand) {
        score->endCmd();
    }

    //! 观测点：写入之后曲线里到底有多少点、我们写的那个 tick 在不在 ——
    //! 用户报「新建的点切页后消失，是不是从来没建成功」，这一行就是答案。
    {
        const AutomationDataConstPtr after = score->automationData();
        const size_t count = after ? after->curve(key).size() : 0;
        std::string ticks;
        if (after) {
            for (const auto& [t, p] : after->curve(key)) {
                ticks += std::to_string(t) + " ";
            }
        }
        LOGW() << "[midi-automation] wrote " << edits.size() << " point(s); curve now has "
               << count << ": " << ticks;
    }

    return int(edits.size());}

bool eraseAutomationPoint(Score* score, int staffIndex, int tick, bool openCommand)
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
    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Remove dynamics point"));
    }
    score->editAutomationPoints(key, edits);
    if (openCommand) {
        score->endCmd();
    }

    return true;
}

std::optional<AutomationPoint> bentAutomationPoint(const AutomationPoint& existing, double t, double value)
{
    const AutomationPoint::Ease bend { real_t::make(std::clamp(t, 0.0, 1.0)),
                                       real_t::make(std::clamp(value, 0.0, 1.0)) };

    const std::optional<AutomationPoint::Ease> currentBend = ease(existing);
    if (currentBend.has_value() && *currentBend == bend
        && !existing.generated && !existing.itemId.has_value()) {
        return std::nullopt;   // 弯折点没变、这个点也已经是用户的：不写，也不压一个空的撤销步
    }

    AutomationPoint written = existing;

    //! 到达值取哪一个，是这里唯一的判断：
    //!  * 原本就显式写过（渐强线的终点、记谱页编辑过的点）→ **保留它**，只换弯折 —— 别把上游
    //!    或记谱页的语义丢掉；
    //!  * 原本是 `ArrivalFromPrevious`（一段"平的"跳变）→ 升级成"到达本点的值"，这样拖手柄
    //!    才真的把这一段变成弯的；否则这一段的 range 是 0，怎么弯都是平的（白拖）。
    const real_t arrival = currentBend.has_value()
                           ? std::get<AutomationPoint::ExplicitArrival>(written.value.inValue).value
                           : written.value.outValue;
    written.value.inValue = AutomationPoint::ExplicitArrival { arrival, bend };

    //! ⚠️ **接管**：碰过一下，这个点就是用户的了（理由见 applyAutomationPointMove）。
    written.generated = false;
    written.itemId = std::nullopt;

    return written;
}

bool applyAutomationPointEase(Score* score, int staffIndex, int tick, double t, double value, bool openCommand)
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

    const std::optional<AutomationPoint> written = bentAutomationPoint(it->second, t, value);
    if (!written.has_value()) {
        return false;
    }

    AutomationPointEdits edits { { tick, AutomationPointEdit::SetPoint { *written } } };

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Bend dynamics curve"));
    }
    score->editAutomationPoints(key, edits);
    if (openCommand) {
        score->endCmd();
    }

    return true;
}

bool applyAutomationPointMove(Score* score, int staffIndex, int fromTick, int toTick, double value, bool openCommand)
{
    const AutomationCurveKey key = dynamicsKey(score, staffIndex);
    if (!score || !key.isValid() || fromTick < 0 || toTick < 0) {
        LOGW() << "[midi-automation] move refused: bad key/ticks key=" << key.isValid()
               << " from=" << fromTick << " to=" << toTick;
        return false;
    }

    const AutomationDataConstPtr data = score->automationData();
    if (!data) {
        LOGW() << "[midi-automation] move refused: no automation data";
        return false;
    }

    const AutomationCurve& curve = data->curve(key);
    const AutomationCurve::const_iterator it = curve.find(fromTick);
    if (it == curve.end()) {
        //! 观测点：**这是"新增后立刻拖动失败"最可能的现场** —— 说明按下时写进去的点，
        //! 到松手时已经不在曲线里了（打印当前曲线上的 tick，便于对比）。
        std::string ticks;
        for (const auto& [t, p] : curve) {
            ticks += std::to_string(t) + " ";
        }
        LOGW() << "[midi-automation] move refused: fromTick " << fromTick << " not in curve, curve has: " << ticks;
        return false;
    }

    const real_t movedValue = real_t::make(std::clamp(value, 0.0, 1.0));
    if (fromTick == toTick && it->second.value.outValue == movedValue) {
        return false;   // 原地没动：不写
    }

    AutomationPoint moved = it->second;
    //! 到达值就是"这一段结束在本点的值"，值一改它就得跟着改 —— 否则拖动一个点会把**上一段**
    //! 的形状弄拧（那段会停在旧值上）。原本显式写过的保留它的弯折，只换值。
    if (const std::optional<AutomationPoint::Ease> bend = ease(moved)) {
        moved.value.inValue = AutomationPoint::ExplicitArrival { movedValue, *bend };
    }
    moved.value.outValue = movedValue;

    //! ⚠️ **接管**：拖一下记号生成的点，它就从"记号的"变成"用户的"。
    //! 不清这两个标志的话，拖动只是把记号点临时搬走 —— 下一次重建（记号还在）会在原 tick
    //! 重新生成一个，用户看到的就是"松手又弹回来"（2026-10-03 用户报的现象）。
    //! 与"新增点即接管"是同一条规则（见 applyAutomationPoints 里的说明）。
    moved.generated = false;
    moved.itemId = std::nullopt;

    //! 上游的 MovePoint：写到新 tick，并把原 tick 上的点删掉。
    AutomationPointEdits edits { { toTick, AutomationPointEdit::MovePoint { moved, fromTick } } };

    if (openCommand) {
        score->startCmd(TranslatableString("midieditor", "Move dynamics point"));
    }
    score->editAutomationPoints(key, edits);
    if (openCommand) {
        score->endCmd();
    }

    return true;
}

MidiLoopRange midiLoopRangeFromDrag(int draggedTick, int releasedTick, int totalTicks, int snapTicks)
{
    MidiLoopRange range;

    const int snap = std::max(1, snapTicks);
    const int lastTick = std::max(0, totalTicks);

    //! Snapping happens first, so both ends land on the grid whatever the drag did - and so the
    //! BoundaryTick trap described in the header cannot be reached (a snapped tick is either 0 or at
    //! least one grid step, never 1 or 2).
    const auto snapped = [snap, lastTick](int tick) {
                             const int step = int(std::lround(double(tick) / double(snap))) * snap;
                             return std::min(std::max(0, step), lastTick);
                         };

    const int a = snapped(draggedTick);
    const int b = snapped(releasedTick);

    range.inTick = std::min(a, b);
    range.outTick = std::max(a, b);

    //! A drag that never left its grid step is a click, not a range. Without this, a plain click in
    //! the ruler would leave a loop of zero length behind and quietly swallow the seek.
    range.valid = (range.outTick - range.inTick) >= snap;

    return range;
}

double midiFollowScrollX(double scrollX, double playheadX, double viewportWidth, double maxScrollX)
{
    if (viewportWidth <= 0.0) {
        return scrollX;
    }

    const double margin = viewportWidth * midiFollowMargin;

    //! Inside the band: hands off. Following on every single tick would make the score creep under the
    //! playhead even when it is perfectly readable, and would fight the user's own scrolling.
    if (playheadX >= margin && playheadX <= viewportWidth - margin) {
        return scrollX;
    }

    //! Outside it: put the playhead back at the left end of the band, i.e. scroll by exactly what is
    //! missing. The clamp matters near both ends of the score, where the requested offset simply
    //! cannot be honoured.
    const double wanted = scrollX + (playheadX - margin);
    return std::min(std::max(0.0, wanted), std::max(0.0, maxScrollX));
}
}
