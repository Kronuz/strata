# strata review: Phase 7c step L5 (`defer_io`, the WorkPool executor binding)

- Date: 2026-10-29.
- Reviewed: `phase7c/l5-defer-io` at strata:5f6733a (code in strata:4563ed1, strata:0bc8969, strata:27c944e) from main strata:8fac1bc;
  `include/strata/core/index.hh` (`apply` `:696-776`, `reclaim_for_space` `:939-948`, `writer_scope` `:2603-2620`, `drain_pending` `:3571-3576`,
  `install` `:1496-1515`), `work_pool_executor.hh`, `executor.hh:27-42`, `faultfs.hh` `calls()`, `test/defer_io_test.cc`,
  `test/work_pool_executor_test.cc`, `test/background_merge_test.cc`, `CMakeLists.txt` (pin threadpool:0244bd3); ledger `docs/status/phase7c-l5.md`;
  note `docs/mechanisms/defer-io.md`.
- Checked against: Xapiand `docs/design/phase7c.md` D1, D4 to D7 (`:125-131`, `:188-290`); `docs/reviews/phase7c-design.md` F5 (R5), Re-check 1's
  R5 analysis (`:420-449`), F23 (C16), F24 (C15); threadpool `docs/reviews/phase7c-l2-code.md`, "Architect ruling on B2 and N1" (`:182-330`), its
  consumer contract items 1 to 7 and AC1; the ruled implementation at threadpool:2947834 (`work_pool.hh` `wait` `:1404-1431`, `check_stack`
  `:1289-1295`, `cancel` `:1370-1397`).
