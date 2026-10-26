# write-buffer-reads

- Design: `docs/design/write-buffer.md` on `origin/design/write-modes` at `8cdbe61`, revision 4:
  step 1 item 4 (D3, D5, D6, D8, D9, D10, D12; WB-21 to WB-23). Reviews:
  `docs/reviews/write-buffer-design.md` there (WB-16 to WB-24, C7 to C11); the architect's model
  `.scratch/rev3/docid_model.py` in that worktree. Shared-reader D12 (`origin/design/shared-reader`).
  Scan structure: `origin/perf/lanebuf-rebased` (`bench/kv/proto/lanebuf*.hh`).
- Branch / worktree: `phase4/write-buffer-reads`, rebased on `origin/main` at `b38b19f` (items 2
  and 3 landed, CR1 to CR6 closed), at `/Users/kronuz/code/Kronuz/strata-wt/write-buffer-reads`.
  Concurrency tier: a code review before it lands.
- State: in review (concurrency tier: a code review before it lands). Every criterion has evidence
  below; I4-11's CI row names the jobs green when this agent stopped.
- Done criteria (the lead's; ticked only with evidence):
  - [x] I4-1 WB-21: records in D3's byte layout (flags at offset 24, the docid prev field at 28 when
    its flag is set, then the document), and every producer that exists at step 1 (the sequencer
    under one writer, T9's replay) reserves the field from the operation's type; a test decodes a
    `replace_docid` record with and without the field.
  - [x] I4-2 WB-23: docid maps built only under the lease, at `writable_buffer`, `locate_docid`'s
    frozen-buffer build (D8's exception), and the flush's install; readers probe through a const
    find that never builds and keys on the table pointer's acquire load; `docids_built` no longer a
    plain `mutable bool` read by readers.
  - [x] I4-3 WB-23 (the lead's ruling below): grown key-map and docid-map tables retired under D10's generation stamp, never
    freed at once; a first docid-map build past D12's threshold posted to the pool under D12's rule
    (on a reactor thread). The ruling replaces the posted build: inline under the lease.
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
  - [x] I4-8 T8's model-check schedules for item 4 at one writer with readers on other threads pass
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
- Criteria evidence (this head):
  - I4-2, I4-13: `probe_docid` (a const find on the table pointer's acquire load); builds in
    `writable_buffer`, `locate_docid` (stage 1, frozen buffers), the flush (`newest`);
    `docids_built` removed (`docids.built()` reads the pointer); `has_docid()` walks a buffer
    without a map (`has_docid_builds_nothing`, mutant `has_docid_builds` killed); the position
    index's directory is 23 fixed atomic block pointers.
  - I4-1 (`f10f160`): `index::record` is D3's bytes: header at 0 to 24, the flags byte at 24 (tombstone,
    docid prev, ordinal), the fields the flags announce at 28 (docid prev first, then the
    inverter's ordinal), the document after them at the next 8-byte offset (32, or 40 with both
    fields); `static_assert`s pin the flags at 24, the first field at 28, the header at 32 B. The
    lane strides by each record's flags (`lane::append_n`, `record::stride()`), so a rejection's
    cut and the destructors walk mixed shapes. Producers at step 1: stage 1's `push_record` only
    (a `replace_docid` sets the flag and reserves the field from its type, filling it when the
    docid's slot differs from the key's); replay is step 3 (the journal), the leader-copy and the
    applier are D8/D7 paths that do not exist yet. Tests: `record_layout` reads the raw bytes of a
    tombstone (flags 1, document at 32), a `replace_docid` with no predecessor (flags 2, field
    empty), a key-changing one (field at 28 names the old key's record), a same-key one (field
    empty, `prev` set), a re-write of a deleted docid (field names the tombstone), and a plain
    replace (flags 0, document at 32); `record_strides` appends 400 records of five shapes with
    heap-string keys, cuts back to record 150, appends 110 more and rereads all (ASan: a wrong
    stride is an overflow or a double free). Mutants killed on ASan+UBSan
    (`.scratch/wbr/out/m_*`): `flags_misplaced` (flags after the padding) and `dprev_misplaced`
    (field at 32) by `record_layout`; `ordinal_first` (ordinal over the docid prev field) by
    `record_strides`; `fields_not_counted` (the document at 32 with both fields) by
    `record_strides`; `no_dprev` (prev missing on a key-changing `replace_docid`) by
    `record_layout`'s field check and by `d6_schedule`. Bytes (Release, `.scratch/wbr/bytes-run.sh`)
    identical to the base `f75de3a`'s: threads 1 files 9 digest 48c6d41502f08fc0 bytes
    4a39e97a9c882d22; threads 3 files 15 bytes ca8b4359e9bad8d7.
  - I4-3, the lead's ruling (2026-10-26): for step 1 the large builds (the docid map's first build,
    map growth, the scan runs' sorts) run inline under the lease, because `apply()` is synchronous
    and a post to the pool would only block the caller; D12's pool rule (post a growth past 65,536
    slots or a first build past 65,536 records and await it as a task) moves to step 3 with the
    async seam. For the architect: D12's rule is not met at step 1 by decision; `build_docids`
    says so in a comment, and nothing in the core awaits a posted job. Retirement by generation
    done for map tables (`tables_wait_for_views`) and run sets (`runs_by_counter`). The scan
    runs' jobs keep their existing shape: posted to the index's own executor group when the host
    gave one and never awaited (the writer and readers pick up a finished splice later), run
    inline when it did not; that is a post that blocks nobody, and `scan_schedule` and the D9
    mutants cover it. If the ruling meant those inline too, it is `runs_group_ = nullptr`.
  - I4-4, I4-8: `write_buffer_reads_test` under ASan+UBSan and TSan (0 ThreadSanitizer reports):
    model check 0 wrong reads, 0 wrong verdicts (200 seeds held views, 8 seeds with 3 reader
    threads, freezes without a flush build frozen maps under held pre-freeze views), growth under
    readers 0 wrong (221,540 reads on TSan). D9: `scan_schedule` (6 seeds x 3,000 steps, keys with
    tied 8-byte prefixes and zero bytes, run builds on a 2-thread pool, run cap lowered to 64,
    3 reader threads scanning bounded ranges, views held across splices and read later, the lease
    holder's own scans): 15,049 accepted, 158,056 reads on ASan and 112,188 on TSan, 217 run sets
    installed, 37 with compacted levels, 0 wrong, 0 ThreadSanitizer reports; `runs_by_counter`
    (writes alone build nothing, a 64-entry tail asks, 40 replacements, 31 with compacted levels,
    the replaced sets wait for a held view); `stale_splice_dropped`.
  - I4-5, I4-6, I4-7: `d6_schedule`, `first_docid_only`; mutants below.
  - I4-9 (`ce15621`, the byte layout and the 16-entry block): full ctest ASan+UBSan 49 of 49 (334 s)
    and TSan 49 of 49 (1,050 s), 0 ThreadSanitizer reports (`.scratch/wbr/ctest-{asan,tsan}.log`);
    before them, at `c06197d`: ASan+UBSan 49 of 49 (333 s), TSan 49 of 49 (1,236 s). Bytes (Release,
    `.scratch/wbr/mem2.out`, `mem3.out`) identical to the base `f75de3a`: threads 1 files 9 digest
    48c6d41502f08fc0 bytes 4a39e97a9c882d22; threads 3 files 15, bytes ca8b4359e9bad8d7. Before
    D9: full ctest ASan+UBSan 45 of 45 (`.scratch/wbr/ctest-asan.log`, 300 s) and TSan 45 of 45,
    no ThreadSanitizer report (`.scratch/wbr/ctest-tsan.log`, 1064 s), at `320b7d4`'s code; the
    later `has_docid` change rebuilt and passed `write_buffer_reads_test`, `compact_slots_test`,
    `index_test` (ASan) and `write_buffer_reads_test` (TSan). Bytes: items 2-3's harness
    (`.scratch/wbr/bytes.cc`, Release) identical to the base `9a9957a`: threads 1 files 9 digest
    48c6d41502f08fc0 bytes 4a39e97a9c882d22; threads 3 files 15, bytes ca8b4359e9bad8d7.
  - I4-10, the first position-index block (`.scratch/wbr/mem4.out`, `.scratch/wbr/memtab/`, the
    recheck's `memtab` harness, Release, macOS x86_64, resident bytes by `task_info`, trees
    interleaved, `quiet strict`, median of 3 trials per cell, one document 100 B values, the
    document layout, nothing flushes; base is `origin/main` at `1ddb581`, the columns are this
    branch with the first block at 8, 16, 32 and 64 entries). Per open index, create plus N
    documents:

    | documents | base | 8 entries | 16 entries | 32 entries | 64 entries |
    |---|---|---|---|---|---|
    | 0 | 2,666 | 2,650 | 2,687 | 2,654 | 2,654 |
    | 1 | 4,977 | 5,226 | 5,296 | 5,513 | 5,853 |
    | 10 | 6,054 | 6,468 | 6,357 | 6,504 | 6,881 |
    | 100 | 29,864 | 29,155 | 29,209 | 30,851 | 30,335 |
    | per record, 100,000 to 300,000 (1 trial) | 220.0 | 231.2 | 231.2 | 236.4 | 236.5 |

    Chosen: 16 entries (192 B), now `stable_array`'s `base_bits = 4` and 25 directory pointers
    (the same 2^29 entries). With 10 documents the branch costs +303 B an index over the base
    (was +827 at 64 entries); with 1 document +319 (was +876); empty indexes cost nothing; at 100
    documents the cells are within the noise of the base (+/- 1,000); the per-record slope is
    +11 B over the base (the entries' 8 added bytes and block rounding) and the same at 8 and 16
    entries. Eight entries saves 96 B at one document and loses to 16 at ten (a second block);
    32 and 64 cost more at every small size. What remains of the +303 B is the reader core's fixed
    words, the position index's 200 B directory and the empty active buffer a view build creates.
    The slope row is one trial per tree; the earlier 3-trial `bufmem` run gave +8 to +11 B.
  - I4-10 with D9 (`.scratch/wbr/mem3.out`, Release, macOS x86_64, 3 interleaved trials against
    `f75de3a`, items 2-3 with CR6's 1 KiB first chunk; `bufmem --scan 1` adds a one-key scan
    every 1,000 writes, which keeps runs built): resident bytes per buffered record, 100 B values,
    unscanned: base 220.7 / 222.7 / 223.7, branch 231.3 / 231.5 / 231.3 (+8 to +11 B: the
    entries' 8 added bytes, blocks overallocating); 1000 B values: base 1135.0 / 1135.8 / 1138.5,
    branch 1143.7 / 1144.1 / 1143.6 (+5 to +9 B); scanned, 100 B: base 351.4 / 366.6 / 352.6
    (two runs of 4-byte refs plus fold copies), branch 298.6 / 329.7 / 316.2 (22 to 53 B less).
    Per open index with 10 buffered documents: base 6352.9 / 6332.4 / 6357.0, branch 7188.5 /
    7176.2 / 7172.1, about +820 to +840 B: the first position-index block (64 entries, 768 B
    against 256 B) and the reader core's +190 to +270 B. The entries were first measured at +20 B
    a record: `pos_entry`'s default member initializers made each block resident at allocation
    (`mem2.out`); trivial entries fixed it. A smaller first block would cut the per-index cost
    (the many-indexes workload): the lead's call, unmeasured.
  - I4-10 before D9 (`.scratch/wbr/mem.out`, items 2-3's method, Release, macOS x86_64, 3 interleaved
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
  - I4-11: CI on `255c702` (the pre-rebase head with D9 and the LSan fix): 8 of 8 jobs green
    (linux-tsan, linux-asan-ubsan, linux-arm64-gcc, linux-x86_64-clang and -gcc, freebsd-x86_64,
    macos-x86_64 and -arm64), run 40344480229. The rebased head's run was queued when this agent
    stopped, not waited on. Earlier: CI on `8c1f489`: 7 of 8 jobs green; linux-asan-ubsan failed `shared_view` and
    `shared_view_mapped` on LeakSanitizer (24 B: the destructor's leak path for retired tables
    allocated a vector LSan could not reach; macOS has no LSan). Fixed by handing them to
    `detail::keep_forever` as the leaked views are; the fixed head's CI was queued when this
    agent stopped, not waited on.
- Done after the rebase, this round: `f10f160` I4-1's byte layout, tests and mutants; `ce15621` the
  first block at 16 entries and the I4-3 ruling in the code; this ledger. Earlier (pre-rebase hashes up to `270944f`; rebased onto `origin/main`): `b7f8cce` ledger; `d17cad7` V, views holding buffers, reads at r, docid prev, lease-only
  docid builds, retirement by generation; `d0a91af` T8's D6 schedule and model check;
  `8c1f489` the retirement test; `320b7d4` ledger; the CR3/WB-23 commit (check_in's contract,
  `has_docid` without builds). After the rebase: `084644e` (C)'s scan (D9); `850adf1` T8's scan
  schedule, the counter, stale splices; `56ec8c3` the D6 schedule's merge leaves deletes pending
  (CR5 made `force_merge` apply them); `c06197d` trivial position-index entries; this ledger.
- Next: the code review; then land. Step 2's per-index cost beyond +303 B is the reader core's
  fixed words (a candidate for a later pass), not the position index.
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
- **(C)'s scan (D9).** Position-index entries are 12 bytes (`wbuf::pos_entry`: the key's first 8
  bytes big-endian, the reference; keyless records carry prefix 0 and enter no run). Each buffer
  holds an atomic `run_set*` (levels biggest first, the cover) that only the lease holder
  installs, with release. `maintain_runs()` runs at each group's end and at the owner's scan:
  it applies finished jobs' splices (`wbuf::apply_splice` drops one whose consumed levels are
  gone), posts a fresh job when a scan set `wants_runs` and the uncovered backlog passes 64, and
  a compaction when two leading levels past the cap (16,384) meet ratio 8. Jobs run on their own
  executor group (`runs_group_`, beside the merges' so a merge never delays a build); without a
  host executor they run inline, as merges do. A job fills its `run_job` slot and stores `ready`
  with release; a replaced set goes to `retired_runs`, stamped by `retire_tables()` with the
  view generation, and the replacement publishes a view (D10); with no readers it is freed at
  once. A scan at r (`part_at`) loads the set once, takes the tail past its cover at or below r
  in [from, to), selects and sorts 32 when the tail passes 64 and sorts the rest when reached;
  `scan_segments` merges segments and every buffer's levels and tail, prefix first, skipping run
  entries newer than r, the newest record winning within a buffer. The owner's scan is the same
  code at r = all published, with the pending-delete set; `buffer_view` and `run_at` are gone.

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
- D9 mutants (`.scratch/wbr/mutants-d9-{asan,tsan}.log`, on `255c702`'s code), 12 of 12 killed:
  on TSan, `runs_install_relaxed` (56 reports: a reader's `part_at` against the install) and
  `job_ready_relaxed` (25 reports: `take_runs` against the job's splice), both by
  `scan_schedule`; on ASan+UBSan, `runs_freed_at_once` by `scan_schedule` (heap-use-after-free)
  and `runs_by_counter`; `oldest_first` and `prefix_only` by `d6_schedule`, the model check and
  `scan_schedule`; `tail_unfinished`, `from_prefix_only`, `levels_from_start` by
  `scan_schedule`; `levels_ignore_r` by the model check, `runs_by_counter`'s held view and
  `scan_schedule`; `splice_unchecked` by `stale_splice_dropped`; `never_asked` and
  `builds_on_writes` by `runs_by_counter`. The 13 earlier mutants were not rerun after the
  rebases.

## Notes for the designer and the reviewer

- **I4-1's byte layout.** The core's record keeps the layout's typed `document` after the fields
  (the core is generic over layouts), so D3's "lengths, key and value inline" is not what follows
  the fields: the document starts at the next 8-byte offset (32 without fields or with one, 40
  with both) and its size is the layout's. D3's memory gap (inline key and value bytes) stays
  open and belongs to a later step; the flags and field offsets are D3's.
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
- 2026-10-26: (C)'s scan (D9) built, one mechanism for the owner's and views' scans; T8's scan
  schedule and 12 D9 mutants; rebased onto items 2-3's review fixes (CR3 conflict resolved:
  their contract and debug check, plus reads off the owner's thread through views and
  `has_docid` without builds), then onto `origin/main`; full ASan and TSan ctest 49 of 49; bytes
  identical; memory with the 12-byte entries measured. I4-1 not started (budget).
