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
#include "editorstate.h"

#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/dom/select.h"
//! `noteAt` - the same lookup the note recipes use, so "the note at m2b3" has one definition.
#include "notelocator.h"

using namespace muse::agentharness;

RecipeResult muse::agentharness::selectMeasures(mu::engraving::Score* score, int firstMeasure, int lastMeasure,
                                                int staff)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }
    if (firstMeasure < 1 || lastMeasure < firstMeasure) {
        return RecipeResult::failure(QStringLiteral("give a range like `firstMeasure` <= `lastMeasure`, both "
                                                    "at least 1 (got %1 to %2)")
                                     .arg(firstMeasure).arg(lastMeasure));
    }

    //! ⛔ EVERY CHECK BEFORE THE FIRST CHANGE. Deselecting and then discovering that measure 99 does not
    //! exist would leave the user with their selection thrown away and nothing to show for it.
    mu::engraving::Measure* first = measureAt(score, firstMeasure);
    mu::engraving::Measure* last = measureAt(score, lastMeasure);
    if (!first || !last) {
        return RecipeResult::failure(QStringLiteral("the score has %1 measures, so %2..%3 is not a range in "
                                                    "it").arg(score->nmeasures()).arg(firstMeasure).arg(lastMeasure));
    }

    const size_t staves = score->nstaves();
    if (staves == 0) {
        return RecipeResult::failure(QStringLiteral("this score has no staves"));
    }
    if (staff < 0 || size_t(staff) > staves) {
        return RecipeResult::failure(QStringLiteral("there is no staff %1; the score has %2")
                                     .arg(staff).arg(staves));
    }

    //! 0 means "every staff", which is what a bare "select these bars" means. A named staff is 1-based, like
    //! every other staff number the tools take.
    const mu::engraving::staff_idx_t firstStaff = staff > 0 ? mu::engraving::staff_idx_t(staff - 1) : 0;
    const mu::engraving::staff_idx_t lastStaff
        = staff > 0 ? mu::engraving::staff_idx_t(staff - 1) : mu::engraving::staff_idx_t(staves - 1);

    //! ⚠️ TWO CALLS, exactly like `Score::cmdSelectAll` (which selects the whole score this way): the first
    //! CREATES a range over the first measure, the second EXTENDS it to the last. One call can only name one
    //! measure - `Score::selectRange` takes a single item - so a one-call version would select one bar and
    //! look like it worked.
    score->deselectAll();
    score->selectRange(first, firstStaff);
    if (last != first || lastStaff != firstStaff) {
        score->selectRange(last, lastStaff);
    }

    //! ── Read back (第 133 条's rule: report the result, not the request) ─────────────────────────────
    const mu::engraving::Selection& selection = score->selection();
    if (!selection.isRange()) {
        //! ⚠️ Not a "should never happen": a score whose measures carry no segment at all would leave the
        //! selection empty, and saying so beats reporting a selection that is not there.
        return RecipeResult::failure(
            QStringLiteral("the selection did not become a measure range, so the commands that act on a "
                           "selection would still act wherever the user's cursor is"));
    }

    //! ⛔ CONTAINMENT, not equality, and that is the honest check: `tickStart` is the first chord-rest
    //! segment of the first measure and `tickEnd` the last segment of the last one, which for an anacrusis
    //! or a measure that starts with a gap are not exactly the measures' own ticks. What must hold is that
    //! the selection lies inside the measures that were asked for - anything wider is the failure that
    //! matters (it would let a command touch a bar the caller never named).
    const mu::engraving::Fraction start = selection.tickStart();
    const mu::engraving::Fraction end = selection.tickEnd();
    if (start < first->tick() || end > last->endTick()) {
        return RecipeResult::failure(
            QStringLiteral("the selection came out as ticks %1..%2, which is not inside measures %3-%4 "
                           "(%5..%6); treat this as failed")
            .arg(start.ticks()).arg(end.ticks())
            .arg(firstMeasure).arg(lastMeasure)
            .arg(first->tick().ticks()).arg(last->endTick().ticks()));
    }

    const int staffStart = int(selection.staffStart()) + 1;
    const int staffEnd = int(selection.staffEnd());
    const QString which = firstMeasure == lastMeasure
                          ? QStringLiteral("measure %1").arg(firstMeasure)
                          : QStringLiteral("measures %1-%2").arg(firstMeasure).arg(lastMeasure);
    const QString stavesText = staffStart == staffEnd
                               ? QStringLiteral("staff %1").arg(staffStart)
                               : QStringLiteral("staves %1-%2").arg(staffStart).arg(staffEnd);

    return RecipeResult::success(
        QStringLiteral("selected %1 (%2, ticks %3..%4). The user's selection has moved here - the commands "
                       "that act on a RANGE selection (join-measures, copy, delete, ...) now act on these "
                       "measures. The score itself did not change.")
        .arg(which, stavesText).arg(start.ticks()).arg(end.ticks()));
}