- Checked by: Homebrew LLVM 23 (`/usr/local/opt/llvm`), x86_64 macOS, builds in `.scratch/review-l5/` (`build.sh asan|tsan|ruled`):
  - ASan+UBSan (`-O1 -fno-sanitize-recover=undefined`, `detect_leaks=0`): ctest 61 of 61 pass, 354 s (`ctest-asan.txt`).
  - TSan (`-O1`): defer_io, defer_io_mapped, work_pool_executor, background_merge, background_merge_workpool, reclaim_bg, shared_view,
    write_buffer: 8 of 8 pass, 0 reports (`ctest-tsan.txt`).
  - ASan+UBSan against the ruled threadpool:2947834 (`threadpool-ruled/`, `build-ruled/`): the binding compiles unchanged; defer_io,
    defer_io_mapped, work_pool_executor, background_merge, background_merge_workpool: 5 of 5 pass (`ctest-ruled.txt`). The ruled `wait` aborts
    on any wait on a group already on the stack (`check_stack`), so no strata path waits on its own group in those suites.
  - Probes (`probes.cc`, compiled by `build_probes.sh`): `host_io`, `same_end`, `lazy_map`, `skip_order`, `host_crash` (outputs `*.txt`); the
    golden test rebuilt against main (`golden_main.cc` over `main-8fac1bc/`); mutants `mut/g1`, `mut/g2` (golden sensitivity), `mut/c1` (crash
    oracle), `mut/r1` (0bc8969 reverted, binary `wpe_r1`).
  - Not run: Linux and FreeBSD (the ledger's preflights are taken as given); LeakSanitizer (not used on this macOS build); any benchmark;
    fuzzing (no decoder changed); `bench/kv`; the full suite against the ruled threadpool (only the five WorkPool and defer_io tests); CI.

## Verdict

**APPROVE WITH CONDITIONS: one blocker, L5-B1 (defer_io still maps segments inside apply when eager_segments is off, and an environment
variable turns that on). Fix it and the step can land.**

What holds, and reproduces:

- Durability and order are unchanged. An owed answer applies nothing and leaves the position where it was (`index.hh:703-713`, `:722-733`).
  Commit, refresh and their syncs are the same writer calls, now run as the host's chores. `host_crash` cuts power at every 4th mutating call of
  a host-mode run with 20 to 25 owed flushes and 2 to 7 owed drains per run: 477 points over three seeds, 0 opens failed, 0 acknowledged commits
  lost, defer on and off alike (`host_crash.txt`). The oracle catches loss: with `sync_dir` made a no-op (`mut/c1`) it reports 5 to 6 lost
  acknowledged commits and 31 to 35 bad points per seed (`host_crash_c1.txt`).
- Defer on and off accept the same writes. `same_end` runs the same workload both ways, retrying each owed answer, over 8 seeds: equal
  contents and equal applied position every time (`same_end.txt`).
- With eager segments, apply makes no device call. `host_io` counts `faultfs::calls()` over 1,498 applies (keyed replaces, erases, merges, a
  150 KB storage permit, 184 flush_owed, 34 drain_owed, 4 no_space): 0 calls inside apply (`host_io.txt`). The tests check only the owed
  applies; this checks every apply.
- C15 holds as implemented. `flush_owed` needs a buffered record or a held rejected record (`index.hh:710`). `drain_owed` is answered once per
  round through `drain_asked_`/`drain_ran_` (`:722-733`, `:939-948`). An ENOSPC inside the refresh chore throws `std::system_error` to the
  host (`guarded`, `:2546-2556`) instead of answering owed again. No path retried more than once in `host_io` (the probe fails a run that
  is owed more than three times in a row).
- C16, strata side: `reclaim_after_release` frees a released view's files through `reclaim()` with no check-in (`defer_io_test.cc:416-449`).
- The default path is identical. `golden_main` rebuilds `default_path_unchanged` against main strata:8fac1bc and prints the same four values
  (verdict string, 163 mutations, file hash `cb9aafd89bf3cd0f`, digest `6cc0ff118bf776d3`; `golden_main.txt`). Disabling the default-path
  drain (`mut/g1`) fails all four checks plus the drain control (`g1.txt`).
- The binding meets the ruled contract with no code change (AC1 done at `executor.hh:32-35`). Every `wait`/`cancel` site matches the ledger's
  audit (`index.hh:3105`, `:3288`, `:3277-3278`, `:3395`, `:3405`, `:3409`). Posted work never waits. The binding drops its `mu_` before
  `g_.wait()` (`work_pool_executor.hh:96-114`), and items mark themselves off-owner (`:66`), which is contract item 4 (AR2). The
  chore-waits-on-merge test (`defer_io_test.cc:502-544`, 1/0, 2/1 and 4/1 threads/reserve at width 1 and 2) passes on both the pinned and the
  ruled threadpool.
- `install()` draining inline is acceptable (L5-N2). Leaving `pool_executor` for now is right (L5-N6).

What blocks: L5-B1.

## Findings

### L5-B1. With `eager_segments` off, `apply` opens and maps a segment, and `STRATA_EAGER_SEGMENTS=0` turns that on for any host (blocking)

`lazy_map` opens a committed index with `defer_io` on. With eager segments, the first replace-by-key apply makes 0 device calls. With
`eager_segments = false`, it makes 4 (an open, a size and a map), and the next apply makes 0 (`lazy_map.txt`). The option's default reads the
environment (`index.hh:371-377`, `:522`). Xapiand sets neither field, so `STRATA_EAGER_SEGMENTS=0` in a server's environment puts a file
open and an `mmap` on a reactor, which D1 forbids (`phase7c.md:125`). The note and the option's comment record the gap
(`defer-io.md:40-42`, `index.hh:528-529`), but a documented exception that an environment variable switches on does not meet this step's
claim, "apply makes no device call". The cold-tier plan also makes lazy segments the future default (`index.hh:519-521`).

Ruling on the developer's question 2b: in L5 the map is not owed; the combination is refused. An owed map needs a verdict raised from inside
`evaluate` mid-transaction, plus the cold tier's segment-loading design (D2, D9 there), which is not built yet. Making the claim true now
takes one check.

Fix: `create` and `open` throw `std::invalid_argument` when `defer_io && !eager_segments`, whatever the environment set. Add a test that
opens with `defer_io` under `STRATA_EAGER_SEGMENTS=0` and sees the refusal, or sees `eager_segments` forced on, if you prefer that to a
throw. State the rule on the option and in `defer-io.md` "Not covered". Record for cold-tier step 1, item 6: lazy segments under `defer_io`
need an owed open (a `map_owed` verdict, or opening in the refresh chore) before they become the default.

### L5-N1. An owed transaction must be retried before any later position, or its retry is `skipped`

`skip_order` (cap of 10 buffered documents) applies a three-operation transaction at position 10 and gets `flush_owed`. A one-operation
transaction at position 11 is then accepted, since it fits. After `refresh()`, the retry at 10 answers `skipped` (`skip_order.txt`;
`index.hh:703-706`). `skipped` means "already applied", so a host that lets another write in between loses the owed write silently. D5's
turn prevents this, because the turn covers apply and its retry (`phase7c.md:211-245`), but strata states no such rule. Fix: say on
`verdict::flush_owed`/`drain_owed` (`index.hh:84-85`) and in `defer-io.md` that the host retries the owed transaction before applying any
other. Add a debug assert: remember the owed position and assert that the next positioned apply carries it. Xapiand's X2 relies on this
(condition C3).

### L5-N2. `install()` draining inline is acceptable; the design text is wrong, not the code

`install` reads the source manifest and every segment (`index.hh:1499-1505`), then writes copies and commits, all before and after the drain
at `:1511`. Deferring only the drain would leave all of that I/O in place. It is a chore by nature, like `refresh` and `commit`. Ruling on
question 2a: keep it inline. Fix: the design's "`install` ... defers its drain the same way" (`phase7c.md:261-262`) becomes "install, like
refresh and commit, runs only as a chore". The option's comment (`index.hh:523-530`) should say that `defer_io` covers `apply` only and that
every other writer call is a chore (condition C2).

### L5-N3. The pin is a commit the L2 ruling rejected; the binding and note describe the ruled behaviour

threadpool:0244bd3 still serves other groups' queries from `wait()` (the AR1 help-anything deadlock and the AR2 identity leak, ruling
`:204-214`). strata posts only merge-class work and no queries, so its own suites cannot reach AR1. Xapiand X2 would. `defer-io.md:103-106`
and `work_pool_executor.hh:9-11` describe the ruled behaviour ("a wait ... serves the awaited group's items inline"), which holds only from
threadpool:7a1a5c5 on. The ruled head threadpool:2947834 needs no binding change and passes the five suites (`ctest-ruled.txt`), and
ThreadPool and TaskGroup are byte-identical between threadpool:628679a and both heads, so `pool_executor`'s users do not move. The ledger
still says the pin is threadpool:2ae90f0 (`docs/status/phase7c-l5.md:46`). Fix: repin to threadpool:2947834 (or L2's landing commit, if
earlier) before landing, and correct the ledger line (condition C1).

