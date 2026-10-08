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

#include <optional>
#include <utility>
#include <vector>

#include "midirecorder.h"

namespace mu::engraving {
class Score;
class Note;
struct AutomationPoint;
struct AutomationCurveKey;
}

namespace mu::notation {
//! NOTE: One rectangle of the piano roll. It keeps the `Note*` it came from, because that is what
//!       the edits are applied to.
struct MidiNoteItem {
    engraving::Note* note = nullptr;

    int tick = 0;
    int durationTicks = 0;
    int pitch = 0;
    int velocity = 0;       //!< as shown in the roll: an unset velocity (0) is shown as 64
    int staffIndex = 0;
    int voice = 0;

    //! Whether this note carries its OWN velocity (Properties `Pid::USER_VELOCITY`).
    //!
    //! This is what makes "tweak one note" possible without touching the engine's driving logic: an
    //! own velocity becomes `ExpressionContext::velocityOverride`, which the synthesisers prefer over
    //! the dynamic level. A note WITHOUT one keeps following the dynamic marks (pp/ff, hairpins).
    //! So this flag is exactly "this note no longer follows the dynamics".
    bool hasVelocityOverride = false;

    //! The "played" layer (`Note::playEvents()`): where the note actually sounds and for how long.
    //! When `hasPlayOverride` is false the played values equal the notated ones and the roll draws a
    //! plain block. Dorico makes the same distinction between played and notated durations.
    bool hasPlayOverride = false;
    int playTick = 0;
    int playDurationTicks = 0;
    int playVelocityPercent = 100;      //!< velocityMultiplier in percent; 100 means untouched
};

//! The span of one measure, used for the bar lines and the measure numbers of the ruler.
struct MidiMeasureItem {
    int tick = 0;
    int endTick = 0;
};

//! Flattens a score into the note rectangles the roll draws.
//!
//! Deliberately a free function that takes a plain `Score*`: it needs no IoC context, so it can be
//! unit tested against a real score (which is how "the MIDI page and the notation page share one
//! data source" is pinned down).
std::vector<MidiNoteItem> collectMidiNotes(const engraving::Score* score);

//! The measure spans of a score, in order.
std::vector<MidiMeasureItem> collectMidiMeasures(const engraving::Score* score);

//! Writes a pitch back into the score, reusing the very command the notation editor uses, so the
//! linked notes stay in sync and undo keeps working. No-op (returns false) when nothing changes.
//!
//! ⚠️ `openCommand`：默认 `true` = 自己开事务（测试与独立调用走这条）。MIDI 页传 **`false`**，
//! 由调用方包在 `INotationUndoStack::transaction()` 里 —— 那条路会在提交后**通知"栈变了"**，
//! 撤销/重做命令的状态（以及主菜单那条 Ctrl+Z）才会跟上；engraving 的 `Score::startCmd/endCmd`
//! 虽然用同一个 undo stack，却**不经过那个通知**（2026-10-03 用户报「Undo 是灰的」的根因，
//! 2026-10-05 又查到它就是「MIDI 页 Ctrl+Z 有按键痕迹却撤销不了」的根因）。
//! 下面四个写入函数都有这个参数，含义相同。
bool applyNotePitch(engraving::Score* score, engraving::Note* note, int pitch, bool openCommand = true);

//! Writes a velocity back into the score, through the same property the Properties panel writes.
//!
//! `velocity` may be 0, which means "no own velocity" - the note goes back to following the dynamic
//! marks of the score. That is the only way to undo a per-note tweak, so it is allowed on purpose.
bool applyNoteVelocity(engraving::Score* score, engraving::Note* note, int velocity, bool openCommand = true);

//! One velocity write for each pair, all under a SINGLE command.
//!
//! This is what a brush stroke needs, and the difference is not cosmetic. Every `startCmd`/`endCmd`
//! pair notifies the whole score, and the subscribers to that notification rebuild things that cost
//! O(score) - the notation view repaints, the playback events are rebuilt. Opening one command per
//! note therefore made a stroke over N notes cost N full-score rebuilds, which is exactly why
//! drawing more notes took proportionally longer. One command means one notification.
//!
//! A pair whose note already has that velocity is skipped, so the count returned may be smaller than
//! the number of pairs. Returns how many notes were actually changed.
int applyNoteVelocities(engraving::Score* score, const std::vector<std::pair<engraving::Note*, int> >& changes,
                        bool openCommand = true);

//! One pitch write for each pair, all under a SINGLE command - the batch form of `applyNotePitch()`.
//!
//! Same reason as `applyNoteVelocities()`: a drag that moves a whole selection by three semitones must
//! cost ONE notification of the score, not one per note. A pair whose note already has that pitch is
//! skipped, so the returned count may be smaller than the number of pairs.
int applyNotePitches(engraving::Score* score, const std::vector<std::pair<engraving::Note*, int> >& changes,
                     bool openCommand = true);

//! One PLAYED-velocity write for each pair, all under a SINGLE command.
//!
//! This is `NoteEvent::velocityMultiplier` - the "played" layer's own loudness, in percent (100 =
//! untouched) - and NOT `Pid::USER_VELOCITY` (`applyNoteVelocities`). The two are different things:
//! the property scales/overrides the dynamic level of the note, while the multiplier scales the
//! dynamic level the note would otherwise get. The roll edits them in two channels of one lane.
//!
//! ⚠️ Only the multiplier is touched here: `ontime` / `len` of the event are left exactly as they are,
//! so setting a played velocity never moves the note's played timing.
//! Returns how many notes were actually changed; a pair that already holds that value is skipped, and
//! a batch that changes nothing writes nothing at all (no empty undo step).
int applyNotePlayVelocities(engraving::Score* score, const std::vector<std::pair<engraving::Note*, int> >& changes,
                            bool openCommand = true);

//! Writes the "played" timing of a note - the piano roll's played layer, i.e. `ontime` and `len` of
//! the first `NoteEvent`. `startTick` and `durationTicks` are absolute ticks; `velocityPercent` is
//! the velocity multiplier in percent (100 = untouched). Uses the existing `ChangeNoteEventList`
//! undo command, so it is undoable and does not invent a second data model.
//! Returns false when nothing changes.
bool applyNotePlayOverride(engraving::Score* score, engraving::Note* note,
                           int startTick, int durationTicks, int velocityPercent, bool openCommand = true);

//! 一个"只用来发声"的临时音符（见 `midiNoteToAudition()`）。`note` 是要交给
//! `IPlaybackController::playElements()` 的那个音符，`chord` 是它的宿主 —— **删除 `chord` 即可**，
//! 它会连同 `note` 一起删掉（`Chord::~Chord()` 里 `DeleteAll(m_notes)`）。
struct MidiAuditionNote {
    engraving::Note* note = nullptr;
    engraving::Chord* chord = nullptr;

