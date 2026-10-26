# Code review: write buffer step 1, item 4 (`phase4/write-buffer-reads` 309d36c)

Reviewer: code reviewer, concurrency tier. Head `309d36c` (pushed; `e6273e5` is the last code
commit, later commits are ledger text). Merge base `b38b19f`; `origin/main` is at `cf2eb48` (fuzz
corpus and `format14_test` only): `git merge-tree` shows no conflict. Design
`origin/design/write-modes` `8cdbe61` revision 4 (D3, D5, D6, D9, D10, D12; WB-21 to WB-23).
Worktree `/Users/kronuz/code/Kronuz/strata-wt/review-wbr`; every probe, mutant and log is under
its `.scratch/` (listed at the end). TSan build (Homebrew LLVM, Debug -O1) for the branch's tests,
an ASan+UBSan build of the libraries for the probes.

## Verdict: APPROVE WITH CONDITIONS

The read path is sound where the tests reach it: V and view positions, `max(V, view)` reads, the
docid walk and check, the first-docid-only skip, lease-only docid builds, retirement by generation
(a mutant freeing at stamp equality is killed), the 12-byte entries, the run counter and the
splices, and the new record bytes all held up against my reading and against the TSan run (0
reports). What blocks landing is one hole in the rejection path that item 4's readers turn into a
heap-use-after-free (CR1), and a test gap on the one D9 mechanism whose removal no test notices
(CR2). Both fixes are small. Conditions: CR1 fixed with a test that fails on this head, CR2's
probe turned into a test, then a scoped re-check (my p1, p1b and p2 are the re-check).

## Blocking findings

### CR1. A rejected transaction's records are destroyed while a reader may still hold them

- Where: `index.hh:3246-3275` (`rollback()` runs `b->lane.truncate(mark_.lane)` at `:3270`),
  `write_buffer.hh:349-356` (`truncate` destroys the records, `::operator delete`s every chunk
  opened after the mark and zeroes its directory entry), `index.hh:3270-3273` (`bytes`, `docs`
  and `order` are rewound; nothing counts the records that stay).
- What: stage 1 stores the rejected record's key-map slot (`push_record`, `:3327`) before the
  transaction's verdict; a reader on another thread can load that slot and run
  `find_key_in`'s compare (`:3130`) or `key_at`'s `position`/`prev` reads (`:1746`) on the record.
  Item 4 is the first step with such a reader. The design says what must happen: D3 "a rejected
  record's slot is restored (D4), which hides it from the map", D4 "its records stay in the lane
  until the buffer is freed, unreferenced; a reader that loaded a slot before its restore reads a
  complete record whose position is above its read position and follows `prev`". The code
  instead destroys the record at the rollback (the step-1 review's CR1 fix, whose own text said
  "items 2 and 3 have no reader off the lease holder", `write-buffer-step1-code.md`, CR1:
  "If the designer wants the records kept for item 4's readers, then count their bytes ..."), so
  item 4 inherits an obligation nobody picked up: the ledger's I4 criteria and the tests do not
  mention it. Three consequences, all reachable by an ordinary optimistic-concurrency retry
  (`{replace k, add <existing key>}`) beside any reader of `k`:
  1. The record's key (and stored) string is freed while the reader compares it: heap-use-after-free.
  2. When the rejected transaction opened a chunk, the chunk is freed and its directory entry is
     zeroed: a reader holding a reference into it dereferences freed memory or a null `p`.
  3. The writer's next record is constructed at the same lane offset (the mark is the same), so a
     stale reader reads another key's header: its `prev` then leads into a different key's chain
     (a wrong answer, not only a fault). I did not catch this one returning a wrong value in
     4.7 million reads; TSan reports the unsynchronized rewrite of the header (below).
