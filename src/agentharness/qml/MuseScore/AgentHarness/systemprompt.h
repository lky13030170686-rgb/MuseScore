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

#include <QJsonArray>
#include <QString>

#include <vector>

#include "tools.h"

namespace muse::agentharness {
//! Build the system prompt, and the tool schemas that travel with the request.
//!
//! WHY THE PROMPT IS FIXED TEXT AND NOT A TEMPLATE ENGINE (技术设计 §5.4): the prompt has exactly one
//! job - tell the model where it is and what the addressing convention is - and that job does not
//! vary. A section/variable system would be more machinery than the content justifies, and every
//! piece of machinery here is something else that can be wrong at runtime.
//!
//! The prompt is *not* where tool descriptions live. Each tool's own `description` is carried in the
//! request's `tools` array (see `toolSchemas()`), which is the channel the model actually reads for
//! capabilities; duplicating them in prose would create two descriptions that can disagree.
QString buildSystemPrompt();

//! The tool schemas as the request's `tools` array.
//!
//! ⚠️ ORDER IS STABLE, and that is deliberate: the array is part of the request envelope, and a
//! request that reorders its tools for no reason is a different request as far as any prefix cache is
//! concerned. `toolTable()` returns a fixed order, so this does too.
QJsonArray toolSchemas();

} // namespace muse::agentharness