    bool isValid() const { return note != nullptr; }
};

//! 造一个音高可以**不是**谱面上那个的临时音符，用来在拖动中试听"拖到的音高"。
//!
//! 卷帘窗拖动改音高只预览、不写谱（松手才提交），而播放层从 engraving 模型渲染事件 —— 直接播原来
//! 那个 `Note*` 的话，响的永远是拖动前的音高。记谱页拖动之所以响的是拖到的音，是因为它的
//! `viewInteraction()->drag()` 实时改谱；这一页不那样做，所以走"临时元素"。
//!
//! 除了音高，其余（track / staffIdx / voice / 位置）都从**原音符**抄过来，播放层才认得出这条轨道。
//! `pitch` 会夹到 0..127。返回的 `chord` 由调用方删除；音符为空表示"没法发声"（不抛错）。
MidiAuditionNote midiNoteToAudition(engraving::Score* score, engraving::Note* note, int pitch);

//! ── 实时录制的写回 ──────────────────────────────────────────────────────────────
//!
//! `MidiRecordedChord`（量化 + 分声部之后的演奏）写进乐谱，**整笔一个命令**
//! （`openCommand == false` 时由调用方开事务 —— 与上面几个写入函数同一个约定，
//! 理由见 `applyNotePitch` 上面那段）。
//!
//! 用的是**记谱页输入音符时的同一套机制**（`Score::setNoteRest`）：跨小节的音自动拆成连音线、
//! 小节写满自动加小节、整个过程可撤销 —— 不另造一套"往谱里插音符"的路。
//!
//! `baseVoice` 是这场录制**从哪个声部起**（MIDI 页传 0），和弦自己的 `voice` 在此之上累加、
//! 超出 `VOICES` 就并回最后一个声部（见 `buildRecordedChords`）。
//!
//! ⚠️ **替换的边界**：写一个音会把它**盖住的那段时值**换成新音符，其余部分不动。
//! 具体说，落在既有音符中间的那个音会先把既有音符从该处切开（前半按原样保留，
//! 但**只保留音高与力度** —— 连音线、记号、附点这些不在"复刻"之列），后半原样留着。
struct MidiRecordedWriteResult {
    int chordsWritten = 0;
    int notesWritten = 0;

