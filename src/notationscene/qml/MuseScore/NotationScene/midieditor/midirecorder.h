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

#include <vector>

namespace mu::notation {
//! ── 实时录制：数据层（不碰 IoC、不碰 QObject、不碰 Score）──────────────────────────
//!
//! 一次演奏是这样变成谱面上的音符的：
//!
//! ```
//! MIDI 事件（音高 + 力度 + 端口时间戳）
//!    └─ MidiRecorder            ← 本文件上半：按"按下/抬起"配对，坐标是**未量化**的分数 tick
//!         └─ quantizeRecordedNotes()   ← 本文件下半：网格 + 强度
//!              └─ buildRecordedChords() ← 同时值同起点的音并成一个和弦，重叠的分到不同声部
//!                   └─ applyRecordedChords()   ← midieditornotes.cpp：真正写进乐谱（一次事务）
//! ```
//!
//! **为什么先原样记下来、提交时才量化**：演奏的时间戳只有一次机会，而"量化多少、要不要
//! 保留摇摆"是事后才决定的 —— 记下来的是 double tick（480 分之一拍），所以 1/32 三连音
//! 这一级的时间差也留得住。录制中途改量化设置只影响**提交**，不会动已经记下的东西。
//!
//! 这一层刻意做成纯函数/纯数据：它能在单元测试里被喂一串"按下 480、抬起 960"这样的事件，
//! 不需要 MIDI 键盘、也不需要乐谱 —— 而写回那一半（需要 `Score`）在 `midieditornotes.cpp`。

//! 一次演奏里"按下到抬起"的一个音。坐标是**未量化**的分数 tick（double）。
struct MidiRecordedNote {
    int pitch = 0;
    int velocity = 0;

    double startTick = 0.0;

    //! 负数 = 这一刻还按着。实时预览要用它把"还按着的那个音"画成一条到播放头的长条，
    //! 提交前调用 `MidiRecorder::stop()` 会把它们按当前位置封口。
    double endTick = -1.0;

    bool isHeld() const { return endTick < startTick; }

    //! 按住时长；还按着的时候返回 0（调用方要用当前位置自己算）。
    double heldTicks() const { return isHeld() ? 0.0 : endTick - startTick; }
};

//! 采集器：把"按下/抬起"配对成音符。
//!
//! 它**只认音高和时间**，不认线程 —— 调用方负责把事件搬到主线程（MIDI 端口是在自己的
//! 回调线程上发事件的，见 midieditormodel.cpp 的队列）。
//!
//! 两种现实里一定会遇到的情况在 `noteOn()` 里就地处理：
//!  * 同一个音高**还没抬起来又按了一次**（颤音、连击、键盘丢了一个 note-off）——
//!    先把前一个按当前时间封口，再开新的，绝不留下两个"还按着"的同音；
//!  * **没有按下过的抬起**（录制开始时手已经按着键、或者设备补发）—— 直接忽略。
class MidiRecorder
{
public:
    void start();
    bool isActive() const { return m_active; }

    //! 收尾：把还按着的音按 `tick` 封口。之后 `notes()` 里不会再有 held 的音。
    void stop(double tick);

    void noteOn(int pitch, int velocity, double tick);
    void noteOff(int pitch, double tick);

    //! 已经按下的键的个数（实时预览里"还亮着的方块"就是它们）。
    int heldCount() const;

    void clear();

    const std::vector<MidiRecordedNote>& notes() const { return m_notes; }

    //! 记录到的音数（含还按着的）—— 工具条上那个"已录 N 个音"的读数。
    int noteCount() const { return int(m_notes.size()); }

private:
    bool m_active = false;
    std::vector<MidiRecordedNote> m_notes;
};

//! ── 量化 ────────────────────────────────────────────────────────────────────────

//! 量化设置。`gridTicks == 0` 表示**不量化**（保留演奏的原始位置，只把 double 取整）。
struct MidiQuantizeSettings {
    //! 网格步长（分数 tick，480 = 一拍，120 = 十六分音符）。
    //! ⚠️ 与 QML 里那个下拉框是同一份数字：`Constants::DIVISION` 是固定 480，
    //! 所以"十六分音符 = 120"在任何工程里都成立，不需要问乐谱要 division。
    int gridTicks = 0;

