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

#include "markdowntext.h"

#include <QRegularExpression>
#include <QStringList>
#include <QVector>

namespace muse::agentharness {
namespace {

//! HTML-escape. ⛔ THIS RUNS FIRST, before any tag is inserted. Skipping it does not merely "render
//! wrong": everything after this point builds markup, so a `<` that came from the model would be read
//! as markup - a message about `<pre>` would become a layout change instead of a sentence.
QString escaped(const QString& text)
{
    QString out;
    out.reserve(text.size() + 16);

    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&': out += QLatin1String("&amp;"); break;
        case u'<': out += QLatin1String("&lt;"); break;
        case u'>': out += QLatin1String("&gt;"); break;
        case u'"': out += QLatin1String("&quot;"); break;
        default: out += c; break;
        }
    }

    return out;
}

//! Whether a link is one we are willing to hand to `Qt.openUrlExternally`. The text came from a model,
//! and the click opens something on the user's machine: `file:`, `javascript:` and the rest are not
//! things an answer should be able to make happen.
bool isOpenableUrl(const QString& url)
{
    const QString lower = url.trimmed().toLower();
    return lower.startsWith(QLatin1String("http://"))
           || lower.startsWith(QLatin1String("https://"))
           || lower.startsWith(QLatin1String("mailto:"));
}

//! The placeholder that stands in for a lifted-out code span while the other inline rules run.
//! ⚠️ `\x01`/`\x02` and not something printable: the marked text is the MODEL's, and any printable
//! sentinel is a string it could contain by accident.
QString codePlaceholder(int index)
{
    return QChar(0x01) + QString::number(index) + QChar(0x02);
}

//! The inline pass, run on text that has ALREADY been escaped.
//! Order matters and is the whole trick:
//!   ① code spans come out first - their contents are literal (`**` inside backticks is two stars, not
//!      bold), and leaving them in would let every later rule rewrite them;
//!   ② links, then bold, then italics - `**` has to be consumed before `*` sees it;
//!   ③ code spans go back, as `<code>`.
QString inlinePass(const QString& escapedText)
{
    //! ① lift out `code spans`
    QStringList codeSpans;
    QString work;
    work.reserve(escapedText.size());

    int i = 0;
    while (i < escapedText.size()) {
        if (escapedText.at(i) == u'`') {
            const int close = escapedText.indexOf(u'`', i + 1);
            if (close > i + 1) {
                codeSpans.append(escapedText.mid(i + 1, close - i - 1));
                work += codePlaceholder(codeSpans.size() - 1);
                i = close + 1;
                continue;
            }
        }

        work += escapedText.at(i);
        ++i;
    }

    //! ② links. ⚠️ `[`/`]`/`(`/`)` survive escaping untouched, so the pattern runs on the escaped text.
    static const QRegularExpression linkRe(QStringLiteral(R"(\[([^\]\n]*)\]\(([^)\s]*)\))"));

    QString withLinks;
    withLinks.reserve(work.size());

    int last = 0;
    QRegularExpressionMatchIterator it = linkRe.globalMatch(work);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        withLinks += work.mid(last, m.capturedStart() - last);

        const QString label = m.captured(1);
        const QString url = m.captured(2);
        if (isOpenableUrl(url)) {
            //! `url` is already escaped, which is what an HTML attribute wants (`&amp;` for `&`).
            withLinks += QStringLiteral("<a href=\"%1\">%2</a>").arg(url, label);
        } else {
            //! Not a link we will open: the words stay, the link does not. Dropping the text too would
            //! make the answer lose a sentence because of its punctuation.
            withLinks += label;
        }

        last = m.capturedEnd();
    }
    withLinks += work.mid(last);

    //! ③ emphasis. `**` before `*`, and `__` before `_`, or the shorter rule eats the longer one's
    //! opening delimiter and the rest of the line comes out italic instead of bold.
    QString out = withLinks;
    out.replace(QRegularExpression(QStringLiteral(R"(\*\*([^*\n]+)\*\*)")), QStringLiteral("<b>\\1</b>"));
    out.replace(QRegularExpression(QStringLiteral(R"(__([^_\n]+)__)")), QStringLiteral("<b>\\1</b>"));
    out.replace(QRegularExpression(QStringLiteral(R"(\*([^*\n]+)\*)")), QStringLiteral("<i>\\1</i>"));
    out.replace(QRegularExpression(QStringLiteral(R"(\b_([^_\n]+)_\b)")), QStringLiteral("<i>\\1</i>"));
    out.replace(QRegularExpression(QStringLiteral(R"(~~([^~\n]+)~~)")), QStringLiteral("<s>\\1</s>"));

    //! ④ the code spans come back, now that nothing else will touch their contents.
    for (int k = 0; k < codeSpans.size(); ++k) {
        out.replace(codePlaceholder(k), QStringLiteral("<code>%1</code>").arg(codeSpans.at(k)));
    }

    return out;
}

bool isRule(const QString& line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.size() < 3) {
        return false;
    }

    const QChar first = trimmed.at(0);
    if (first != u'-' && first != u'*' && first != u'_') {
        return false;
    }

    for (const QChar c : trimmed) {
        if (c != first) {
            return false;
        }
    }

    return true;
}