    //! 放不进去的（超出谱面末尾、位置切不开…）。**必须报出来**：丢音是静默的，
    //! 用户只会觉得"录少了"，而不知道是哪一段放不下。
    int chordsSkipped = 0;

    //! 最后一个写进去的音的起点（-1 = 一个都没写）。给"录到哪了"的提示用。
    int lastTick = -1;
};

MidiRecordedWriteResult applyRecordedChords(engraving::Score* score, int staffIndex, int voice,
                                            const std::vector<MidiRecordedChord>& chords, bool openCommand = true);

//! ── 结构性编辑：增 / 删 / 移 / 改记谱时长 / 复制粘贴 ─────────────────────────────────────────
//!
//! 前四组编辑（音高、力度、演奏起点、演奏时长）只**改属性**，音符对象不动。这一组不同：它们改的是
//! **谱面结构**（哪一段上有没有和弦），所以一切都要走**上游输入音符那套**（`Score::setNoteRest` /
//! `Score::changeCRlen` / `Score::deleteItem`）—— 小节完整性、连音线、休止符补齐、连动谱表都由它们
//! 负责，我们自己拼 segment 就会做出「测试全绿但工程打不开」的东西（`维护手册.md` §4.1）。
//!
//! ⚠️ 两条**贯穿全组**的纪律：
//!  ① **`Segment*` / `Note*` 不能跨一次写入复用** —— 写一个音会让被替换的 ChordRest 连同它的段一起
//!     被删掉（`setNoteRest()` 内 `makeGap()` 的副作用），所以每一步都要**从乐谱里按 tick 重新找**；
//!     音符对象更是会被整个换掉（新音符是新对象）。
//!  ② 所以凡是"先删再写"的操作，**必须先把要保留的数据抄出来**（`MidiNoteData`），
//!     写完之后手里的旧 `Note*` 一律作废。

//! 一个音符的**全部可编辑数据**，与它所在的 `Note*` 解绑。
//!
//! 存在的理由是上面那条纪律：结构性编辑会造出**新的**音符对象，所以"等一下还要写回去的东西"
//! 必须先抄成值。`tick` 是绝对值（复制粘贴用的相对偏移在 `MidiClipboardNote` 里）。
struct MidiNoteData {
    int tick = 0;
    int durationTicks = 0;
    int pitch = 0;
    int velocity = 0;               //!< `Pid::USER_VELOCITY`；0 = 没有自己的力度（跟随表情记号）
    bool hasPlayOverride = false;   //!< 演奏层是不是"被改过"（中性事件不算改过）
    int playTick = 0;
    int playDurationTicks = 0;
    int playVelocityPercent = 100;
};

//! 复制到剪贴板里的一个音：位置是**相对**最早那个音的偏移，所以粘贴时整块跟着落点走。
struct MidiClipboardNote {
    int tickOffset = 0;
    int durationTicks = 0;
    int pitch = 0;
    int velocity = 0;

