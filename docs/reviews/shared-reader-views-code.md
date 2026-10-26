# Code review: phase4/shared-reader-views (step 1, items 3 and 5, with the write lease)

- Reviewed: `phase4/shared-reader-views` at `428d415` (`909ff4e`, `25dae46`, `4392d39`, `bc59e2b`, `428d415`), merge base `cee731b`;
  `origin` equals `HEAD`, the contributor's worktree has no uncommitted changes. Worktree `strata-wt/review-srv` (detached at `428d415`).
- Against: `docs/design/shared-reader.md` revision 2 (`b1568a7` on `design/shared-reader`: D1, D3, D4, D6, D7, D9, D11, SR-D1, SR-D2,
  SR-D6, SR-D15, SR-D16, step 1 items 3 and 5), its review (`shared-reader-design.md`, the uncommitted revision-2 section in the design
  worktree: SR-D19, SR-D20, C7), the write buffer's lease (`write-buffer.md` D2 on `design/write-modes`; the review prompt said D1),
  the ledger `docs/status/shared-reader-views.md`, `docs/reviews/shared-reader-core.md`, `docs/reviews/calibration.md`.
- Checked by: my own builds (Homebrew LLVM 23.1.2, macOS x86_64, `quiet light`): TSan (Debug -O1) full ctest, 43 of 43, 0 reports
  (`.scratch/out/ctest-tsan.log`); ASan+UBSan (Debug -O1) of `shared_view_test` (read and mapped), `shared_read_test` (read and mapped),
  `hazard_test`, `reclaim_backlog_test view` (read and mapped), `flush_memory_test`, all pass (`.scratch/out/asan-subset.log`); the probes
  below. Probes, scripts, mutants and logs are in `.scratch/` of the review worktree (list at the end).

## Verdict

**APPROVE WITH CONDITIONS.** I could not make a reader see a retired, freed or truncated segment, or make `close()`, the destructor
or move-assignment race a live guard: a stress of my own (two writers on two lease threads, one registry, pair transfers with a sum
invariant, erase-and-re-add, rejected transactions, `heal()`, `install()`, `close()` -> `try_finish_close()` -> reopen with guards
out, destruction with guards out, mapped faultfs with 4 KiB `reclaim_step`) is clean under ASan (4 seeds, read and mapped) and the
retention and close design holds. What blocks landing is one real defect in the publication rule (CR1: a rejected transaction moves
the applied position and never publishes, so `publish_through(p)` can never be satisfied for it), and three places where the branch's
own claims are not backed by a test that fails without the code (CR2, CR3, CR4). All four are small and fit one fix round. The CI
run on this commit has one red job that this branch did not cause (CR7); landing needs a green rerun.

## Blockers (conditions to land)

1. **CR1. A rejected transaction advances the applied position and publishes nothing; `publish_through(p)` for its position never
   completes (SR-D16, D9).** `apply()` ends with `if (at != files::position{}) { m_.applied = at; }` (`index.hh:676`) for an accepted or a
   rejected verdict alike, but `changed()` runs only in the accepted branch (`index.hh:673`). A refresh then publishes nothing
   (`publish_point`, `index.hh:1771`), the view's `position()` stays below the rejected transaction's position, and
   `publish_through({.., p})` (`index.hh:1035`) re-runs `refresh()` on every call (its check `current->position_ >= p` is never true), so
   D9's "one refresh serves every search waiting" does not hold, a refresh flushes whatever is buffered once per call, and a host
   that waits for `published_position() >= p` waits forever. SR-D16 says a point publishes "when the applied position moved ... a delete of
   a flushed document and a metadata or schema transaction move it and buffer nothing"; a version conflict (a routine 409) moves it the
   same way. Probe `.scratch/probes/rejected_position.cc` (TSan and ASan builds): accepted at {1,1}, rejected at {1,2}, `refresh()`,
   `publish_through({1,2})` three times: published position stays 1/1, generation unchanged, CHECK fails (`out/` prints
   "published 1/1 gen 2", then `FAIL: 1`). Fix: `changed()` when `m_.applied` moves in `apply` (one line, `mut/rejected_moves_position_changed`
   is the fix as a header copy); add the case to `publication_points`/`publish_through_coalesces` (a rejected transaction at a new position,
   then `publish_through` of it). Re-run `rejected_position.cc`: it must exit 0. With that header the probe prints "published 1/2 gen 3 ... ok" and exits 0 (`out/rejected_position_fixed.log`).