- Probes (kept):
  - `.scratch/p1b_reject_chunk.cc`, ASan+UBSan, `./p1b_asan 100000 100`: three readers loop
    `h.live().at.get(K)` (a 41-byte key, 100-byte value) while the writer applies
    `{replace K, add X}` (X exists, so version_conflict) 100,000 times. Result on the first run
    and on every run since: `AddressSanitizer: heap-use-after-free ... READ of size 41 ... memcmp
    ... find_key_in`'s lambda, freed by `record::~record() <- lane::destroy <- rollback() <- apply`
    (`.scratch/p1b_asan.out`). The same at a 700-byte value (the chunk-crossing case).
  - `.scratch/p1_reject_race.cc`, TSan, `./p1_tsan 300000`: 2 data-race reports, the writer's
    `lane::append_n` writing a record header (`position` at +0x20 into the block, the flags byte)
    that `key_at` read on a reader thread (`.scratch/p1_tsan_big.out`). 4,732,886 reads, 0 wrong.
  - Why the suite is silent: `model_check` and `scan_schedule` apply single-operation
    transactions, which reject in stage 1 before `push_record`; `growth_under_readers` applies
    multi-operation ones, every one accepted (`CHECK(ok(...))`); the one test of a rejection that found a segment copy by docid
    (`write_buffer_reads_test.cc:286`) has no reader thread. A rejection that already pushed a
    record needs two operations, and none runs beside a reader. T8's "some rejected" writes do not
    cover it.
- Fix, either of the step-1 review's two forms, chosen by the developer and confirmed by the
  architect (see "For the architect"):
  - With `live_reads()` (`:2388`), do not truncate: leave the rejected records in the lane
    unreferenced, as D4 says, keep their bytes in the buffer's `bytes` (do not restore it) so
    `needs_flush()` sees them, and let `freeze()` (`:3586`, which returns on an empty `order`)
    freeze a buffer that holds only rejected records; or
  - keep `truncate` for readerless indexes and for inverting layouts (`live_reads()` false), and
    keep-and-count for the rest.
  Test: p1b's loop as a TSan/ASan test with a small `buffer_share`, asserting no report, and
  `buffered_bytes()` bounded (the step-1 CR1 probe, `p4_reject_growth`, must still pass: 4,000
  rejected 64 KiB transactions must not grow RSS without bound). It must fail on this head.

### CR2. The select-32 tail and the full-tail sort are not tested: removing `nth_element` leaves every test green

- Where: `index.hh:1824-1831` (`part_at`: a tail past `2 * tail_select` selects and sorts 32,
  `buffer_part::finish` sorts the rest when the merge reaches it, `settle` `:1895`).
- What: the code is correct (my p2 passes at tails of 1, 31, 32, 33, 63, 64, 65, 66, 96, 97, 200
  and 700 entries, scans of 1, 5, 31, 32, 33, 40, 64, 65 and all results, with 1 distinct key per
  record and with 3 records per key, as ordered vectors: 216 of 216). The tests do not notice if it
  is wrong. Mutant `.scratch/mut/e` deletes the `std::nth_element` line (the first 32 entries are
  then arbitrary, sorted among themselves, with the rest sorted apart): `write_buffer_reads_test`
  passes (`.scratch/mut/e/out.log`, 0 wrong over 109,641 reads), `scan_test` passes; p2 on the
  mutant fails 105 of 216 (first emitted key wrong, duplicates, wrong counts).
  Why: `scan_all` and `scan_range` (`write_buffer_reads_test.cc:106`, `:543`) collect into a
  `std::map`, which hides order and duplicates, and the schedules keep tails at about 64 because a
  scan asks for a build at 64 and the pool builds it; `runs_by_counter`'s one 200-entry tail is
  compared through `scan_all`. `scan_test`'s sorted inserts leave the tail already in order.
- This is the "test passes without the code it tests" shape for a mechanism D9 names (the 32
  smallest first), and the ledger's 12 D9 mutants do not include it. Fix: add p2 (or its core:
  random-order keys, tails at 63, 64, 65 and 200, scans of 1 and 40 results compared as ordered
  vectors, one case with repeated keys) to `write_buffer_reads_test`, and make the scan
  comparisons in the schedules ordered (a vector, not a map) where the model can say the order.
  The mutant must fail it.

## Ruling on the posted scan-run jobs

Keep them posted for this step; do not set `runs_group_ = nullptr`. Unawaited jobs are neither a
lifetime nor an ordering hazard here:

