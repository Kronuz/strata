> Status: CURRENT (2026-10-27) the fields-and-keys design workstream. Branch `design/fields-and-keys`, worktree `strata-wt/fields-and-keys`.

# fields-and-keys

- Design: docs/design/fields-and-keys.md (revision 1); review: docs/reviews/fields-and-keys-design.md (approve with conditions, on revision 0).
- Branch / worktree: design/fields-and-keys from main 53d93fe, at /Users/kronuz/code/Kronuz/strata-wt/fields-and-keys
- State: designing (ready for preliminary review): revision 1, for the re-check of FK-R1 to FK-R5
- Done criteria (set by the lead; contributors tick, never edit): none set yet. The design proposes F1's, X6's, F2's and SF's in section 7.
- Done:
  - Revision 0 (strata:eef97f2): the problem from the code, Lucene's and Tantivy's models, D1 to D13, steps F1 and X6, rows T1 to T7, Q1 and Q2; the `terms.py`/`dictprobe` probe.
  - Revision 1 (this commit): FK-R1 (D2: `declared(i)` references for new fields, ids under a chained `expected_fields` mark, `fields_conflict`; the four races; F1's tests), FK-R2 and FK-R3 (section 2 on Xapiand's real terms with directory and norms; D3 pool < 256 and a sampled LEB directory with entries only for own-dictionary or normed fields; D4 and D5 statistics and norms only where read; T1 restated), FK-R4 (D5 host lengths with the id term's wdf, D12 freqs, T3b), FK-R5 (D14, `shards.hh` changes, T5b), FK-R6 into SF, FK-R7 to FK-R11 into D3, D10, F2's criteria and T8; the lead's rulings on Q1 (a, under C1 to C4) and Q2 (a key field).
  - Revision 1's probe: `docs/design/fields-and-keys/{xapiand_terms.py,sparse_corpora.py,fieldsize.py,fieldprobe.cc,run-r1.sh,probe-r1.txt}`. Under `bm25_xapian`, with the directory: bank 0.875x flat in one segment and 0.879x over five shards; sparse-kw 1.110x and 1.072x; sparse-num (2,100 engine fields) 0.936x and 0.881x; Wikipedia 0.921x.
- Doing: nothing.
- Next: the rechecker confirms FK-R1 to FK-R5; the lead adds C2's grep check to `xapiand-on-strata.md`'s S5 entry criteria and writes F1's and X6's done criteria.
- Blocked on: nothing.
- Findings folded in: FK-R1 to FK-R11 and C1 to C6 (design section "Review findings folded in").
- Last verified: the whole probe ran twice end to end under `quiet light fields-rev` (Xapiand 7fcbf0000 indexing bank, sparse-kw and sparse-num; termdict cdb8042), with all 18 reports byte-identical between runs; each shard's wdf sum matched its total length; the architect's `dirprobe.py` rerun reproduces its 530 B and 18,000 B. Every new file:line re-read at the commits the design's header names. No strata, prospect or Xapiand build or test ran.

## Log

- 2026-10-27: design revision 0 and the probe; ready for preliminary review.
- 2026-10-27: architect review: approve with conditions (FK-R1 blocks F1; FK-R2 to FK-R5 block F2).
- 2026-10-27: revision 1 closes FK-R1 to FK-R5 with the real-terms probe; Q1 and Q2 ruled; ready for the re-check.
