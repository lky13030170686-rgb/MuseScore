//! ══ ONE PLACE FOR THE NUMBERS (第 167 条) ══════════════════════════════════════════════════════════
//! Why this file exists: a sweep of the 17 AgentHarness QML files found **34 radius literals using
//! 11 different values** — 1 / 3 / 5 / 6 / 8 / 12 / 14 / 16 / 17 / 22 / 28. DSH uses **six**.
//! Some of the odd ones are arithmetic in disguise (`17` is half of the 34px button, `5`/`3`/`1` are
//! hairlines and dots), and that arithmetic is invisible at the call site: two files can say `12` and
//! `14` and a reader has no way to tell whether that is a decision or an accident.
//!
//! ⛔ So this is a SINGLE SOURCE, not a theme. It holds the values we are willing to defend, and
//! deliberately does NOT invent a scale for everything: `1`, `3`, `5` and `17` stay at their call sites
//! until somebody decides what they mean, because pretending they fit a scale would be worse than
//! leaving them visible.
//!
//! ⚠️ QML has no CSS variables, so "one place" here means a singleton that every file imports:
//!     import MuseScore.AgentHarness 1.0
//!     radius: AgentMetrics.radiusLarge
//! The compiler cannot stop a new literal, so the guard is a grep in the review checklist
//! (`AgentHarness/界面规格.md` §七): a new `radius: <number>` outside this table is a question to answer.

pragma Singleton

import QtQuick

QtObject {
    //! ── The six radii, matched to DSH's token scale ────────────────────────────────────────────────
    //! Source: `packages/client/ui-theme/src/styles/base.css` on 2026-10-31 —
    //! `--dsw-radius-xs: 4 / sm: 8 / md: 12 / lg: 16 / xl: 20 / panel: 28`.
    //! ⚠️ They are NOT copied as "these are the numbers DSH likes": they are copied because our own
    //! values cluster around four of them, and having four names is what makes the other seven visible.
    readonly property int radiusXs: 4      // hairlines, dots, tiny chips
    readonly property int radiusSm: 8      // session rows, inline chips
    readonly property int radiusMd: 12     // cards, tool rows, result blocks
    readonly property int radiusLg: 16     // bubbles, larger containers
    readonly property int radiusXl: 20     // the user bubble (was 22 before 2026-10-31)
    readonly property int radiusPanel: 28  // dialogs and floating panels

    //! ── Full-round helpers ────────────────────────────────────────────────────────────────────────
    //! ⚠️ These are written as half-width because `Rectangle.radius` does not understand "50%".
    //! Every call site that used to say `backgroundRadius: 17` for a 34px button is ONE decision —
    //! a button is round — and it should read that way.
    function pill(height) {
        return Math.round(height / 2)
    }

    //! ── Spacing we will defend ────────────────────────────────────────────────────────────────────
    //! Source: `packages/client/ui-chat/README.md` (2026-10-31) — "Ordinary flow rows sit 4px apart.
    //! An expanded group title has 8px before its content, Assistant responses have 12px from adjacent
    //! process rows, and completed-Turn controls have 16px from their input."
    //! ⚠️ Four levels, not a spacing scale: DSH picked them per RELATIONSHIP (row→row, title→content,
    //! reply→process, control→input), so each name says the relationship, not the size.
    readonly property int gapRows: 4        // between ordinary flow rows
    readonly property int gapTitleContent: 8 // an expanded group title to its content
    readonly property int gapReply: 12      // an assistant reply and its neighbours
    readonly property int gapControl: 16    // a turn's control row and its input

    //! ── Control sizes that appear more than once ──────────────────────────────────────────────────
    //! ⚠️ 28 is DSH's action-button size (`MessageIconActions.module.css`), and we use it for the copy
    //! button, the search toggle and the error affordance — three places, one number.
    readonly property int actionButton: 28
    readonly property int sessionRow: 32    // `ui-workspace/.../Rows.module.css:103-110`
    readonly property int panelRow: 36      // panel navigation rows — a DIFFERENT thing (界面规格.md §四)
}
