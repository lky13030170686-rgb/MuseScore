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

#include <gtest/gtest.h>

#include "agentharness/qml/MuseScore/AgentHarness/markdowntext.h"

using namespace muse::agentharness;

//! WHY THIS SUITE EXISTS. The answer is the only part of the conversation the user actually asked for, and
//! this function is the whole of how it is drawn. Two of its jobs cannot be judged from a screenshot at
//! all:
//!   ① ESCAPING. A message about `<pre>` must arrive as the word `<pre>`. Get the order wrong and the
//!      answer silently restyles itself - which looks like a rendering quirk, not like a bug;
//!   ② THE LINK WHITELIST. A click on an answer is handed to `Qt.openUrlExternally`. Only the test can
//!      say that a `file:` link did NOT become clickable, because on screen it looks the same either way.
//! The rest - paragraphs, lists, fences - are pinned because "the answer keeps its shape" is a claim
//! about the conversation, the same way the tool fold is (第 132 条).

namespace {
QString toRich(const QString& markdown)
{
    return markdownToRichText(markdown);
}

bool has(const QString& haystack, const QString& needle)
{
    return haystack.contains(needle);
}
} // namespace

TEST(AgentHarness_Markdown, PlainTextBecomesOneParagraph)
{
    EXPECT_EQ(toRich(QStringLiteral("Two measures, four beats each.")),
              QStringLiteral("<p>Two measures, four beats each.</p>"));
}

TEST(AgentHarness_Markdown, BlankLinesSeparateParagraphs)
{
    //! ⛔ The measured complaint (第 150 条): without this, a three-paragraph answer arrived as one block
    //! of run-together sentences, because a plain `Text` throws the structure away.
    const QString rich = toRich(QStringLiteral("First.\n\nSecond.\n\nThird."));

    EXPECT_EQ(rich.count(QStringLiteral("<p>")), 3);
    EXPECT_TRUE(has(rich, QStringLiteral("<p>First.</p>")));
    EXPECT_TRUE(has(rich, QStringLiteral("<p>Third.</p>")));
}

TEST(AgentHarness_Markdown, ASoftWrapInsideAParagraphIsKept)
{
    //! The DSH renderer keeps the model's own line breaks (`white-space: pre-wrap`), so a wrapped list of
    //! short lines does not get re-flowed into one sentence.
    const QString rich = toRich(QStringLiteral("one\ntwo"));

    EXPECT_EQ(rich, QStringLiteral("<p>one<br/>two</p>"));
}

TEST(AgentHarness_Markdown, EmphasisAndInlineCode)
{
    const QString rich = toRich(QStringLiteral("a **bold** and *italic* and `code` end"));

    EXPECT_TRUE(has(rich, QStringLiteral("<b>bold</b>")));
    EXPECT_TRUE(has(rich, QStringLiteral("<i>italic</i>")));
    EXPECT_TRUE(has(rich, QStringLiteral("<code>code</code>")));
}

TEST(AgentHarness_Markdown, DoubleDelimiterIsBoldNotNestedItalic)
{
    //! ⛔ ORDER IS THE WHOLE TRICK: `**` has to be consumed before `*` sees it, or the opening delimiter
    //! of a bold run becomes an italic run and the rest of the line comes out wrong.
    const QString rich = toRich(QStringLiteral("**loud**"));

    EXPECT_TRUE(has(rich, QStringLiteral("<b>loud</b>")));
    EXPECT_FALSE(has(rich, QStringLiteral("<i>")));
}

TEST(AgentHarness_Markdown, CodeSpanContentsAreLiteral)
{
    //! `**` inside backticks is two stars, not bold. If the code spans were not lifted out first, every
    //! later rule would rewrite what the user is being shown as literal code.
    const QString rich = toRich(QStringLiteral("use `**not bold**` here"));

    EXPECT_TRUE(has(rich, QStringLiteral("<code>**not bold**</code>")));
    EXPECT_FALSE(has(rich, QStringLiteral("<b>")));
}

TEST(AgentHarness_Markdown, FencedBlockIsOnePreAndKeepsItsContentsLiteral)
{
    const QString rich = toRich(QStringLiteral("before\n\n```\n- not a list\n**not bold**\n```\n\nafter"));

    EXPECT_TRUE(has(rich, QStringLiteral("<pre>- not a list\n**not bold**</pre>")));
    EXPECT_FALSE(has(rich, QStringLiteral("<li>")));
    EXPECT_FALSE(has(rich, QStringLiteral("<b>")));
}