2. **CR2. A `heal()` that replaces a segment object is untested at the publication point D1 names (D1 table, D11).** `heal()` calls
   `changed()` and `publish_point()` (`index.hh:832`, `:841`); in `shared_view_test` the only `heal()` call returns empty on an undamaged
   index (`:159`), so neither line is exercised. Mutants `mut/heal_no_changed` and `mut/heal_no_publish` (each line removed):
   `heal_no_changed` **survives** `shared_view_test` (read and mapped, rc 0, `out/mutants.log`), and `heal_no_publish` removes a line no test reaches (not built against the suite); my probe `heal_view.cc` (198 transient bit flips over every segment file, `heal()` after a read meets
   `corrupt_segment`) kills both ("heal replaced a segment but no new view", 107 of 107 cases, new view bad 107) and passes the real code:
   107 heals, 107 new views, 107 old guards meet a typed `corrupt_segment` and none crashes, new view digest right (`out/` and the run
   above). Fix: add `heal_view.cc` (or its core) to `shared_view_test`.

3. **CR3. The destructor's leaked directory lock is untested (D11, SR-D2).** `~basic_index` leaks `lock_` "so no reopen in this process
   truncates their files" (`index.hh:560`); `destroy_and_move` (`shared_view_test.cc:~308`) destroys the index with a guard out, checks the
   break count and reads the leaked view, but never reopens. Mutant `mut/dtor_no_lock_leak` (that statement removed): **survives** `shared_view_test` (read and mapped, rc 0).
   My stress probe checks it after the leak (`kv_index::open(ioa, ..)` must throw; "lock held after leak: yes" in every seed). Fix: one
   `CHECK(throws([&]{ (void)kv_index::open(io, ...); }))` after the leaking destruction.

4. **CR4. The order of the new position atomics is only tested by stepping, which x86_64 cannot falsify (SR-D15).** The tests
   (`publication_order`, `shared_view_test.cc:~382`) call a probe between the owner's stores, which checks the logical order of the
   code; no thread reads `published_position()` then `acquire()` against a publishing owner, so releasing the stores
   (`index.hh:1805-1811`) to relaxed (mutant `mut/position_relaxed`: **survives**, rc 0 read and mapped) cannot fail on x86_64, and TSan does not model it
   (atomics only). The arm64 CI jobs (macos-arm64, linux-arm64-gcc) would catch it only if the suite has the assertion. Probe
   `fastpath.cc` (3 readers: read `generation()` and `published_position()`, `acquire()`, require the view at or past both; one
   publishing thread): on the real code under TSan, 26,080,831 checks, 0 bad (`out/fastpath_tsan.log`); it is the assertion the suite lacks, valid and cheap, and it can only fail on arm64. Fix: add that check to `views_under_readers` (readers already loop on `acquire()`); say in the test
   comment that only arm64 can fail it, as `hazard_litmus_test` does for its load-only rows.

## Findings that do not block

5. **CR5. The lease check is weaker than the text of SR-D19 in two ways (rule below).** (a) The leaf check `owner_only()` asks only
   whether this thread is inside any lease scope (`owner.hh:58-61`, `leases == 0`), not this index's: a thread holding index A's lease
   drops index B's share with no report (`lease_gaps.cc` (a): 0 breaks). (b) `get`, `scan`, `snapshot()`, `views_held()` and `segments()`
   take no lease scope although `scan` writes `scanning_` and the sorted view: a second thread scanning while another holds the lease is
   not reported (`lease_gaps.cc` (b): 0 breaks). Neither is a regression (main had no check at all), and the single-writer-per-index host
   model makes (a) theoretical, but the revision-3 text must say which it is (ruling below). Fix owed later, not now: let the leaf
   carry an index identity, or scope the reads.

