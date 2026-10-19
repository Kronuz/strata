# research-no-stalls

- Design: none; the research note is `docs/research/no-stalls.md` (the bound, the survey, each
  idea with its prototype's numbers, the recommendation). Prototypes live in
  `research/no-stalls/`, built with `-DSTRATA_RESEARCH=ON`; the RocksDB driver against a pinned
  release (`STRATA_ROCKSDB_SRC`, `STRATA_ROCKSDB_BUILD`).
- Branch / worktree: `research/no-stalls` at `/Users/kronuz/code/Kronuz/strata-wt/no-stalls`,
  from `origin/main` at `734949e`.
- Brief (owner, 2026-10-18): RocksDB's write stalls are the failure mode strata must not inherit;
  slow merges slow everything at the worst possible time, like garbage collectors. Deliver the
  bound, the survey (SILK, ScyllaDB's controllers, Dostoevsky, PebblesDB, WiscKey, Lucene,
  RocksDB, others), at least three ideas with one that breaks a common assumption, each with a
  prototype measured on strata at sustained ingest against RocksDB under the same load, write
  and query latency over time (p50/p99/p999 per window).
- State: calibrated; the core matrix is next. Calibration at 5,000 docs/s, unthrottled, one
  background thread (`.scratch/runs/cal-*.tsv`): strata merges 5.6 MB/s against 2.1 MB/s
  flushed (2.6 rewrites) at 60% of a core, segments 10 to 27, query p50 0.1 to 0.7 ms rising
  with the count; RocksDB (uncompressed) compacts 28.8 MB/s leveled and 20.7 MB/s universal
  over 5.4 MB/s of flushes, zero stalls, 0.6 to 1.4 cores. strata's write sojourn p99 is 400
  to 530 ms every window: the inline flush (400 ms per second of ingest, 80 µs/doc) on the
  writer thread, ten times anything merge-related; the merge yields at 1 MiB output chunks,
  64 to 512 ms apart (max 1.7 s).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Worktree and branch; corpus samples cloned into `.scratch/` (100k and 1M documents from
    perf/index-size's scratch); strata_engine Release built in `.scratch/build-rel`.
  - RocksDB pinned at v11.8.1 (2026-08-04, the newest tag, older than two weeks): source in
    `.scratch/rocksdb-src`, db_bench and `librocksdb.a` building in `.scratch/rocksdb-build`.
  - Additive hooks in `include/strata/index.hh` for a host that schedules: `segment_view::bytes`,
    `take_snapshot()` (owned views a reader thread searches), `schedule_merge(ids)`, `policy()`.
  - `research/no-stalls/`: `windows.hh` (per-window percentiles, open-loop pacing),
    `ingest.cc` (the strata driver with the controllers), `rocks.cc` (the RocksDB driver),
    `CMakeLists.txt`.
- Doing: building and dry-running the drivers.
- Next: the baseline runs (strata today against RocksDB at the same offered rate and merge
  budget), then each idea, interleaved; the note.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: not yet.
