# write-buffer-reads

- Design: `docs/design/write-buffer.md` on `origin/design/write-modes` at `8cdbe61`, revision 4:
  step 1 item 4 (D3, D5, D6, D8, D9, D10, D12; WB-21 to WB-23). Reviews:
  `docs/reviews/write-buffer-design.md` there (WB-16 to WB-24, C7 to C11); the architect's model
  `.scratch/rev3/docid_model.py` in that worktree. Shared-reader D12 (`origin/design/shared-reader`).
  Scan structure: `origin/perf/lanebuf-rebased` (`bench/kv/proto/lanebuf*.hh`).
- Branch / worktree: `phase4/write-buffer-reads`, stacked on `phase4/write-buffer` at `9a9957a`
  (items 2 and 3, in code review), at `/Users/kronuz/code/Kronuz/strata-wt/write-buffer-reads`.
  Concurrency tier: a code review before it lands.
- State: building: the reader core is in and verified; (C)'s scan structure (D9), I4-1's byte
  layout and D12's pool rule remain (Next). Stopped at a clean commit for the next agent.
- Done criteria (the lead's; ticked only with evidence):
  - [ ] I4-1 WB-21: records in D3's byte layout (flags at offset 24, the docid prev field at 28 when
    its flag is set, then the lengths), and every producer that exists at step 1 (the sequencer
    under one writer, T9's replay) reserves the field from the operation's type; a test decodes a
    `replace_docid` record with and without the field.
  - [x] I4-2 WB-23: docid maps built only under the lease, at `writable_buffer`, `locate_docid`'s
    frozen-buffer build (D8's exception), and the flush's install; readers probe through a const
    find that never builds and keys on the table pointer's acquire load; `docids_built` no longer a
    plain `mutable bool` read by readers.
  - [ ] I4-3 WB-23: grown key-map and docid-map tables retired under D10's generation stamp, never
    freed at once; a first docid-map build past D12's threshold posted to the pool under D12's rule
    (on a reactor thread).
  - [x] I4-4 WB-23's T8 additions pass under TSan and ASan at one writer with readers on other
    threads: a first build and a growth of the active buffer's docid map while readers run the docid
    check; a frozen buffer's build while a reader holds its pre-freeze view.
  - [x] I4-5 T8's D6 schedule passes: a `delete_docid` and a key-changing `replace_docid` of a
    keyed segment copy, then a `replace_docid` writing the deleted docid again, each read by both
    keys from another thread at positions on both sides, before the freeze, after it, and across a
    merge install that carries the pending delete.
  - [x] I4-6 T9's mutants, each killed by its named test: `no_docid_check` (a reader skipping the
    docid check) and `walk_prev_only` (the docid walk following `prev` where the docid prev field
    holds a reference), both by T8's D6 schedule; a docid-map table freed at once, by I4-4 under
    ASan.
  - [x] I4-7 (C10) WB-22's first-docid-only skip (D6): set before V passes the position, restored
    on a rejection; T8's D6 schedule passes with the skip, and a test shows a reader below the
    position skips the check.
  - [ ] I4-8 T8's model-check schedules for item 4 at one writer with readers on other threads pass
    under TSan and ASan, mirroring the architect's model (0 wrong reads), with every T9 mutant for
    item 4 killed by a named test.
  - [x] I4-9 The core's suites pass unchanged; flushed bytes identical to the base on the items 2-3
    byte harness.
  - [x] I4-10 Memory per buffered record and per open index reported against the base (the 12-byte
    position-index entries cost bytes: report them).
  - [ ] I4-11 CI green on the branch (push and move on).
  - [x] I4-12 (CR3, the items 2-3 code review, `docs/reviews/write-buffer-step1-code.md` on
    `phase4/write-buffer`) The contract narrowed: the index's own reads (`get`, `scan`,
    `has_docid`, `find_key`, `segments()`, `digest()`, `buffered_count()`) are the owner's and
    run on the lease holder's thread; reads from other threads go only through views at a
    position (`reader_handle::live()`, `view::at(r)`); no lease on `get`.
  - [x] I4-13 (the review's WB-23 specifics) A const, non-building docid probe with "built" from
    the table pointer; grown tables retired by generation; `stable_array`'s block directory never
    reallocated under readers; `has_docid() const` builds no map.
- Criteria evidence (this head; rerun the sanitizer and byte rows once D9's scan replaces the
  interim one):
  - I4-2, I4-13: `probe_docid` (a const find on the table pointer's acquire load); builds in
    `writable_buffer`, `locate_docid` (stage 1, frozen buffers), the flush (`newest`);
    `docids_built` removed (`docids.built()` reads the pointer); `has_docid()` walks a buffer
    without a map (`has_docid_builds_nothing`, mutant `has_docid_builds` killed); the position
    index's directory is 23 fixed atomic block pointers.
  - I4-3: retirement by generation done (`tables_wait_for_views`); D12's pool rule not done: the
    core's `apply()` is not a task on a reactor yet, so a first build past 65,536 records runs
    inline. Open.
  - I4-4, I4-8: `write_buffer_reads_test` under ASan+UBSan and TSan (0 ThreadSanitizer reports):
    model check 0 wrong reads, 0 wrong verdicts (200 seeds held views, 8 seeds with 3 reader
    threads, freezes without a flush build frozen maps under held pre-freeze views), growth under
    readers 0 wrong (221,540 reads on TSan). I4-8 stays open until D9's scan has its schedule.
  - I4-5, I4-6, I4-7: `d6_schedule`, `first_docid_only`; mutants below.
  - I4-9: full ctest ASan+UBSan 45 of 45 (`.scratch/wbr/ctest-asan.log`, 300 s) and TSan 45 of 45,
    no ThreadSanitizer report (`.scratch/wbr/ctest-tsan.log`, 1064 s), at `320b7d4`'s code; the
    later `has_docid` change rebuilt and passed `write_buffer_reads_test`, `compact_slots_test`,
    `index_test` (ASan) and `write_buffer_reads_test` (TSan). Bytes: items 2-3's harness
    (`.scratch/wbr/bytes.cc`, Release) identical to the base `9a9957a`: threads 1 files 9 digest
    48c6d41502f08fc0 bytes 4a39e97a9c882d22; threads 3 files 15, bytes ca8b4359e9bad8d7.
  - I4-10 (`.scratch/wbr/mem.out`, items 2-3's method, Release, macOS x86_64, 3 interleaved
    trials, no readers): resident bytes per buffered record, 100 B values, base 223.4 / 228.7 /
    223.2, branch 223.4 / 222.9 / 220.3; 1000 B values, base 1141.3 / 1135.6 / 1133.3, branch
    1133.0 / 1140.9 / 1135.8: no change (the record is the same size; position-index entries are
    still 4 B: D9's 12-byte entries will add 8 B per record and must be measured then). Per open
    index, empty: base 2842.6 / 2809.9 / 2814.0, branch 2797.6 / 2801.7 / 2822.1; with 10 buffered
    documents: base 9371.6 / 9363.5 / 9363.5, branch 9637.9 / 9584.6 / 9556.0, about +190 to
    +270 B: the position index's fixed directory (23 pointers, 184 B) plus its published count,
    the buffer's base and first-docid-only words (24 B). With readers, a view also holds an
    empty active buffer after each publication (not measured).
  - I4-12: `check_in()`'s comment narrowed; other threads read only through views.
  - I4-11: CI on `8c1f489`: 7 of 8 jobs green; linux-asan-ubsan failed `shared_view` and
    `shared_view_mapped` on LeakSanitizer (24 B: the destructor's leak path for retired tables
    allocated a vector LSan could not reach; macOS has no LSan). Fixed by handing them to
    `detail::keep_forever` as the leaked views are; the fixed head's CI was queued when this
    agent stopped, not waited on.
- Done: `b7f8cce` ledger; `d17cad7` V, views holding buffers, reads at r, docid prev, lease-only
  docid builds, retirement by generation; `d0a91af` T8's D6 schedule and model check;
  `8c1f489` the retirement test; `320b7d4` ledger; the CR3/WB-23 commit (check_in's contract,
  `has_docid` without builds).
- Next (in order):
  1. (C)'s scan (D9): the 12-byte prefixed position index, the binary counter of runs (ratio 8),
     fresh and compaction jobs applied by the lease holder as splices, the build flag past a
     64-entry tail, the select-32 tail, the prefix-first merge, run sets retired by D10. It
     replaces both scans that exist now (the lease holder's two-run `buffer_view` and a reader's
     private sort, `run_at`): one mechanism.
  2. I4-1's byte layout (below), I4-3's pool rule (D12), I4-9's byte check and I4-10's memory.
- Blocked on: nothing.

## What item 4 builds so far

- **V and view positions (D5, D7).** `visible_` is the accepted prefix, one per accepted
  transaction at one writer; a buffer's `base` is V when it became active and a record's position
  is `base + position`. At each `apply()`'s end (the group's end) `publish_cover()` publishes a
  view when its buffers are not the index's or a map table was replaced (rule 4), then V is
  stored with release in the reader state (rules 1 and 3; verdicts return after it, rule 2).
  Views hold the active and frozen buffers (`shared_ptr`) and `visible_`. With readers, a view
  build creates an empty active buffer, so the next write needs no view of its own (this keeps
  the shared-reader suites' publication counts unchanged). Inverting layouts keep views without
  buffers: search reads at a freeze (D11), and their tables are freed at once.
- **Reads at r.** `reader_handle::live()` loads V, acquires a view, and reads at
  `max(V, view.visible())`; `view::at(r)` is the same for a held view. `get_at`: the key's slot,
  `prev` while newer than r, frozen buffers newest first, then segments with the view's bits
  and D6's docid check against the view's active buffer, skipped below its first docid-only
  supersession. `scan_at`: per buffer, the published position index cut at r by binary search,
  sorted by key, newest first (a private run), merged with the segments; a segment copy selected
  as its key's newest passes the docid check, emitted or not.
- **Records (WB-21).** `slot` gave way to `docid_prev` (no size change); the scan's tie-break
  within a buffer compares references (one lane: they grow with appends). Stage 1 sets the
  docid-prev flag on every `replace_docid` record and fills the field with the docid's slot
  when it differs from `prev`.
- **Docid maps (WB-23).** `docids_built` is gone: the lease holder tests `docids.built()`;
  readers use `probe_docid`, a const find on the table pointer's acquire load. Builds happen in
  `writable_buffer`, in `locate_docid` (a frozen buffer's, D8's exception), and in the flush
  (`newest`); the owner's `digest()` also reaches `newest`.
- **Retirement (D10).** `version_map::grow` keeps the replaced table; `retire_tables()` stamps it
  with the current generation (or frees it when no reader reads buffers) and the replacement
  publishes a view; `free_tables()` frees stamps below every view the registry still names. The
  registry scan runs every 8 groups or past 32 retired objects. `reserve` grows to the final
  size in one step. The position index's directory is fixed at creation (23 blocks of 64 << k
  entries) and its count is published at the group's end.

## Evidence

- ASan+UBSan (Debug -O1, Homebrew clang 23.1.2, macOS x86_64): `write_buffer_reads_test`:
  D6 schedule 18 reads at 9 positions, 0 wrong; model check 200 seeds with held views (52,035
  accepted, 20,077 rejected, 53,396 reads, 0 wrong, 0 verdicts wrong) and 8 seeds with 3 reader
  threads (2,041 accepted, 67,234 reads, 0 wrong); growth under readers 468,002 reads, 0 wrong.
  `write_buffer_test` and `shared_view_test` unchanged and passing.
- Mutants (`.scratch/wbr/mutant.py`, in place, rebuilt, restored; logs `.scratch/wbr/out/`), 12
  of 12 killed on ASan+UBSan: `no_docid_check` and `walk_prev_only` by `d6_schedule` (and the
  model check); `check_ignores_r`, `key_ignores_r`, `index_unpublished` by `d6_schedule`;
  `fdo_never_set` by `d6_schedule` and `first_docid_only`; `fdo_not_restored` by
  `first_docid_only`; `no_dprev` by `d6_schedule`'s field check; `no_cover` by `d6_schedule`
  and the model check; `v_before_cover` by the threaded model check (wrong reads at r 16, 55);
  `table_freed_at_once` by `growth_under_readers` (UBSan: a reader's probe of a freed table,
  "applying non-zero offset to null pointer", rc -6, twice); `tables_freed_early` by
  `tables_wait_for_views`; `has_docid_builds` by `has_docid_builds_nothing` (13 of 13).

## Notes for the designer and the reviewer

- **I4-1's byte layout.** The core's record is a typed struct around the layout's `document`
  (items 2-3); D3's byte layout (flags at 24, the docid prev field at 28, then lengths, key and
  value inline) is a separate change to every record producer and reader, and it is also what
  closes D3's memory gap. Item 4 carries the field and both flags in the typed record; I4-1 stays
  open until the layout lands.
- **Replay** does not exist in the core at step 1 (the journal is step 3), so the only producer
  is stage 1.
- **The empty active buffer** a view build creates (only with readers) is not in the design,
  which creates a buffer at the next write; without it, the first write after every refresh
  publishes a view (D5's rule), which changes `shared_view_test`'s publication counts.
- **Merges** while records are buffered still publish no view (the shared reader's rule): the
  current view keeps its old segments, which are exact at its position, and later records sit
  in the active buffer it holds. A view lists a merge's output once something else publishes.
- 2026-10-26: reader core built (V, views with buffers, reads at r, docid walk and check,
  first-docid-only skip, lease-only builds, retirement by generation); tests and 13 mutants;
  full ASan and TSan ctest green; bytes identical; memory measured. The shared-reader-views
  build's `_deps` vanished mid-run, so builds use copies in `.scratch/wbr/deps` (same pins as
  main). Coordinator's note folded in: CR3 and the review's WB-23 specifics (I4-12, I4-13).
