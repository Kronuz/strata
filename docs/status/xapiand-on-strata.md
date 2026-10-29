# xapiand-on-strata

- Design: docs/design/xapiand-on-strata.md (revision 1); reviews: none yet.
- Branch / worktree: design/xapiand-on-strata from main 6e13416, at /Users/kronuz/code/Kronuz/strata-wt/xapiand-on-strata
- State: Phase 7a gate passed (2026-10-29); the owner split Phase 7's gate the same day (single-node 7a and 7c, then Phase 8; 7b later, PLAN.md). Next: the 7c design on Xapiand:addacf386 + Xapiand:0d194335b: the architect's gate review (docs/reviews/phase-7a-gate.md) rejected on L1 and L2, and its re-check (strata:5e34482) closed L1, L2 and C1-C4; C5 (Linux, FreeBSD, arm64) is owed before gate 7b. The rest of this header records the design stage.
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - The seam inventory (call-site counts by group, from Xapiand 855f75281), the coverage and gap table, decisions D1 to D12, the ordered path (X1 to X5, P1 to P4, L1, S1 to S5), gate rows T1 to T10, and open questions Q1 to Q3.
- Doing: nothing. The design stops at the first buildable steps (X1, X2; P1 after Q2; L1's design).
- Next: the architect's preliminary review; the owner's Q1 to Q3; the lead writes done criteria and launches X1 and X2.
- Blocked on: Q2 (opaque `bytes` slots for 7a) blocks P1; Q3 (queryparser on the 7a path) blocks S4. Nothing blocks X1 to X4.
- Findings folded in: none yet.
- Last verified: nothing compiled or run (designer, read-only study). Counts come from grep over `git ls-files src` minus `src/xapian/`; line citations checked against the files at the commits named in the design header. One disclosure: the first 140 lines of `src/server/remote_protocol_views.cc` were read before checking PLAN lists it as GPL; nothing past them.

## Clean-room note (2026-10-20)

While drafting, the designer read lines 1-140 of Xapiand's `src/server/remote_protocol_views.cc`
(includes and the constructor) before noticing that PLAN lists it as GPL. Nothing past line 140 was
read, and the design takes no content from it: its remote and replication rows rest on grep counts
alone. That file is replaced by the shard RPC in 7b and is never ported. `src/xapian/` and
`sortable_serialise` were never opened.

## Owner answers (2026-10-20)

Q1 single-node 7a, Q2 opaque `bytes` slots first, Q3 `queryparser` on the 7a path. Next: X1, P1, L1.

## Owner answers (2026-10-29)

- Phase 7's gate splits: single-node (7a and 7c), then Phase 8; 7b later (PLAN.md).
- Non-Apple storage keeps the ordering barrier, with a warning, rather than a full flush by default.
- The queryparser accepts wildcards, fuzzy and partial words inside phrases (match_phrase_prefix), after 7c.
- Removed features stay removed when nobody would really use them or a native alternative is
  coming; the owner hears about the rest. Two have real users and native replacements, proposed
  for after 7c: "more like this" (re-analyze the seed documents' stored text, as Elasticsearch
  does without term vectors), replacing `?fuzzy` and `?nearest`; and `min_score` in the
  collector, replacing the weight cutoffs.
