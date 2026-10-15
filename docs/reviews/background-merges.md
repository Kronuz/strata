# strata review: background, rate-limited merges

- Date: 2026-10-15.
- Reviewed: branch `phase3/background-merges` at `8c077cc` (worktree `strata-wt/bgmerge`):
  - `include/strata/merge_scheduler.hh` (new);
  - `include/strata/index.hh`;
  - `test/background_merge_test.cc`;
  - `docs/design/phase-3.md`.
- Reviewed for: my Phase 3 lifecycle review's condition 7, and the RocksDB survey
  review's K4 ruling (background merges with a per-node rate limit, before Phase 3's
  ratchet rows are recorded).
- Checked against: `phase-3.md` (D11, D17, D18, T1, T5, Scale); ARCHITECTURE
  ("Concurrency and async", the scheduler concept); PLAN Phase 1 (`task`) and Goal 5;
  `completion/include/completion/io.h`; `faultfs.hh`, `budget.hh`, `residency.hh`;
  `Kronuz/task` `f64b836`, `Kronuz/threadpool` `2467e50`, `Kronuz/scheduler` `01e9d8b`.
- Checked by reading. Nothing was built or run. CI run 38768069359 was in progress; its
  result is not part of this review.

## Verdict

**APPROVE WITH CONDITIONS.** The index side is correct, and I approve it as written:

- the job model;
- shared, immutable sources;
- the copy of each source's deletes;
- publication by object identity;
- the exactly-once carry-over;
- discard and reclaim;
- `queue_` placed first and ended first.

`merge_scheduler.hh` is not approved in its present form. It gives the engine its own
threads, which ARCHITECTURE rules out. It cannot run deterministically, so T1's
crash-point enumeration cannot cover it. And it duplicates what the catalog's `task`
library already defines (R1, R2).

Three further defects:
- the merge output is held whole in memory, uncharged, on every worker (R3);
- the build phase has no cancellation point, so `install`, `close`, and destruction can
  block for a whole merge (R4);
- an invariant violation during carry-over leaves the index half-changed and unfenced
  (R6).

Condition 7 and K4 stay open until R1–R6 land.

## What holds (checked)