6. **CR6. In release builds nothing orders one lease holder's changes before the next's.** D3's plain share count relies on "the
   lease's handoff orders one holder's changes before the next's" (`deleted_bits.hh:5`, design D3/D4). The token that does that exists only
   with `STRATA_LEASE_CHECKS` (default: not under `NDEBUG`, `owner.hh:19-24`), so in a release build the host's own serialization is the
   only happens-before. That is what SR-D20 asked for ("the host keeps serializing"), but D3 and D4 say the lease provides it and must say
   instead that the host's serialization must. Also `STRATA_LEASE_CHECKS` changes `write_lease`'s size, so it is a layout switch: mixing
   TUs built with and without `NDEBUG` is an ODR violation; say so next to the macro.

7. **CR7. CI on `428d415`: 7 of 8 jobs green; `linux-x86_64-clang` red in `hazard_litmus` (run 40293639065).** "FAIL: the litmus did not catch
   'reader: record write release only' on this x86_64 host" (retry 0, claim 0, cap 20). This branch changes neither the litmus test nor
   the orders in `hazard.hh` (only `keep_forever`, `for_each`, `leak` are added); the same failure class is on main
   (`linux-x86_64-gcc`, run 39783377258, "owner: publish release only"). So not caused here, but K5 is not met until a rerun is green, and the
   litmus's probabilistic mutant check is a standing flake the lead should owe a follow-up (a larger iteration count or a retry for the
   reader-only rows, as `test: retry an uncaught litmus mutant before failing` did for another row).

8. **CR8. `reader_handle::published_position()` can wait on a preempted owner.** It spins and yields while the sequence is odd
   (`index.hh:997`); the owner's three stores are short, but a descheduled owner stalls readers there, against D9's "acquire never blocks".
   `try_published_position()` exists; say in the header which one a reactor thread calls.

9. **CR9. `views_held()` is `const` and reads `segs_` and the retired list with no lease scope** (`index.hh:1050`); it is an owner call and
   must be documented as such (the design lists it among the closing index's calls, which a host may make from any thread).

10. **CR10. The in-branch stress is small (T1 and T2(b) in small, as the ledger says).** It has no `install()`, `heal()`, destruction or
    move-assignment under readers, and no rejected transaction; my `stress_views.cc` adds them (ASan read and mapped, 4 seeds clean;
    TSan seed 1 clean, 0 reports, `out/stress_tsan_1.log`; the other TSan seeds were not run, see Not run). Item 6's T1 should include them.

## The pre-existing hazard the contributor flagged: `install()` and the source's files

Reachable, with a live source index in the same process. `install()` maps its segments from the source's files (`load_files(source, ...)`
at `index.hh:1151`, then `segs_ = std::move(got.segments)` at `:1189`), writes copies under the target's own names, and keeps the
mapping of the source's file (`s->ref` is rewritten, the mapping is not). When the source later queues and truncates those files
(it merges, commits, releases the checkpoint, and `reclaim()` runs) the target maps a truncated file: SIGBUS on a real filesystem, the
faultfs abort in the tests. Probe `install_source_reclaim.cc` with `STRATA_FAULTFS_MAP=1`: abort "a file truncated below a live
mapping" (rc 134) when the target installed; the control without the install runs clean (`out/install_source_reclaim*.log`). Unmapped IO
(`STRATA_FAULTFS_MAP` unset) does not fail because the bytes were read, not mapped. Views make it matter more (a view of the target
holds those segments), but D7's retention on the target never sees the source's files. What it would take: after `commit_files()` in
`install()`, reopen each segment from the target's own new file (`load_segment(*io_, m_, s->ref, ...)`, as `heal()` does) and drop the
source-mapped object, or copy the bytes into owned storage when the source is not a transfer directory the host owns; one extra read
of the installed bytes. Where the source is a transfer directory that is only unlinked afterwards, POSIX keeps the mapping valid, so
the live-source case is the reachable one (a same-process replica, tests). Not a blocker for this branch.

