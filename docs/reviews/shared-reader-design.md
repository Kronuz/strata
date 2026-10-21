# strata review: the shared reader (preliminary design review)

- Date: 2026-10-21.
- Reviewed: `docs/design/shared-reader.md` revision 0 at `f466701` and its ledger
  `docs/status/shared-reader.md` at `9c2b7ca` (branch `design/shared-reader`, HEAD equals
  `origin`); strata `main` `75143f9` for every code citation (`c379909`, main's head, changes only
  `docs/perf/exploration-log.md`); prospect `59f36fe`; residency `cdc5236` (strata's pin).
- Checked against: PLAN Goal 5 (`docs/PLAN.md:112-119`); ARCHITECTURE item 9 (`:108-109`), Read
  path (`:156`), Concurrency (`:173-180`), Memory ownership (`:188-192`), the non-blocking host rule
  (`:6`); `phase-3.md` Retention (`:157-198`), `visible_position()` (`:436`), T6 (`:524`);
  `reviews/phase-3-design.md` R7 (`:707-717`); `reviews/rocksdb-survey.md` R1 (`:43-56, 203-205`);
  `reviews/background-merges.md` R4 (`:178`); `core-split.md:80-85`; `xapiand-on-strata.md` D7
  (`:192-196`), D13 (`:224-236`), T10 (`:290`); `workload-profiles.md:7`;
  `perf/benchmark-coverage.md:48-50, 93-94, 144, 198, 245`; `reviews/calibration.md` (every row
  applied: rulings addressed to this subject collected, HEAD against origin, limits, the honest peer
  whose view lags).
- Checked by: reading `index.hh` at every cited line and the writers of `segs_`, `deleted`, and
  `open_segment`'s other fields; standalone probes of D5/D6 as written, in `.scratch/review/`,
  built with Homebrew clang 23.1.2 on macOS x86_64 (i9-9980HK, 8 cores, 16 threads) under a light
  `quiet run`:
  - `sb_litmus.cc` (output `litmus.out`): the handshake's store-load shape, 2,000,000 iterations per
    memory order, native.
  - `hazard_stress.cc` with `build.sh` and `run.sh` (output `runs.out`): one owner publishing
    20,000 views, 8 readers, 1 in 16 guards released on another thread, H = 64, under TSan, ASan +
    UBSan, and natively; mutants `-DMUTANT_WEAK`, `-DMUTANT_NOVALIDATE`, `-DMUTANT_NOSCAN`; modes
    `parking` and `drift` for the record caches.
  - `codegen.c`: what the handshake's orders compile to on arm64 (`-mcpu=apple-m1` and generic).
  - Not run: no strata code was built or tested (design only; the probes model D5 and D6, not
    strata); no arm64, Linux, or FreeBSD host (arm64 by codegen only); no benchmark or timing; no
    model checker; prospect untouched.

## Verdict

**APPROVE WITH CONDITIONS: the hazard handshake is correct as written and fits R7; three
required changes (SR-D1 to SR-D3) fold into revision 1 before the code they shape is written.**

What holds, and reproduces:

- D5's handshake under sequentially consistent operations. The single-total-order argument
  (`shared-reader.md:160-168`) is right: it is Dekker's store-load pair, and both sides use seq_cst.
  `litmus.out`: 0 free-while-read outcomes in 2,000,000 iterations at seq_cst, against 30 at
  release/acquire and 831 at relaxed on the same x86_64 host. `codegen.c`: seq_cst is `stlr` then
  `ldar` on arm64 and `xchg` on x86_64, both forbidding the reorder.
- The full protocol under stress. `runs.out`: `hs_tsan_correct` clean, with 546,038 reads, 16,908
  cross-thread releases, at most 10 retired views, no wrong checksum, no generation going backwards.
  ASan and native runs are clean as well (natively at most 45 retired views, under H = 64).
  `-DMUTANT_NOSCAN` fails under all three builds, `-DMUTANT_NOVALIDATE` under TSan.
- Address reuse (ABA) is harmless, as D5 says: a reader that validates a recycled address holds
  the current view. The native runs recycle addresses freely and read no wrong checksum.
- Every flush runs at a transaction boundary: `flush()` is called only from `refresh()` and
  `commit()` (`index.hh:569, 626`), and `apply` refreshes before stage 1 (`:527`). D1's
  merge-between-refreshes rule (`shared-reader.md:89-92`) is the right guard.
- Retention at 0: a view holds only segments that were in `segs_` when it was published, so no file
  `retain()` queued can be in a view. `retain()` looks at every unqueued file again each round
  (`index.hh:2627-2629`), so files skipped for a view are reconsidered. With D7, no truncate lands
  under a mapping (but see SR-D6 on ordering).
- Today's segment reads are safe off the owner thread. `get` keeps only a thread-local decode buffer
  (`index.hh:279-312, 965-981`), residency's verified state is one atomic per region
  (`residency.hh:156`), and prospect's mutable fields are on per-query cursors
  (`prospect/reader.hh:1338, 1369-1370`, `postings_iterator`).

What blocks the parts it touches: SR-D1 (publication of a state holding part of a transaction),
SR-D2 (destruction and move-assignment free protected views), SR-D3 (the record caches park
records and send every cross-thread acquire to the mutex).

## Findings

1. **SR-D1. Two publication points can expose part of a transaction (required).**
   - D1 publishes only where the buffer was just flushed, and it holds merges between refreshes to
     "nothing applied since the last view" (`shared-reader.md:85-92`). Two other publication
     points skip that rule:
     - D4: "the first `reader()` publishes the current flushed state" (`:128-129`).
     - D11: `heal()` "publishes when it replaced a segment object" (`:92`, `:297-298`).
   - Between flushes, the live bits already hold the deletes of buffered transactions: a
     replace sets the old copy's bit in place (`index.hh:2028`) while the new copy waits in the
     buffer. A view built then shows the replace as a delete, which is a partial transaction, the
     thing D1 rules out. T2's invariant-sum transactions would catch it, but only if T2 publishes
     at these points with a pending replace, and its procedure does not say so (`:308`).
   - Fix: one predicate, "publishable" (nothing applied since the last flush), guards every
     publication point. `reader()` refreshes first when it does not hold, or leaves the handle
     empty until the next flush point (acquire then throws a stated error). `heal()` publishes
     only when it holds, else at the next flush point. T2's deterministic schedule creates the
     first handle and calls `heal()` with a pending replace, and the mutant "publish without the
     predicate" must fail it.

2. **SR-D2. Destroying or move-assigning an index frees views readers protect (required).**
   - `~basic_index()` only resets the merge group, and both move operations are defaulted
     (`index.hh:497-499`). D11 covers `close()` and the move constructor (`shared-reader.md:291-294`),
     but not the destructor or move-assignment onto an index that has views.
   - Under D4, the owner keeps the owning pointers (`:131-133`). Destroying them frees views that
     hazard records still name: a use-after-free in every reader holding one, the bug
     `MUTANT_NOSCAN` shows (`runs.out`, ASan heap-use-after-free). Destruction also drops the
     directory lock, so a reopen's orphan deletion can truncate files a surviving view maps.
   - Fix: the destructor and move-assignment follow `close()`'s rule, whichever Q2 picks. The
     owner settles which: background-merges R4 made destruction wait for merges
     (`reviews/background-merges.md:178`), and SR-D11 explains why waiting is unsafe here. Until
     then, a protected view at destruction aborts in debug builds and leaks (never frees) in
     release builds. T1 destroys an index with guards outstanding under the chosen rule.

3. **SR-D3. The record caches park records and fall back to the mutex on every cross-thread
   release (required).**
   - D5 claims a record from a per-thread cache. A cache keeps records up to a bound, and only
     the excess returns to the pool (`shared-reader.md:172-176`). Two probes show where that
     breaks:
     - `parking` (`runs.out`): H = 16, cache bound 4, four idle threads that each used four
       guards. No guard is held, yet a fifth thread's acquire throws `too_many_readers`. Records
       parked in idle threads' caches are lost to the node, so H has to cover threads times the
       bound plus real demand, and nothing says so.
     - `drift`: acquire on one thread and release on another, D9's resumed task. 100% of
       102,647 claims took the pool mutex, and 99,992 releases returned records under it. In the
       stress run, pool takes track cross-thread releases one for one (16,916 takes for 16,908
       cross releases). D5's "the fast path never takes" the mutex (`:176`) and D9's "acquire
       never blocks" (`:266`) are false for the pattern D9 exists for.
   - The design also leaves three cases open: a thread that exits with cached records, a registry
     destroyed before the threads that cached its records, and several registries in one process
     (a `thread_local` cache must be keyed per registry).
   - Fix (recommended): drop the caches and the pool. A record word is free (null) or names a
     view. Acquire loads `current`, claims a record by a seq_cst compare-exchange from null
     straight to that view, starting at a thread-local hint index and probing on, and validates.
     A retry stores the new view into the record it already owns. Release is one release store of
     null, from any thread.
   - What that gives: no mutex, no parking, no thread-exit or lifetime rules, and any number of
     registries. `too_many_readers` then means H really are in use. The compare-exchange is the
     same full barrier the seq_cst store already is (`lock cmpxchg` against `xchg`; `casal`
     against `stlr`), on a line this thread usually just released. The owner's scan is unchanged.
   - D10's row and T3 measure that instead.

4. **SR-D4. The step-1 tests as stated cannot catch a memory-ordering bug.**
   - T1 relies on TSan (`:307`), and D5 picks seq_cst over fences for TSan's sake (`:170-171`).
     But TSan does not reorder stores. `hs_tsan_weak` (release store, acquire load) runs clean,
     and so does `hs_asan_weak`. Natively, the same weakening yields 30 free-while-read outcomes
     in 2,000,000 on x86_64 (`litmus.out`).
   - On arm64 the weakening is invisible on Linux and visible on Apple silicon. `codegen.c`:
     release/acquire compiles to `stlr`/`ldar` for generic arm64, which happens to forbid the
     reorder, and to `stlr`/`ldapr` for `-mcpu=apple-m1`, which allows it. So a weakened build
     passes Linux arm64 CI and fails only on Apple silicon or x86_64.
   - `hs_asan_novalidate` also passes. Only TSan caught the skipped validation, so ASan runs
     alone prove little here.
   - Required for step 1:
     - a native (unsanitized) litmus test of `hazard.hh`'s own claim, protect, and validate
       against publish and scan, at least 10^7 iterations on x86_64 in CI (the probe saw about
       1.5 × 10^-5 per iteration), with a weakened-order mutant that must fail it;
     - T2(a)'s seeded schedule split acquire into its atomic steps (load, protect, validate) and
       the owner into publish, retire, scan, free, so its 1,000 seeds explore the protocol's
       sequentially consistent interleavings. That is the model check this design needs; a C11
       model checker is optional.
   - T2's mutant list gains: weakened order (killed by the litmus); the scan before the publish
     store; the bitmap clone skipped on the rollback path (`index.hh:1997`); and SR-D1's
     predicate removed. Each mutant names the test that kills it.

5. **SR-D5. Merge sources can be unmapped on a pool thread today (not introduced, but D4 and D6
   rest on it).**
   - `schedule()` posts a closure that captures the `std::shared_ptr<merge_job>` (`index.hh:1438`).
     The pool's wrapper keeps that closure until after it returns (`pool_executor.hh:66-72`).
   - `publish_merges()` drops the owner's copy as soon as `done` reads true (`index.hh:1704-1709`),
     so the pool thread's closure can hold the last reference. Destroying `merge_job` there
     releases `sources` (`:1279`): a source the install removed from `segs_` is then destroyed,
     and unmapped, off the owner thread.
   - That contradicts D4 ("touched by the owner thread alone", `shared-reader.md:134-136`), D6
     ("unmapped there, on the owner thread", `:210-211`), and ARCHITECTURE's ownership rule
     (`:188-192`). It is harmless for safety (the merge no longer reads, and the count is atomic),
     but it breaks the "one owner unmaps" half of R7.
   - Fix in step 1: the owner clears `job->sources` (and any frozen bits a job later shares) when
     it installs, discards, or abandons a job. A debug assertion checks that an `open_segment`
     is destroyed on the owner thread.

6. **SR-D6. Commit's order decides whether an idle handle delays reclamation.**
   - D1 publishes "at the end of commit()" (`:86`), while D6 frees "at commit() before retain()"
     (`:209`). If commit publishes after `retain()`, the view this commit retires still holds the
     sources of merges it installed, so `retain()` skips them, and they are queued one commit
     late even with no reader. That is a disk regression for every index with a handle, against
     phase-3's retention measurement (`phase-3.md:159-165`).
   - Required: inside `commit()`, publish, then scan and free, then `retain()`, then `reclaim()`.
     A test shows that with a handle and no guards, the retention tests' queue timing matches
     today's.
   - Separately, freeing a view at `reclaim()` or `close()` queues nothing, because only
     `commit()` and `install()` run `retain()` (`index.hh:630, 943`). An idle index keeps the
     released files until its next commit or reopen. Either freeing a view runs the data-file
     half of `retain()`, or D6 states the delay.
   - T1's no-SIGBUS oracle is faultfs's abort on a truncate below a live mapping
     (`faultfs.hh:341-344`), which needs faultfs with mapping on, plus one run on the POSIX IO.

7. **SR-D7. A reader that never releases: the count is bounded, the bytes are not, and
   the blast radius is the node.**
   - Retired views per index stay at or below H after a scan (the probe's assertion held). But one
     guard pins a whole generation: a `force_merge` under it doubles the index's disk until
     release, and storage admission then refuses writes with `no_space`. That is safe (never a
     SIGBUS), and the design says so in Risks (`:370-373`).
   - The registry is node-wide, so a leak of guards on one index (a coroutine frame never destroyed)
     sheds reads on every index of the node.
   - Required test: a guard held while the writer merges, commits, and reclaims for many rounds.
     It checks that retired views stay at or below H, that no held file is truncated, that
     `no_space` comes with a short budget, and that space returns at the first commit after release.
   - Required observability: per index, the views held and their bytes; per registry, the
     high-water mark and each record's claim time, so a host can find a leak.

8. **SR-D8. The scan's cost and H's size at the many-indexes scale are not stated.**
   - D9 sizes H by "the query concurrency ARCHITECTURE already caps per core" (`:271-273`), but
     ARCHITECTURE caps only demoted tasks (`:179-180`). A Xapiand query also holds one guard per
     local shard it searches (`xapiand-on-strata.md:195-196`).
   - The owner's scan reads all H records at every publication (`:160-161`). Node-wide, that is
     publications per second times H line loads, on lines readers write. For example, 10^4 open
     indexes refreshing at 1 Hz with H = 4,096 is 4 × 10^7 loads per second. D10 and T3 cost one
     index (`:281, 309`).
   - Required:
     - T3 adds an aggregate row, many indexes publishing at the host cadence with H at the
       Xapiand-sized value.
     - D4 recommends one registry per owner thread (which `index_options::readers` already
       allows), so each owner scans only the records of the indexes it owns, and a leak stays on
       one core.
     - A refresh that flushed nothing and installed no merge publishes nothing.

9. **SR-D9. D2 does not contradict xapiand-on-strata D7; it requires the seam to refresh before a
   search (ruling).**
   - D7 already says strata "calls `refresh()` ... before a search when the shard has unpublished
     writes" (`xapiand-on-strata.md:192-195`). Under D13 that means: the search task, on a pool
     thread, compares the view's position with the last acknowledged position. When the view is
     behind, it posts a refresh to the owner, awaits it, and only then acquires. The task's
     completion orders the publication before the acquire, so the acquire sees that view or a
     newer one.
   - What D2 adds is cost. Without coalescing, interleaved indexing and search flush once per
     search: T7's shape (1,103 docs/s, 4 searchers) would produce a tiny segment per query.
   - Required:
     - D9 states the pattern: wait for a publication at or past a position, coalesced so one
       owner refresh serves every search waiting on it.
     - The handle exposes the published position and generation without an acquire.
     - Linearizable get-by-id (`workload-profiles.md:7`) stays on the owner, as D2 already says.
   - For the lead, to xapiand-on-strata: D7 names this mechanism and gains a cost row (refreshes
     and segments per second under the 7a mixed load). T5 adds a refresh-on-demand variant beside
     the 1 s cadence, so the visibility asymmetry the row states (`:311`) also has a price.

10. **SR-D10. `heal()` and the fields views must not read.**
    - D11 says views keep the old objects, "whose bytes passed the same checks" (`:297-298`). That
      is the opposite of what happens: `heal()` replaces only a segment that failed
      `verify_all()` or its consistency check (`index.hh:711-745`). Readers on older views keep
      the failing object and meet `corrupt_segment`, a typed error on memory-safe decoders, until
      they release. That is acceptable, once the design says so.
    - The owner also writes `open_segment` fields after publication: `damaged` (`:726, 1749`);
      `unsynced`, `ref.live*`, and `deletes_dirty` at commit (`:2521-2527`); `deletes_dirty` on
      rollback and delete (`:1999, 2030`).
    - Required: D1 says a view dereferences an `open_segment` only for `seg` and its mapping, and
      copies `id` and the dead count at publication. T1 under TSan then catches a violation.

11. **SR-D11. Q2's recommendation (a) blocks the owner thread and can deadlock (for the owner's
    decision).**
    - Waiting in `close()` blocks the owner's thread, which ARCHITECTURE forbids for a host
      reactor (`:6`).
    - It deadlocks whenever a guard's release needs that thread to run, such as a task holding a
      guard while suspended on the owner's own executor. The debug check by "the guard's claiming
      thread" (`:333`) does not see that case.
    - Recommendation to the owner, beside (a) to (c): a non-blocking close. `close()` stops
      acquires, frees what no record names, keeps the lock and the held files while views remain,
      and reports "closing". A later owner call, or `try_finish_close()`, completes it, and a
      deadline falls back to (b). SR-D2's destructor rule then requires a finished close.

12. **SR-D12. Cost claims to restate.**
    - T4 promises an index without readers "no allocation and at most 8 bytes over today"
      (`:310`), but D3 changes every index's `deleted` (`index.hh:1263`) into a shared
      copy-on-write bitmap, a reference count per segment for readerless indexes too. Keep the
      allocation count equal (header and words in one block) and state the bytes per segment.
    - Handle copies are a shared read-modify-write (`:129-130`), so the seam keeps one handle per
      shard and lends it by reference.
    - T3's "flat at 16 threads" runs at most one thread per physical core, and it gains a
      cross-thread-release arm (SR-D3).
    - Copy-on-write by `use_count()` is exact only while the owner thread alone copies and drops
      the counts (SR-D5). The debug check covers that too.

13. **SR-D13. R7 ruling: the change from per-core handles to hazard records is accepted.**
    - D5 and D6 meet R7 (`reviews/phase-3-design.md:707-717`). One owner unmaps (once SR-D5 is
      fixed), and it learns by its own scan. No refcount is shared on a read path: acquire and
      release write only the reader's record. No collector or timer runs.
    - The deferred work is bounded as ARCHITECTURE requires (`:188-192`): at most H retired views
      per index, O(H) per scan, on the owner thread.
    - D5's reasons against message handles hold (`:200-204`): Xapiand's offload threads and the
      benchmarks' threads have no message loop.
    - This also answers rocksdb-survey R1 (`reviews/rocksdb-survey.md:43-56`) once step 1 lands.
      The lead then edits `phase-3.md:178-185` and `ARCHITECTURE.md:156`.

14. **SR-D14. The ledger has no done criteria** (`docs/status/shared-reader.md:8-9`). The lead
    writes them before step 1; the proposed set is under Conditions (C1).

## Blocking items / Required changes

Revision 1 resolves these before the code each shapes; a scoped architect re-check (high) covers
their text only.

- **SR-D1**, before publication (step 3): one "publishable" predicate at every publication point;
  `reader()` and `heal()` obey it; T2 exercises both with a pending replace.
- **SR-D2**, before views and handles (step 3): the destructor and move-assignment follow `close()`'s
  rule; a protected view at destruction aborts in debug and leaks in release, never freed.
- **SR-D3**, before the registry (step 1, item 1): records claimed by compare-exchange from null
  (or another design that has no parking and no mutex on cross-thread release), with `parking` and
  `drift` as its tests.

Step 1's other items (D3's single accessor for `deleted`, D8's factoring of the segment reads, the
comment at `index.hh:1003-1006`) may start now.

