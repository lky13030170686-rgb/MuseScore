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

#include "midirecorder.h"

#include <algorithm>
#include <cmath>
#include <map>

//! ⚠️ 整个文件必须待在 `namespace mu::notation` 里：头文件把这一组函数声明在 `mu::notation`，
//! 而成员函数与自由函数的定义若留在全局作用域（哪怕写了 `using namespace mu::notation;`），
//! 得到的是**另一个** `::quantizeTickValue` —— 调用点会同时看见两个候选，MSVC 直接判"调用不明确"。
namespace mu::notation {
void MidiRecorder::start()
{
    m_notes.clear();
    m_active = true;
}

void MidiRecorder::clear()
{
    m_notes.clear();
}

void MidiRecorder::stop(double tick)
{
    m_active = false;

    for (MidiRecordedNote& note : m_notes) {
        if (note.isHeld()) {
            //! 还按着的音按当前位置封口。⚠️ 用 `max(startTick, tick)`：录制停止与"最后一个音的
            //! 按下"可能落在同一个 tick 上（手还按着就按了停止），长度会是 0 而不是负数 ——
            //! 负数会一路传到"写谱"那里变成一个往回走的音。
            note.endTick = std::max(note.startTick, tick);
        }
    }
}

void MidiRecorder::noteOn(int pitch, int velocity, double tick)
{
    if (!m_active) {
        return;
    }

    const double at = std::max(0.0, tick);

    //! 同音高还没抬起来又按了一次（颤音、连击、键盘丢了 note-off）：先把前一个按当前时间封口。
    //! 不做这一步的话，"还按着"的同音会同时存在两个，而 `noteOff()` 只能配上最近的那个 ——
    //! 前一个会一直挂到 `stop()`，在卷帘窗里表现为一条拖到录制结束的长条。
    for (auto it = m_notes.rbegin(); it != m_notes.rend(); ++it) {
        if (it->pitch == pitch && it->isHeld()) {
            it->endTick = std::max(it->startTick, at);
            break;
        }
    }

    MidiRecordedNote note;
    note.pitch = std::clamp(pitch, 0, 127);
    note.velocity = std::clamp(velocity, 0, 127);
    note.startTick = at;
    note.endTick = -1.0;
    m_notes.push_back(note);
}

void MidiRecorder::noteOff(int pitch, double tick)
{
    if (!m_active) {
        return;
    }

    const double at = std::max(0.0, tick);

    for (auto it = m_notes.rbegin(); it != m_notes.rend(); ++it) {
        if (it->pitch == pitch && it->isHeld()) {
            it->endTick = std::max(it->startTick, at);
            return;
        }
    }

    //! 没有按下过的抬起（录制开始那一刻手已经按在键上、或者设备补发一个 note-off）：忽略。
    //! 这里绝不能"造一个音"—— 那会在谱面上凭空多出一个没人弹过的音。
}

int MidiRecorder::heldCount() const
{
    int count = 0;
    for (const MidiRecordedNote& note : m_notes) {
        if (note.isHeld()) {
            ++count;
        }
    }

    return count;
}

// ── 量化 ────────────────────────────────────────────────────────────────────────────────────────

int quantizeTickValue(double tick, int gridTicks, int strengthPercent)
{
    if (tick < 0.0) {
        tick = 0.0;
    }

    if (gridTicks <= 0) {
        return int(std::llround(tick));
    }

    const double strength = double(std::clamp(strengthPercent, 0, 100)) / 100.0;
    const double snapped = std::round(tick / double(gridTicks)) * double(gridTicks);

    //! 强度是"往回拉多少"，不是"吸到哪一格" —— 所以 50% 是"半量化"（保留一半人味），
    //! 而且它**永远不会把一个音拉到比最近的格子更远**。
    const double result = tick + (snapped - tick) * strength;

    return std::max(0, int(std::llround(result)));
}

int quantizeMinDurationTicks(const MidiQuantizeSettings& settings)
{
    //! 网格有效时给半格：`quantizeEnd` 会把结束位置吸到最近的格，而一个"轻轻一碰"
    //! （按住不到半格）吸完会变成零长度甚至负长度 —— 零长度的音在谱面上不存在。
    if (settings.gridTicks > 0) {
        return std::max(1, settings.gridTicks / 2);
    }

    //! 不量化时也不能零长度：给一个三十二分音符（480/8 = 60 tick）。
    return 60;
}

std::vector<MidiQuantizedNote> quantizeRecordedNotes(const std::vector<MidiRecordedNote>& notes,
                                                     const MidiQuantizeSettings& settings)
{
    const int minDuration = quantizeMinDurationTicks(settings);

    std::vector<MidiQuantizedNote> result;
    result.reserve(notes.size());

    for (const MidiRecordedNote& note : notes) {
        //! 还按着的音不提交。调用方（模型）在提交前一定先 `MidiRecorder::stop()`，
        //! 这一句是给"直接拿 notes() 来量化"的调用方兜底 —— 少一个音总好过写一个长度不确定的音。
        if (note.isHeld()) {
            continue;
        }

        MidiQuantizedNote quantized;
        quantized.pitch = std::clamp(note.pitch, 0, 127);
        quantized.velocity = std::clamp(note.velocity, 0, 127);
        quantized.tick = quantizeTickValue(note.startTick, settings.gridTicks, settings.strengthPercent);

        const int end = settings.quantizeEnd
                        ? quantizeTickValue(note.endTick, settings.gridTicks, settings.strengthPercent)
                        : std::max(quantized.tick, int(std::llround(note.endTick)));

        quantized.durationTicks = std::max(minDuration, end - quantized.tick);
        result.push_back(quantized);
    }

    //! 按「起点 → 音高」排序：写回乐谱是按这个顺序一个个放进去的（`setNoteRest` 依赖
    //! "从前往后"的次序），画在卷帘窗里的顺序也应该是同一个。
    std::stable_sort(result.begin(), result.end(), [](const MidiQuantizedNote& a, const MidiQuantizedNote& b) {
        if (a.tick != b.tick) {
            return a.tick < b.tick;
        }
        return a.pitch < b.pitch;
    });

    return result;
}

// ── 分组：量化后的音 → 谱面上的和弦 ──────────────────────────────────────────────────────────────

std::vector<MidiRecordedChord> buildRecordedChords(const std::vector<MidiQuantizedNote>& notes, int maxVoices)
{
    if (maxVoices < 1) {
        maxVoices = 1;
    }

    std::vector<MidiRecordedChord> chords;
    chords.reserve(notes.size());

    for (const MidiQuantizedNote& note : notes) {
        MidiRecordedChord* target = nullptr;
        for (MidiRecordedChord& chord : chords) {
            if (chord.tick == note.tick && chord.durationTicks == note.durationTicks) {
                target = &chord;
                break;
            }
        }

        if (!target) {
            MidiRecordedChord chord;
            chord.tick = note.tick;
            chord.durationTicks = note.durationTicks;
            chords.push_back(chord);
            target = &chords.back();
        }

        const bool alreadyThere = std::any_of(target->notes.begin(), target->notes.end(),
                                             [&note](const MidiRecordedChord::Note& existing) {
            return existing.pitch == note.pitch;
        });

        if (!alreadyThere) {
            target->notes.push_back(MidiRecordedChord::Note { note.pitch, note.velocity });
        }
    }

    //! 声部：同一个起点上**时值短的先落到 voice 0**，长的往后排。
    //! 为什么不是"按先来后到"：一个音在这一次演奏里是什么声部，应该在整场里保持一致 ——
    //! 按"先来后到"的话，同一 tick 上先记下的那个音决定了声部，而"先记下谁"取决于音高顺序，
    //! 于是长音有时在 0 声部、有时在 1 声部，看上去像随机分的。按时值排是确定的、也讲得通。
    std::map<int, std::vector<size_t> > byTick;
    for (size_t i = 0; i < chords.size(); ++i) {
        byTick[chords[i].tick].push_back(i);
    }

    for (auto& pair : byTick) {
        std::vector<size_t>& indexes = pair.second;
        std::stable_sort(indexes.begin(), indexes.end(), [&chords](size_t a, size_t b) {
            return chords[a].durationTicks < chords[b].durationTicks;
        });

        for (size_t k = 0; k < indexes.size(); ++k) {
            //! 超出 `VOICES` 就并回最后一个声部：宁可挤在一起，也不静默丢音。
            chords[indexes[k]].voice = std::min(int(k), maxVoices - 1);
        }
    }

    return chords;
}

// ── 网格表 ──────────────────────────────────────────────────────────────────────────────────────

const std::vector<MidiQuantizeGrid>& midiQuantizeGrids()
{
    //! 480 = 一拍（`Constants::DIVISION` 是固定的 480，与工程无关）。
    //! ⚠️ 三连音不是二连音的一半：八分三连音 = 480/3 = 160，十六分三连音 = 480/6 = 80。
    //! 照"1/8 的一半"去写 120 的话，三连音会被量化到最接近的二连音格子上，听起来像"没对上"。
    static const std::vector<MidiQuantizeGrid> grids {
        { "—", 0 },         //!< 不量化
        { "1/4", 480 },
        { "1/8", 240 },
        { "1/8T", 160 },
        { "1/16", 120 },
        { "1/16T", 80 },
        { "1/32", 60 },
    };

    return grids;
}
}
