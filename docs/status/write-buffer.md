# write-buffer

- Design: docs/design/write-buffer.md (revision 0); reviews: none yet. Concurrency tier: the
  architect's design review comes before anything but the step-0 bake-off prototype is built.
- Branch / worktree: `design/write-modes` from main `41fde48`, at
  `/Users/kronuz/code/Kronuz/strata-wt/write-modes`. Not merged by the designer. (Started as "write
  modes"; the owner's correction of 2026-10-21 made it one buffer, so the document is
  `write-buffer.md`; the branch name stays.)
- State: designing (ready for preliminary review)
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
- Doing: nothing; waiting for the architect's preliminary review of this design and of
  shared-reader revision 2.
- Next:
  1. Fold in the architect's findings, citing their IDs.
  2. Step 0 (`bench/kv/proto/lanebuf`) may start now on its own branch: it is benchmark code
     outside the engine; the lead measures T1 to T4, T6, and T11 on it.
  3. Step 1 after the review clears it.
- Blocked on: the architect's review (concurrency) for steps 1 to 4; the owner for Q1 (visibility
  at acknowledgement), Q2 (search reads at a freeze), Q3 (the multi-writer threshold), none of
  which blocks step 0. Step 3 also needs Kronuz/journal to accept staged batches from any thread
  (D7): durability-modes and journal's owner, with Detent's agent told first.
- Findings folded in: none yet.
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
