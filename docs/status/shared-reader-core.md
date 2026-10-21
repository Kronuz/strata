# shared-reader-core

- Design: `docs/design/shared-reader.md` on `design/shared-reader` (revision 1 `a04671d`; revision 2
  `b1568a7`, which accepted the checkout model and one registry per pool partition). Reviews:
  `docs/reviews/shared-reader-design.md` on that branch (the preliminary review, the re-check, and
  the revision 2 review). Done criteria K1 to K8 are in that branch's `docs/status/shared-reader.md`.
  This branch carries only step 1's items 1, 2, and 4.
- Branch / worktree: `phase4/shared-reader-core` from main `41fde48`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader-core`. It is lock-free concurrency, so it
  takes a code review before it lands.
- State: the code review (`docs/reviews/shared-reader-core.md`) approved with conditions. CR1 to
  CR3 are fixed in the review-fix commit; ready to land. Items 3 and 5 wait for revision 2's text.
- Done:
  - `6373595` item 1: `strata/core/hazard.hh`: H records, each on its own line. A claim is a
    seq_cst compare-exchange from null, starting at a per-thread hint, on a record loaded null
    first (SR-D3, SR-D17). Release is one store of null from any thread. The owner's `publish`
    and `scan` and `basic_retired` keep at most H retired pointers. Several scanners are safe: the
    protection pass takes no lock, and the claim-time table and high-water mark are stamped by one
    scan at a time under a try-lock taken after that pass (SR-D18). Tests: `hazard_test` and
    `hazard_litmus_test` (T8).
  - `302a414` items 2 and 4: `deleted_bits` is copy-on-write in one allocation (D3). Every bit
    write goes through `set`, `reset`, or `mutable_words`, which clone shared words first. The
    owner drops a merge job's sources, bits, and output at install, discard, abandon, `close()`,
    and destruction (SR-D5). `owner.hh` provides the off-owner mark, and `pool_executor` sets it
    for its workers; segment and shared-count drops counted and asserted there.
    `get_segments` and `scan_segments` are one read implementation for the index and the new
    `basic_index::snapshot` (D8, SR-D10). The comment at `index.hh:1003-1006` is corrected. Test:
    `shared_read_test`, read and mapped.
  - `461ac14` `bench/shared_reader/acquire.cc`, T3's single-index arm as far as step 1 goes:
    acquire and release on one published pointer, a get on a snapshot of 3 segments (100,000
    keys) with and without a guard, `std::atomic_load` of a `shared_ptr` as the comparison arm,
    and the cross-thread-release arm with a control that uses the same handoff.
  - Review fixes (CR1 to CR3, plus CR6's test), each with its revert check:
    - CR1: move-assignment runs the destructor (`end_merges()`, then member teardown) and
      move-constructs in place, so the target lets go of its merge jobs on the owner.
      `merge_sources_on_owner` path 3 moves onto an index whose finished merge the worker still
      holds. Revert (`moveassign_default`): the owner-only assertion aborts.
    - CR2: `shared_registry_stress` (the review's `stress2.cc`): 2 and 3 lease holders on one
      registry, a thread spinning on `claims()` so scans lose the stamping lock, and poisoned
      same-size views. Revert (`lostlock_names_nothing`, protection moved after the lock): ASan
      heap-use-after-free, and a TSan data race.
    - CR3a: T8 now starts the reader and the owner from one start word, so both sides' store-load
      pairs race. The owner-side mutants must fail on x86_64 too, and they do: publish release
      only, retry 20 and claim 20 (cap 20); publish release with scan acquire, 20 and 20. The
      reader-side mutants give 20 in retry, and every-order-weak gives 20 and 20. Loads weakened
      alone give 0 and 0 on x86_64, as the comment says. Revert: under the old one-sided harness
      the owner mutants gave 0 in 10^7 (the review's `litmus_variants.log`).
    - CR3b: `merge_sources_on_owner` path 4 runs `install()` with a finished merge held.
      Revert (`abandon_nodrop`): the owner-only assertion aborts.
    - CR6: `pool_marks_workers` checks that a `pool_executor` task runs marked. Revert
      (`nowrap_pool`): that check fails.
  - CI fix (run 39662330405, `hazard` on freebsd-x86_64 and macos-arm64): neither failure was a
    `bad` count. (1) `reads > 0`: on a loaded 2-4 core runner the owner finished its publications
    and closed before the readers ran (0 reads). The owners now go on until the readers reach
    500 reads each (at most 120 s), and the test checks that minimum. (2) Line 330's failing clause
    was `high_water() >= 1`, at 0: `claims()` in its tight loop won the stamping lock on every
    scan, which CR7 allows. That check is now split by clause, and the stamp is checked after the
    run with one guard held. Locally clean: 30 native runs, 24 oversubscribed (6 at once), 10
    under TSan, 5 under ASan.
- Evidence (macOS x86_64, i9-9980HK, Homebrew clang 23, light holds):
  - The full ctest passes under ASan+UBSan (Debug, -O1) and TSan (-O1): see "Last verified".
  - T8 natively (RelWithDebInfo -O2, asserts on): seq_cst shows 0 free-while-read outcomes in
    10^7 retry and 2.5×10^6 claim iterations. Weakened orders do fail: release stores with
    acquire loads gave 3, 4, 4, 7, 7, and 20 in 10^7 retry iterations over six runs. With only
    the reader side weakened, each run stopped at its cap of 20 (one run reached 15). The claim
    shape cannot fail on x86_64, because `lock cmpxchg` is a full barrier at any order. Under
    sanitizers the litmus runs 20,000 iterations and checks only seq_cst's zero.
  - Before SR-D18's fix, the scan took its lock before the protection loads, and that locked
    instruction fenced the owner's side: the weakened mutant showed 0 in 10^7. Moving the lock
    after the protection pass is what lets T8 test the protocol's own orders.
  - `hazard_test` under ASan: parking leaves 0 records held, and a fifth thread claims all 16.
    Drift runs 100,000 claims, all released on another thread, with no lock in the registry. Two
    scanners named 101,258 records, with a high-water mark of 4. The stress runs 1, 4, and 8
    readers at H = 64 and 8 readers at H = 4: at most 35 retired views at H = 64 and 4 at H = 4,
    with no wrong sums and no generation going backwards.
  - `shared_read_test`: 3 seeds of the snapshot differential, about 115 publishable points and
    about 17,500 gets each. Four readers ran on frozen shares for 12 rounds; 11 deletes cloned
    shared bits. 0 segments or shares were dropped off the owner.
  - Mutants: each was applied, built, and run with `.scratch/sr-core/mutant.py`, and each was
    killed. `novalidate` (acquire skips validation): ASan use-after-free in the stress. `noscan`
    (frees without the scan): ASan use-after-free. `steal` (claim by store, no compare-exchange):
    the too_many_readers check plus a use-after-free. `noclone` (no clone on a shared write): the
    bits checks, plus readers seeing changed bits and wrong reads. `nojobdrop` (SR-D5 fix
    reverted): the owner-only assertion aborts. `scan_ignores_bits` and `get_ignores_bits`: the
    snapshot differential. `stamp_unlocked` (SR-D18 fix reverted): see "Last verified". The
    weakened orders are T8's own mutants.
- T3, measured once as the lead asked (`quiet run sr-core 20 "hazard cost"`, strict, at `302a414`
  plus the bench; i9-9980HK, 8 physical cores, so 8 threads is one per core). There were two runs.
  The first had no cross-thread control. Figures are ns per operation per thread, given as
  "first run / second run":

  | threads | acquire+release | get | acquire+get+release | shared_ptr load+drop | release on next thread (control) |
  |---|---|---|---|---|---|
  | 1 | 9.8 / 10.0 | 1022 / 1104 | 1036 / 1120 | 26.7 / 28.3 | |
  | 2 | 10.1 / 9.9 | 1045 / 1105 | 1058 / 1170 | 129.5 / 129.5 | 216 / 233 (144) |
  | 4 | 10.5 / 10.7 | 1080 / 1119 | 1095 / 1181 | 294.5 / 295.2 | 239 / 286 (191) |
  | 8 | 10.8 / 10.8 | 1142 / 1162 | 1154 / 1231 | 630.0 / 643.4 | 268 / 280 (193) |

  - Acquire plus release at 8 threads is 1.10× and 1.08× its 1-thread cost, within T3's 1.1×.
  - The guard adds 1.0% and 5.9% to the get at 8 threads (1.3% and 1.4% at 1). The two runs
    straddle T3's 5%, so this needs the lead's interleaved runs.
  - The shared_ptr arm costs 58× the hazard pair at 8 threads.
  - Release on another thread, restated against the hand-off control (CR4). Any cross-thread
    hand-off alone costs 144 to 193 ns, so T3's "within 1.1× of same-thread release" (9.8 ns)
    cannot be met by any registry. Over that control, the registry adds about 90 ns (1.46× to
    1.63×): one round trip of the record's line, written by the releasing core and pulled back by
    the next claim. The review asks the architect to restate the row as the registry's cost added
    over the control. These numbers are from this branch on one host; the lead's runs on main
    decide T3.
- Next:
  1. Code review of items 1, 2, and 4 (lock-free concurrency).
  2. Items 3 and 5 once revision 2's text is final, with SR-D19 and SR-D20: the lease token
     becomes the primary owner check, and `owner.hh`'s worker mark stays as a second check that
     merge work never drops owner state.
  3. T3 in full (views, the aggregate row, the cross-thread arm at 16 threads, other platforms):
     the lead measures it on main.
- Blocked on: nothing for items 1, 2, and 4.
- Findings folded in: SR-D3, SR-D5, SR-D10, SR-D17, SR-D18 (with its two-scanner test); code
  review CR1, CR2, CR3a, CR3b; CR4 and CR5 in this ledger; CR6's test. CR7 (diagnostics exact to
  the last stamping scan; `claims()` blocks for one pass and stays off reactor threads) is
  documented in `hazard.hh`. On CR8: CI builds RelWithDebInfo without NDEBUG, so the assertions
  are live there; a `-DNDEBUG` build keeps only the counter check.
- Cross-library: `segment_view::deleted` is now `const deleted_bits*` (it was
  `const std::vector<std::uint64_t>*`). Prospect's `segment_snapshot` and `live_scorer` take the
  new type when prospect repins. That is a type rename: `deleted_bits` keeps `operator[]`,
  `size()`, `begin()`/`end()`, and `data()`. `basic_merge_source::deleted` (`merge.hh:32`, `:51`)
  stays `const std::vector<std::uint64_t>*`, so prospect's call sites that build merge sources
  from segment views stop compiling too: `test/reorder_test.cc:119`, `test/merge_test.cc:91`, and
  similar ones (CR5). The repin converts them, or a follow-up gives `basic_merge_source` the same
  type. `check_segment` now takes `const deleted_bits*`; prospect's one call passes null and
  compiles unchanged.
- Last verified (2026-10-21, macOS x86_64, Homebrew clang 23, at the review-fix commit):
  - ASan+UBSan (Debug -O1): ctest 35 of 35.
  - TSan (-O1): ctest 35 of 35.
  - Native RelWithDebInfo -O2 with asserts on: T8 gives seq_cst 0 in 10^7 retry and 2.5×10^6
    claim iterations, and every store-weakening mutant is caught.
  - Mutants: 13 of 13 killed. The first 9 are listed above; the review round added
    `lostlock_names_nothing` (ASan and TSan), `moveassign_default`, `abandon_nodrop`, and
    `nowrap_pool`.
  - Not run: Linux, FreeBSD, arm64 hardware (CI covers them); prospect against this branch (it
    pins an older strata and needs the type renames above).
  - CI: pushed; no jobs were green yet when the session stopped.