    //! 演奏层相对**自己记谱起点**的偏移（可以 != 0：演奏起点可以早于/晚于记谱起点）。
    int playOffsetTicks = 0;
    int playDurationTicks = 0;
    int playVelocityPercent = 100;
};

//! 把一个音符移到另一个 tick（可同时改记谱时长与音高）。
//! `durationTicks <= 0` = 保持原时长；`pitch < 0` = 保持原音高（**0 是合法音高**，不能用 0 表示"不改"）。
//!
//! ⚠️ 为什么音高也挤进这条"移动"的路：Cubase 式**移动工具**一次手势既改时间又改音高，而
//! "一次手势 = 一个命令"（§4.8）⇒ 两件事必须在**同一个事务**里做完。
//! ⚠️ 但**只改音高**（tick 不变）时**不要**走这里：这条路是"删了重建"，会把连音线/记号丢掉 ——
//! 用 `applyNotePitch()` / `applyNotePitches()`（只改属性、不换对象）。
struct MidiNoteMove {
    engraving::Note* note = nullptr;
    int tick = 0;
    int durationTicks = 0;
    int pitch = -1;
};

//! 删掉这些音，**整批一个命令**。
//!
//! 语义与记谱页按 Delete 完全一致（走 `Score::deleteItem()`）：和弦里还有别的音时只去掉这一个音；
//! 它是和弦最后一个音时，整个和弦换成**同时值的休止符** —— 小节因此永远是满的（`sanityCheck` 不会
//! 报 `Incomplete measure`）。返回真正删掉的音数。
//!
//! ⚠️ 删除会连带删段、换对象：调用方手里的 `Note*` 与"行号"在这一次调用之后**全部作废**。
int deleteMidiNotes(engraving::Score* score, const std::vector<engraving::Note*>& notes, bool openCommand = true);

//! 在 (staff, voice, tick) 处放一个音，时值 `durationTicks`。
//!
//! 三条边界，都是**故意**的：
//!  * 那个 tick 上**已经有和弦**时，音是**加到这个和弦上**（变成和弦音），时值参数被忽略 —— 这正是
//!    记谱页"再点一个音就是加和弦音"的行为；
//!  * 那个 tick 上只有休止符/空隙时，走 `setNoteRest()`：它会切开前面的音符、按需要补休止符；
//!  * **谱面之外一律不写**（不会为了一个点击凭空长出几十小节），返回 false。
//! 同一个音高已经在那个和弦里时是**无操作**（返回 false），不会写出重复音。
//! `insertedNotes`（可选）拿回新建出来的音符 —— 视图要"插入即选中"，而插入可能建出连音线
//! （跨小节时不止一个音符），所以只有乐谱自己知道该选哪些。
bool insertMidiNote(engraving::Score* score, int staffIndex, int voice, int tick, int durationTicks, int pitch,
                    bool openCommand = true, std::vector<engraving::Note*>* insertedNotes = nullptr);

//! 移动（可同时改时值 / 音高）一批音，**整批一个命令**。
//!
//! 做法是**先把要保留的数据抄出来 → 删掉全部源音 → 在新位置上重建**：这样"把 A 挪到 B 头上"这种
//! 批内重叠不会互相踩。代价（如实记录，与录制的写回边界同源）：**连音线、记号、附点不在复刻之列**，
//! 音高 / 力度 / 演奏层是保留的（演奏层按**相对**偏移平移）。
//!
//! 落在谱面之外的会被跳过（返回值 = 真正移动成功的音数）。
//!
//! `movedNotes`（可选）拿回**新建出来的**音符：结构编辑会换对象，视图的选中要跟着搬到新对象上，
//! 否则松手之后选中就空了。
int moveMidiNotes(engraving::Score* score, const std::vector<MidiNoteMove>& moves, bool openCommand = true,
                  std::vector<engraving::Note*>* movedNotes = nullptr);

//! 改一批音的**记谱时长**，整批一个命令。
//!
//! 走 `Score::changeCRlen()`（记谱页在选中音符上换时值用的**同一个调用**），所以：变短时自动补休止符、
//! 变长时自动按小节切开并生成连音线、谱面不够长时自动加小节 —— 全部由上游负责。
//! ⚠️ 时值是 **ChordRest 级**的：改一个和弦音 = 改整个和弦（同一个和弦里的音不可能各自有时值）。
//! 同一个和弦被请求多次时只做第一次。返回真正改动的和弦数。
int changeMidiNoteDurations(engraving::Score* score, const std::vector<std::pair<engraving::Note*, int> >& changes,
                            bool openCommand = true);

//! ── 剪刀：在 `atTick` 处把一个音切开 ────────────────────────────────────────────────────────────
//!
//! 切开的是**整个和弦**（同一 tick 上的音本来就是一个 `ChordRest`，只切一个音会让两边对不上），
//! 新生成的两半之间自动加**连音线** ⇒ **声音完全不变**（这正是"切一刀"该有的语义，也是记谱页表示
//! "一个音写成多个音符"的唯一办法）。要"重新起音"的切法（Cubase 那种两个独立音）本函数**不做**。
//!
//! 做法：① 先把和弦**截短**成前半（`changeCRlen()` —— 原有的连音线/记号/附点因此全都留着）；
//! ② 在切点重建后半（`writeChordAt()`）；③ 逐音高补连音线（`Factory::createTie()` +
//! `undoAddElement()`，与 `Score::createCRSequence()` 同一个recipe）。
//!
//! `atTick` 必须**严格落在音的内部**（<= 起点或 >= 终点都是无操作，返回 false）——
//! 用户切在边上的那一下不该把音弄坏。`rightHalf`（可选）拿回后半段的音，视图要选中它们。
bool splitMidiNote(engraving::Score* score, engraving::Note* note, int atTick, bool openCommand = true,
                   std::vector<engraving::Note*>* rightHalf = nullptr);

//! 读出这些音的完整数据（复制的内容）。**纯读**：不改乐谱、不开命令。
//! 位置已换算成相对**最早那个音**的偏移，顺序按 tick 排好。
std::vector<MidiClipboardNote> copyMidiNotes(const std::vector<engraving::Note*>& notes);

//! 把剪贴板的内容粘到 `atTick` 处（`atTick` 对应剪贴板里最早的音），整批一个命令。
//! 力度与演奏层一起写回；谱面之外的部分跳过。返回真正写进去的音数。
//! 同一个 `tickOffset` 上的音会**落成一个和弦**（与复制时的形状一致）。
//! `pastedNotes`（可选）拿回新建出来的音符，理由与 `moveMidiNotes` 相同。
int pasteMidiNotes(engraving::Score* score, int staffIndex, int voice, int atTick,
                   const std::vector<MidiClipboardNote>& notes, bool openCommand = true,
                   std::vector<engraving::Note*>* pastedNotes = nullptr);

//! One point of the Dynamics automation curve of a staff: a tick, and a level in 0..1.
struct MidiAutomationPoint {
    int tick = 0;
    double value = 0.0;

