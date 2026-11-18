# Code review: phase4/reclaim-backlog

- Reviewed: `phase4/reclaim-backlog` at `c8a97af` (`9dba464` plus the ledger commit), base main `88a56e7`; `origin` equals `HEAD`; GitHub CI on
  `c8a97af` is green (run 39699236094). Worktree `strata-wt/reclaim-backlog`, no uncommitted changes to code.
- Against: `docs/status/reclaim-backlog.md`, `docs/design/limits.md` ("Reclaim step and quota"), `docs/design/shared-reader.md` D6, D7 (on
  `design/shared-reader`), `docs/reviews/calibration.md`, `docs/reviews/shared-reader-core.md`.
- Checked by: my own builds in `.scratch/review/` (`b-rel` -O2 with asserts, `b-asan` Debug -O1 ASan+UBSan, `b-tsan` -O1 TSan; Homebrew clang 23.1.2,
  macOS x86_64, `QUIET_LEVEL=light quiet run review-reclaim`, 4 jobs at nice 19); the full ctest under ASan+UBSan and TSan; the probes and mutants below.

## Verdict

**APPROVE WITH CONDITIONS.** The new path is safe: I could not make a publish truncate or delete a file that a pending merge, an open mapping, a
kept or pinned manifest, or the newest manifest names (2,281 ASan crash points and the mapped oracle, CR1), and the backlog is gone (the queue stays at
about 10 to 40 files where main's grows without bound, CR6). What blocks landing is not the deletes but two claims the branch writes down and the code
does not keep: that a publish's reclaim is bounded by the step (it is bounded per syscall only: one owner call frees as many bytes as the merges it
publishes obsoleted, CR2), and that the row's cost is "a bounded fraction" of a merge (it lands on the owner, and the 64 KiB row loses 9 to 17% of
run-phase throughput (mine 9%, the ledger's 14 to 17%), CR3). Both settle in one fix round with no engine change: correct the text, add the two missing tests (CR4), and put the
background-executor delete (prototyped, CR3) on the ledger as the owed follow-up. Nothing here needs the architect or a security review.

## Conditions to land (one fix round)

1. **CR2.** Replace the "never stalls one call" / "bounded" wording in `index.hh:413-417` (the options comment), `limits.md` (the new row's Rationale
   cell) and the ledger with what holds: one syscall is at most `reclaim_step`; one owner call frees at least what the merges it published
   obsoleted. Or cap the call (a carried debt, see CR2). Either closes it.
2. **CR3.** Record in the ledger that the 64 KiB `ycsb_a` run-phase row is lower than main's by the owner-thread deletes (the prototype below recovers
   it) and that moving already-unreferenced deletes to the executor is the owed follow-up, named so the ratchet reader does not take the number for
   the engine's.
