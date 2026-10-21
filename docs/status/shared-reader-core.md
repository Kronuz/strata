# shared-reader-core

- Design: `docs/design/shared-reader.md` on `design/shared-reader` (revision 1 `a04671d`; revision 2
  `b1568a7`, which accepted the checkout model and one registry per pool partition). Reviews:
  `docs/reviews/shared-reader-design.md` on that branch (the preliminary review, the re-check, and
  the revision 2 review). Done criteria K1 to K8 are in that branch's `docs/status/shared-reader.md`.
  This branch carries only step 1's items 1, 2, and 4.
- Branch / worktree: `phase4/shared-reader-core` from main `41fde48`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader-core`. It is lock-free concurrency, so it
  takes a code review before it lands.
- State: ready for code review (items 1, 2, and 4). Items 3 and 5 wait for revision 2's text.
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
  - (this commit) `bench/shared_reader/acquire.cc`, T3's single-index arm as far as step 1 goes:
    acquire and release on one published pointer, a get on a snapshot of 3 segments (100,000
    keys) with and without a guard, `std::atomic_load` of a `shared_ptr` as the comparison arm,
    and the cross-thread-release arm with a control that uses the same handoff.
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
  - Release on another thread misses T3's 1.1×: it is 1.46× to 1.63× its control, about 90 ns
    more per operation. That is the record's line moving to the releasing core and back for the
    next claim. These numbers are from this branch on one host; the lead's runs on main decide T3.
- Next:
  1. Code review of items 1, 2, and 4 (lock-free concurrency).
  2. Items 3 and 5 once revision 2's text is final, with SR-D19 and SR-D20: the lease token
     becomes the primary owner check, and `owner.hh`'s worker mark stays as a second check that
     merge work never drops owner state.
  3. T3 in full (views, the aggregate row, the cross-thread arm at 16 threads, other platforms):
     the lead measures it on main.
- Blocked on: nothing for items 1, 2, and 4.
- Findings folded in: SR-D3, SR-D5, SR-D10, SR-D17, SR-D18 (with its two-scanner test).
- Cross-library: `segment_view::deleted` is now `const deleted_bits*` (it was
  `const std::vector<std::uint64_t>*`). Prospect's `segment_snapshot` and `live_scorer` take the
  new type when prospect repins. That is a type rename: `deleted_bits` keeps `operator[]`,
  `size()`, `begin()`/`end()`, and `data()`. `check_segment` now takes `const deleted_bits*`;
  prospect's one call passes null and compiles unchanged.
- Last verified (2026-10-21, macOS x86_64, Homebrew clang 23, at `302a414`):
  - ASan+UBSan (Debug -O1): ctest 35 of 35.
  - TSan (-O1): ctest 35 of 35.
  - Native RelWithDebInfo -O2 with asserts on: the new tests pass, read and mapped.
  - Mutants: 9 of 9 killed. 7 under ASan; `stamp_unlocked` (TSan data race in `two_scanners`)
    and `noclone` (bit checks and a TSan race) under TSan.
  - Not run: Linux, FreeBSD, arm64 hardware (CI covers them); prospect against this branch (it
    pins an older strata and needs the type rename).
  - CI: pushed; no jobs were green yet when the session stopped.
