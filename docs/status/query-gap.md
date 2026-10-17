# query-gap

- Design: docs/design/query-gap.md, revision 0 (this commit); reviews: none yet (the preliminary review goes to docs/reviews/query-gap-design.md)
- Branch / worktree: design/query-gap at /Users/kronuz/code/Kronuz/strata-wt/query-gap
- State: designing (ready for preliminary review)
- Done criteria (set by the lead; contributors tick, never edit):
  - None set yet. The design proposes its gate rows for them: T1 attribution, T2 exactness, T3 item A/Bs, T4 format-13 candidate tables before the freeze, T6 other axes; T5, T7, and T8 are the lead's rows on main.
- Done: design revision 0 (a489a72), with the queued indexing gap noted in the holds and risks (this commit): per-class gaps from the recorded runs (n1 at 2e774db, n2 at db0a891, P1 T2 at c80996b) with the reordered-index projection labeled an estimate; H1–H19, each with the measurement that decides it; M1–M23 with estimated effects, costs, and format-13 dependence; D1–D14; the format-13 decision table; the experiment queue E0–E17 (E0–E7 before the freeze); T1–T8; owner questions Q1–Q3; items for the lead and the architect
- Doing: nothing; waiting for the preliminary review
- Next:
  1. The architect's preliminary review; fold its findings in, citing their IDs.
  2. The owner's Q1–Q3; the lead's base branch for the experiments (design, "For the lead", item 1) and E0.
  3. E1–E7 to a performance engineer under holds the lead grants; E3–E7's tables go to format 13's candidate report before the freeze (F13-R7 item 4, F13-R8).
- Blocked on: nothing for revision 0. The experiments wait on the lead (base branch, E0, holds) and the owner (Q1 before M17 or M18 enters format 13; Q2 only if E3 finds finer partitions pay; Q3 before P5).
- Findings folded in: none yet
- Last verified: 2026-10-17: nothing built, run, or measured (design only; `quiet check` exited 1 under the strict hold strata-trials, `night_gate.sh n1017`, until about 01:17). Code citations checked against origin/main 5691ec7, format 13's against phase4/format13 4266374; the gate VM's raw result files (`~/bench/results/`) not read.