    //! True when the point is the user's own, i.e. the lane may remove it again. False when the score
    //! derived it from an engraving item - a Dynamic mark, a hairpin - because those are regenerated on
    //! every rebuild, so removing one is not the lane's to do.
    //! Only meaningful when READ back; a write always writes an authored point.
    bool authored = true;

    //! 该点"到达段"的弯折控制 —— 上游 `AutomationPoint::Ease`，也就是**二次贝塞尔曲线的弯折点**：
    //! `controlT` 是弯折位置（沿这一段的横向比例 0..1），`controlValue` 是弯折处的纵向比例 0..1
    //! （相对该段起止值）。`muse::mpe::evaluateAt()` 把一段拆成两条在弯折点相切的二次贝塞尔弧，
    //! 所以 `{0.5, 0.5}` = 直线，`{0.3, 0.8}` = 先慢后快的那种弯。
    //!
    //! `hasEase == false` 表示这一点是 `ArrivalFromPrevious`（到达值等于前一点，即连续斜坡、无弯折）。
    //! 只有 `hasEase` 为真时才写 `ExplicitArrival` —— 这样"没动过曲率的点"在文件里形状不变。
    //! 与 `authored` 一样，**只在读回时有意义**。
    bool hasEase = false;
    double controlT = 0.5;
    double controlValue = 0.5;

