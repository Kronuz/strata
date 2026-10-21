# Code review: phase4/shared-reader-core (step 1, items 1, 2 and 4)

- Reviewed: `phase4/shared-reader-core` at `461ac14` (`6373595`, `302a414`, `461ac14`), base main; `origin` equals `HEAD`.
  Worktree `strata-wt/shared-reader-core`, no uncommitted changes to code.
- Against: `docs/design/shared-reader.md` revision 2 and `docs/reviews/shared-reader-design.md` (SR-D3, SR-D5, SR-D10, SR-D17,
  SR-D18, C7) on `design/shared-reader`; ledger `docs/status/shared-reader-core.md`; `docs/reviews/calibration.md`.
- Checked by: my own builds (`build-rv-asan` Debug -O1 ASan+UBSan, `build-rv-tsan` -O1, `build-rv-rel` -O2 NDEBUG; Homebrew clang 23.1.2,
  macOS x86_64, light holds, 4 jobs), the full ctest on ASan+UBSan and TSan, and the probes below, all in `.scratch/review/`.

## Verdict

**APPROVE WITH CONDITIONS.** The hazard protocol, the copy-on-write bits and the shared read path are correct as written: I could not make
any of them free a view a reader holds, show a bit change under a share, or return a read that differs from main. What blocks landing is
one real ownership hole (CR1: move-assignment drops a merge job's sources off the owner) and two test gaps that leave the branch's own
claims unverified in the exact places SR-D18 and SR-D5 asked for evidence (CR2, CR3). All three are small and fit one fix round.

## Blockers (conditions to land)

1. **CR1. Move-assignment onto an index with a merge in flight drops merge sources off the owner (SR-D5 hole).**
   `index.hh:500` `basic_index& operator=(basic_index&&) noexcept = default;`. The defaulted assignment ends the old `group_`
   (`index.hh:3076`, first member) and then replaces `pending_` and `segs_` without `job_drop` (`index.hh:1919-1927`), which only
   `publish_merges` (`:1805`), `end_merges` (`:1931`, close and destruction) and `abandon_merges` (`:1941`) run. An executor that keeps a finished
   job's closure after `done` (the case SR-D5 describes, and `holding_executor` in the test models) then drops the last reference to the
   sources on its worker. Probe `moveassign_probe.cc` (asan): `ix = std::move(other)` with a finished merge held by the worker aborts at
   `owner.hh:30` ("owner-only state changed off the owner thread"). The ledger lists "install, discard, abandon, close(), and destruction"
   (`shared-reader-core.md`, Done) and omits move-assignment; the design's SR-D2 does cover it, for step 3. With the real `pool_executor`
   I could not hit it (`poolprobe.cc`, 6 seeds x 40 rounds with about 45 move-assigns and 1,400+ pending merges per seed: 0 off-owner changes),
   because `cancel_and_wait` usually returns after the closure is gone; the window exists and the assertion is the first thing a debug host
   would see. Fix: define the move-assignment to run `end_merges()` on the target first (then move the members), and add the case to
   `merge_sources_on_owner`. Probe to re-run: `moveassign_probe.cc` (must print 0).

2. **CR2. Nothing in the suite fails if a scan that loses the stamping try-lock stops protecting (SR-D18's own question).**
   The code is right: `hazard.hh:133-136` names every record in the lock-free pass before `try_to_lock` (`:137`), so a scan that loses
   the lock is still safe to retire with. But the tests cannot tell. `two_scanners` (`hazard_test.cc:289`) publishes and scans from two
   threads yet retires and frees nothing, and `stress` has one owner and no thread contending the lock, so the try-lock is almost never lost.
   Mutant `mut/lostlock_names_nothing` (the protection pass moved inside the lock section: `if (!stamp) return;` first) passes the stock
   `hazard_test` under ASan (3 runs) and TSan (2 runs): `out/hz_lostlock_asan.log`, `out/hz_lostlock_tsan.log`. My `stress2.cc` (2 to 4
   lease holders each with its own retired list on one registry, 8 to 12 readers choosing an index at random, 1 in 8 guards released on a
   releaser thread, a thread spinning on `claims()` so most scans lose the lock, same-size poisoned views so addresses recycle) kills it with
   an ASan heap-use-after-free in `view::intact` (`out/m_lostlock_asan.log`). Fix: add that shape to `hazard_test.cc` (the probe is a drop-in)
   and name the mutant in the ledger. Stock result, for the record: `stress2` clean on the real code, native K=2 R=8 H=32 300,000 publications per
   holder (2.98M reads, 372k cross-thread releases, 0 bad), native K=4 R=12 H=16, ASan, TSan K=2 and K=3: all 0 bad, retired never above H,
   0 left after the last reclaim.

3. **CR3. The litmus and the `abandon_merges` drop have no test that fails when they are broken.**
   a. *T8 does not test the owner's side.* `hazard_litmus_test.cc:65-100` stores `go` and then publishes, so the reader starts after the
      owner's store has drained and only the reader's store-load pair races. Weakening only the owner (`publish` release, or `publish` release plus `scan`
      acquire) gives 0 in 10^7 iterations (`litmus_variants.cc`, `litmus-variants.log`); a release `publish` is a plausible "this is not
      the hot path" edit and would pass CI on x86_64. With both threads released from one start word (`litmus_sym.cc`,
      `litmus-sym.log`, same steps, same `hazard.hh`) seq_cst gives 0 and `publish` release-only reaches the cap of 50 in both shapes (claim and
      retry), as does the reader-side mutant in the retry shape. Fix: use the symmetric rendezvous in `hazard_litmus_test.cc` and add the owner-only
      mutants to the "must fail on x86_64" check. (Validate-load-only and scan-load-only weakening cannot show on x86_64, where the adjacent
      locked store or the load order already fences them; only the arm64 jobs can, and the test does not require them to. Say so in the test comment.)
   b. *The `job_drop` in `abandon_merges` (`index.hh:1941`, reached from `install()` at `:954`) is untested.* Mutant
      `mutsrc/abandon_nodrop` (that one line removed) survives `shared_read_test` (read and mapped; `out/mm_abandon_nodrop.log`, rc 0). My
      `abandon_probe.cc` (install with a finished merge held by the worker, then `release()`) passes on the real code and aborts on the
      mutant (`out/ab_stock.log`, `out/ab_mut.log`). Fix: add it as path 3 of `merge_sources_on_owner`.

## Findings that do not block

4. **CR4. T3, release on another thread: the target is mis-specified; the code is at the floor the design implies.**
   The design's row says "within 1.1x of same-thread release" (`shared-reader.md` T3). Same-thread release is 9.8 ns and any cross-thread
   hand-off costs 200 ns or more by itself (the ledger's control: 216 to 268 ns), so the row as written cannot be met by any registry. The
   ledger's control (same hand-off, release where acquired) is the right comparison, and the registry adds about 90 ns over it: the record's
   line is written by the releasing core and must come back for the next claim. I read `bench/shared_reader/acquire.cc:149-181`: the control
   is sound (the same flag traffic, the guard released on the acquiring thread). No timing run by me (told not to). So: **the target is wrong,
   not the code.** For the architect: restate T3's row as a bound on the registry's added cost over a hand-off control (for example at most
   2x, or an absolute ns), reported per platform. One untried idea for whoever measures next, not a finding: the first probe at the hint is a load and then a
   compare-exchange (`hazard.hh:194`), two coherence steps on a line last written by another core; a bare compare-exchange at the hint, with the load kept for
   later probes, would take one, at the price of SR-D17's guarantee when hints collide. Measure before believing it.

5. **CR5. The API note understates the break for prospect.** `segment_view::deleted` is `const deleted_bits*` (`index.hh:759`), but
   `basic_merge_source::deleted` and its sibling (`merge.hh:32`, `:51`) stay `const std::vector<std::uint64_t>*`. Prospect builds merge
   sources straight from segment views at `test/reorder_test.cc:119`, `test/merge_test.cc:91` and similar call sites (and `segment_snapshot::deleted`,
   `live_scorer` at `searcher.hh:26-45`, which the ledger names). All of them stop compiling on repin, not only the two the ledger lists. Nothing
   else public changed unexpectedly: new `strata::deleted_bits`, `off_owner_scope` and `detail::owner_only` (`owner.hh`), `basic_index::snapshot` and
   `snapshot_scannable()`, `check_segment`'s pointer type, `~basic_index` running `end_merges()`, and `pool_executor` marking its workers.
   Follow-up: list the merge-source sites in the repin note, or give `basic_merge_source` the same type.

6. **CR6. The `pool_executor` worker mark is untested wiring.** Mutant `mutsrc/nowrap_pool` (`pool_executor.hh:72` removed) passes everything
   (`out/mm_nowrap_pool.log`); the tests mark their worker by hand. A debug tripwire, so low risk: one test that runs a `pool_executor` task and
   reads the flag would pin it. Also K7's check is by thread mark, not lease; SR-D19 already owns that, and note a host running owner work
   inside a `pool_executor` task trips it with `-DNDEBUG` off.

7. **CR7. Diagnostics are exact only to the last stamping scan** (`hazard.hh:133-147`): a scan that loses the try-lock skips the stamp and the high-water
   update, so a short peak can be missed and `since` uses the stamper's clock. Documented in the header; fine for SR-D7's purpose. `claims()` takes a blocking mutex for
   one pass over H records (`hazard.hh:157`); keep it off reactor threads.

8. **CR8. CI note.** My RelWithDebInfo build compiles with `-DNDEBUG`, so there the owner assertion is off and only the counter in
   `shared_read_test` (`CHECK(after == before)`) guards SR-D5; it still passes (`ctest-rel-new.log`). The ledger's "asserts on" figure is
   the contributor's own configuration.

## What I checked, against the five questions

1. **Hazard handshake and SR-D18.** Read `hazard.hh` end to end: claim is a load then a seq_cst compare-exchange from null at a per-thread
   hint; the validation loop re-stores into the record it owns; release is one release store; scan loads every record seq_cst before any lock, so
   two scanners, or a scanner and a retirer, free only pointers no record names at their own load, and a scan that loses the try-lock
   still names everything (`hazard.hh:133-136`). The argument holds for recycled addresses and for records naming another index's pointer. Evidence: T8 natively 3 runs,
   seq_cst 0 free-while-read in 2.5x10^6 claim and 10^7 retry iterations each, the committed weak mutant 3, 3 and 7 in the retry shape and
   reader-side-only 14 to 20 (`litmus-rel-*.log`); `stress2` as in CR2; stock `hazard_test` and mutants `novalidate`, `noscan`, `steal`
   (ASan use-after-free, `out/hm_*.log`), `scan_stops_at_first_null`, `settle_no_restore`, `release_relaxed` (TSan race) all killed by stock
   `hazard_test`; `stamp_unlocked` killed by `two_scanners` under TSan (`out/m_stampunlocked_hazard_tsan.log`). Gap: CR2 and CR3a.
2. **Copy-on-write bits.** `deleted_bits.hh` has no unshared write path (`data/begin/end/operator[]` are const; `set`, `reset`, `mutable_words` clone
   when the count is above one); heal's move (`index.hh:737`) keeps the count; the carry and load paths write a fresh or sole-owner copy. One allocation
   per segment, as the vector was, plus one clone per delete after a share. `bits_under_readers` plus seeds 21 to 23 under ASan and 21, 22 under TSan: no change under a
   share, rollback clones; mutants `noclone` and `reset_inplace` (reset writing through `data()`) killed. The merge-source assertion: covered for publish, close and
   destruction (mutants `nojobdrop`, `endmerges_nodrop` killed, and a 6 x 40-round `pool_executor` probe with 0 off-owner changes), not for install or move-assignment
   (CR3b, CR1).
3. **Shared read path.** `diffprobe.cc` compiled against main's `include/` and the branch's: same workload, same seeds, adds, replaces, erases,
   rejected transactions, refresh, commit, force_merge, wait_merges, close and reopen with and without a commit, `auto_merge` on odd seeds: the digests of
   every get, full scan, bounded and limited scan are identical on all 12 seeds (`out/diff_main.log` vs `out/diff_branch.log`, about 220 read points each),
   and the branch's snapshots (fresh, and frozen shares held across later deletes, merges, commits and reopens, about 440 checks per seed) equal the model; ASan 4
   more seeds clean. Mutants `scan_ignores_bits` and `get_ignores_bits` killed.
4. **T3.** CR4.
5. **API.** CR5, CR6.

## Calibration checks applied

Mutants run for each criterion rather than read from the ledger (a test that passes without its code: CR2, CR3); limits: no new cap beyond H, which the host
sizes; every unpushed commit: none (`origin` equals `HEAD`); CI on other platforms is the contributor's, not green when the ledger was written (below).

## Probes kept (`.scratch/review/`)

`stress2.cc` (two-retirer shared-registry stress), `litmus_variants.cc` and `litmus_sym.cc` (per-order mutants, committed and symmetric harness),
`diffprobe.cc` (main vs branch differential), `poolprobe.cc` (real pool), `abandon_probe.cc` and `moveassign_probe.cc` (held-closure executor), `mut/`
and `mutsrc/` (mutants of `hazard.hh`, `deleted_bits.hh`, `index.hh`, `pool_executor.hh`; `mkmut.py` writes `mut/`), `pb.sh` and `cfg.sh` (builds), `stage*.sh`, `jobs*.txt`,
`out/` and the `ctest-*.log` and `litmus-*.log` results. `main-src/` is `git archive main`.

## Verified green

ctest 35 of 35 under ASan+UBSan (Debug -O1) and under TSan (-O1, 0 reports); the new tests plus scan and index at -O2 NDEBUG, 8 of 8.

## Not run

Linux, FreeBSD, arm64 hardware (so validate-load and scan-load weakening on arm64 is unchecked); CI on GitHub for the branch; prospect against this branch;
T3 and any timing beyond the litmus (CR4 is from the ledger's figures and the bench source); a C11 model checker; `STRATA_FAULTFS_MAP=1` with the frozen-share differential
(retention does not yet know views); the contributor's `build-*` directories (I built my own).