//! `# ` .. `###### ` -> the level, or 0.
int headingLevel(const QString& line)
{
    int level = 0;
    while (level < line.size() && line.at(level) == u'#' && level < 6) {
        ++level;
    }

    if (level == 0 || level >= line.size() || line.at(level) != u' ') {
        return 0;
    }

    return level;
}

//! `- `, `* `, `+ ` -> unordered; `1. `, `12) ` -> ordered. 0 = neither.
int listMarkerLength(const QString& line, bool* ordered)
{
    *ordered = false;

    if (line.size() >= 2 && (line.at(0) == u'-' || line.at(0) == u'*' || line.at(0) == u'+')
        && line.at(1) == u' ') {
        return 2;
    }

    int digits = 0;
    while (digits < line.size() && line.at(digits).isDigit()) {
        ++digits;
    }

    if (digits > 0 && digits + 1 < line.size()
        && (line.at(digits) == u'.' || line.at(digits) == u')') && line.at(digits + 1) == u' ') {
        *ordered = true;
        return digits + 2;
    }

    return 0;
}

} // namespace

QString markdownToRichText(const QString& markdown)
{
    const QStringList lines = markdown.split(u'\n');

    QString out;
    QString paragraph;
    //! Which list element is open, if any. Kept as a string (`ul`/`ol`/empty) rather than a bool pair
    //! because the only question ever asked of it is "is it the one this line wants?".
    QString openList;
    bool inFence = false;
    QString fenceBody;

    const auto flushParagraph = [&]() {
        if (paragraph.isEmpty()) {
            return;
        }

        QString rendered = inlinePass(escaped(paragraph));
        //! A single newline inside a paragraph is a soft wrap: the model's line breaks are kept (the
        //! DSH renderer keeps them too - its body is `white-space: pre-wrap`), rather than re-flowed.
        rendered.replace(QLatin1Char('\n'), QLatin1String("<br/>"));
        out += QStringLiteral("<p>%1</p>").arg(rendered);
        paragraph.clear();
    };

    const auto closeList = [&]() {
        if (!openList.isEmpty()) {
            out += QStringLiteral("</%1>").arg(openList);
            openList.clear();
        }
    };

    for (const QString& rawLine : lines) {
        const QString line = rawLine;

        //! ── Inside a fence: everything is literal until the closing fence ─────────────────────────
        if (inFence) {
            if (line.trimmed().startsWith(QLatin1String("```"))) {
                out += QStringLiteral("<pre>%1</pre>").arg(escaped(fenceBody));
                fenceBody.clear();
                inFence = false;
            } else {
                if (!fenceBody.isEmpty()) {
                    fenceBody += QLatin1Char('\n');
                }
                fenceBody += line;
            }
            continue;
        }

        const QString trimmed = line.trimmed();

        if (trimmed.startsWith(QLatin1String("```"))) {
            flushParagraph();
            closeList();
            inFence = true;
            fenceBody.clear();
            continue;
        }

        //! ── Blank line: ends whatever block was open ─────────────────────────────────────────────
        if (trimmed.isEmpty()) {
            flushParagraph();
            closeList();
            continue;
        }

        if (isRule(trimmed)) {
            flushParagraph();
            closeList();
            out += QStringLiteral("<hr/>");
            continue;
        }

        const int level = headingLevel(trimmed);
        if (level > 0) {
            flushParagraph();
            closeList();
            out += QStringLiteral("<h%1>%2</h%1>")
                   .arg(QString::number(level), inlinePass(escaped(trimmed.mid(level + 1))));
            continue;
        }

        if (trimmed.startsWith(QLatin1String("> ")) || trimmed == QLatin1String(">")) {
            flushParagraph();
            closeList();
            const QString body = trimmed.size() > 1 ? trimmed.mid(2) : QString();
            out += QStringLiteral("<blockquote>%1</blockquote>").arg(inlinePass(escaped(body)));
            continue;
        }

        bool ordered = false;
        const int marker = listMarkerLength(line, &ordered);
        if (marker > 0) {
            flushParagraph();

            const QString wanted = ordered ? QStringLiteral("ol") : QStringLiteral("ul");
            if (openList != wanted) {
                closeList();
                out += QStringLiteral("<%1>").arg(wanted);
                openList = wanted;
            }

            out += QStringLiteral("<li>%1</li>").arg(inlinePass(escaped(line.mid(marker))));
            continue;
        }

        //! ── Anything else is paragraph text ──────────────────────────────────────────────────────
        closeList();
        if (!paragraph.isEmpty()) {
            paragraph += QLatin1Char('\n');
        }
        paragraph += trimmed;
    }

    //! Unterminated fence: emit what there was rather than dropping it. A model that got cut off mid
    //! block should still have its last words on screen - the same rule the streamed text follows.
    if (inFence && !fenceBody.isEmpty()) {
        out += QStringLiteral("<pre>%1</pre>").arg(escaped(fenceBody));
    }

    flushParagraph();
    closeList();

    return out;
}

} // namespace muse::agentharness
