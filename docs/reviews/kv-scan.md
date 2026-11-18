# kv-scan code review: `phase4/kv-scan` at a746d02

Reviewer: code reviewer (fresh context). Base: `f777ef9`; main is now `6105148`, 21 commits ahead.
Scope: `basic_index::scan` (two overloads), the buffer's sorted view, the `keys::reader::range`
cursor, the guard, `fuzz_stored`'s cursor walk, `bench/kv/kv_strata.cc` as a caller, `scan_test`.

## Verdict: APPROVE WITH CONDITIONS

One blocker (CR1: `close()` from `f` is a use-after-free), small to fix and test. The merge, the
tie rule, the `to` bound, `n`, resuming, and the buffer view are correct on every probe. The
snapshot rule holds against the pool executor. Conditions: CR1 fixed with a test, CR4 (rebase onto
main and rerun) before landing.

## Blocking

### CR1. `close()` from `f` frees the segments under the scan (blocker)

`include/strata/core/index.hh:484` `close()` has no `live()` and no scan check: it runs
`segs_.clear()` and `clear_buffer()` while `scan_keys` holds `source` entries pointing into
`segs_` (`index.hh:1091-1098`) and a view into the buffer. Every other public writer call goes
through `live()` and throws (`index.hh:1270-1273`); `close()` is the one the guard misses. It
needs no corrupt state to trigger: calling `close()` in `f` is enough.

- Probe: `.scratch/review/probe_close.cc` (`probe_close.asan`): index with one segment and a
  buffer, `f` calls `ix.close()` on its third record. Result: ASan heap-use-after-free, read in
  `termdict::block::step` from `keys::reader::range::next()` (`keys.hh:261`), freed by
  `open_segment::~open_segment` from `close()`.
- Not a probe artifact: mutant `m1_noguard` (guard removed from `live()`) fails the same way in
  `scan_test` (`mut/m1_noguard`), so the guard is what keeps this out for the other seven calls.
- Probe `.scratch/review/probe_guard.cc` (`probe_guard.asan`): from inside `f`, all 13 other
  public writer paths throw `std::logic_error`: apply, refresh, merge, force_merge,
  force_merge_deletes, maybe_merge, wait_merges, commit, checkpoint, release, reclaim, heal,
  install. `scan_test` exercises seven of them (`scan_test.cc:265-271`), none of them `close`.

Fix: `close()` throws `std::logic_error` when `scanning_` is set (not `live()`, which also
rejects a fenced index, and a fenced index must stay closable), and the snapshot test gains
`close`. Optionally run the other twelve through a table in the test, so a new public writer
cannot skip the guard unnoticed.

Move-assigning or destroying the index inside `f` has the same effect (`probe_move.cc`, not
run: the caller destroys the object it is calling on). I do not ask for a guard there.

## Conditions

### CR2. Concurrency: what is and is not enforced (answers focus 1)

- Pool executor: background merges publish only on the owner's thread. `publish_merges` is
  reached from `refresh`, `merge`, `wait_merges`, `commit`, `run_merges`, each behind `live()`
  (`index.hh:546-549`, `602-606`, `685`); a pool task (`index.hh:1359`) touches only its job and the
  shared sources, which a scan only reads. Flush is synchronous on the owner thread (even with
  `flush_threads > 1` it joins inside `refresh`). So nothing publishes during a scan.
- Probe: `.scratch/review/probe_tsan.cc` (`probe_tsan.tsan`): `pool_executor` with 2 threads,
  `auto_merge`, small tiers, 3,000 steps of puts, deletes, refreshes, `wait_merges` and full scans
  taken while merges run (yielding in `f`), every scan compared with a `std::map`. TSan: 0 reports;
  mismatches 0; 8 segments at the end.
- Not enforced across threads: `scanning_` is a plain `mutable std::uint32_t` (`index.hh:2878`),
  and `scan` is `const` yet writes `scanning_` and `view_` (`index.hh:2877`). `probe_tsan.tsan
  readers` (two threads scanning one index between owner steps) gives TSan data races on
  `scanning_`. A writer on another thread during a scan is therefore neither blocked nor reliably
  detected. This matches the owner-core model (`docs/ARCHITECTURE.md:43`, the header comment at
  `index.hh:957-958`, "Scans are for the owner core's thread"), and `get` has no such state.
  Condition: say it where a reader looks: `scan` is not callable concurrently with itself or with
  a writer, unlike `get`, which a `const` signature suggests. (Making `scanning_` atomic would
  not make `view_` safe; no code change asked.)

### CR3. The `inverting()` branches are untested

