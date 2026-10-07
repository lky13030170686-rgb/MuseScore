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
#ifndef MU_CONTEXT_SHORTCUTCONTEXT_H
#define MU_CONTEXT_SHORTCUTCONTEXT_H

#include <array>
#include <string>

#include "shortcuts/shortcutcontext.h"
#include "global/containers.h"

namespace mu::context {
// common shortcuts (re declared for convenience)
static const std::string CTX_DISABLED = muse::shortcuts::CTX_DISABLED;
static const std::string CTX_ANY = muse::shortcuts::CTX_ANY;
static const std::string CTX_NOTATION_OPENED = muse::shortcuts::CTX_PROJECT_OPENED;
static const std::string CTX_NOTATION_FOCUSED = muse::shortcuts::CTX_PROJECT_FOCUSED;
static const std::string CTX_NOT_NOTATION_FOCUSED = muse::shortcuts::CTX_NOT_PROJECT_FOCUSED;

/// We're not [in note input on a TAB staff] (i.e. either not in note input mode, or in note input mode but not on a TAB staff)
static const std::string CTX_NOTATION_NOT_NOTE_INPUT_STAFF_TAB("notation-not-note-input-staff-tab");
/// We're in note input on a TAB staff
static const std::string CTX_NOTATION_NOTE_INPUT_STAFF_TAB("notation-note-input-staff-tab");
/// We're in note input on a Jianpu staff
static const std::string CTX_NOTATION_NOTE_INPUT_STAFF_JIANPU("notation-note-input-staff-jianpu");

static const std::string CTX_NOTATION_TEXT_EDITING("notation-text-editing");

static const std::string CTX_NOTATION_LIST_SELECTION("notation-list-selection");

/// 工程页**打开着** —— 记谱页**或**自建页面（MIDI 编辑页 `musescore://midi`）。
///
/// 这一页不是记谱页，但它显示/编辑的是**同一个工程**，所以"播放/暂停"这类**工程级**快捷键应当在
/// 两页都生效（用户 2026-10-07 指定：MIDI 页空格 = 播放，与记谱页一致）。
///
/// ⚠️ **不能**直接复用 `CTX_NOTATION_FOCUSED`：那是"记谱页有焦点"（`UiCtxProjectFocused`），
/// 而 MIDI 页解析出的 UI 上下文是 `UiCtxUnknown`（`resolveCurrentUiContext()` 只认
/// notation/publish/devtools 三个 URI）⇒ 复用它等于让**整页记谱类快捷键**（输入音符、方向键改音高…）
/// 在 MIDI 页一起生效 ✗。所以单开一个上下文，只给"两页都该能用"的那几条快捷键用。
static const std::string CTX_PROJECT_PAGE_OPENED("project-page-opened");

class ShortcutContextPriority : public muse::shortcuts::IShortcutContextPriority
{
public:

    bool hasLowerPriorityThan(const std::string& ctx1, const std::string& ctx2) const override
    {
        static const std::array<std::string, 9> CONTEXTS_BY_INCREASING_PRIORITY {
            CTX_ANY,

            CTX_NOTATION_OPENED,
            CTX_NOT_NOTATION_FOCUSED,
            CTX_PROJECT_PAGE_OPENED,
            CTX_NOTATION_FOCUSED,

            CTX_NOTATION_NOT_NOTE_INPUT_STAFF_TAB,
            CTX_NOTATION_NOTE_INPUT_STAFF_TAB,
            CTX_NOTATION_NOTE_INPUT_STAFF_JIANPU,

            CTX_NOTATION_TEXT_EDITING
        };

        size_t index1 = muse::indexOf(CONTEXTS_BY_INCREASING_PRIORITY, ctx1);
        size_t index2 = muse::indexOf(CONTEXTS_BY_INCREASING_PRIORITY, ctx2);

        return index1 < index2;
    }
};
}

#endif // MU_CONTEXT_SHORTCUTCONTEXT_H
