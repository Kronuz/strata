# Code review: `phase4/reclaim-bg` (reclaim's deletes on the host's executor)

Reviewed `strata:9af34f6` (pushed, `origin/phase4/reclaim-bg`, 6 commits over main `1d76408`), against
`docs/status/reclaim-bg.md` (done criteria), `docs/reviews/reclaim-backlog.md` CR3, and the `limits.md` row
"Reclaim step, quota and cap". Tier: concurrency plus file deletion.

## Verdict: APPROVE WITH CONDITIONS

The engine change is sound. I found no way to delete a live file, double count a byte, or leak a dead one
in the paths the tests do not already cover, and the lifetimes hold. Two conditions are owed before it
lands (CR1: the channel's lock is invisible to the branch's TSan evidence; CR2: close, the destructor and
`drain()` now wait where they used to cancel, so the executor's progress contract must be written down).
Everything else is a follow-up. Concurrency is a sensitive area, so CR1 and CR2 get a scoped re-check.

## Blocking (conditions for landing)

### CR1. The failure channel's lock is not covered by any test, TSan included

`delete_chan::report()` and `collect_deletes()` hold `mu` around the vectors and the `ready` count; that is
correct (details under "Checked and sound"). But remove the lock in `report()` and the branch's tests do not
notice: `reclaim_bg_test` passes three times in a row under TSan (`.scratch/mut-nolock.bg.{1,2,3}.log`, no
report, rc 0). The ledger says the same ("not flagged in one run") and treats the clean threaded run as the
check. It is not one: `threaded()` has 2 workers, 60 rounds, and 2 injected faults, so two jobs rarely
finish at the same instant.

My probe `drift` (`.scratch/probes/rbg_probe.cc`: 3 workers, 800 rounds, 400 random truncate, sync and remove
faults, before and after the effect, a reopen on every fence) kills the same mutant every run: rc 134, 75 and
63 TSan reports (`.scratch/mut-nolock.drift.{1,2}.log`). Unmutated, `drift` ran clean under TSan 4 times
and clean under ASan+UBSan. (Its `storage.used() >= fs.used()` check is not valid under write faults: the
inline path violates it more often, over 30 times against 3 on the pool, so it is not attributable to this
branch. Drop that line when adopting the probe; keep the per-round `sum_files == on_disk_`,
`sum_posted == inflight_`, the final `on_disk_ == bytes on disk`, and the reopen comparison.)

Condition: replace or extend `threaded()` with a stress at least as hard as `drift` (more workers, many
failing deletes), and show the lockless mutant killed under TSan in the ledger's mutant table.

### CR2. Close, the destructor, `drain()` and `wait_deletes()` now wait without a way out

Before this branch the executor's work was only cancelled at close, so a stopped or dead pool could not hang
an index. Now `end_deletes()` (close and the destructor) and `finish_deletes()` (`drain()`, `wait_deletes()`)
call `del_group_->wait()`, which blocks until every posted delete has run.

Probe `poolgone` (`.scratch/probes/rbg_probe.cc`): a `pool_executor` over a `ThreadPool` on which
`end()` and `join()` were called, then `commit()` and `reclaim()` on the live index. `post()` does not throw (the pool's
`enqueue` returns false inside `TaskGroup::post_ticket`, the task stays queued with no ticket), so the branch's
"an executor whose post throws leaves the file queued" path never fires, and `ix.close()` blocks forever
(the probe's watchdog exits 3 after 5 s). A host that calls `pool.end()` before closing its indexes, which is
an ordinary shutdown order, hangs where main did not. `executor.hh` and `pool_executor.hh` already say the pool
outlives the groups, but nothing says that a group's work must keep running until the group ends, and nothing
says `close()` and `drain()` now depend on it.

My answer to the open point (a pool that never runs the group stalls `drain()`): it is the host's contract,
not a warning or a timeout. A timeout cannot tell a slow pool from a dead one, and giving up would either
lose the in-flight accounting or leak the file. The fix is a statement, plus a test of the boundary:

1. State in `executor.hh` (`group::wait`) and `pool_executor.hh` that `close()`, the destructor, `drain()`
   (admission) and `wait_deletes()` wait for posted work, so the executor must keep running a group's work
   until the group ends; ending the pool first hangs them.
2. Same sentence in the `limits.md` row and the ledger.
3. A host that runs an index's owner on the only worker of the pool its deletes need deadlocks in `drain()` the
   same way `wait_merges()` already does. Say so in the same place. (I did not probe it: the owner lease is
   thread-bound; see "Not run".)

No code change is needed for this condition.

## Non-blocking (follow-ups; no second review)

### CR3. Two paths are killed only by accident or not at all

Mutants run against the branch (`.scratch/mut.sh`, logs `.scratch/mut-*.log`, TSan Debug -O1 build):

| mutant | `reclaim_bg_test` | `reclaim_test bg` |
|---|---|---|
| `drain()` early return without `!inflight_` | fails, only in `dropped()` through `peek::drain` | passes |
| `reclaim_posted` ignores `bytes` (cap) | fails (`quota`, drain preconditions) | passes |
| ENOENT from a delete reported as a failure | passes | passes |
| channel lock removed | passes (CR1) | not run |

1. The first is real code with a real consequence: admission with everything already posted and nothing queued
   must wait for the in-flight bytes, not answer "nothing to free". `drain_waits()` only covers the queued
   case. My probe `drainposted` (admission on a tight budget with the file posted, not run) fails under the
   mutant and passes on the branch; adopt it.
2. The ENOENT branch in `run_delete` (a remove that took effect and then failed, `faultfs::fault.after = true`,
   leaves the file gone but reported failed; the retry must treat ENOENT as done, not requeue forever at the
   head of the queue) has no test. Add one with `{remove, 1, EIO, after=true}`.

### CR4. Posted deletes are invisible to the queue the limits row bounds, and nothing bounds them without a budget

`reclaim_posted` never blocks, files leave `reclaim_pending()` when posted, and `drain()` runs only when an
admission is short of space. The `limits.md` sentence "the queue stays bounded while the merges obsolete no
more than the cap on average" is therefore a statement about the posting rate, not about dead bytes on disk. If
the shared pool serves deletes slower than the owner posts them (many indexes on few workers: each group runs
at most one job at a time, round-robin), the dead set grows with no backpressure unless a storage budget makes
admission drain. I did not measure it (structural reading; benchmarks are outside this review); the A/B
measures disk after settle, not the peak. Ask: say this in the row, expose in-flight bytes next to
`reclaim_pending()` so a host can see it, and have the architect decide whether posting should stop at some
multiple of the cap in flight (leaving the rest queued as debt, where the existing bound applies).

On the open point "one publish posts the cap plus at most one whole file": it is a load-bearing limit in the
sense that matters (the test `quota` asserts `cap <= inflight <= cap + largest`, and the cap-ignored mutant
fails it), and the row states it honestly. What it bounds changed: it was the owner's syscalls per publish,
it is now the bytes handed to the pool per publish. A 5 GiB merged file is one job of about 320 truncates on
one worker, without a yield between them, though `stop` is checked each step. Acceptable; the row should
say "one job can occupy a worker for as long as the file's truncation takes".

### CR5. The A/B's raw log is not committed

The ledger cites 8 interleaved rounds: main 10,619 ops/s mean, branch 12,319 (1.16x main), get digest
`a46c3543d251394d`. I checked the developer's untracked log (`strata-wt/reclaim-bg/.scratch/ab/ab.log`): the
numbers match (mean main 10,618.5, branch 12,319 ops/s; every round faster; same disk and digest in all 16
runs). The calibration rule is that a cited baseline has its raw result committed. Put `ab.log` and `ab.sh`
under `docs/perf/` and cite the path. (I did not rerun it: benchmarks are outside a review.)

### CR6. `reclaim_test bg` is a regression net, not evidence

It kills none of the mutants above either (rc 0 under the three it ran against, including cap-ignored), and it takes 102 s
under TSan with the default 400 points per workload (3,871 points, 1,707 truncated orphans). It earns its
place only as the broad power-loss sweep over the posted path: it exercises mid-delete cuts that
`crash_mid_delete` (100 points) samples thinly. Keep it, say in the ledger that it is a net and not a mutant
kill, and run it at a lower `STRATA_CRASH_POINTS` in the sanitizer jobs.

### CR7. Smaller notes

1. `wait_deletes()` collects but does not sync the directory, so after it `storage.used()` still counts the
   removed bytes in `unlinked_` until the next commit or `drain()`. Say so in its comment (a host's settle
   that reads the permit will see them).
2. `open()` returning before its orphans are gone is fine (the orphans are tracked in `files_`, counted in the
   permit, and the lock is held until close waits). Document that a host copying the directory right after
   `open()` sees truncated files that no manifest names; and that `open` posts at most `reclaim_slice` of them
   (unchanged from main, the rest wait in the queue).
3. `wait_deletes()` as public API: acceptable, named like `wait_merges()`. It needs the owner lease, like
   `reclaim()`; its comment says "waits and collects"; add "owner thread".
4. After `close()` with views still held (the closing state), `inflight_` and `posted_` are not collected
   again, so the permit stays conservatively high until `try_finish_close()`. Harmless; no change needed.
5. `run_delete` leaves `left` at the tracked size if `f->size()` throws after a truncate that took effect;
   the retry would then truncate "up" to `left - step`. Same shape as the inline path; rare; no change.

## Checked and sound

1. **Referenced files.** `retain()` (queue time) is the only gate, and it is unchanged: it keeps files of
   kept manifests, `segs_`, pending merges, and every view. A posted file is out of `files_`, so no later
   `retain()` sees it; nothing can reference it after the post because ids and live-docs counters are never
   reused (`recover` starts them above every name on disk, `m_.next_segment++` otherwise). `open()`'s orphans
   are tracked before `retain()` queues them. Probes: `view` and `viewpool` hold a reader's view (mapped
   faultfs aborts on a truncation below a live mapping) across 60 rounds of merges, commits and posted
   deletes, on an inline and on a 2-worker pool executor: clean, and the old segments read correctly.
2. **Failure requeue.** `left` only decreases after a truncate succeeds, so a requeued size is never below the
   file; the failure of the truncation itself re-reads `f->size()`. `requeue_delete` puts the name back at the
   front, restores `files_`/`on_disk_`, and adds `left` to `debt_`; a failed post nets
   `debt_ = D - posted + size`, which is right. Tests `failed` x3 cover truncate, sync and remove; `drift`
   covers many.
3. **Accounting.** `inflight_ == sum(posted_)` and `sum(files_) == on_disk_` hold after every round in `drift`
   (800 rounds, 400 faults, 3 workers, repeated fences and reopens); at the end `on_disk_` equals the bytes
   on disk, nothing is pending, and `storage.used()` before close equals a fresh reopen's (1,545 both). A name
   cannot be posted twice: it enters `posted_` only from `reclaim_`, and leaves it only by collect or
   requeue.
4. **The channel.** The count is changed only under `mu` (report: push then `fetch_add`; collect: swap then
   `store(0)`), so a report cannot be wiped by the reset; the unlocked acquire load can only delay a
   collection, never lose one, and `finish_deletes()` collects after `wait()`. An `OOM` in `report()` leaves
   the file removed but reported failed, or unreported; both heal (a requeue that gets ENOENT, or the
   `dropped` rule at the next wait). I found no lost failure.
5. **Lifetimes.** A job captures a `shared_ptr` to the channel, the name, the sizes, and the host's `IO`
   pointer; it touches no index state. Close and the destructor wait before the index lets go of the IO
   (the host's) and the permit. `end_merges()` ends the merge groups first, then waits for deletes; a moved
   index keeps the group, channel and map. Probe `openrace`: 40 opens that post orphans' deletes and are
   destroyed at once, then a reopen: clean under TSan, nothing lost, accounting equals the disk. Probe
   `stopped`: `stop_all()` mid-run, then writes, `reclaim()`, `wait_deletes()` and `close()` do not hang; every
   delete reports ECANCELED and the files stay queued (20) for the next open.
6. **Ordering.** Dir sync only counts a removal once collected after it ran (`done` is posted after
   `io.remove`), and `sync_dir()` zeroes `unlinked_` only; `inflight_` survives it. Manifests are removed and
   synced before `retain()` queues data files, unchanged.
7. **Adapted tests.** `reclaim_backlog_test` 4a and 4b: the bounds are the same constants
   (`freed >= cap - step`, `<= cap + step`, `muts <= 3 * (cap/step + 2)`, `freed <= owed + step`); `muts` now
   also counts the merge's own writes, so the mutation bound is stricter, and `lost()` measures bytes lost by
   the pre-merge files, equal to the old `used0 - fs.used()`. They now test the owner-thread step bound; the
   executor version of the cap is `quota`. 3b: only the added `run_all()`. `background_merge_test`: only
   the `wait_deletes()` calls an open's orphans need. None weakened.
8. **Sanitizers.** TSan (Debug -O1): `reclaim_bg_test` x4, the mapped variant, `reclaim_backlog_test`, its
   `view` run, `background_merge_test`, `reclaim_test bg`, and all probes above: no report. ASan+UBSan
   (Debug -O1): `reclaim_bg_test` (plain and mapped) and the probes `view`, `viewpool`, `openrace`, `stopped`,
   `drainposted`, `drift`: clean.
9. **Scope.** The diff is the engine change, its tests, the bench's `wait_deletes()` shim (guarded by
   `requires`), the limits row and the backlog ledger's Owed paragraph. Nothing beyond the design.
10. **Cross-library.** `reclaim()` returns bytes posted rather than freed when a host passes an executor.
    Prospect and other callers of `options::background` get deletes posted to their executor on the next pin;
    their CI should run after the bump.

## Probes and mutants kept

All under `strata-wt/review-rbg/.scratch/` (untracked, not deleted):

1. `probes/rbg_probe.cc`, `probes/build.sh <builddir>`: modes `view`, `viewpool`, `drift`, `stopped`,
   `openrace`, `drainposted`, `poolgone`, `ownerpool` (the last does not exercise what it names: see "Not run").
2. `mut.sh` (mutants `drain_noinflight`, `cap_ignored`, `enoent_fails`, `nolock`), `mut.out`, `mut-*.log`.
3. Builds: `b-tsan` (Debug -O1 `-fsanitize=thread`), `b-asan` (Debug -O1 address,undefined).

## Not run

Full ctest (53 tests) under either sanitizer: I ran the reclaim, backlog and background-merge tests and the
probes only; the developer's full runs stand on their word. Other platforms (Linux, FreeBSD) and
RelWithDebInfo: CI. The A/B and any benchmark. The `ownerpool` probe (an owner on the pool's only worker
waiting for its deletes): the owner lease is thread-bound, so the probe never posted a delete; the claim in CR2
item 3 is from reading `TaskGroup::wait`. A security review: none needed (no untrusted input). The architect:
needed only for CR4's bounding question.

## Re-check

Scoped re-check of CR1, CR2 and CR3 on `phase4/reclaim-bg` at a9f1a17 (rebased; the review was written on
9af34f6). Builds: `.scratch/b-tsan` (Debug -O1, `-fsanitize=thread`). Mutants run once each with
`.scratch/mut3.sh`; logs `.scratch/mut3-*.log`. No code was changed by me.

| # | Ruling | Evidence |
|---|---|---|
| CR1 | **CLOSED** | `test/reclaim_bg_test.cc:419-479` (`threaded()`) is the `drift` shape: 3 workers, 800 rounds, 400 random truncate, sync and remove faults (EIO and ENOSPC, before and after the effect), a reopen on every fence, per-round `sum_files == on_disk` (l.452) and `sum_posted == inflight` (l.453), a final settle with `on_disk == bytes on disk` and `segments == live` (l.462-467) and the reopen comparison of the permit (l.469-475). It drops the invalid `storage.used() >= fs.used()` line, as asked. Mutant "lock removed in `delete_chan::report()`": `reclaim_bg_test` rc 134 with 30 TSan data-race reports, all in `delete_chan::report` (`.scratch/mut3-nolock.bg.log`). Unmutated: rc 0, 0 reports. `rbg_probe drift` on the new head: 0 TSan reports (its only failure is its own line-117 `storage.used() >= fs.used()` check, which the CR1 text says is invalid under write faults and to drop; every other check passes). |
| CR2 | **CLOSED** | The contract is stated in `include/strata/core/executor.hh:30-33` (`group`: the engine waits in `wait_merges()`, `drain()`, `wait_deletes()`, `close()` and the destructor and never cancels; the executor must keep running a group's work; stopping the pool first, or running an owner on the only worker its work needs, hangs them), `include/strata/core/pool_executor.hh:5-7` (ending or joining the pool first hangs them; `post()` on a stopped pool queues without a ticket), the `limits.md` reclaim row (waits "with no timeout, so the host keeps the pool running until its indexes close"; nothing caps in-flight bytes, the host sets a budget) and the ledger's CR2 item (both cases, and why there is no timeout). Nit, not a condition: the owner-on-the-only-worker case is in `executor.hh` and the ledger only; `pool_executor.hh` and the limits row name the stopped-pool case. `wait_deletes()` also now says it is an owner-thread call and does not sync the directory (`index.hh:838-842`, CR7). Read only; no code change. |
| CR3 | **CLOSED** | `drain_posted` (`reclaim_bg_test.cc:481-518`): mutant "drain returns early without `!inflight_`" (`index.hh`, the `reclaim_.empty() && !unlinked_` early return) fails it at l.513 (`apply(...) == accepted`), and the earlier `drain waited` test at l.367, 368, 375 (`.scratch/mut3-drain_noinflight.bg.log`; rc 1). `enoent_is_done` (`reclaim_bg_test.cc:520-549`): mutant "ENOENT reported as a failure" fails it at l.542 (`reclaim_pending() == queued`), the only failing line (`.scratch/mut3-enoent_fails.bg.log`; rc 1). Both mutants restored; the worktree is clean. |
| Spot-check | **53 of 53** | Full ctest under TSan at a9f1a17: 100% passed, 53 of 53, 957 s at -j4, 0 ThreadSanitizer lines in the log (`.scratch/rc-ctest.log`). |

Notes for the lead, outside the scope: none found. The CR4 to CR7 items were not re-checked (CR4 is an architect
question; CR5 to CR7 are documentation).

Not run: ASan+UBSan full ctest (the ASan build was not reconfigured); the `drainposted`, `poolgone` and `ownerpool`
probes (CR2 is read only; the claims in `executor.hh` stand on the original probes); other platforms and
RelWithDebInfo (CI); any benchmark or the A/B.
