> Status: CURRENT (2026-10-27) the fields-and-keys design workstream. Branch `design/fields-and-keys`, worktree `strata-wt/fields-and-keys`.

# fields-and-keys

- Design: docs/design/fields-and-keys.md (revision 0); reviews: none yet.
- Branch / worktree: design/fields-and-keys from main 53d93fe, at /Users/kronuz/code/Kronuz/strata-wt/fields-and-keys
- State: designing (ready for preliminary review)
- Done criteria (set by the lead; contributors tick, never edit): none set yet. The design proposes F1's and X6's in section 7.
- Done:
  - The problem from the code (Xapiand's concatenation collisions, strata's key-prefix rules, S1's `0xff` adapter), Lucene's and Tantivy's models, decisions D1 to D13, format and API changes, the in-flight verdicts, first steps F1 and X6 with done criteria, gate rows T1 to T7, owner questions Q1 and Q2.
  - The dictionary probe: `docs/design/fields-and-keys/{terms.py,dictprobe.cc,probe.txt}` (per-field dictionaries 7.9% smaller than flat on Wikipedia 100k, 13.1% on bank, 77.9% larger on 2,000 sparse fields, 14.2% with one-block fields).
- Doing: nothing.
- Next: the architect's preliminary review (a format and API shape); the owner's Q1 and Q2; the lead writes F1's and X6's done criteria and launches them.
- Blocked on: nothing. Q1 decides only when S1 to S4 may land.
- Findings folded in: none yet.
- Last verified: the probe compiled and ran under `quiet light fields` (development Mac, shared host, so its find times are informational); every file:line in the design re-read at the commits its header names. Nothing else was compiled or run.

## Log

- 2026-10-27: design revision 0 and the probe; ready for preliminary review.
