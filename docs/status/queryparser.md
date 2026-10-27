> Status: CURRENT (2026-10-26) ledger of workstream L1 (queryparser design).

# queryparser

- Design: docs/design/queryparser.md (revision 1); reviews: none yet.
- Branch / worktree: design/queryparser from main 5a0299b, at ~/code/Kronuz/strata-wt/queryparser
- State: designing (ready for preliminary review)
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - strata:2731d45 Design with D1 to D10 (incl. the Lucene/Tantivy extension survey requested by the owner's
    2026-10-26 scope change), gate rows T1 to T7, open questions Q1 to Q5, steps 1 to 5+.
- Doing: nothing.
- Next: the architect's preliminary review; the owner's Q1 to Q5; the lead writes done
  criteria and launches step 1 (analysis word scanner and parser core; unblocked).
- Blocked on: Q1 (who records the V2 vectors) blocks step 3; Q3 blocks the extension steps.
- Last verified: nothing compiled or run. Oracle facts from strata-oracle a2e1d82
  `vectors/out/parse.jsonl` (528 rows, summarized with a Python script); Xapiand citations
  checked at engine-query a808ecc9d; Lucene at 9983b7c, Tantivy 0.26.0 sources.

## Log

- 2026-10-26: Clean room held: no Xapian source, no GPL file (`oracle.cc` not opened);
  xapian.org `queryparser.html` read as public documentation.
