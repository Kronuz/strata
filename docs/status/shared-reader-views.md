# shared-reader-views

- Design: `docs/design/shared-reader.md` revision 2 (`b1568a7` on `design/shared-reader`), step 1
  items 3 and 5; its review `docs/reviews/shared-reader-design.md` there (the revision 2 review,
  SR-D18 to SR-D21, is in the design worktree's uncommitted copy). The lease is the write-buffer
  design's D2 (`design/write-modes`, `docs/design/write-buffer.md`).
- Branch / worktree: `phase4/shared-reader-views` from main `cee731b`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader-views`. Lock-free concurrency: a code review
  and the architect's scoped check before it lands.
- State: in review. Every criterion but K5 is met with evidence; K5 was green at `4392d39`
  (all 8 jobs) and the last push needs its run.
- Done criteria:
  - [x] K1 Items 3 and 5 as D1, D4, D6 to D9 and D11 state them (`909ff4e`): `view`,
    `reader_handle`, `view_guard`, `options::readers`, `reader()`, the publishable predicate at
    every point of D1's table (refresh, commit, merge publication, heal, install, the first
    reader), retirement and the scan, `views_held()`; `retain()` naming unfreed views' segments,
    commit's order, `reclaim()`'s data-file pass, the non-blocking `close()`,
    `try_finish_close()`, the destructor and move-assignment, `publish_through()`. IDs in the
    commit messages and in comments where a rule needs one (SR-D1, SR-D2, SR-D6, SR-D15, SR-D16,
    SR-D19, SR-D20).
  - [x] K2 SR-D19 and SR-D20 closed (`909ff4e`, `4392d39`, the NDEBUG fix below). Revert checks,
    each by `.scratch/srv/mutant.py` (in place, rebuilt, restored):
    - SR-D19, lease primary: `owner_only()` back to the mark alone (`lease_mark_only`):
      `lease_is_primary` fails (`shared_view_test.cc:432`, an unleased drop on an unmarked thread
      goes unreported).
    - SR-D19, mark second: the mark checked before the lease (`mark_beats_lease`):
      `lease_on_any_thread` fails (`:467`, a host's writer calls in a pool task are reported).
    - SR-D20, the holder token: no second-holder check (`lease_no_token`): `lease_is_primary`
      fails (`:432`).
    - SR-D20, coalescing under the lease: `publish_through` without its check
      (`publish_through_always`): `publish_through_coalesces` fails (`:363`).
  - [x] K3 15 mutants, 15 killed (`.scratch/srv/mutants.log`, logs in `.scratch/srv/out/`), by
    `shared_view_test` read and mapped unless named: the four above; `pos_before_view` (SR-D15
    order) and `no_odd_seq` (the position's sequence) by `publication_order` (`:389`, `:383`);
    `no_publishable` (SR-D1) and `view_shares_no_bits` (D3) by `publication_points` (`:158`);
    `keyed_on_flush` (SR-D16) by `publication_points` (`:138`); `views_not_retained` (D7) by
    `retention_under_guard` (`:235`) and `reclaim_backlog_view` (faultfs truncate abort);
    `free_without_scan` (D6) by `retire_and_free` (`:194`, then ASan heap-use-after-free);
    `close_frees_held` (D11) by `close_with_guards` (`:275`, then ASan heap-use-after-free);
    `reclaim_no_data_pass` (D6) by `retention_under_guard` (`:240`); `commit_retain_first`
    (SR-D6) by `retention_under_guard` (`:258`); `dtor_no_report` (SR-D2) by `destroy_and_move`
    (`:325`).
  - [x] K4 macOS x86_64, Homebrew clang 23, light holds, at `4392d39`: ASan+UBSan (Debug -O1)
    ctest 43 of 43 (`.scratch/srv/ctest-asan-2.log`); TSan (Debug -O1) 43 of 43, 0 reports
    (`ctest-tsan-2.log`). After the NDEBUG fix: `shared_view_test` and `shared_read_test` pass
    under ASan, `shared_view_test` under TSan with 0 reports, and an NDEBUG RelWithDebInfo build
    passes `shared_view_test`, `shared_read_test`, `hazard_test`, `reclaim_backlog_test view`.
  - [ ] K5 CI: 25dae46 red (flush_memory, fixed below); `4392d39` green on all 8 jobs (run
    40280629907). The NDEBUG fix and ledger are pushed after it; no job had finished when I stopped.
  - [x] K6 See "Public API" below.
- Done: `909ff4e` items 3 and 5 with the lease; `25dae46` test order; `4392d39` close() refuses
  before the lease (CI fix); `bc59e2b` ledger; the NDEBUG build fix (`owner.hh` used
  `if constexpr` on members that exist only with lease checks; CI builds none with NDEBUG).
- Review round (`docs/reviews/shared-reader-views-code.md`, approve with conditions): CR1 to CR4
  folded in, rebased onto origin/main (cf77b3a), CR6's comment fix; see "Review round" below.
- Next: code review (lock-free concurrency) and the architect's scoped check of SR-D19 and SR-D20;
  then step 1's item 6 (T1, T2's seeded schedule, T9, T4) is the next branch's.
- Blocked on: nothing. Note for the architect: SR-D19 and SR-D20 are not yet in a revision 3 text
  (C7 asks for that before items 3 and 5); this branch builds the review's own fix text, recorded
  in the log below.
- Findings for the lead, not changed here:
  - `install()` keeps the installed segments mapped from the source's files until a reopen
    (`load_files(source, ...)` then `segs_ = got.segments`), so a source that later reclaims its
    checkpoint would truncate files the target maps. Found when the mapped test destroyed the
    source's faultfs first.
  - The views keep T2's single-thread seeded schedule, T1's full sizes and T9 for item 6; the
    stress here is T1 and T2(b) in small (4 readers, 3,000 transactions, about 4,500 reads).

## Public API (K6)

- New in `strata/core/index.hh`: `basic_index_options::readers`; `basic_index::view`,
  `view_guard`, `reader_handle` (`acquire()`, `published_position()`, `try_published_position()`,
  `generation()`), `reader()`, `publish_through(p)`, `views_held()` and `views_report`,
  `try_finish_close(deadline)`, `closing()`; aliases `basic_view<L>`, `basic_reader<L>`; errors
  `index_closed`, `readers_active`.
- New in `owner.hh`: `write_lease`, `lease_scope`, `detail::owner_break`,
  `detail::abort_on_owner_break`, `detail::lease_token`, `STRATA_LEASE_CHECKS` (default: on unless
  `NDEBUG`). In `hazard.hh`: `basic_retired::for_each`, `leak`, `detail::keep_forever`.
- Changed: `detail::off_owner_flag()` returns `bool`, not `bool&`. `owner_only()` requires a lease
  on the calling thread in lease-checked builds, so code that shares or drops a segment's bits or
  segments outside an index call must hold the index's lease (only tests did; migrated:
  `shared_read_test`, `background_merge_test`, `reclaim_backlog_test`). `close()` with views held
  leaves the index `closing()`; without a registry it behaves as before. `reclaim()` frees views
  readers released, then queues their files. The destructor reports and leaks views still held.
  ctest `reclaim_backlog_view_xfail` became `reclaim_backlog_view` (it now passes).
- Callers outside strata: prospect has no call site of a changed name (its `index_test_access`
  helpers swap segments and set a cap, dropping nothing); it pins an older strata and needs CR5's
  repin first, so it was not built against this branch. Xapiand does not link strata yet. Detent
  links the cluster libraries, not this; nothing it links changed.

## Log

### 2026-10-25

- Read the core ledger and review, the design's D1 to D12 and step 1, the revision 2 review
  (SR-D19, SR-D20 in the design worktree), and the write buffer's D2 lease.
- Decisions taken from the review text, for the reviewer to confirm:
  - SR-D20: step 1's lease is the debug-checked holder token only. Every owner duty (each public
    writer call, `reader()`, `publish_through()`, `close()`, `try_finish_close()`, `open()`, the
    destructor) runs in a `lease_scope` on the index's `write_lease`; the outermost scope
    compare-exchanges the token from free (acquire) and stores free at its end (release). A second
    thread's scope while one holds it is reported. Release builds keep no token.
  - SR-D19: `owner_only()` (at bit shares and drops and segment destruction) requires a lease held
    on this thread in debug builds. The worker mark stays as the second check in every build:
    background work holds no lease, so owner state changing on a marked thread with no lease is
    counted. A marked thread that takes a lease (a host request run on the pool) is owner work.
- `909ff4e` items 3 and 5 with the lease; `25dae46` test order fix (the installed segments stay
  mapped from the install source's files until a reopen, so the source must outlive the index:
  a pre-existing property of `install()`, found when the mapped run touched a destroyed faultfs;
  for the lead, not changed here).
- CI on `25dae46`: flush_memory aborted on linux-asan-ubsan, linux-tsan, linux-x86_64-gcc and
  macos-arm64 (lead's report). Cause: `close_inside_a_flush` calls `close()` from whichever thread
  writes first; with three runs that is often a run thread holding no lease, and the lease check
  reported a second holder. Fix `4392d39`: `close()` refuses inside a flush or scan callback before
  taking the lease (a refusal changes nothing). The test now always calls it from a run thread
  with three runs. Revert check: with the old order the test aborts at `owner.hh:54` (rc 134,
  `.scratch/srv/fm-revert.log`); with the fix it passes (`fm-fixed.log`). The ASan-only
  `flush_memory_test.cc:393` bound is the flake the lead sees on main.

### 2026-10-26 review round

- Rebased onto origin/main (clean, 6 commits). Fixes, each checked by rebuilding the test against
  a mutated copy of `index.hh` (`.scratch/cr/mk.py`, `pm.sh`, `runmut.sh`), ASan+UBSan, read and
  mapped runs of `shared_view_test`:
  - CR1: `apply()` calls `changed()` when `m_.applied` moves, accepted or rejected. Tests: a
    rejected transaction at {7,1} in `publication_points`, and one at {4,1} then
    `publish_through` twice in `publish_through_coalesces`. Fix reverted: 3 CHECKs fail (read and
    mapped).
  - CR2: `heal_publishes` (bit flips over every segment file, `heal()` after a read meets
    `corrupt_segment`; 26 heals, 26 new views, old views meet a typed error). It turns map_files
    off so the media faults apply in the mapped run too. `changed()` removed from `heal()`
    (`heal_no_changed`): fails (read and mapped).
  - CR3: after the leaking destruction, `kv_index::open` on the same files must throw. Lock leak
    removed (`dtor_no_lock_leak`): fails (read and mapped).
  - CR4: `position_under_readers` (3 readers: read generation and position, `acquire()`, view at
    or past both; one owner publishing 2,000 positions). Clean under ASan and TSan. The
    `position_relaxed` mutant (the four position stores relaxed) survives on x86_64, as the
    reviewer expected; only arm64 can fail it. Kept; the test comment says so.
- CR6 (comment half): `deleted_bits.hh` now says the host's serialization, not the lease, orders
  one holder's changes before the next's (revision 3, D4).
- Follow-ups, not fixed here:
  - CR5: the leaf check `owner_only()` asks for any lease on the thread, not this index's; `get`,
    `scan`, `snapshot()`, `views_held()` and `segments()` take no lease scope. Fix later: an index
    identity in the leaf, or scope the reads.
  - CR6 (rest): say next to `STRATA_LEASE_CHECKS` that it changes `write_lease`'s size, so TUs
    built with and without `NDEBUG` must not be mixed (ODR).
  - CR7: the litmus flake on linux-x86_64-clang: addressed by cf77b3a on main; K5 needs a green
    rerun.
  - CR8: `reader_handle::published_position()` spins while the sequence is odd, so a descheduled
    owner stalls it; document which of it and `try_published_position()` a reactor thread calls.
  - CR9: `views_held()` is `const` and reads `segs_` and the retired list without a lease scope;
    document it as an owner call.
  - CR10: the in-branch stress has no `install()`, `heal()`, destruction or move-assignment under
    readers and no rejected transaction; item 6's T1 should include them (the reviewer's
    `stress_views.cc`).
  - `install()` hazard: reachable with a live source in the same process. After `commit_files()`
    in `install()`, reopen each segment from the target's own new file (`load_segment(*io_, m_,
    s->ref, ...)`, as `heal()` does) and drop the source-mapped object, or copy the bytes into
    owned storage when the source is not a transfer directory the host owns. Probe:
    `strata-wt/review-srv/.scratch/probes/install_source_reclaim.cc` with `STRATA_FAULTFS_MAP=1`
    (aborts: a file truncated below a live mapping).
- Results at the rebased head (macOS x86_64, Homebrew clang 23, Debug -O1, light holds): ASan+UBSan
  ctest 43 of 43. TSan ctest 42 of 43 in the full run with 0 reports; `format14` hit my 900 s
  timeout on a loaded host (it takes 1000 to 1450 s under TSan) and passed alone (1035 s). Not
  run: Linux, arm64.