### L5-N4. The cancel-order fix (0bc8969) has no test that fails on this platform

With the fix reverted (`mut/r1`, binary `wpe_r1`), `cancel_drops_and_stops` (`work_pool_executor_test.cc:71-93`) passes 40 of 40 serial runs
and 100 of 100 four-way parallel runs here. It failed 3 of 3 only on FreeBSD (ledger `:52`). The fix is right: no item starts after the stop
once the queued items are unlinked first (`work_pool_executor.hh:96-104`). But its regression test depends on the OS scheduler. Fix: widen the
window deterministically. Have the running item, once it sees the stop, return immediately, and use a hook that delays the canceller between
the stop request and the unlink in the reverted order. A stop callback on the cancel source works, if task's `cancel_source` offers one.
Failing that, loop the case 10,000 times in one process and record the reverted failure rate on mac and Linux.

### L5-N5. `drain_ends` asserts less than the note claims

`defer_io_test.cc:287` checks `again.result != verdict::drain_owed`, so `accepted` would pass. The note says the retry after a round answers
`no_space` (`defer-io.md:31-33`), and the removal fault leaves the room short. Fix: `CHECK(again.result == verdict::no_space)`.

### L5-N6. `pool_executor` beside `work_pool_executor`: two bindings of one mechanism, migrate with the repin to landed L2

