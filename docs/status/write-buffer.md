# write-buffer

- Design: docs/design/write-buffer.md (revision 3); reviews: `docs/reviews/write-buffer-design.md`
  (preliminary, REJECT, 2026-10-21: WB-1 to WB-6 blocking, WB-7 to WB-15, C1 to C5; probes in
  `.scratch/review/`, kept; re-check of revision 1 at `baf669d`: APPROVE WITH CONDITIONS, WB-16 to
  WB-19 new, C6 and C7).
- Branch / worktree: `design/write-modes` from main `41fde48`, at
  `/Users/kronuz/code/Kronuz/strata-wt/write-modes`. Not merged by the designer. (Started as "write
  modes"; the owner's correction of 2026-10-21 made it one buffer, so the document is
  `write-buffer.md`; the branch name stays.)
- State: designing (revision 3 ready for the architect's re-check: it extends D5's and D6's reader
  rules for docid-addressed writes; step 1's item 4 waits for it, items 1 to 3 do not. Revision 2's
  C7 text for WB-16 to WB-19 still awaits the code reviewer's file:line confirmation)
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
  - Revision 2 (C7): WB-16 in D6 and D5 (frozen key-value buffers carry no bits; shadowing by key
    or docid; pending deletes name segments only; the flush install clears copies newer buffers
    hold); WB-17 in D9 and D10 (builds only when a scan's tail passes 64; publications restated
    from the prototype's counts; T3 reports them); WB-18 in D4 and D5 (a component epoch keys
    pre-resolution, the generation keys retirement); WB-19 in D3 and D10 (a fixed chunk directory
    and a push-only lane list, never replaced); D9 replaced by the owner's accepted structure with
    its measured costs and the two queued optimizations. Cited against `perf/lanebuf-rebased`
    `5a41d53` (code identical to `522faec`).
  - Revision 3: the two gaps from step 1's ledger (`phase4/write-buffer` `f21a10d`). Gap 1, a keyed
    segment copy deleted or re-keyed by docid: D6 shadows it by docid through the view's active
    buffer's docid map, checked by every reader's get and scan (option (c)); resolving the key at
    stage 1 (a) and a bitmap per segment (b) weighed with costs and rejected. Gap 2, one `prev`:
    D3 adds a docid prev field to `replace_docid` records, filled when the docid's predecessor is
    not the key's; D5's docid walk follows it. T2 variant, T8 schedule, two T9 mutants; step 1's
    item 4 records the two deferrals (map-table retirement, D10; 12-byte position index, D9),
    unchanged. Both gaps extend the reader protocol: architect re-check due.
- Doing: nothing.
- Next:
  1. The architect re-checks revision 3 (D3's docid prev, D5's docid walk, D6's docid check)
     before step 1's item 4.
  2. The code reviewer confirms WB-16 to WB-19 at file:line (C7). The prototype has no dedicated
     cases for T8's WB-16 and WB-19 schedules, nor docid operations; step 1 (WB-16) and step 2
     (WB-19) build them.
  3. C6: WB-6's remaining choice in D7, with durability-modes' agreement, before step 3.
  4. D14 for Xapiand: the lead's T11a/T11b result (bake-off at `5a41d53`, rule 3) is not folded
     into this revision; it lands as its own decision update.
  5. Step 1 on `phase4/write-buffer`.
- Blocked on: step 1's item 4, on the architect's re-check of revision 3; nothing else for steps 1
  and 2. One note for the code reviewer: WB-16's keyless half (a docid tombstone shadowing a keyless
  document in a frozen buffer, and the flush install probing newer docid maps) extends revision 1's
  docid tombstone from the active buffer to frozen buffers; step 1's `keyless_docid_shadowing` test
  covers it at one writer, and the re-check of revision 3, which extends shadowing by docid to
  keyed segment copies, can rule on both together. Step 3 needs no journal change (WB-11, C2): the
  lead tells Detent's agent that the staging inbox is strata's.
- Findings folded in: WB-1 to WB-15 and C1 to C5 in revision 1; WB-16 to WB-19 (C7) in revision 2;
  step 1's two docid gaps in revision 3.
- Last verified: nothing compiled or run (design only). Revision 3's `file:line`s checked on
  2026-10-26 against `phase4/write-buffer` `f21a10d` (`index.hh`, `reader.hh`, `keys.hh`,
  `merge.hh`, `deleted_bits.hh`) and Kronuz/termdict `8b1e964`. Revision 2's `file:line`s checked on
  2026-10-26 against `perf/lanebuf-rebased` `5a41d53` and the re-check at `baf669d`; publication
  and pre-resolution counts read from the lead's raw lines. Revision 1's checked on 2026-10-21
  against strata main `41fde48`, `design/durability-modes` `95357d9`, `explore/buffer-bytes`
  `0a8c85a`, RocksDB 11.11 (`~/.scratch/rocksdb-11.11.fb`), and Lucene 10.4.0's
  `DocumentsWriterPerThread.java`.

## Owner answers (2026-10-21)

- Q1: readers see a write only once it is acknowledged at the index's durability level.
- Q2: search reads at a freeze (publish_through coalesced); gets and scans read live.
- Q3: a stricter bar than proposed: Xapiand keeps multi-writer only if it gives at least 1.5x the
  requests per second or 0.5x the request p99 on the Xapiand-shaped load, at most 3% slower with one
  writer, and at most 5% more CPU per request; otherwise one writer with live readers.
- Q4 (2026-10-21): confirmed: T11a (the journaled applier path) decides Xapiand's mode against the
  1.5x/0.5x bar; T11b measures the unjournaled write-group path.
- WB-6 (2026-10-21): the visible point V tracks the index's default durability level. A write that
  asks for a stronger level waits for its own reply but never holds back later writes' visibility.
- D9 (2026-10-26): C is the write buffer; its scan p99.9 at full write speed, 1.5 to 1.8x
  RocksDB's, is accepted; a sampled index per big level and the visibility position in the entry
  are queued after the buffer is built (bake-off `:270-275`).
