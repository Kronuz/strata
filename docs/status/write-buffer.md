# write-buffer

- Design: docs/design/write-buffer.md (revision 1); reviews: `docs/reviews/write-buffer-design.md`
  (preliminary, REJECT, 2026-10-21: WB-1 to WB-6 blocking, WB-7 to WB-15, C1 to C5; probes in
  `.scratch/review/`, kept). Concurrency tier: a scoped architect re-check of revision 1 comes
  before step 1; step 0 continues under C1.
- Branch / worktree: `design/write-modes` from main `41fde48`, at
  `/Users/kronuz/code/Kronuz/strata-wt/write-modes`. Not merged by the designer. (Started as "write
  modes"; the owner's correction of 2026-10-21 made it one buffer, so the document is
  `write-buffer.md`; the branch name stays.)
- State: designing (ready for the architect's scoped re-check of revision 1)
- Done criteria (for the lead to set; contributors tick, never edit): proposed for step 0 in the
  design's "First buildable step" (K1 to K4).
- Done:
  - Revision 0: the owner's goals; candidates A (RocksDB memtable), B (Lucene DWPT), C (lanes and
    one shared version map) compared on every axis; C recommended with A' as the other finalist;
    D1 to D14 (the checkout and write lease, lanes, the leader's stage 1, the visible position,
    deletes across writers, turn-log order, freeze, scans, retirement, search, the async seam,
    costs, the single-writer fallback); T1 to T12 with the bake-off's two decision rules; Q1 to Q3;
    step 0 and steps 1 to 4 (`56fd828`).
  - shared-reader revision 2 on `design/shared-reader` (SR-D15 to SR-D17, the write lease, D12
    for this buffer, ledger housekeeping: `b1568a7`; the architect's re-check committed as
    written: `f39dfc6`).
  - Revision 1: WB-1 to WB-15 and C1 to C5 answered (the design's "Changes in revision 1"); the
    owner's Q1 to Q3 recorded; Q4 added from WB-5; the review file committed with it.
- Doing: nothing; waiting for the architect's scoped re-check of revision 1.
- Next:
  1. Fold in the re-check's findings, citing their IDs.
  2. Step 0 (`perf/lanebuf`) continues under C1: the pieces revision 1's "First buildable step"
     lists, K1 to K5; the lead writes the decision rules into `docs/perf/write-buffer-bakeoff.md`
     before the first timed run and measures T1 to T4, T6, T7, T11a, and T11b.
  3. Step 1 after the re-check clears it.
- Blocked on: the architect's scoped re-check (concurrency) for steps 1 to 4. The owner for Q4
  (where the multi-writer decision binds, given that journaled indexes apply in one applier;
  recommended: the split, T11a for Xapiand's journaled path and T11b for journal-less indexes),
  which blocks no step. Step 3 needs no journal change (WB-11, C2): the lead tells Detent's agent
  that the staging inbox is strata's.
- Findings folded in: WB-1 to WB-15 and C1 to C5 in revision 1.
- Last verified: nothing compiled or run (design only). Every `file:line` checked on 2026-10-21
  against strata main `41fde48`, `design/durability-modes` `95357d9`, `explore/buffer-bytes`
  `0a8c85a`, RocksDB 11.11 (`~/.scratch/rocksdb-11.11.fb`), and Lucene 10.4.0's
  `DocumentsWriterPerThread.java`.

## Owner answers (2026-10-21)

- Q1: readers see a write only once it is acknowledged at the index's durability level.
- Q2: search reads at a freeze (publish_through coalesced); gets and scans read live.
- Q3: a stricter bar than proposed: Xapiand keeps multi-writer only if it gives at least 1.5x the
  requests per second or 0.5x the request p99 on the Xapiand-shaped load, at most 3% slower with one
  writer, and at most 5% more CPU per request; otherwise one writer with live readers.