    //! 该点的"到达值"（`ExplicitArrival::value`）—— **到达这一点的那个段结束时的值**。
    //! 上游求值 `evaluateAt(point, prevOut, t)` 用的就是它（`ArrivalFromPrevious` 时取 prevOut）。
    //! QML 要复刻同一份公式来画曲线，所以必须把这个值给出去：否则屏幕上的线与合成器听到的
    //! 不是同一条（"看到的 ≠ 听到的"是这类编辑器最难查的一类 bug）。
    //! `hasEase == false` 时这个字段没有意义。
    double arrival = 0.0;
};

//! The Dynamics automation curve of one staff, in tick order - what a crescendo, a diminuendo or an
//! fp really is.
//!
//! NOTE: this is not a second curve of our own. It is the one the notation page draws next to the
//! mixer, addressed with the same key (`AutomationCurveKey::staff(Dynamics, staff->id())`), and the one
//! `MuseSamplerSequencer::loadDynamicEvents()` plays - so both pages edit one thing rather than two.
//!
//! Deliberately a free function taking a plain `Score*`, for the same reason `collectMidiNotes` is one:
//! it needs no IoC context, so "both pages address the same curve" is pinned by a unit test instead of
//! by a comment.
std::vector<MidiAutomationPoint> collectAutomationPoints(const engraving::Score* score, int staffIndex);

//! Writes points of that curve as ONE undoable command - one notification for the whole stroke, for the
//! reason spelled out at `applyNoteVelocities`.
//!
//! A point that already holds that value is left alone; when nothing at all changes, nothing is written,
//! because a stroke that changes nothing must not cost an undo step either. Returns how many points were
//! written.
//!
//! ⚠️ `openCommand`：默认 `true` = 自己开事务（测试与独立调用走这条）。MIDI 页传 **`false`**，
//! 由调用方包在 `INotationUndoStack::transaction()` 里 —— 那条路会在提交后**通知"栈变了"**，
//! 撤销/重做命令的状态（以及主菜单那条 Ctrl+Z）才会跟上；engraving 的 `Score::startCmd/endCmd`
//! 虽然用同一个 undo stack，却**不经过那个通知**（2026-10-03 用户报「Undo 是灰的」的根因）。
//! 下面四个写入函数都有这个参数，含义相同。
int applyAutomationPoints(engraving::Score* score, int staffIndex, const std::vector<MidiAutomationPoint>& points,
                          bool openCommand = true);

//! Removes the point at `tick`, if it is the user's own.
//!
//! A point the score derived from a Dynamic mark or a hairpin is NOT removable: the score regenerates it
//! on the next rebuild, so erasing it here would look like a dead gesture - and would take the mark's own
//! shape away until that rebuild. The notation page's lane refuses exactly the same points
//! (NotationAutomationController::requestRemovePoint), and the two pages must not disagree about what is
//! the user's to delete. Returns false when nothing was removed.
bool eraseAutomationPoint(engraving::Score* score, int staffIndex, int tick, bool openCommand = true);

//! 把某个点的"到达段"弯折控制写成 `ExplicitArrival { outValue, Ease { t, value } }` ——
//! 也就是拖手柄调曲率。点的出值（`outValue`）保持不变，只有弯折点变。
//!
//! `t` / `value` 都夹到 0..1；`t` 贴到 0 或 1 时上游按"无弯折"处理（见 `muse::mpe::evaluateAt`），
//! 所以这里不做特殊处理，交给同一个求值函数。
//! 点不存在、不是本谱表的曲线、或值没变时返回 false（**不写**、不压撤销步）。
bool applyAutomationPointEase(engraving::Score* score, int staffIndex, int tick, double t, double value,
                              bool openCommand = true);

//! 拖手柄之后那个点应该长什么样 —— **这就是"改曲率"的全部语义**，两个页面共用：
//!  * 到达值（`ExplicitArrival::value`）取原本就显式写过的那个，**不改成 outValue** ——
//!    渐强线终点、记谱页编辑过的点都带着自己的到达值，那是它们的语义，拖手柄不该把它丢掉；
//!  * 原本是 `ArrivalFromPrevious`（"到达值 = 前一点"，即一段平的跳变）的点没有显式到达值，
//!    升级成"到达本点的值"，否则这一段的 range 是 0，怎么弯都是平的（白拖）；
//!  * **接管**：碰过一下这个点就是用户的了（清 `generated` / `itemId`）—— 否则记号会在下一次
//!    重建时把旧形状原样生成回来，用户看到的是"拖了又弹回去"。
//!
//! 返回 **nullopt** 表示"没什么可写"（弯折点一模一样、而且这个点已经是用户的），
//! 调用方据此**不要开命令** —— 不然一次没改变任何东西的手势也会压一个空的撤销步。
//!
//! 刻意做成纯函数（不碰 score、不开事务）：两个页面的事务机制不同且**必须**不同
//! （记谱页走 `INotationUndoStack::transaction()`，MIDI 页走同一套但由调用方开），
//! 所以共用的只能是"写成什么"，不是"怎么提交"。
std::optional<engraving::AutomationPoint> bentAutomationPoint(const engraving::AutomationPoint& existing,
                                                              double t, double value);

//! 把一个点移到另一个 tick（可同时改值），走上游的 `MovePoint`：目标 tick 上的点被它取代，
//! 原 tick 上的点消失 —— 这就是"拖动控制点"。
//! `fromTick` 上没有点时返回 false（不写）。
//! NOTE: 名字与 `applyAutomationPoints` / `applyAutomationPointEase` 同族，**刻意不叫
//!       `moveAutomationPoint`** —— 模型里有同名成员函数，成员会遮蔽外层同名自由函数，
//!       在成员函数体里调用时重载解析直接失败（编译期就报，不会静默）。
bool applyAutomationPointMove(engraving::Score* score, int staffIndex, int fromTick, int toTick, double value,
                              bool openCommand = true);

//! The default velocity shown for a note the user has never given an explicit velocity.
int midiDisplayVelocity(int userVelocity);

//! The loop range a drag in the roll's ruler describes, in score ticks.
struct MidiLoopRange {
    int inTick = 0;
    int outTick = 0;