RecipeResult muse::agentharness::selectElementAt(mu::engraving::Score* score, const ScoreAddress& address,
                                                 int voice)
{
    if (!score) {
        return RecipeResult::failure(QStringLiteral("no score"));
    }

    //! ⛔⛔ A NOTE, NOT THE CHORD - and this is measured, not assumed (第 138 条). The first version selected
    //! the `ChordRest` at the address, and `command://notation/split-measure` then did NOTHING: its handler
    //! reads `selection()->element()` and returns unless it `isNote() || isRest()`, and a Chord is neither.
    //! The probe said so in one line - `[split-probe] element=Chord isNote=0 errorBefore=0`, with no
    //! `errorAfter` at all, because the function never reached its transaction.
    //! ⚠️ A REST is a legal target (that is the other half of the same condition), so the chord rest is the
    //! fallback when the beat carries no note.
    QString problem;
    mu::engraving::EngravingItem* target = nullptr;
    //! The chord rest the report quotes ticks from: a Note has no `ticks()` of its own (its chord does),
    //! and both branches below already know which one it is.
    mu::engraving::ChordRest* asChordRest = nullptr;

    const NoteLookup found = noteAt(score, address, voice, -1);
    if (found.found() && found.note) {
        target = found.note;
        asChordRest = found.note->chord();
    } else {
        asChordRest = chordRestAt(score, address, voice, problem);
        if (!asChordRest) {
            return RecipeResult::failure(problem.isEmpty()
                                         ? QStringLiteral("there is nothing to select at that address")
                                         : problem);
        }
        target = asChordRest;
    }

    //! ⛔ `SelectType::SINGLE`, not `RANGE`: the commands that read one element look at `selection()->element()`
    //! and do nothing when it is null, so a range here would make them silently useless.
    score->select(std::vector<mu::engraving::EngravingItem*> { target },
                  mu::engraving::SelectType::SINGLE, target->staffIdx());

    const mu::engraving::Selection& selection = score->selection();
    if (selection.element() != target) {
        return RecipeResult::failure(
            QStringLiteral("the selection did not land on the element at that address, so the commands that "
                           "act on one selected element would act elsewhere; treat this as failed"));
    }

    //! ⛔ THE PRECONDITION THE COMMANDS ACTUALLY CHECK, asserted here rather than hoped for: both
    //! `splitSelectedMeasure` and its siblings accept a note or a rest and decline everything else WITHOUT A
    //! WORD. A tool whose whole purpose is "make that command work" has to leave the selection in the shape
    //! they accept - so the shape is checked, not assumed.
    if (!selection.element()->isNote() && !selection.element()->isRest()) {
        return RecipeResult::failure(
            QStringLiteral("the selection landed on a %1, and the commands that act on one selected element "
                           "only accept a note or a rest - so they would silently do nothing; treat this as "
                           "failed").arg(QString::fromLatin1(selection.element()->typeName())));
    }

    const bool isRest = selection.element()->isRest();
    return RecipeResult::success(
        QStringLiteral("selected the %1 at measure %2 beat %3 (staff %4, voice %5, ticks %6..%7). The user's "
                       "selection has moved here - commands that act on ONE selected element (split-measure, "
                       "and anything else that reads the selected note) now act on it. The score itself did "
                       "not change.")
        .arg(isRest ? QStringLiteral("rest") : QStringLiteral("note"))
        .arg(address.measure).arg(address.beat).arg(int(target->staffIdx()) + 1).arg(voice + 1)
        .arg(asChordRest->tick().ticks()).arg(asChordRest->endTick().ticks()));
}
