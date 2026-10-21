# xapiand-on-strata

- Design: docs/design/xapiand-on-strata.md (revision 1); reviews: none yet.
- Branch / worktree: design/xapiand-on-strata from main 6e13416, at /Users/kronuz/code/Kronuz/strata-wt/xapiand-on-strata
- State: designing (ready for preliminary review)
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - The seam inventory (call-site counts by group, from Xapiand 855f75281), the coverage and gap table, decisions D1 to D12, the ordered path (X1 to X5, P1 to P4, L1, S1 to S5), gate rows T1 to T10, and open questions Q1 to Q3.
- Doing: nothing. The design stops at the first buildable steps (X1, X2; P1 after Q2; L1's design).
- Next: the architect's preliminary review; the owner's Q1 to Q3; the lead writes done criteria and launches X1 and X2.
- Blocked on: Q2 (opaque `bytes` slots for 7a) blocks P1; Q3 (queryparser on the 7a path) blocks S4. Nothing blocks X1 to X4.
- Findings folded in: none yet.
- Last verified: nothing compiled or run (designer, read-only study). Counts come from grep over `git ls-files src` minus `src/xapian/`; line citations checked against the files at the commits named in the design header. One disclosure: the first 140 lines of `src/server/remote_protocol_views.cc` were read before checking PLAN lists it as GPL; nothing past them.