    //! False when the drag was too short to mean a range (i.e. it was a click).
    //!
    //! That is not a detail of the maths but the whole gesture: "click in the ruler = play from here"
    //! and "drag in the ruler = loop this" share one strip, so something has to tell them apart, and
    //! one grid step is both the natural threshold and the smallest range worth looping.
    bool valid = false;
};

//! Turns a ruler drag (in raw ticks, in whichever direction) into a loop range.
//!
//! Both ends snap to `snapTicks` and are clamped to `0..totalTicks`, the order of the two ticks does
//! not matter, and a drag that stays inside one grid step comes back `valid == false` so the caller
//! can treat it as a click instead.
//!
//! ⚠️ **Snapping is also what keeps the loop API out of a trap of its own**: upstream
//! `INotationPlayback::addLoopBoundary()` reads the tick as one of `BoundaryTick` when it is 0, 1 or 2
//! (`FirstScoreTick` / `SelectedNoteTick` / `LastScoreTick`, see inotationplayback.h) - so a loop
//! boundary of "tick 1" silently means "wherever the score cursor is" and "tick 2" means "the end of
//! the score". Every tick this function returns is a multiple of `snapTicks`, so only 0 can occur,
//! and 0 means the same thing either way.
MidiLoopRange midiLoopRangeFromDrag(int draggedTick, int releasedTick, int totalTicks, int snapTicks);

//! Where to scroll so that the playhead stays visible while the score plays.
//!
//! `playheadX` is the playhead in viewport pixels (already scrolled). While it sits comfortably inside
//! the viewport - `midiFollowMargin` of the width at each end - the scroll position is returned
//! unchanged, so the score does not creep while playing. Once it leaves that band the view jumps just
//! enough to put the playhead back at `midiFollowMargin` from the left edge, which is what makes a long
//! score readable while it plays. The result is clamped to `0..maxScrollX`.
double midiFollowScrollX(double scrollX, double playheadX, double viewportWidth, double maxScrollX);

//! The fraction of the viewport width the playhead is kept away from the edges - see midiFollowScrollX().
inline constexpr double midiFollowMargin = 0.15;
}
