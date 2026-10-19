# Thread scaling of one index (scale-threads)

Why one index barely gets faster with threads, and what makes it scale. Query class: none;
the footprint is indexing wall time and CPU per document for one index at 1, 2, 4 and 8
threads, at 100k documents (keyed upserts, the T4 shape). Base: perf/inverter `75b8715`
(inverter C at one thread, 3.4x Tantivy's one-thread builder on the gate VM).

## Profile (Mac, i9-9980HK, 8 cores, 100k, macOS `sample` on every thread)

Two runs, sampled (`.scratch/prof.sh`, `.scratch/tools/thread_busy.py`; the corpus arrives 3 s
after launch, so the walls below are the timed runs' minus 3 s):

| Run | Wall | CPU (user + sys) | CPU / wall |
|---|---:|---:|---:|
| `75b8715`, one thread (inverter C) | 2.29 s | 1.92 s | 84% |
| `a2d6e68`, `STRATA_THREADS=8` (docid runs, E1 parse threads, E2b), the best configured eight-thread path | 2.84 s | 6.67 s | 235% |

Where the eight-thread run's threads go (busy samples per thread):

- The owner thread is busy for most of the wall: stage 1 (`evaluate`, the document copy
  into the docid buffer: `memmove` 25% of its samples, `madvise` 10%), then E2b's placement.
- The eight E1 parse threads are 3 to 10% busy: parsing was never the bottleneck once the
  split moved into the buffer (inverter E31).
- The eight flush threads (the docid runs) run at 100% for about a sixth of the wall each,
  inverting with the pre-C inverter (`segment_writer::buffer_for`, `add`, `put_stream`:
  35% of all busy samples), then eight pool threads encode E2b's ranges.
- In all, 5,912 busy samples against 1,610 for the one-thread C run: the configured
  parallel path spends 3.5 times the CPU of today's one-thread inverter, and its wall is
  longer than one thread's. Today, no thread setting makes one index faster than C alone.

Where C's one thread goes (1,610 busy samples, inclusive):

| Phase | Samples | Share | Under a writer per thread |
|---|---:|---:|---|
| `apply`: `evaluate` (validation, keys, docids, version map) | 42 | 2.6% | serial (log order) |
| `apply`: `invert_pending` (the split, tuple pushes, batch drains) | 779 | 48.4% | per writer |
| `commit`: `flush_active` (merge, decode, `flush_from`) | 675 | 41.9% | per writer |
| `commit`: `commit_files` (writes, `sync_files`), `clear_buffer` | 80 | 5.0% | serial |
| reading lines, finding fields, `fill_document` | about 25 | 1.5% | serial (reading) |

## The bound

C's work has no shared state between documents except stage 1 (docids and the version map,
in log order) and the commit. With a writer per thread, everything else (98% of today's
serial path less the 9% above) runs on N threads, so the serial fraction is s = 0.09 and
Amdahl's bound is 1.83x at 2 threads, 3.15x at 4, 4.9x at 8, 11x at any count. Today's
paths stop far below it: the staged flush (indexing-gap E5) reaches only the 42% flush, and
the add phase, 48%, stays on the owner (E5's 132% CPU); the docid runs parallelize the old
inverter's work, not C's.

## Hypothesis

A writer per thread, as Lucene's DWPT and Tantivy's per-thread segment writers do: N
independent C buffers, each filled by its own thread (parse, split, push, drains) and
flushed as its own segment on its own thread, all in the same commit. Stage 1 stays serial
under the index's lock in input order, so docids, versions, and the logical content equal
one thread's; segment counts are N per flush. Expected: within 20% of the bound at 2 and 4
threads, memory bandwidth limiting 8.

Mechanism (`index.hh`, `index_options::writers`): `apply_to(w, ticket, t, at)` runs stage 1
under a mutex in ticket order, into writer w's pending list, and records each buffered
document (docid, version, key, writer, ordinal, dead) in an owner-side table, so stage 1
never reads a buffer another thread is filling; stage 2 inverts on the caller's thread
outside the lock. A flush (budget, a buffer's 32-bit limits, or commit) waits for every
inversion in flight, applies the table's dead bits, and writes one segment per writer on
as many threads; ids are allocated in writer order first. One writer keeps today's path
unchanged. The engine (`STRATA_WRITERS=N`): N threads each read a batch of 1,000 lines
under a lock, take its ticket, parse, and apply into their own writer.

## Procedure

1. Correctness: `strata_engine digest` (D12's logical digest) of the N-writer index equals
   the one-thread index's for the same input, at N = 2, 4, 8, at the default budget and at
   one small enough to flush mid-run, with replaced keys and deletes.
2. Mac, 100k, interleaved, five rounds: strata at 1, 2, 4, 8 writers against Tantivy 0.26's
   builder at 1, 2, 4, 8 threads (KEYED=1, NO_MERGE=1, `writer_with_num_threads(N, 2 GB)`),
   under a strict hold of at most 15 minutes. Wall, CPU, peak RSS, bytes, segments left.
3. The gate VM, once, the same rows.

Keep if the 8-writer wall over one writer's has its interval's lower bound above 2.0 on
both platforms with equal digests and peak RSS no worse than one writer's plus 25%; remove
otherwise.

## Status (stopped 2026-10-18, the coordinator's change of priority)

Built and checked for correctness; no timing has been measured yet, so there is no curve and
no keep decision.

Correctness (prototype `c6bafd4`, Mac Release build): `strata_engine digest` gives the same
digest at 1, 2, 4 and 8 writers in every case below; segment counts differ, as allowed.

| Input | Budget | Digest | Segments at 1 / 2 / 4 / 8 writers |
|---|---|---|---|
| 100k | 2,056 MB | `d592df5b59ed265a` | 1 / 2 / 4 / 8 |
| 100k (mid-run flushes, merges) | 64 MB | `d592df5b59ed265a` | 4 / 4 / 7 / 2 |
| 27k with 7k replaced keys, deletes every 7th | 16 MB | `2b73b3489de71743` | 8 / 5 / 6 / 6 |
| 27k with 7k replaced keys, auto-merge off | 2,056 MB | `3ec11ed05d1cf53e` | 1 / 2 / 4 / 8 |

Limits of the prototype, by design or not yet handled: a mid-run flush stops every writer
(a full flush) and runs the policy's merges inline under the lock; ENOSPC in a parallel
flush fences the index instead of keeping the buffer; flush threads are dedicated
`std::thread`s, not the shared pool's tasks.

## Results (the lead's runs, 2026-10-18)

Prototype `c6bafd4`, `STRATA_WRITERS=N`, against Tantivy 0.26's builder with
`TANTIVY_THREADS=N` and `NO_MERGE=1`, interleaved (`.scratch/tools/ab_threads.py`).

Mac (i9-9980HK, 8 cores), 100k documents, 5 rounds:

| N | strata wall | speedup over 1 | Tantivy wall | strata throughput / Tantivy |
|---:|---:|---:|---:|---:|
| 1 | 2.22 s | | 4.11 s | 1.85 [1.84, 1.99] |
| 2 | 1.44 s | 1.54 [1.50, 1.60] | 2.72 s | 1.90 [1.85, 1.93] |
| 4 | 1.08 s | 2.06 [1.97, 2.09] | 2.42 s | 2.24 [2.12, 2.38] |
| 8 | 1.04 s | 2.15 [2.06, 2.18] | 5.46 s | 5.18 [5.06, 5.51] |

Gate VM (8 vCPUs, tsc), 1M documents, 3 rounds, warm corpus, indexes on disk:

| N | strata wall | strata CPU | speedup over 1 | Tantivy wall | strata throughput / Tantivy | Peak RSS strata / Tantivy |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 27.8 s | 27.0 s | | 78.9 s | 2.84 [2.65, 2.88] | 1,848 / 2,660 MiB |
| 2 | 16.0 s | 29.7 s | 1.78 [1.74, 1.80] | | | 2,133 MiB |
| 4 | 10.2 s | 34.6 s | 2.86 [2.56, 2.88] | 40.9 s | 4.01 [3.83, 4.17] | 2,373 / 2,852 MiB |
| 8 | 8.2 s | 50.5 s | 3.47 [3.38, 3.57] | 41.4 s | 5.03 [4.98, 5.46] | 2,736 / 3,029 MiB |

Tantivy stops scaling past 4 threads (1.93x at 4, 1.88x at 8). strata reaches 3.47x at 8,
against the 4.9x Amdahl bound; segments equal N (8 at 8 writers), and bytes grow 6% at 8.
Not yet done: the untouched-base byte check, unit tests and TSan, the merge cost of N segments.

## Landing checks (2026-10-18)

- One writer against 75b8715 built untouched (`.scratch/src-75b8715`), Mac Release: every
  file of the index (segment, manifests, metadata) is byte-identical at 20k and 100k, at
  `c6bafd4` and again after merging perf/inverter `c2fff83` and main `d8d219c`. Wall, one
  run each (light hold): 100k 2.07 s against 2.29 s, then 2.52 against 2.50 after the
  merge; 20k three interleaved runs 0.73/0.81/0.79 s against 0.74/0.84/0.81.
- Unit tests: index_test gains a check of 4,000 transactions (replaced keys, deletes,
  version-checked replaces that are sometimes rejected) at 1, 2, 4, 8 writers, unbounded,
  at a 256 KiB share (mid-run flushes), with auto-merge, and with force_merge(1): digest and
  live contents equal one writer's, also after reopening. index, multisegment and
  tuple_buffer pass.
- TSan (Homebrew LLVM, RelWithDebInfo): `strata_engine index` at 4 writers and a 16 MB
  budget on 20k (digest `f2644527b7c35921`, equal to one writer's; 4 segments against 7),
  and index_test, multisegment_test, tuple_buffer_test: no reports. After the merges, CI's
  linux-tsan job covers the rerun (a strict hold stopped the local one).

## Merge cost of N writers' segments (Mac, 1M, one run, strict hold)

`.scratch/runs/merge/run.sh`, the prototype at the engine's default budget, each committed
index cloned and force-merged to one segment.

| Writers | Index wall | Segments | Merge, reorder off | Merge, reorder on |
|---:|---:|---:|---:|---:|
| 1 | 41.3 s (CPU 40.4) | 6 | 31.8 s | 258.7 s |
| 8 | 93.8 s (CPU 154.5) | 9 | 34.5 s | not run (the hold's 15 minutes) |

All four digests equal `ba8fa820cf8d9bb4`. Merging 9 segments costs 2.7 s (8.5%) more than
merging 6; the reordering merge, the same bisection over the same million documents in
both, dominates. At this budget eight writers were 2.3x slower than one on the Mac at 1M,
unlike the gate VM's 3.47x speedup: each writer's buffer counts against the share, so the
buffer flushes far more often (246 segment files on disk after the commit, against 6) and every mid-run
flush stops all writers and runs the policy's merges inline under the lock. One run, not a
measurement for a decision; the pool design below bounds slots by the share for this.

## Writers on the shared pool (design note, not built)

Today the writer count is fixed (`index_options::writers`, `STRATA_WRITERS`), each writer is
a caller thread, and a flush starts `std::thread`s. The plan has no thread setting:
parallelism comes from the node's shared pool, `WorkPool` in Kronuz/threadpool's
`feature/scheduler` (one `WorkGroup` per index, deficit round robin between groups, classes
`query`, `flush`, `merge`). The mapping:

1. A writer is a slot, not a thread. The index keeps a free list of slots (buffer, scratch)
   created on demand; a slot exists only while it holds buffered documents, so an idle index
   holds none (the many-indexes workload pays nothing per open index).
2. A batch is a `WorkItem` posted to the index's group, class `flush`. Stage 1 keeps the
   log's order without parking a worker: a batch whose ticket is not next leaves its parsed
   transaction in a small reorder ring and returns; the worker that runs the next ticket
   also runs every consecutive ready one (a combining sequencer), under the index's lock as
   now. Stage 2, the inversion, then runs on that item's worker into a leased slot.
3. Slots are leased most-recently-used first, and a new one is made only when every slot is
   busy. So the slot count follows the workers the pool actually gives the group: one index
   alone fans out to every worker (up to the group's `width`) and gets N segments per flush;
   with many busy indexes each group gets about one worker, one slot fills, and the flush
   writes one segment, byte-identical to one writer's today.
4. The cap is memory, not threads: slots are also bounded by `buffer_share` over a slot's
   minimum footprint. Each slot's arena counts against the share, so at a small share more
   slots mean more flushes (the unit test's 256 KiB share: 8 flushes at one writer, 28 at
   eight).
5. A flush posts one `flush` item per non-empty slot and the flushing worker calls
   `WorkGroup::wait()`, which serves the group (and other groups' queries) instead of
   blocking; this replaces `flush_writers`' `std::thread`s. Segment ids stay allocated in
   slot order before the items run, so files do not depend on timing.
6. Merges stay class `merge` under the pool's `merge_cap`; the N segments a parallel flush
   leaves are the policy's to merge (cost below), and the reserve keeps queries ahead of
   both.

Open for the design review: whether stage 1 moves to the log's applier (serial, no
sequencer) once parsing leaves it; how a slot's minimum footprint is measured; and whether
`width` defaults to the pool's threads minus the query reserve.

## The largest-writer flush (2026-10-18)

The finding above: at the engine's default 256 MB share, every mid-run flush stopped all
writers and wrote all eight buffers. Two changes, `7483098` and `4f3398e`:

1. Past the share only the writer holding the most flushes (Lucene's
   `FlushByRamOrCountsPolicy`). Its buffer is detached under the lock and written outside it
   while the other writers index; if it is inverting, its own caller flushes it when the
   inversion ends. Stage 1 keeps reading the detached documents from the owner-side table,
   and deletes that land during the write reach the segment at publish. Flushes in flight
   past twice the share stall stage 1 (Lucene's stall control).
2. A writer's batch scaled to its share: at 2^20 tuples each writer's batch scratch was about
   30 MB before any run (a 300k diagnostic flushed buffers of 44 to 63 MB holding 6,000 to
   27,000 documents), so eight writers' scratch nearly filled 256 MB alone. The writers'
   batches now take at most a quarter of the share (2^17 tuples at 8 writers and 256 MB;
   one writer keeps 2^20 and stays byte-identical to main 734949e at 20k, 100k and 1M).

Hypothesis: flushes of about share/N to 2 share/N each instead of eight of share/8 at once,
so 8 writers beat one at 1M at the default share. The policy's merges still run inline
under the lock (no executor in the engine), and N writers leave more segments for the
tiered policy than one writer's six, so merges may now be the serial cost; `STRATA_TIMERS`
reports their time.

Procedure (`.scratch/tools/ab_threads.py`, `STRATA_BUDGET_MB=unset`, auto-merge on):

1. Mac, 1M, strata at 1, 2, 4, 8 writers, three rounds, interleaved, under a strict hold of
   at most 15 minutes.
2. Mac, 1M, the same strata rows beside Tantivy 0.26's builder at 1, 2, 4, 8 threads
   (`KEYED=1 NO_MERGE=1 TANTIVY_HEAP_MB=256`, the same memory), one round, a second hold.
3. The gate VM, 1M, the same eight rows, one round.

Reported: wall, CPU, peak RSS, segments left, and the speedup over one writer.