- The job closure (`index.hh:1703`) owns a `shared_ptr<buffer>` and the fresh/compaction job
  slot it fills lives in that buffer (`wbuf::run_job`); the lambda captures nothing else of the
  index (`run_cap_` is copied, `entry_less` holds the buffer). A job that outlives the index, a
  flush or the lease finds its buffer alive, reads only position-index entries below the count
  published when it was posted (never a rejected record) and stores `ready` with release; nobody
  has to take it.
- Ending the index ends the group first: `end_merges()` (`:2951`) resets `runs_group_`, whose
  destructor cancels, dropping queued jobs and waiting for running ones (`pool_executor.hh:59-80`,
  `executor.hh:27-29`), and both `close` paths call it (`:557`, `:584`). A queued job that was
  dropped leaves `live` set on a buffer that is going away.
- Only the lease holder applies a splice (`take_runs`), and a splice whose consumed levels are gone
  is dropped (`stale_splice_dropped`); ordering between a fresh and a compaction job is by
  consumed-level identity, so a late job cannot install over newer state. `scan_schedule` runs
  this on a 2-thread pool, TSan 0 reports.
- The cost is small and non-blocking: a posted job keeps a flushed buffer's memory alive until it
  runs (not charged to the budget), and `start_runs` leaves `live` set if `post` throws, so that
  buffer never builds again (see N3).

## Non-blocking notes (follow-ups, no second review)

- N1. **Writerless scans never build runs.** D9: "a scan on an index nobody writes links a
  writerless group to get one (D2)". `part_at` only sets `wants_runs`; `maintain_runs()` runs at a
  group's end and in the owner's scan (`:1637`, `:1735`), not in `run_chores`/`check_in`. A view
  scan on a quiet index sorts its whole tail every time (correct, O(tail log tail)). Step 2's
  checkout work owns it; put it in that ledger.
- N2. **Truncation versus T3's "rejected-record bytes".** The design reports them (T3); once CR1 is
  fixed they exist, so the T3 row should include them.
- N3. `start_runs` (`:1697-1704`): if `runs_group_->post` throws, `j.live` stays true and the
  buffer never posts again; set `live` after the post, or reset it in a catch.