## Ruling on SR-D19 and SR-D20 against C7

The implementation follows the review's fix text, and with the points below it can stand in for revision 3's text on those two items.
SR-D19: an index-level holder token (`write_lease`, `owner.hh:74-102`), set by an acquire compare-exchange in the outermost
`lease_scope` and cleared by a release store at its end (`:104-128`), checked at the entry of every owner duty (`apply`, `refresh`,
`merge`, `force_merge*`, `maybe_merge`, `wait_merges`, `commit`, `reclaim`, `heal`, `checkpoint`, `release`, `install`, `close`,
`try_finish_close`, `reader`, `publish_through`, `open`, `create_deferred`, the destructor), with the worker mark kept as the second
check and stated so (`owner.hh:8-12`); both of the review's reverts are tested (`lease_mark_only`, `mark_beats_lease`, per the ledger,
and `lease_no_token`). SR-D20: the lease is the debug-checked token only, no lock, no queue, no executor; `publish_through` takes the
lease and then rechecks the position (`index.hh:1035`), which is the coalescing the review asked for. What revision 3 must still say,
because the code decided it and the review did not: (1) the leaf check is "this thread holds some lease", not "this index's" (CR5a);
(2) reads (`get`, `scan`, `segments`, `views_held`) are outside the lease (CR5b), so D2's "the lease holder's reads are unchanged" means
unchecked; (3) scopes nest on one thread and are one hold, and a moved lease starts free; (4) in release builds there is no token, so
D3 and D4's "handoff orders" is the host's serialization (CR6); (5) a marked pool thread that takes a lease is owner work (the ledger
says so; D4 should); (6) SR-D16's "applied position moved" includes a rejected transaction (CR1); (7) D2 and D9 still read "posted to
the owner" and "the owner runs posted calls in order" at `b1568a7`; they become "under the lease" and "each holder rechecks after it
takes the lease". No further architect re-check is needed for those if the text states them; the code reviewer's confirmation (this
file) is what C7 asked for.

## What I checked, against the brief

1. **A view or reader observing a retired or reclaimed segment.** Views hold `shared_ptr<const open_segment>` and share-counted bits;
   retirement is only through `free_views()` (the registry scan, `index.hh:1825`), which every freeing path uses (publish, commit,
   `reclaim()`, `close()`, `try_finish_close()`, the destructor, `install()`); the destructor and `close()` retire the current view first
   and publish null so acquires fail (`retire_current`, `index.hh:1818`). The stress above (ASan, mapped faultfs aborting on a truncate
   below a mapping) never failed. Heal leaves the failing object with older views and hands them a typed error (`heal_view.cc`).
2. **A publish point skipping the publishability rule.** Every `publish_view` goes through `publish_point` (`index.hh:1770-1775`, the only
   caller), and its guard is `buffer_size() != 0`; its callers are refresh, commit, merge publication, heal, install and (through
   `refresh`) the first `reader()`. Merge installs and heal with a pending replace keep the old view (tested, `publication_points`).
   Gap: CR1 (position) and CR2.
3. **Retention freeing a file a view still uses.** `retain()` names every unfreed view's segments (`index.hh:3155-3158`), in `retain(true)`
   too; `reclaim()` runs the data-file pass after freeing a view (`:769`); commit's order is flush, publish, free, `retain()`, reclaim
   (`:756-759`). The held-view test is the faultfs oracle; my stress with `reclaim_step` 4096 mapped did not abort. The one hole is the
   `install()` source (above).
4. **`close()`, the destructor or move-assignment racing a live guard.** None freed a held view in the stress (guards on the retired,
   current and leaked views, mapped); the destructor reports (abort in debug) and leaks views, segments and the lock; close refuses a
   second `close()`; a closing index throws `index_fenced` from the writer calls. The move-assignment is `destroy_at` then `construct_at`
   (`index.hh:542`), so it follows the destructor. The destructor's lock leak is untested (CR3).