`buffered_dead`, `buffered_key`, `buffered_stored` with `inverting()` true (`index.hh:1030-1034`,
`2160-2176`) never run in `scan_test`: it uses `document_layout`, which has no inverter. The
constraint at `index.hh:965` and `972` is `contains<stored_plugin>()`, so it admits a layout with
both an inverter and `stored`, and the ledger says so ("untested here"). I read the accessors:
buffer index equals ordinal in `active_`, which `pending_docs_` continues, and the view is only
read between applies, so I found no fault; I did not run it. Condition: whoever adopts it in a
layout with an inverter adds the scan test first (or the constraint excludes `inverting` layouts
until then). Also name `keys_plugin` in the constraint: the body uses it.

### CR4. Rebase onto main, rerun (main is 21 commits ahead)

`git merge-tree` main HEAD: `CMakeLists.txt` conflicts (main re-pins fusefilter and adds
`keys_fuse_test` beside the lines this branch adds for `scan_test`); `index.hh` auto-merges. Main's
`needs_flush` now reads `buffered_bytes() + map_bytes() >= min(buffer_share, capacity) / div`
under pressure (`pressure_flush_divisor`), so the view's bytes (CR5) are part of that gate.

- I built the merged tree: `git archive main` plus `git diff main...HEAD` over `include test fuzz
  bench` applies without rejects (`.scratch/review/merged/`). `scan_test` under ASan+UBSan: ok.
  `background_merge_test` (main's new pressure-flush tests included): ok.
- Not done: the CMake conflict resolved in git, and the full suite on the merged tree. The
  developer's rebase reruns `scan_test` and the full ctest under both sanitizers.

### CR5. `map_bytes()` counting the view (answers focus 4)

Not a blocker, no tiny-flush mechanism found.

- The view is at most 2 x 4 B per buffered record (capacity doubling; `buffer_view::bytes`,
  `index.hh:1031`) and is reset with the buffer (`clear_buffer`, `index.hh:2217-2218`), so it
  cannot outlive the records it indexes. A buffer small enough to be "tiny" has a tiny view.
- Probe `.scratch/review/probe_bytes.cc` (`probe_bytes.asan`; byte counts, `buffer_share` 8 MiB,
  inserts with and without a one-record scan every 64): records per flush are identical with and
  without scans: 16,385 at value sizes 0, 8 and 100 bytes and 7,276 at 1 KiB. One buffered record
  charges 153 to 1,153 B, so the view adds at most about 5% (8 of 153 B) at tiny values, 0.7% at
  1 KiB. It moves flushes earlier only by that share; the map criterion
  (`map_bytes() >= buffer_share / 4`) is unchanged in kind.
- Uncounted: the fold's transient allocations (`index.hh:1056-1063`: `merged`, `a`, `b`, up to
  three copies of main plus delta, 12 B per record). At 10 million buffered records that is about
  120 MB outside the permit for the fold's duration. Note for the arena-buffer step (step 4),
  not a condition. The view is charged at the next `apply`'s `charge()`, while `needs_flush` reads
  it at once; harmless.

## Findings that need no fix before landing

### CR6. Correctness of the merge (answers focus 2): no defect found

- `.scratch/review/probe_diff.cc` (`probe_diff.asan`): 24 seeds, each 1,500 steps against a
  `std::map`: binary keys (`\0`, `\xff`, `\x01`, keys that are prefixes of each other), keys of
  70,000 bytes, keyless documents, deletes of segment keys, up to 27 segments, `auto_merge`,
  `flush_threads` 1 and 2, bursts of 4,200 puts past the delta cap (seeds 16 and 18 end with about
  4,650 keys), random `from`/`to`/`n`. Each query is also run as pages of 3 resumed from
  `last_key + '\0'` after `f` returned false, and as nested scans from inside `f`; reopen at the
  end. Result: 0 mismatches.
- Ties across 3 or more sources, the buffer's delete over a live segment key, the `to` bound,
  `n`, and the both-runs tie are checked in `scan_test` (`ties`, `newest_wins`,
  `limits_and_bounds`). Mutants, built from header copies in `.scratch/review/mut/` and each
  failing `scan_test`: `m1_noguard` (guard removed), `m2_noreset` (`clear_buffer` keeps the view:
  stale results after a refresh, `scan_test.cc:100/124`), `m4_delta_head` (the delta's first dead
  record not skipped).
- Buffer view lifetime (focus 1): every buffer clear goes through `clear_buffer()`
  (`index.hh:2217`: reached from `flush()` 2237, `close()` 488, `install()` 890; `buffer_.clear()`
  and `active_.reset()` appear nowhere else). Growth between scans appends only the records
  above `covered`; `rollback` shrinks the buffer only inside `apply`, when no scan can run. The
  `covered > size` reset (`index.hh:1040`) is therefore a defensive fallback, not the mechanism;
  `m2_noreset` shows the real reset is what the test needs. A dropped dead record at a fold is
  safe because a dead flag never reverts outside a rollback. An exception while building the view
  resets it (`index.hh:1067-1070`).