TEST(AgentHarness_Markdown, AnUnterminatedFenceStillShowsItsText)
{
    //! A model that was cut off mid-block (a stopped turn, a spill) should still have its last words on
    //! screen. Dropping them would make the answer *shorter* than what the model said, silently.
    const QString rich = toRich(QStringLiteral("```\nmeasure 2 is empty"));

    EXPECT_TRUE(has(rich, QStringLiteral("measure 2 is empty")));
    EXPECT_TRUE(has(rich, QStringLiteral("<pre>")));
}

TEST(AgentHarness_Markdown, UnorderedAndOrderedLists)
{
    const QString bullets = toRich(QStringLiteral("- one\n- two"));
    EXPECT_TRUE(has(bullets, QStringLiteral("<ul>")));
    EXPECT_TRUE(has(bullets, QStringLiteral("<li>one</li>")));
    EXPECT_TRUE(has(bullets, QStringLiteral("<li>two</li>")));
    EXPECT_TRUE(has(bullets, QStringLiteral("</ul>")));

    const QString numbered = toRich(QStringLiteral("1. first\n2. second"));
    EXPECT_TRUE(has(numbered, QStringLiteral("<ol>")));
    EXPECT_TRUE(has(numbered, QStringLiteral("<li>first</li>")));
    EXPECT_TRUE(has(numbered, QStringLiteral("</ol>")));
}

TEST(AgentHarness_Markdown, SwitchingListKindClosesTheFirst)
{
    //! The one way a list can produce malformed markup: `<ol>` left open when an `<ul>` starts. Qt would
    //! still render it, which is why nobody would notice.
    const QString rich = toRich(QStringLiteral("- a\n1. b"));

    EXPECT_TRUE(has(rich, QStringLiteral("</ul>")));
    EXPECT_TRUE(has(rich, QStringLiteral("<ol>")));
}

TEST(AgentHarness_Markdown, HeadingsQuotesAndRules)
{
    EXPECT_TRUE(has(toRich(QStringLiteral("# Title")), QStringLiteral("<h1>Title</h1>")));
    EXPECT_TRUE(has(toRich(QStringLiteral("### Deeper")), QStringLiteral("<h3>Deeper</h3>")));
    EXPECT_TRUE(has(toRich(QStringLiteral("> quoted")), QStringLiteral("<blockquote>quoted</blockquote>")));
    EXPECT_TRUE(has(toRich(QStringLiteral("---")), QStringLiteral("<hr/>")));
    //! `#` without a space is not a heading - it is a hash.
    EXPECT_TRUE(has(toRich(QStringLiteral("#5 is a chord")), QStringLiteral("<p>")));
}

TEST(AgentHarness_Markdown, EverythingIsEscapedBeforeAnyTagIsInserted)
{
    //! ⛔ RULE ① OF THE HEADER. This is the test that fails if the escaping is ever moved after the
    //! substitutions - and the failure it prevents is not cosmetic: the model's words would be read as
    //! markup, so a sentence about `<pre>` would restructure the answer.
    const QString rich = toRich(QStringLiteral("the tag is <pre> and 5 < 6 & 7 > 2"));

    EXPECT_TRUE(has(rich, QStringLiteral("&lt;pre&gt;")));
    EXPECT_TRUE(has(rich, QStringLiteral("5 &lt; 6")));
    EXPECT_TRUE(has(rich, QStringLiteral("&amp;")));
    EXPECT_FALSE(has(rich, QStringLiteral("<pre>")));
}

TEST(AgentHarness_Markdown, OnlyOpenableLinksBecomeLinks)
{
    //! ⛔ RULE ② OF THE HEADER, and the one no screenshot can settle: a click is handed to
    //! `Qt.openUrlExternally`. The two controls differ only in what the click does.
    const QString good = toRich(QStringLiteral("see [the docs](https://example.com/a?b=1&c=2)"));
    EXPECT_TRUE(has(good, QStringLiteral("<a href=\"https://example.com/a?b=1&amp;c=2\">the docs</a>")));

    const QString bad = toRich(QStringLiteral("see [a file](file:///C:/Windows/System32/calc.exe)"));
    EXPECT_FALSE(has(bad, QStringLiteral("<a ")));
    //! The words stay: losing a sentence because of its punctuation would be a different bug.
    EXPECT_TRUE(has(bad, QStringLiteral("a file")));
}

TEST(AgentHarness_Markdown, EmptyInputIsEmptyOutput)
{
    EXPECT_EQ(toRich(QString()), QString());
    //! ⚠️ Not `<p></p>`: an empty paragraph is a visible gap, and an empty answer is not a blank line.
    EXPECT_EQ(toRich(QStringLiteral("\n\n")), QString());
}