3. **CR4.** Add to `test/reclaim_backlog_test.cc` the two mutants that survive (the quota's upper bound, and the discarded-output count), and a test
   of the call-site contract the shared reader depends on (CR5): a held view across `refresh()` at retention 0.

## Findings

### CR1. Safety: no file a live reader, pending merge, mapping or kept manifest needs is truncated (no defect)

What I tried, and what each result is:

1. **Held views across merges at retention 0 under faultfs mapping** (`views_probe.cc`, `out/views.log`; the view is `index_test_access::freeze`, the shape the
   reader core's views hold: a `shared_ptr` to each `open_segment`, so the mapping is live). faultfs aborts on a truncate below a live mapping
   (`faultfs.hh:341-344`). Results:

   | build | host calls | result |
   |---|---|---|
   | main | `refresh()` only | survives 38 rounds (nothing reclaims between commits) |
   | main | `refresh()` + `commit()` | **aborts**, "a file truncated below a live mapping" (the existing `commit()` path) |
   | branch | `refresh()` only | **aborts** (the new publish-time reclaim) |
   | branch | `refresh()` + `commit()` | **aborts** (as main) |
   | branch + `retain()` naming the view's segments (`hook-inc`, `-DRV_HOOK`: three lines in a copy of `index.hh`) | both | survives 38 rounds |

   Reading: on main, no code holds a view across a writer call. `hazard.hh` is not wired into `basic_index` (`grep hazard include/strata/core/index.hh`
   finds nothing), `segments()` and `snapshot` are valid only until the next writer call, and `index_test_access::freeze` is a test helper. So the abort
   is not reachable from any public API today, and **the branch adds no defect**. It does move the line: a view that survives a `refresh()` was safe on main
   whenever the host did not commit (the durability-none case this branch fixes), and is not safe here. The branch routes the new reclaim through
   `retain()` first (`index.hh:1860`), which is the single gate D7 extends, so the fourth set (the segments of every unfreed view) in `retain()` covers
   the new call site with no further change (the hooked run). CR5 is the condition this puts on the shared reader.
2. **Crash points across the new reclaim steps** (`crash_probe.cc`, derived from `reclaim_test.cc`). Seeded workloads of puts, replaces and deletes,
   refreshes, commits, checkpoints and releases, `force_merge`, `maybe_merge`, and a manual executor stepping merges one at a time (merges pending across
   commits and across crash points), with `auto_merge` at a 2-segment tier so nearly every refresh publishes, `reclaim_step` 256, `reclaim_slice` 0 or
   1,024, `retained_predecessors` 0 or 2, faultfs mapping on, power lost after every n-th mutation (`power_off_after`) then `crash(seed)`. After each
   crash: every manifest on disk names only files that exist whole with the entry's size, footer checksum, live-docs and metadata checksums; the reopen
   succeeds; `check()` is clean; the content equals the model at some commit, checkpoint or release attempt; after `reclaim()`, `commit()`, `reclaim()`
   no strata file is unnamed. Without a crash, the index equals a `std::map` model after every step. Results: ASan+UBSan build, 16 seeds, 2,281 crash
   points: 0 damaged manifests, 0 failed opens, 0 content or check failures, 0 unnamed files, 3,716 truncated orphans seen and reaped by the next open
   (`out/crash_asan.log`). Optimised build, stock code: 239 points on 4 seeds, the same.
   (My first run reported content mismatches. They were my oracle: `release()` commits, so its model is a commit attempt. The same mismatches appeared
   on main's headers. Fixed in the probe; no engine finding.)
3. **Mutants** (`mut.sh`, `out/mut_no_pending.log`, `out/mut2.log`). Dropping the pending-merge sources from `retain()` (`index.hh:2776-2779`): killed by
   the crash probe and by `reclaim_backlog_mapped` (abort), but not by the plain `reclaim_backlog`. `reclaim(owed / 4)`: killed (`reclaim_backlog` fails,
   40 dead files). Two survive, see CR4.
4. **Ordering reads.** `retain()` (2747) works from `files_`, `m_`, `segs_` and `pending_` only. A merge's in-flight output is not in `files_` until
   `install_merge` tracks it (`:1947`), so a running merge's output file is never queued. A discarded output is queued by `install_merge` itself
   (`:1901-1908`), which is unreferenced by construction. The sources are removed from `segs_` before the `retain()` that follows the loop, and the
   jobs `publish_merges` processed are erased from `pending_` first, so only the unfinished jobs' sources add to the referenced set. A kept manifest
   unreadable (`retention_blocked_`) makes `retain()` return before queueing, and `reclaim(owed)` then frees only what was queued earlier. A fenced
   index throws from `reclaim()`'s `live()` before any file is touched.
5. **Real files.** The branch's `kv_strata` on the POSIX IO with mapping (the 64 KiB `ycsb_a` row, 6 interleaved rounds, `out/ab3.log`): the get digest
   `a46c3543d251394d` equals main's in every run.

### CR2. Bounds: the step bounds a syscall, not an owner call; one publish frees as much as it obsoleted

`reclaim(owed)` (`index.hh:1859-1862`) loops until `freed >= owed` (`:665-709`). The 16 MiB step bounds each `truncate`, and a file at most one step long
is unlinked whole; nothing bounds the number of steps in the call. `owed` is the sum of the sources of every merge the call published, so it is the
merge's size: up to `max_merged_bytes` (5 GiB by default, `merge_policy.hh:21`) per merge, several merges per call, and unbounded for forced merges.
Probe `bound_probe.cc` (`out/bound_probe.log`): eight segments, the manual executor runs the merge outside the measured call, then `refresh()` publishes it.
The mutating operations (truncate, sync, unlink) inside that one `refresh()`, with `reclaim_step` 16,384:

| bytes freed by the call | mutations in the call |
|---|---|
| 983,040 | 68 |
| 4,128,768 | 260 |
| 16,580,608 | 1,020 |

Linear in the merge. At the default step a 5 GiB merge is about 320 truncations and as many unlinks and syncs on the owner, inside `refresh()` (which in
the shared-reader design is what a reader waiting on `publish_through` waits for, D9) or `commit()` or `wait_merges()`. That is arithmetic, not a timing
(I ran none). Against the text: the options comment says "freeing a large file never stalls one call" (`index.hh:413-417`), the limits row says "the
step bounds one call's longest syscall, so freeing a large file never stalls the owner" and "a publish would stall on more work than the merge it
follows" at the quota. The first two claim the opposite of what the code does at the call level, and the third is true only of the work, not of where it
runs: the merge's work ran on the pool, the reclaim runs on the owner.

Not a safety finding. Two honest ways to close it: state the bound that holds (CR2 condition 1), or cap the call and carry the rest: add `debt_ += owed`,
pay `min(debt_, k * reclaim_step)` per owner call (publish and commit), forgive it when the queue is empty. The cap must still be at least the merge
rate on average, or the queue grows again; the debt keeps that. The background-executor delete (CR3) removes the stall instead, and makes the cap
moot. I would take the text fix now and the executor as the follow-up.

I also checked whether the quota keeps pace for hosts that commit (a worry: a merged source named by the last manifest is not queued until the next
commit, so `reclaim(owed)` pays older files). `cadence_probe.cc` (`out/cadence.log`): auto-merge at tiers 2 and 8, 600 to 2,400 rounds, a commit every
1, 2, 4, 16, 64 rounds or never, `reclaim_slice` 8 KiB. Branch: the queue stays at 9 to 39 files and at most 55 KB dead on disk at every cadence. Main at
2,400 rounds, commit every 16 rounds: 2,361 files and 4.9 MB, growing. The queue does not outrun the merges with commits either. (At a 1 KiB slice,
below the size of the live-docs files that `owed` does not count, the branch's queue also grows slowly at one cadence; that is the probe's scale, not
the engine's, since the real slice is 64 MiB.)

### CR3. Cost: the 64 KiB row's run-phase drop is the owner-thread deletes, and moving them to the executor recovers it

**Can the deletes run on the executor?** Yes, mechanically, and it fits the architecture ("the engine owns no threads: the host passes an executor",
`executor.hh:1`). By the time `reclaim()` runs on a queued name, `retain()` has already proved it unreferenced by any kept manifest, `segs_` or pending
merge, and the file's size is known; truncating in steps, syncing and unlinking need only the `IO` and the name. A crash at any point leaves an
orphan that the next open's `retain()` queues, which `reclaim_test` already exercises (the "truncated orphans" count). What the owner must keep: the
accounting (the file leaves `files_`, enters `unlinked_`, as `forget_file` does), the failure path (today a failed step leaves the file queued for a
later round; a background failure needs a channel back, an atomic count and a list to requeue), `drain()` for admission (it must wait for in-flight
deletes before it reports space), `close()` (it should wait, not cancel, or leave the orphans to the next open), and a group of its own so
`wait_merges()` does not wait on deletes.

**Prototype** (`bg-inc/include/strata/core/index.hh`, a copy of the branch's header; `reclaim_bg`: pops queued names up to `owed`, does the accounting
at once, and posts truncate, sync, unlink to `group_`; failures swallowed; no `drain()` or `close()` handling; `out/kv_bg` is the branch's `kv_strata`
linked against it). `ab2.sh`, `QUIET_LEVEL=light quiet run`, `ycsb_a`, 2,000 records, 20,000 ops, 64 KiB values, 256 MB, 1 thread, durability none,
six interleaved rounds A (main), B (branch), BG (prototype), `out/ab3.log`. The machine was shared (other agents' light holds ran sanitizer builds
throughout), so read the order, not the third digit:

| arm | run ops/s per round | mean | total s (mean) | disk after settle | get digest |
|---|---|---|---|---|---|
| A, main `88a56e7` | 6805, 7383, 7592, 8889, 8434, 8432 | 7,922 | 3.45 | 652 to 814 MB, 54 to 63 files | `a46c3543d251394d` |
| B, branch | 6989, 6796, 6945, 7515, 7589, 7205 | 7,173 (0.91x A) | 3.76 | 82 to 90 MB, 6 to 16 files | same |
| BG, prototype | 8416, 7812, 8341, 8629, 8962, 8250 | 8,402 (1.17x B, 1.06x A) | 3.49 | 72 to 85 MB, 4 to 10 files | same |

An earlier five-round B/BG run (`out/ab2.log`) gave B 6,453 to 7,082 and BG 6,916 to 8,259 (mean 6,800 and 7,720, 1.14x). BG matched or beat main in
run-phase throughput and total time with the same final disk level as the branch, so the loss (9% in my six rounds, 14 to 17% in the ledger's) is the deletes
(truncate and unlink of about 600 MB) on the owner thread, and the executor recovers it. Note the cost does not vanish: the CPU for the deletes
moves to a pool thread, so on a many-indexes host with one thread per index (memory: many-indexes workload) the owner's latency and throughput
improve while the process's CPU per byte merged does not. Settle is slightly longer on BG (0.4 to 0.7 s against 0.3 to 0.6) since it waits for the
deletes.

**Blocker or follow-up?** Follow-up. The branch is correct, drains the queue (disk 8x lower than main, level with RocksDB per the explorers), and the
loss is confined to a delete-heavy row by a bounded, measurable amount. Landing it with the loss recorded (CR3 condition 2) is better than holding a
safe fix for a design change that touches `drain()`, `close()`, the failure path and the accounting, which wants its own review (it is concurrency in
file deletion). RocksDB's and Lucene's analogues delete on the compaction or merge thread (`limits.md`'s own columns), which is this follow-up.

### CR4. Tests: two mutants survive, and no test pins the contract the shared reader needs

1. **The quota's upper bound is untested.** `reclaim(owed)` to `reclaim()` (drain everything on every publish: the unbounded stall of CR2 made
   worse) passes `reclaim_backlog`, `reclaim_backlog_mapped` and my crash probe (`out/mut2.log`, mutant `drain_all`). With `reclaim_slice` 0 and
   the queue empty after each publish, a test cannot see how much a call freed. Fix: queue a backlog first (flush segments, no merge), then publish
   one small merge and assert that the call freed no more than `owed + reclaim_step` bytes (`fs.used()` before and after, as `bound_probe.cc` does).
2. **The discarded-output count is untested.** Removing `obsoleted_ += job.output->ref.size` (`index.hh:1904`) passes every test (mutant
   `no_discard_count`; test 3 ends with `commit()` and `reclaim()`, which drain what the publish did not). Fix: in test 3, assert `reclaim_pending()`
   is 0 right after the publish that discarded the output.
3. The ledger's revert check (`reclaim_backlog` fails with the header reverted) is right, and `reclaim_backlog_mapped` earns its place: only the
   mapped run kills the pending-sources mutant. Say so in the test comment.

### CR5. Design hand-off: the shared reader's D7 is now load-bearing for `refresh()`, not only `commit()` (for the architect and the shared-reader step)

`shared-reader.md` D6 puts the reclaim at "every publication, commit, reclaim, close" scans and D7 adds the unfreed views' segments to `retain()`'s
referenced set. This branch calls `retain()` then `reclaim()` from `publish_merges`, so from `refresh()`, `merge()`, `force_merge()` and
`wait_merges()` as well as `commit()`. D7 as written covers it because the gate is `retain()`, but D6's order ("publish, scan and free, `retain()`,
`reclaim()`") must also hold at the new call site: the scan and free must run before the `retain()` at `index.hh:1860`, or a view freed in this
call keeps its files one round longer (safe, late). The risk is the other direction: a step-3 change that adds the views to `retain()` only in
`commit()`. Whichever of the two branches lands second owns a test: a frozen view across `refresh()` at retention 0 under faultfs mapping
(`views_probe.cc` is the drop-in; it aborts on the branch now and passes with the three-line hook). Also order the reader-visible publication before the
reclaim in `publish_merges`, so a reader waiting on `publish_through` does not wait for the deletes.

### CR6. What the fix does, for the record (no defect)

`cadence_probe.cc`, `out/cadence.log`: see CR2. The ledger's rows reproduce in direction: A leaves 54 to 63 files and 650 to 814 MB; B and BG leave
4 to 16 files and 72 to 90 MB, digests identical in all 18 runs.

## Notes that do not block

1. `bench/kv/kv_strata.cc:148`: `settle()` now drains by default (`KV_SETTLE_RECLAIM=0` restores the old settle). Every row measured from this
   branch on has a drained queue at the end; earlier rows (disk, files) are not comparable. The ledger says so; the bench README should.
2. `obsoleted_` is reset at the top of `publish_merges` (`:1825`) and consumed at the end, so it is per call; a publish that fences keeps the sources'
   files for the next open to reap (same as before the branch).
3. `unlinked_` (removed, awaiting a directory sync) grows with publish-time reclaims for a host with a storage budget that never commits, until
   admission's `drain()` syncs the directory (`:2057-2066`). Self-correcting, and bytes counted not freed; nothing to fix.
4. `retain()` now runs on every publish that obsoleted bytes (it was per commit). Its cost is a pass over `files_` plus a manifest-name encode for
   the head; I did not measure it, and it is within the 64 KiB row's A/B numbers above.

## Probes, mutants and logs (all in `.scratch/review/`, kept)

- `build.sh`, `pc.sh` (compile a probe against the b-rel deps and a chosen include root), `mut.sh` (one-replacement mutant of `index.hh`, builds and runs
  the crash probe and `reclaim_backlog_test` plain and mapped), `ab2.sh` (the A/B/BG rows).
- `crash_probe.cc` (CR1.2), `views_probe.cc` (CR1.1), `bound_probe.cc` (CR2), `cadence_probe.cc` (CR2), logs in `out/`.
- Trees: `main-inc/` (main's `include/` and `bench/kv/`), `hook-inc/` (branch plus the `retain()` hook), `bg-inc/` (branch plus the background delete),
  `carry-inc/` (a debt-carry variant that did not change the cadence result; kept as a record), `mut/*/` (mutants).
- Logs: `ctest-asan.log`, `ctest-tsan.log`, `ctest-tsan-format14.log`, `out/ab2.log`, `out/ab3.log`, `out/mut*.log`, `out/views.log`.

## Test results

- ASan+UBSan (Debug -O1), full ctest: 39 of 39 passed, 397 s (`ctest-asan.log`).
- TSan (-O1), full ctest at a 900 s per-test timeout: 38 of 39 passed (`ctest-tsan.log`); `format14` timed out at 900 s under a loaded machine (the developer's run
  of the same test took 1,084 s). Rerun alone with a 2,400 s timeout: passed, 988 s (`ctest-tsan-format14.log`). So 39 of 39 under TSan; no race report in any test.

## Not run

Linux and FreeBSD (CI on `c8a97af` is green; I ran macOS x86_64 only); arm64; a real-POSIX truncate under a live mapping (faultfs's abort is the
oracle; no SIGBUS test on real files); the background-delete prototype under TSan, with a storage budget, or with `drain()` and `close()` (a prototype
for the timing question only); any timing of the owner stall in CR2 (arithmetic only); `retain()`'s per-publish cost; the Xapiand-side hosts; a
security review (none needed: no new input, parser, permission or wire surface).