- Guard against `f` throwing: RAII `in_scan` (`index.hh:1076-1080`, the guard struct); `scan_test` checks the index
  usable afterwards.

### CR7. API shape (answers focus 5), for the architect, not the code

1. `n == 0` means no limit (D3, `index.hh:955`). A caller who computes a limit that reaches 0
   (a page size minus rows taken) gets a full scan silently, and that cannot be changed once
   callers exist. Alternatives: a named `unlimited` constant with 0 returning nothing, or
   `std::optional<std::size_t>`. The design chose it, so this is a question, not a defect.
2. The return value counts the call that returned false. Documented; `probe_diff` pages rely on it.
3. Resuming has no token: `last_key + '\0'` works (the 24-seed paging probe), until the pulled
   `cursor` exists. The comment could say so.
4. `scan` is `const` but shared mutable state makes it unsafe for concurrent callers (CR2).
5. The new exception contract: a writer call from `f` throws `std::logic_error`, where it used to
   work (in a caller that scanned by other means). All of `kv_strata`'s callers pass.
6. `f` may return anything convertible to `bool` or `void` (`index.hh:1152-1156`); a non-bool,
   non-void return type that is not convertible is a compile error inside the template, not at
   the constraint. Fine.

### CR8. Ledger evidence

- "ycsb_e CPU per op within noise of 21.7 us" is ticked `[x]` on a loaded host: relative runs
  (explorer 114.7 vs core scan 104.4 us median, 4 to 5 times the explorer's own figure) and one
  unheld run of 19.7 us. The ledger defers the absolute figure to the lead's quiet measurement;
  that is the gate. I ran no benchmark (instructed).
- Digest `81bb0f0b7fe8d333` at seed 42: not rerun (it is a benchmark driver).
- `bench/kv/kv_strata.cc` shrinks by 86 lines and uses the core scan: in scope (the ledger's
  stated purpose). `git diff main..HEAD` also shows `docs/perf/*` reverted; that is main having
  moved on (`main...HEAD` does not touch them), not a change on the branch.
- Linear head scan: O(S + 2) per record; 27 segments in `probe_diff` pass. Ledger names the heap.

## Evidence run

- ASan+UBSan (Homebrew LLVM 23.1.2, `-O1 -fsanitize=address,undefined -fno-sanitize-recover=all`),
  `.scratch/review/build-asan`: `scan_test` ok (1.4 s); full ctest 25 of 25 passed.
- TSan (`-fsanitize=thread`), `.scratch/review/build-tsan`: `scan_test` ok (3.5 s); full ctest 25
  of 25 passed, 0 ThreadSanitizer reports in `ctest-tsan.log` (lifecycle 451 s, format14 1,213 s).
- `scan_test` is registered twice (plain and `STRATA_FAULTFS_MAP=1`); the mapped variant passed
  in both ctest runs.
- Probes and scripts, all kept in `.scratch/review/`: `probe_close.cc`, `probe_guard.cc`,
  `probe_move.cc` (written, not run), `probe_keyless.cc` (keyless index: scan returns 0 and 0),
  `probe_diff.cc`, `probe_tsan.cc` (and `readers` mode), `probe_bytes.cc`; `buildprobe.sh`,
  `buildmerged.sh`, `buildmut.sh`, `guarded.sh` (RSS watchdog; my first probe asked for a
  4 GiB key, `format::max_term_bytes` is `fst::max_key_length`, 2^32 - 1: the limit is not
  load-bearing as a memory bound but is not this branch's); `mut/m1_noguard`, `mut/m2_noreset`,
  `mut/m4_delta_head`; `merged/` (main plus this branch's patch); logs `ctest-asan.log`,
  `ctest-tsan.log`, `cfg-*.log`, `build-*.log`, `diff.out`.
- HEAD equals `origin/phase4/kv-scan` (a746d02); nothing unpushed.

## Calibration checks applied

- Platforms: CI on every platform was not available to me; main's lesson from `std::jthread`
  applies: the branch uses only `std::` facilities already used in `index.hh`; I built on macOS
  x86_64 only. Run the branch's CI on Linux and FreeBSD after the rebase.
- Limits: no new cap; the delta cap of 4,096 is a performance threshold with a mechanism (the
  fold), not a bound on records.
- No new thread, queue, or cache.

## Not run

Benchmarks and the ycsb_e digest (instructed); Linux and FreeBSD builds; the full suite on the
merged tree (only `scan_test` and `background_merge_test` under ASan+UBSan); the `inverting()`
buffer path (no layout in this repo); `probe_move.cc`; a mutant for the `to` bound and for
`skip_dead` (the developer's own mutants cover them, I did not rerun those).