    //! 量化强度（百分比）。100 = 完全吸附到网格，0 = 一点都不动。
    //! 中间值是"半量化"—— 保留一点人味，这是所有 DAW 都有的一档。
    int strengthPercent = 100;

    //! 结束位置也吸附（长度跟着走）。关掉则只量起点，长度保留演奏值再吸附到网格。
    bool quantizeEnd = true;
};

//! 量化之后的一个音：整数 tick（写进乐谱要用整 tick）。
struct MidiQuantizedNote {
    int pitch = 0;
    int velocity = 0;
    int tick = 0;
    int durationTicks = 0;
};

//! 把一个（可能是小数的）tick 吸到网格上：先四舍五入到最近的格，再按强度往回拉。
//! `gridTicks <= 0` 时只做四舍五入（= 不量化）。结果不会是负数。
int quantizeTickValue(double tick, int gridTicks, int strengthPercent);

//! 音短到网格以下时至少给多长：网格有效时给半格，不量化时给一个三十二分音符（60 tick）。
//! 「至少」是必须的 —— 不然"轻轻一碰"（按下到抬起 < 半格）量化后会变成零长度的音。
int quantizeMinDurationTicks(const MidiQuantizeSettings& settings);

//! 量化整场演奏。还按着的音**不会被提交**（调用方应先 `MidiRecorder::stop()`），
//! 返回的列表按「起点 → 音高」排序，且长度都 ≥ `quantizeMinDurationTicks()`。
std::vector<MidiQuantizedNote> quantizeRecordedNotes(const std::vector<MidiRecordedNote>& notes,
                                                     const MidiQuantizeSettings& settings);

//! ── 分组：量化后的音 → 谱面上的和弦 ──────────────────────────────────────────────

//! 一个和弦：同一个起点、同一个时值的一组音。
//!
//! 记谱上「一个和弦只能有一个时值」，所以同一 tick 上时值不同的音**必须**分开 ——
//! 它们被分到不同的声部（voice 0、1、2…），而不是被截成一样长（那会丢掉演奏的长度）。
struct MidiRecordedChord {
    struct Note {
        int pitch = 0;
        int velocity = 0;
    };

    int tick = 0;
    int durationTicks = 0;

    //! 声部（0 起）。同一 tick 上第一个和弦进 0，第二个进 1……超出 `VOICES` 就并回最后一个。
    int voice = 0;

    std::vector<Note> notes;
};

//! 把量化后的音并成和弦：同 (起点, 时值) 归一个和弦，同音高只留一个。
//! `maxVoices` 传乐谱的 `VOICES`（4）—— 同一 tick 上的第 5 种时值并回最后一个声部，
//! 宁可挤一点也**不丢音**（丢音是静默的，用户只会觉得"录少了"）。
std::vector<MidiRecordedChord> buildRecordedChords(const std::vector<MidiQuantizedNote>& notes, int maxVoices = 4);

//! ── 网格：工具条上那个下拉框的一份数据 ──────────────────────────────────────────
//!
//! 放在这里而不是 QML 里，是为了让"下拉框里有什么、写进乐谱的是什么"只有一份来源 ——
//! 量化测试量的就是这张表。
struct MidiQuantizeGrid {
    const char* label;   //!< 显示名（不翻译的记号：1/4、1/8T…）
    int ticks;           //!< 0 = 不量化
};

//! 可选的量化网格，按"从粗到细"排列。⚠️ 三连音不是二连音的整数分之一：
//! 八分三连音 = 480/3 = 160 tick，十六分三连音 = 80 tick。
const std::vector<MidiQuantizeGrid>& midiQuantizeGrids();
}