5. **The lease token bypassable on a path that mutates owner state.** CR5. No writer call lacks a scope except reads.
6. **Ordering on new atomics.** `reader_state::seq/at_index/at_applied` is a sequence lock: the odd store before the data, release on
   each store, acquire loads and a second `seq` load (`index.hh:1805-1811`, `:990-1003`); by reading it is correct (a release store orders
   the earlier odd `seq` store before it; the reader's acquire data loads keep the second `seq` load after them). The `current` pointer is
   the registry's seq_cst path (the core review). The lease token is acquire/release on a debug-only word. The mutants the suite cannot
   kill on x86_64: CR4.
7. **Tests that pass without the code.** CR2, CR3, CR4. The contributor's list of 15 mutants I did not re-run in full; I re-ran the
   ones named in the table below and read the rest.
8. **Scope.** Nothing beyond the design except `keep_forever`/`leak` (D11), the `abort_on_owner_break` test switch, and ctest
   `reclaim_backlog_view_xfail` -> `reclaim_backlog_view` (which the design asked for). Callers outside strata: not built (prospect pins
   an older strata; the ledger says so).

## Mutants and probes run by me

| Name | What | Result |
|---|---|---|
| `heal_no_changed` (built and run), `heal_no_publish` (generated; same untested lines) | `changed()` / `publish_point()` removed in `heal()` | survive `shared_view_test`; killed by `heal_view.cc` |
| `dtor_no_lock_leak` | destructor does not leak `lock_` | survives `shared_view_test` (read, mapped) |
| `position_relaxed` | the three position stores relaxed | survives `shared_view_test` (read, mapped); x86_64 cannot kill it |
| `rejected_moves_position_changed` | the CR1 fix | `rejected_position.cc` exits 0 with it; `shared_view_test` with it not run |

## Not run

Linux, FreeBSD and arm64 hardware (the position-order mutant and the litmus's load-only rows need arm64); the full ASan ctest (a subset
only: `shared_view`, `shared_view_mapped`, `shared_read`, `shared_read_mapped`, `hazard`, `reclaim_backlog_view`, `flush_memory`); the
contributor's other mutants (`lease_*`, `pos_before_view`, `no_odd_seq`, `no_publishable`, `views_not_retained`, `free_without_scan`,
`close_frees_held`, `reclaim_no_data_pass`, `commit_retain_first`, `publish_through_always`, `dtor_no_report`, `view_shares_no_bits`)
beyond reading them; `shared_view_test` with the CR1 fix applied; the TSan stress for seeds 2, 3 and the mapped one (the first TSan run reported races that were my probe's own unlocked read of its guard deque, fixed; seed 1 was rerun clean); the commit-order and install-order mutants I wrote (`commit_no_free_views`, `install_no_free_views`,
`commit_files_no_changed`, `merge_no_changed`, `dtor_no_retire_current`: generated in `.scratch/mut/`, not built; the host's strict holds
paused my light holds for most of the session); prospect against this branch; any timing; a model checker; GitHub CI beyond reading
the run's log.

## Probes kept (`.scratch/` of `strata-wt/review-srv`)

`cfg.sh` (configure), `pb.sh` (build a probe), `pm.sh` (build a test or probe against a mutated header), `mkmut.py` (writes
`mut/<name>/strata/core/index.hh`), `runstress.sh`, `runstress2.sh`, `runmut.sh`, `runmisc.sh`, `runfinal.sh`;
`probes/rejected_position.cc` (CR1), `heal_view.cc` (CR2), `stress_views.cc` (CR10 and the retention/close/destroy stress),
`lease_gaps.cc` (CR5), `fastpath.cc` (CR4), `install_source_reclaim.cc` (the install note); logs in `out/`.

## Re-check (2026-10-26)