## Conditions

- **C1. Done criteria** (for the lead to write; each with its evidence):
  - K1: `hazard.hh` with the litmus (SR-D4), `parking` and `drift`, and the claim and release tests,
    clean under TSan, ASan, and UBSan;
  - K2: T1 at its stated counts on Linux x86_64, macOS, and FreeBSD, plus the stuck-reader test
    (SR-D7);
  - K3: T2 (a) and (b) with every mutant of T2 and SR-D4 killed by a named test;
  - K4: `install()`, `heal()`, `close()`, destruction, and move-assignment under live guards
    (SR-D1, SR-D2, SR-D10);
  - K5: retention timing unchanged with an idle handle, and no truncation under a mapping, on
    faultfs with mapping on and on the POSIX IO (SR-D6);
  - K6: T4's allocation test against the restated target (SR-D12);
  - K7: the owner-thread destruction assertion for `open_segment` (SR-D5);
  - K8: CI green on every platform.
- **C2.** The lead gets SR-D9's text to xapiand-on-strata (D7's mechanism and cost row) before its
  S-step that first searches a strata shard from a pool thread.
- **C3.** T3 gains the aggregate scan row and the per-owner-registry configuration (SR-D8) before
  the lead measures it; Xapiand's H is sized from its shard fan-out, not from demoted tasks.
- **C4.** The owner answers Q2 with SR-D11 in view; the destructor rule (SR-D2) follows the answer.
- **C5.** On landing, the lead edits `phase-3.md:178-185` and `ARCHITECTURE.md:156` per SR-D13 and
  marks rocksdb-survey R1 closed.

Not run: strata build and tests, arm64 hardware (codegen only), Linux, FreeBSD, any benchmark or
timing, a C11 model checker, prospect.

Probes kept in `.scratch/review/`: `sb_litmus.cc`, `litmus.out`, `hazard_stress.cc`, `build.sh`,
`run.sh`, `runs.out`, `codegen.c`. Rerun with `build.sh` then `run.sh` under a light hold.
