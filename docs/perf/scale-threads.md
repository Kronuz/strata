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