Scope: CR1 to CR4 against `59b66d5` (fix `c485ef5`), a worktree `strata-wt/recheck-srv` built with Homebrew LLVM, ASan+UBSan and TSan,
`shared_view_test` only. The rebase changed nothing in the reviewed code: `git range-diff cee731b..428d415 e5ab2d0..631903a` (old
base to old tip against main's merge-base to the rebased tip) shows all five commits `=`; the only tree differences between
`428d415` and `59b66d5` outside docs are `CMakeLists.txt` (`SKIP_RETURN_CODE 77` on `hazard_litmus`), `test/hazard_litmus_test.cc` and
`test/hazard_test.cc`, which come from main, and the fix commit's four files.

Unmutated `shared_view_test`: ASan+UBSan read and `STRATA_FAULTFS_MAP=1` mapped both exit 0 (`heal publishes: 26 heals, 26 new views,
26 old views met a typed error, 0 bad`; `position under readers`: 45.4M and 43.4M checks, 0 bad); TSan read and mapped exit 0, zero
ThreadSanitizer reports.

| Finding | Status | Evidence |
|---|---|---|
| CR1 | CLOSED | `index.hh:675-678`: `apply()` calls `changed()` when `at != {}` moves `m_.applied`, rejected transactions included. The reviewer's `rejected_position.cc` (copied, unedited) exits 0 under ASan and TSan (`published 1/2 gen 3` after the rejected `{1,2}`; `publish_through({1,2})` x3 leaves it there); against the same header with the call removed (mutant `apply_no_changed`) it prints `FAIL: 1`. Mutant run of `shared_view_test`: 3 failures, at `:150` (`publication_points`) and `:376`, `:378` (`publish_through_coalesces`), read and mapped. |
| CR2 | CLOSED | `heal_publishes` (`shared_view_test.cc:491`, called at `:699`) reaches the replacement branch of `heal()` (`index.hh:835` `changed()`): 26 heals replace a segment (`media_retries_` moves), all 26 publish a new view, 26 old views meet `corrupt_segment`, 0 bad. Mutant `heal_no_changed` fails `:553` (`26 heals, 0 new views, ... 26 bad`) in read and mapped; `heal_no_publish` also fails. The reviewer's `heal_view.cc` still passes (`cases 198, healed 107, new view published 107, failures 0`). Note: the test sets `fs.map_files(false)` because media faults apply to reads, so the "mapped" run is the same code path; the mutant is killed in both. |
| CR3 | CLOSED | `shared_view_test.cc:331`: after the leaking destruction a `kv_index::open` must throw. Mutant `dtor_no_lock_leak` fails exactly `:331`, read and mapped; unmutated passes. |
| CR4 | PARTIAL by design | `position_under_readers` exists (`:560`, called at `:700`), reads generation and position, then acquires, and requires a view at or past both; 3 readers against 2000 apply+refresh, TSan clean in read and mapped (8.6M and 8.7M checks, 0 bad). Mutant `position_relaxed` survives in read and mapped (exit 0), as expected on x86_64; the comment at `:557-559` says only arm64 can fail it and TSan does not model it. Not provable here; needs an arm64 run. |

`deleted_bits.hh:5-7` now reads "the host's serialization orders one holder's changes before the next's (debug builds check it)",
which matches `origin/design/shared-reader` `15b801a` D3/D4 (host serialization in every build, the token's handoff and the leaf check
in debug builds) and `owner.hh:5-6`.

New finding (cosmetic, N1): `test/shared_view_test.cc:484-488`, the comment describing `views_under_readers` (T1 and T2(b)) now sits
directly above the `heal_publishes` comment, detached from its function at `:590`; move it back. Outside scope, one line: an `apply()`
that returns `invalid` or `no_space` before the position step still leaves `m_.applied` unmoved (unchanged behaviour, and not what
CR1 named).

Not run: arm64 hardware (CR4's mutant), Linux and FreeBSD, the full ctest under either sanitizer (only `shared_view_test`, read and
mapped), `shared_read_test`, the hazard tests, GitHub CI, timing, TSan with the mutants (ASan only), the reviewer's other probes
(`stress_views`, `lease_gaps`, `fastpath`, `install_source_reclaim`).

Probes and mutants for this re-check: `strata-wt/recheck-srv/.scratch/` (`cfg.sh`, `pm.sh`, `mkmut.py` with the new `apply_no_changed`,
`runmut2.sh`, `probes/`, logs in `out/`).