1. **Lifetime.**
   - A task captures `job` (a `shared_ptr`), `io`, `sched`, and the key prefix by
     value. It never captures the index (`index.hh` `schedule`).
   - Sources are `shared_ptr<open_segment>`, so a segment that leaves `segs_` (heal,
     install, commit's all-deleted erase) stays alive until the job releases it.
   - The worker destroys the closure before it decrements `outstanding`
     (`merge_scheduler.hh:154-156`), so `cancel()` returning means no task still holds a
     source.
   - `queue_` is declared first and reset first in `~index()`. Its move assignment
     cancels the destination's lane before `segs_` and `pending_` are replaced. A moved
     index's running tasks continue against jobs the new owner holds, and the moved-from
     index ends nothing.
2. **Data access.**
   - The merge thread reads `job.sources[k]->seg` and `job.deleted[k]` only, never a
     source's live `deleted`, `dead`, or `unsynced`.
   - Region verification and the lengths state are atomics per region
     (`residency.hh:156-238`), so concurrent reads of one segment by the writer, search,
     and a merge are safe.
   - `job.done` is a release/acquire pair, and the writer touches a job's fields only
     after `done` or after `cancel()` returns.
   - `byte_budget` is atomic (`budget.hh:89-90`), and `faultfs` serializes behind one
     mutex (`faultfs.hh:247`).
3. **Lock order.** The scheduler's `mu_` comes before `rate_mu_` (`cancel`), `throttle`
   takes only `rate_mu_`, and `work` takes only `mu_`. `stop` is stored under `rate_mu_`
   and read in the wait's predicate, so a waiter cannot miss it. No index lock exists,
   and the budget's calls never block. I find no cycle.
4. **Exactly-once carry-over.** The bitmaps are copied on the writer thread at
   `schedule`, atomically with respect to stage 1. Deletes after the copy set bits in
   the sources, which stay in `segs_` and keep answering stage-1 lookups until
   publication. `install_merge` maps `now & ~then` through `plan.ordinals` once, then
   erases the sources, so no bit can be carried twice. Each case:
   - **Double delete:** cannot set a bit twice, since stage 1 removes only live copies.
   - **Replace of a key whose old copy is in a merge input:** sets the old copy's bit in
     the source, which carries over, while the new copy is in the buffer.
   - **A delete of a document the merge dropped:** cannot happen, and `logic_error`
     guards it (but see R6).
   - **An output whose documents all die after the merge:** reaches `dead == doc_count`
     and leaves at the next commit.
5. **Determinism.** Background merges change which physical segments exist and when.
   They do not change verdicts, docids, versions, revision, or logical content, so the
   digest is independent of merge timing. The inline-twin tests check exactly that,
   under seeds and two threads with two indexes.
6. **Storage.**
   - A job reserves its estimate on a permit of its own and grows it on the worker
     thread.
   - At publication the output's file is tracked before the job's reservation settles,
     so no window undercounts.
   - Discarded and abandoned outputs are tracked, then queued for reclamation.
   - The ENOSPC path removes the partial file and syncs the directory before reporting.
7. **Policy.** Segments under merge are excluded from candidates. An explicit `merge()`
   of a segment under background merge consumes it, and the background job is then
   discarded. Correct, if wasteful.

## Required changes

1. **R1. Execution goes through the scheduler concept; the catalog supplies the pool.**
   ARCHITECTURE says the engine "defines `post(handle)` and `now()`. The host's per-core
   executor is the run queue; the engine owns none". It names "a deterministic inline
   scheduler for tests and simulation". `merge_scheduler` creates `std::thread`s inside
   strata.
   - **Concept.** strata states what it needs from a background executor:
     - post work;
     - a cancellation group per index, with cancel-and-wait;
     - a clock.
     `Kronuz/task` already defines the scheduler concept (`post`, `now`), a
     `cancel_token`, and `inline_scheduler` for deterministic runs (`task/README.md`).
     Merges use them: one `cancel_token` per index, and its running tasks counted for
     the wait.
   - **The thread pool lives in the catalog.** The default threaded executor for tools,
     tests, and hosts without their own is `Kronuz/threadpool`. It is extended with an
     owner-scoped task group (cancel queued tasks, then wait for running ones). That
     primitive is generic, and Xapiand's indexers need it too.
     - Not `Kronuz/scheduler`: it is a timer wheel over `stash`, and its slot cleanup on
       a timer is the deferred reclamation Goal 5 forbids in strata.
     - `threadpool`'s 100 ms timed dequeue is acceptable for one pool per node. Its
       extension lands with its own CI, and the `cpp-libraries` catalog is updated.
   - **The rate limiter is one mechanism and stays in strata.** No catalog library has
     one (I searched the Kronuz tree), and the Detent stack has none either. Phase 3
     already names three users:
     - merges;
     - `install` transfers, "rate-limited like merges" (`phase-3.md:374`);
     - off-core reclamation (P19).
     So it is one strata component, clocked by the executor's `now()` so it is
     deterministic under `inline_scheduler`, and shared by all three. If Detent's
     snapshot transfer later wants it, it moves to a library then.
   - **Check:** `background_merge_test` runs unchanged on the threaded executor and on
     `inline_scheduler`. Under TSan, threaded only.

2. **R2. T1's crash enumeration covers background merges** (open question: required,
   not optional).
   - The random-schedule test cuts power once per seed, at the end. T1's standard is
     every mutation of a randomized workload, at least 10,000 points
     (`phase-3.md`, T1).
   - Background merges add states T1 has never seen:
     - a merge's output file, complete or partial, that no manifest names;
     - an output published with carried-over deletes, then committed with its first
       live-docs generation;
     - a discard whose reclamation is queued;
     - the merge thread's directory sync landing between the writer's rename and its own
       directory sync.
   - The lifecycle gate report said T1 ran with "merges on the writer's thread, so no
     delete arrives during one". That premise is gone.
   - Required: `lifecycle_test`'s T1 workloads gain background merges on the
     deterministic executor (R1). Queued merge tasks run at seeded points between
     transactions, and partially: the write loop yields per chunk, so a crash can fall
     inside a merge's write. Deletes and replaces land between `schedule` and
     publication. Crash points cover every mutation of both the writer and the merge
     task. The 10,000-point minimum and the second crash during recovery apply as before.