- N4. `stable_array` capacity (`write_buffer.hh:179-181`): 16 x (2^25 - 1) = 2^29 - 16 entries;
  the comment says "more than a lane's 32,768 chunks hold", but 32,768 chunks of 1 MiB at the
  64-byte minimum record is exactly 2^29. Unreachable in practice (a 32 GiB buffer), and
  `push_back` throws `length_error` mid-transaction (apply has no rollback on an exception, as
  step 1's review noted); say "about" or add one block.
- N5. The branch is behind `origin/main` by the fuzz-corpus commits; rebase or merge at landing
  (no conflict).
- N6. I4-11 (CI) is unticked: run 40387780558 on `e6273e5` was still running when I looked, and
  no run exists for `309d36c`; landing needs it green, as the ledger says.

## What held (so a re-check does not redo it)

- **Reader/writer races.** `visible_`, `order.publish()` and `pub_->rs->visible` store in the
  right order (`:670-688`): entries published, then the cover (`publish_cover`: runs, tables,
  view), then V; `live()` loads V before acquiring a view (`:1171`). `part_at`'s binary search
  starts at the run cover and ends at `published()`; a rejected transaction's `rewind` asserts
  `n >= published_` (`write_buffer.hh`), so entries a reader can see are never cut. The 16-entry
  first block, growth boundaries at 16, 48, 112 (locate/at checked by hand for 0, 15, 16, 47, 48)
  and the directory of 25 atomic pointers are consistent; `at()` loads the block with acquire.
- **Docid check.** Maps are built under the lease only (`build_docids`, `writable_buffer`,
  `locate_docid`, the flush); readers use the const `probe_docid`. A partial map is never probed by
  a reader that needs it: a build happens only while `first_docid_only` is `none` (the first
  docid-addressed operation builds before it sets the flag, and the flag is stored with release
  before V passes it), and `superseded` skips below the flag. A reader's frozen-at-publication
  view keeps the segment bits the freeze copied, so the docid check against `v.active_` is
  consistent across a freeze (the bits and the buffers move together in the cover publication).
- **Retirement (D10).** Grown tables and replaced run sets are stamped with the generation at
  `retire_tables()` (>= every view that could have loaded them) and freed only below the oldest
  generation any retained view names (`free_tables`); the cover publication happens after the
  stamp. Mutant `.scratch/mut/a` (`<` to `<=` in `free_tables`) is killed: a TSan report from a
  reader against the freed run set and `scan seed 1: wrong scan`, rc 134. Buffers keep their own
  retired lists alive while any view holds them, so a flushed buffer's unstamped objects are safe.
- **D9.** Ratio 8 (`counter_merges`, `<=`), tail threshold 64 (`> tail_build`), select 32, prefix
  first then key then newest position, fresh jobs merging levels up to 16,384, compaction on the
  bigger levels, `apply_splice`'s consumed-level identity check and the cover monotonicity check
  read correctly; prefix ties, zero bytes and short keys are in `scan_keys`; keyless records enter
  no run (`keyed`).
- **Record bytes.** Flags at 24, first field at 28, document at 32 or 40, `stride_for` per shape,
  `static_assert`s on the offsets; `append_n` asserts the stride it laid out; `destroy` reads the
  stride before destroying. `record_layout` reads raw bytes; `record_strides` cuts and appends
  across mixed shapes under ASan.
- **TSan run.** `write_buffer_reads_test`, `write_buffer_test`, `scan_test`, `index_test`,
  `shared_view_test` on this head: all rc 0, 0 ThreadSanitizer reports
  (`.scratch/tsan-run.log`). Not the full ctest (the ledger's 49 of 49 under TSan and ASan are the
  developer's; CI covers the platforms).

## For the architect

- CR1 puts two earlier rulings against each other: D4 keeps rejected records (T3 reports the
  bytes) and the step-1 review's CR1 required a bound, fixed there by truncating. Neither text
  says what bounds the kept records once readers exist. My proposed rule (count them in the
  buffer's bytes so `needs_flush()` and the freeze reclaim them with the buffer, as the step-1
  review offered) needs your confirmation, not a design change; the developer can implement it in
  the fix round.
- D9's writerless group (N1) is a step 2 obligation, not step 1.

## Calibration check

- Concurrency reviews should include a multi-operation rejection beside a reader (the earlier
  review's CR1 fix removed a lifetime guarantee that the design relied on for item 4).
- "Mutants killed" lists should name the design's structural constants (select 32, tail 64) among
  the mutants; order-insensitive comparisons (a `std::map` collecting a scan) cannot kill an
  ordering mutant.

## Probes and artifacts (all under `.scratch/`, kept)

| File | What |
|---|---|
| `p1_reject_race.cc`, `p1_tsan`, `p1_tsan.out`, `p1_tsan_big.out` | CR1, TSan: rejected `{replace K, add X}` beside 3 readers; 2 race reports |
| `p1b_reject_chunk.cc`, `p1b_asan`, `p1b_asan.out` | CR1, ASan+UBSan: heap-use-after-free in `find_key_in`, freed by `rollback` (arguments: iterations, value bytes) |
| `p2_tail_select.cc`, `p2_real`, `p2_mut_nonth` | CR2: ordered-vector scans over tails 1 to 700; passes on the branch, 105 of 216 fail on mutant e |
| `mut/a/` | mutant: `free_tables` frees at stamp equality; killed by `write_buffer_reads_test` |
| `mut/e/` | mutant: no `nth_element` in `part_at`; survives `write_buffer_reads_test` and `scan_test` |
| `mut/build-a.sh`, `mut/build-e.sh`, `build-probe.sh`, `build-asan-probe.sh`, `deps-args.txt` | how to rebuild them (the developer's dependency copies are read, not modified) |
| `tsan-run.log`, `build-tsan.log`, `build-asan.log` | the TSan run of the five suites and the builds |

## Not run

Full ctest under TSan and ASan (the developer's 49 of 49 are cited, not reproduced); the byte
harness and memory measurements (I4-9, I4-10; not re-measured); Linux, FreeBSD and arm64 (CI);
the model check under ASan (I ran it under TSan only); a probe that turns CR1's stale-header read
into a wrong value (the ASan use-after-free and the TSan race stand on their own); the benchmark
rows (T1 to T7).