Both bindings implement the executor concept over a node pool. Once D4 makes WorkPool the node's one scheduler, ThreadPool's binding is the
parallel implementation (one implementation per mechanism) and goes, its callers migrated, with no shim. Not before L5 lands: WorkPool is not
on threadpool main, and moving the 8 suites (`flush_memory`, `reclaim_bg`, `write_buffer_reads`, `shared_read`, `shared_view`,
`write_buffer`, `background_merge`, plus `defer_io`'s use) and `bench/kv/kv_strata.cc` onto a branch scheduler would put every threaded strata
test there. Xapiand does not use `pool_executor`. Fix (condition C4): in the step that pins threadpool to L2's landing commit, migrate them,
delete `pool_executor.hh`, drop `threadpool.hh`/`task_group.hh` from `cmake/check_core_includes.cmake:9` if nothing else needs them, and note in
`bench/kv`'s results that merges now run in the merge class, behind queries and flushes.

### L5-N7. The golden test does not pin the order of the default path's flush and replay check

Moving the default path's inline refresh after the replay check (`mut/g2`) passes every test, the golden included (`g2.txt`, exit 0). The
golden's one replayed apply (`defer_io_test.cc:481`) does not arrive while a flush is due. The method is otherwise sound: the values were
captured on main and reproduce there (`golden_main.txt`), and `mut/g1` dies. Fix: add a positioned replay at a point where a flush is due
(the buffer past its share), so the golden records whether a skipped apply flushes.

## Blocking items / Required changes

- L5-B1. `defer_io` refuses `eager_segments` off at `create`/`open` (or forces it on), whatever `STRATA_EAGER_SEGMENTS` says. Add a test
  under `STRATA_EAGER_SEGMENTS=0`, document the rule on the option and in `defer-io.md`, and record the owed open as cold-tier step 1's.

## Conditions

- C1 (before landing): repin threadpool to threadpool:2947834 or L2's landing commit; rerun defer_io, defer_io_mapped, work_pool_executor and
  background_merge_workpool there; fix the ledger's pin line (L5-N3).
- C2 (lead, Xapiand design, before X2): D6.2 says install runs only as a chore; strata's option comment says `defer_io` covers `apply` only
  (L5-N2).
- C3 (X2): the turn retries an owed transaction before releasing the turn or applying another; strata's debug assert from L5-N1 is in place.
- C4 (the repin to landed L2): `pool_executor` is retired and its callers migrated (L5-N6).
- L5-N4, N5 and N7 land with the B1 fix or before C1's repin.

Not run: Linux and FreeBSD builds, LeakSanitizer, the full suite against the ruled threadpool, benchmarks (`bench/kv` included), fuzzing, CI.
No shared host or VM was touched.

## Probes (`.scratch/review-l5/`)

- `build.sh asan|tsan|ruled`: the three review builds (deps from `.scratch/build/_deps`; `ruled` over `threadpool-ruled/` at
  threadpool:2947834).
- `build_probes.sh SRC OUT [INCLUDE_ROOT] [BUILD]`: compiles a probe or a mutant test with a review build's flags.
- `probes.cc` (`probes host_io|same_end|lazy_map|skip_order|host_crash [points]`); outputs `host_io.txt`, `same_end.txt`, `lazy_map.txt`,
  `skip_order.txt`, `host_crash.txt`, `host_crash_c1.txt`.
- `golden_main.cc` over `main-8fac1bc/` (`git archive` of main's `include/` and `test/kv_test.hh`): `golden_main.txt`.
- Mutants: `mut/g1` (default-path drain off), `mut/g2` (default-path flush after the replay check), `mut/c1` (`sync_dir` a no-op, for the
  crash oracle), `mut/r1` (`work_pool_executor.hh` before 0bc8969).
- `work_pool.0244bd3.hh`, `work_pool.2947834.hh`: the pinned and ruled WorkPool, for comparison.