3. **R3. Merge memory is charged, and the output streams to its file** (D17).
   - `run_merge` builds the whole output in a `std::string` with a `string_spill` for
     positions, and only then writes it through the throttle. The policy's maximum
     merged size is 5 GiB (`merge_policy.hh:21`). With two worker threads across a
     node's indexes, that is up to 10 GiB of uncharged memory on a 16 GB gate VM.
   - D17 names "merge buffers" among the charged categories (`phase-3.md:301-303`).
     Inline merges had the same uncharged buffer. Concurrency multiplies it, and the
     flush-charge commit (`422c227`) did not cover merges.
   - Required:
     - the merge writes through a file output, in chunks admitted by the throttle, with
       positions spilled to a file;
     - its remaining working memory (decode buffers, the term union's per-term vectors,
       the stored-record rebuild) is charged to the indexing budget on a permit of the
       job's own;
     - unlike a flush, a merge on a worker may wait for budget room.
   - **Check:** the indexing memory row under background merges, and a test whose merge
     waits for budget while flushes continue.

4. **R4. A cancellation point in the build** (`run_merge`).
   - `stop` is observed only before the merge starts and between write chunks.
     `merge_segments` (the term union and stored rebuild) runs to completion, which takes
     minutes for a multi-GiB merge.
   - `install` (Phase 3R's catch-up path), `close`, and destruction all wait for it.
   - The close and destruction test uses a slow rate limit, so the stop lands in the
     write loop and the test does not see this.
   - Required: `merge_segments` takes the stop flag (or the `cancel_token` of R1) and
     checks it per term and per stored frame. `install`'s worst-case wait becomes one
     term's or one frame's work.
   - **Check:** a gated merge stopped during its term union, timed.

5. **R5. Concurrent IO is a stated contract.** The merge task calls `create_exclusive`,
   `File::write_at`, `remove`, and `sync_directory` while the writer runs commits on the
   same `kronuz::journal::IO`. `completion/io.h` (`IO`, `103-150`) states no thread
   safety, and it notes that a quiescent inventory requires "no concurrent mutations".
   - The design states what strata relies on: these four calls are safe concurrently on
     distinct names. A directory sync from either thread only makes earlier namespace
     changes durable sooner, which no protocol step forbids.
   - This joins strata's consumer registration for `completion/io.h` with the Detent work
     (D20), since it is a new use.
   - `open`'s directory scan runs before any merge is scheduled, which the code already
     guarantees. The design says so.

6. **R6. Failures at publication.**
   - `install_merge` throws `logic_error` in the middle of carry-over
     (`a delete names a document the merge dropped`), after setting some of the output's
     bits. Nothing is fenced, and the sources are still in `segs_`. An invariant
     violation fences the index.
   - Any error at publication that is neither `space_error` nor `system_error` (a
     `corrupt_segment` from a damaged source, a `logic_error` from `merge_segments`)
     propagates with the job already removed. The policy then reschedules the same
     sources at every refresh. This matches today's inline behavior, but in the
     background it repeats silently.
   - Required:
     - a damaged source is reported through `heal()`'s damaged list, and the policy
       skips it until healed;
     - invariant violations fence.
   - **Check:** a merge over a segment with a damaged postings region, and one with a
     forced invariant violation.

7. **R7. Scale and fairness** (`phase-3.md`, Scale: merges "scheduled across indexes
   from one per-node budget").
   - The queue is one FIFO across lanes. One index's `force_merge` waits behind every
     other index's queued merges, and at 1,000 indexes per node that is unbounded in
     their count.
   - Required:
     - round-robin across lanes, or the stated reason FIFO suffices;
     - the per-node statement: worker count, rate, and queue depth at 1,000 indexes.
   - Two smaller fixes in the rate limiter:
     - a task that `throttle` cancels leaves its chunk reserved in `next_`, delaying
       every other task. The reservation is refunded;
     - `shut_down` does not stop running tasks, so it waits out their rate-limited
       writes. Shutdown sets every lane's `stop`.

## Rulings on the accepted gaps

1. **An output left unpublished by `close`, move assignment, or destruction** is an
   orphan file that is out of the storage budget until the next open removes it.
   - Accepted. A closed index's files are outside the node budget in the existing model:
     its permit is released at close, and reopening recharges what it finds. This is
     the same rule.
   - `phase-3.md`'s T5 states it. The bound is the merges in flight per index, at most
     the worker count times the maximum merged size.
2. **A partial file whose removal failed.** The removal's failure is a `system_error`
   from the merge thread, so publication fences the index (`publish_merges`), and
   reopening removes the file as an orphan.
   - Accepted, as fail-closed. The design notes that the uncounted bytes last only until
     that reopen.
3. **A merge discarded because one source was erased as all-deleted** throws away the
   other sources' merged work (`install_merge`, identity check).
   - Correct; not required. A source that left only through commit's all-deleted erase
     could be treated as present with every ordinal dead. That is a measured
     improvement, if write-amplification rows show it matters.

## Library reuse

**Do not keep a new pool in strata; extend the catalog** (R1).

- The agent is right that neither `threadpool` nor `scheduler` offers per-owner
  cancel-and-wait or rate limiting today. The rule is to extend, not to duplicate.
- Cancel-and-wait for an owner's tasks is a generic pool feature: `threadpool` gains it.
- The deterministic, concept-based side already exists in `task`.
- Only the rate limiter has no home in the catalog. It stays in strata as the one
  limiter that merges, install, and reclamation share.

## Which work starts when

| Item | Starts | Waits for |
|---|---|---|
| `index.hh`'s job model, carry-over, publication, discard | Approved as written | R3, R4, and R6 inside it |
| `threadpool` owner-scoped task group | Now, in its own repository with CI | — |
| Executor concept over `task`, the shared rate limiter | Now | The `threadpool` extension for the threaded default |
| `lifecycle_test` with background merges (R2) | After R1's deterministic executor | — |
| Merging to `main` | — | R1–R6, CI green on every platform including TSan, and R2's crash points |
| Condition 7 and K4 closed | — | The above; then Phase 3's ratchet rows are recorded on this writer |

## Revision check 1

- Date: 2026-10-15.
- Reviewed: `b9bff3c` (R1, R3–R7), `447ebb2` (R2), and `d5a94ee` (publication in segment
  id order) on `phase3/background-merges`, plus `Kronuz/threadpool` `fdc0a1e`
  (`TaskGroup`).
- CI run 38778329828 is reported green on all eight jobs, TSan and ASan/UBSan included.
  I did not see it.
- Checked by reading. Nothing was built or run.

### Verdict

**APPROVE WITH CONDITIONS.** R1–R7 are closed as specified, with one exception. The
order check that `d5a94ee` adds to `commit_files` throws `std::logic_error`, and nothing
on the commit path fences on it (S1). That is the one change required before merging to
`main`. The 10,000-point run may follow the merge, and the K4 ruling fixes when it is
due (S2).

### Status

- **R1. Closed.**
  - The engine owns no threads: `executor` (`executor.hh:21-46`) is the concept, with a
    group per index, `cancel()` that waits for running work, and `now()`.
  - `inline_executor` sits on `task::inline_scheduler` with its manual clock.
  - `pool_executor` (`pool_executor.hh`) adapts `Kronuz/threadpool`'s new `TaskGroup`,
    which landed in the catalog with its own CI.
  - The one `rate_limiter` (`rate_limiter.hh`) is clocked by the executor's `now()`,
    refunds a cancelled reservation (`34-37`), and is named for install transfers and
    reclamation too (`phase-3.md`).
- **R2. Closed in design and CI.** `stepping_executor` passes a baton, so exactly one of
  the writer and one piece of merge work runs at a time, and a run replays. Pieces end at
  each 4 KiB chunk and each limiter wait. Crash points cover every mutation of both
  sides, with deletes and replaces landing between scheduling and publication. A dry
  run's commit points must equal an inline twin's. CI runs 2,000 points; the 10,000-point
  result is owed (S2). The pass has already paid for itself: it found `d5a94ee`'s bug.
- **R3. Closed.**
  - The output streams through `merge_file` in throttled chunks, with positions spilled
    to `seg-<id>.spill`.
  - Working memory is charged to a `flush_charge` on the job.
  - A merge waits up to `merge_memory_wait` for room, then draws past it, as a flush
    does. That bound is acceptable because drawing past raises pressure, which suspends
    stage 1.
  - The output's bytes, read back into the `open_segment` (`run_merge`), stay charged
    until publication. Holding segment bytes in memory is the index's existing model, not
    something this change adds.
- **R4. Closed.** `merge_segments` takes the stop token and the charge, checking per
  document, term, and stored frame.
- **R5. Closed.** `phase-3.md` states the concurrent-IO contract in D20's registration:
  the calls, distinct names, the directory sync, and the open scan before any merge.
- **R6. Closed for publication.**
  - In `publish_merges`, a stop is dropped, `space_error` retries later, a corrupt source
    is marked `damaged` and skipped by `schedule`, and anything else fences.
  - `install_merge` checks every carried delete before it changes anything, and fences
    with the output reclaimed and both charges settled.
  - Open: the new commit check (S1).
- **R7. Closed.**
  - `TaskGroup` serves groups round-robin through tickets, with at most one merge per
    index (`per_group` 1).
  - `phase-3.md` states the per-node bounds: W threads, rate R, and one queued merge per
    segment.
  - Shutdown requests every group's stop (`pool_executor.hh:41-47`).
  - T5 states the orphan bound.
- **`d5a94ee`. Correct.**
  - A merge takes its id at scheduling, so a flush published meanwhile holds a higher
    id. Inserting the output at its id's position keeps `segs_` in manifest order.
  - Key lookups do not depend on that order, since one live copy per key holds. Its new
    meaning ("by id") is the right invariant for `segs_`.

### Required

1. **S1. The commit's order check fences.** `commit()` runs
   `guarded([&] { commit_files(); })` (`index.hh:427`). `guarded` fences only on
   `std::system_error` (`845-854`), so the `std::logic_error` thrown at `1706` escapes
   unfenced.
   - The index then keeps applying transactions that every later commit refuses again.
     Nothing is lost, since nothing commits, but it is the state R6 forbids.
   - Required: the check calls `fence(...)` (`publish_merges`' helper) instead of
     throwing `logic_error`. Or `guarded` also fences on `std::logic_error`, the rule for
     every invariant check on a write path.
   - `commit_files` has two other callers: `install`'s guarded block (`index.hh:722`)
     and recovery's re-commit of an older head (`1968`), which is outside `guarded`.
     Putting `fence(...)` inside the check itself covers all three, so that is the form
     to use. Widening `guarded` would cover only the first two.
   - **Check:** a test forces the order regression (a test hook, or a hand-ordered
     `segs_`) and asserts `index_fenced` and that no manifest was written.

### Rulings

2. **S2. The 10,000-point run may follow the merge to `main`, and must precede recording
   Phase 3's ratchet rows.**
   - CI's 2,000 points under sanitizers, plus TSan and the inline-twin tests, are enough
     to merge.
   - The 10,000 points are T1's gate standard, not a merge criterion, and the shared VM
     is busy with timed trials, so the run waits for a free host.
   - It is due before Phase 3's ratchet rows are recorded, for the K4 reason: a bug it
     finds would change the writer those rows measure.
   - The gate report records its seed range, point count, both crash counts (first loss
     and loss during recovery), and the commit.
3. **Finding.** `pool_group::cancel` swallows anything `cancel_and_wait` throws
   (`pool_executor.hh:81-84`). The only possible throw is a mutex error. If the wait did
   not happen, though, running work may outlive its index's state. The catch calls
   `std::terminate()`, since there is no safe way to continue. This is minor and lands
   with S1.

### Which code starts when

| Item | Status |
|---|---|
| Merging `phase3/background-merges` to `main` | After S1 and green CI on every platform |
| The 10,000-point `lifecycle_test` run | When the host is free; before Phase 3's ratchet rows are recorded |
| Condition 7 (lifecycle review) and K4(a) | Closed when S1 lands and the 10,000-point result is in the gate report |
