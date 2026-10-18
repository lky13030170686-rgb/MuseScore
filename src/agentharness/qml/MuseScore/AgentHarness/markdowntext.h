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

#include <QString>

namespace muse::agentharness {
//! The assistant's answer, as the rich text QML's `Text` can draw (`textFormat: Text.RichText`).
//!
//! \b Why this exists. Every surface drew the answer with a plain `StyledTextLabel`, and a `Text` with
//! no format throws away everything but the words: the model's paragraphs ran together into one block,
//! `- item` lines stayed literal dashes, and a fenced block arrived as a wall of text with the fences
//! still in it. Measured on a real turn (第 150 条): the answer is the only part of the conversation the
//! user actually asked for, and it was the worst-formatted part of it.
//!
//! \b Why it is a pure function in C++ rather than QML. "The answer is drawn as what it says" is a claim
//! about the conversation, so it is a test rather than something somebody has to notice on a screenshot -
//! the same argument as `foldToolRows` (第 132 条) and `sessionTitleFrom` (第 144 条). It also means the
//! parsing rules live in one place instead of in a `Repeater` full of ternaries.
//!
//! \b What it supports, and what it deliberately does not. Supported: paragraphs, hard-wrapped lines,
//! headings (`#`..`######`), unordered and ordered lists, fenced code blocks, inline code, bold,
//! italic, strikethrough, links, block quotes and horizontal rules - the shapes a model actually uses
//! when it explains what it did to a score.
//! ⛔ NOT supported: tables, footnotes, math, images, task-list checkboxes, reference-style links, and
//! nested-list indentation. Those are the parts of the DSH renderer that need a real parser (it uses
//! remark + KaTeX); half-doing them would produce markup that renders as literal punctuation, which is
//! worse than the plain text we started from. ⚠️ An unsupported construct is left as the user typed
//! it - visible, not silently swallowed.
//!
//! \b Two rules that are load-bearing rather than cosmetic:
//!   ① **Everything is HTML-escaped before any tag is inserted.** A model message about `<pre>` must
//!      arrive as the word `<pre>`; escape-after-substitute would let the answer restyle itself.
//!   ② **Only `http`/`https`/`mailto` links are emitted as links.** A click on an answer is handed to
//!      `Qt.openUrlExternally`, so an answer must not be able to make the user's machine open a
//!      `file:` path. Anything else keeps its text and loses its link.
QString markdownToRichText(const QString& markdown);

} // namespace muse::agentharness
