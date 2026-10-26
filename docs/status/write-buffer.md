# write-buffer

- Design: docs/design/write-buffer.md (revision 4); reviews: `docs/reviews/write-buffer-design.md`
  (preliminary, REJECT, 2026-10-21: WB-1 to WB-6 blocking, WB-7 to WB-15, C1 to C5; probes in
  `.scratch/review/`, kept; re-check of revision 1 at `baf669d`: APPROVE WITH CONDITIONS, WB-16 to
  WB-19 new, C6 and C7; re-check of revision 3 at `35ddfbb`: APPROVE WITH CONDITIONS, WB-20 to
  WB-24 new, C8 to C11; its model in `.scratch/rev3/`, kept).
- Branch / worktree: `design/write-modes` from main `41fde48`, at
  `/Users/kronuz/code/Kronuz/strata-wt/write-modes`. Not merged by the designer. (Started as "write
  modes"; the owner's correction of 2026-10-21 made it one buffer, so the document is
  `write-buffer.md`; the branch name stays.)
- State: designing (revision 4 ready for review: it answers WB-20 to WB-24 and C8 to C11 as the
  re-check of revision 3 specified; step 1's item 4 may proceed under C9; C8's rule awaits the code
  reviewer's file:line confirmation before step 2, and C7's for WB-16 to WB-19 is still owed)
- Done criteria (for the lead to set; contributors tick, never edit): proposed for step 0 in the
  design's "First buildable step" (K1 to K4).
- Proposed done criteria for step 1's item 4 (C9; the lead sets them):
  - [ ] I4-1 WB-21: records in D3's byte layout (flags at offset 24, the docid prev field at 28 when
    its flag is set, then the lengths), and every producer that exists at step 1 (the sequencer
    under one writer, T9's replay) reserves the field from the operation's type; a test decodes a
    `replace_docid` record with and without the field.
  - [ ] I4-2 WB-23: docid maps built only under the lease, at `writable_buffer`, `locate_docid`'s
    frozen-buffer build (D8's exception), and the flush's install; readers probe through a const
    find that never builds and keys on the table pointer's acquire load; `docids_built` no longer a
    plain `mutable bool` read by readers.
  - [ ] I4-3 WB-23: grown key-map and docid-map tables retired under D10's generation stamp, never
    freed at once; a first docid-map build past D12's threshold posted to the pool under D12's rule
    (on a reactor thread).
  - [ ] I4-4 WB-23's T8 additions pass under TSan and ASan at one writer with readers on other
    threads: a first build and a growth of the active buffer's docid map while readers run the docid
    check; a frozen buffer's build while a reader holds its pre-freeze view.
  - [ ] I4-5 T8's D6 schedule passes: a `delete_docid` and a key-changing `replace_docid` of a
    keyed segment copy, then a `replace_docid` writing the deleted docid again, each read by both
    keys from another thread at positions on both sides, before the freeze, after it, and across a
    merge install that carries the pending delete.
  - [ ] I4-6 T9's mutants, each killed by its named test: `no_docid_check` (a reader skipping the
    docid check) and `walk_prev_only` (the docid walk following `prev` where the docid prev field
    holds a reference), both by T8's D6 schedule; a docid-map table freed at once, by I4-4 under
    ASan.
  - [ ] I4-7 (C10, proposed with item 4 because T2's and T3's docid variants time the engine after
    it) WB-22's first-docid-only skip (D6): set before V passes the position, restored on a
    rejection; T8's D6 schedule passes with the skip, and a test shows a reader below the position
    skips the check.
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
  - Revision 4 (WB-20 to WB-24, C8 to C11; the design's "Changes in revision 4"): D4's third
    pre-resolution rule (WB-20); D3's layout with the flags at offset 24 and every record producer
    named (WB-21); D6's docid-check cost per segment copy considered, the first-docid-only skip,
    T3's variant, and where T2's runs (WB-22); lease-only docid-map builds, the const probe, D8's
    exception, D10's docid-map retirement, D12's first-build rule, T8 and T9 additions (WB-23);
    D6's buffers-newer-than-segments invariant and D7's follower install rule (WB-24); item 4's
    done criteria proposed above.
- Doing: nothing.
- Next:
  1. The lead sets item 4's done criteria (proposed above, C9); step 1's item 4 on
     `phase4/write-buffer`.
  2. The code reviewer confirms WB-20's rule in D4 at file:line before step 2 (C8), and WB-16 to
     WB-19 (C7). The prototype has no dedicated cases for T8's WB-16 and WB-19 schedules, nor docid
     operations; step 1 (WB-16) and step 2 (WB-19, WB-20) build them.
  3. C10: the lead times T2's and T3's docid variants on the engine after item 4 (WB-22); if T3's
     shows the O(range) cost, the lead decides on a freeze triggered by docid-only supersessions.
  4. C6: WB-6's remaining choice in D7, with durability-modes' agreement, before step 3; C11's
     follower install rule (D7, WB-24) is in place for step 3.
  5. D14 for Xapiand: the lead's T11a/T11b result (bake-off at `5a41d53`, rule 3) is not folded
     into this revision; it lands as its own decision update.
- Blocked on: nothing for item 4 or step 2's design text. Step 3 needs no journal change (WB-11,
  C2): the lead tells Detent's agent that the staging inbox is strata's. One wording note for the
  lead: WB-23's D8 text keeps the review's fix (a frozen buffer's first docid-map build is a
  lease-holder exception after the freeze) rather than building every frozen buffer's map at the
  freeze, which would cost O(records) per freeze on keyed workloads that never create a docid map.
- Findings folded in: WB-1 to WB-15 and C1 to C5 in revision 1; WB-16 to WB-19 (C7) in revision 2;
  step 1's two docid gaps in revision 3; WB-20 to WB-24 (C8 to C11) in revision 4.
- Last verified: nothing compiled or run (design only). Revision 4's new `file:line`s checked on
  2026-10-26 against `phase4/write-buffer` `f21a10d` (`index.hh:1575-1585`, `:1827`, `:2675-2692`,
  `:2714`, `:3118-3134`; `write_buffer.hh:85-90`, `:104-120`; `docids.hh:166-175`;
  `format.hh:119`); the model's counts read from `.scratch/rev3/docid_model.out`. Revision 3's `file:line`s checked on
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
