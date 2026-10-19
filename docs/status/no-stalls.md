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
  - The pause pair (I3, one pair so far, `.scratch/runs/pause-*.tsv`): a merge parks at a
    checkpoint 69 ms (p50) to 225 ms (max) after the request; with merges paused during
    flushes the flush runs 20 to 30% faster in every window and the run merges more in
    total (838 against 754 MB): interference on a host with idle cores. Two more pairs queued.
  - The note's sections 1 to 5 (problem, bound, survey, harness, calibration, ideas).
  - CI run 39206012974 at `c72f0f1` (the index.hh hooks and the drivers): green on all eight
    jobs (Linux x86_64 gcc and clang, Linux arm64, macOS x86_64 and arm64, FreeBSD,
    ASan+UBSan, TSan).
  - Core round 1 (`.scratch/runs/core/*-r1.tsv`): RocksDB leveled stalled 120 s of 180
    (writes of 1.27 s, admission cliffs), universal 19.7 s with one 12.6 s stop, stalls off:
    L0 to 65 and gets unchanged; strata today: no stall, segments 5 to 97, query p50 17×
    and p99 20× slower, a 167 s drain at the end; I1 (count-keyed ramp): service bounded at
    48 ms and no stop, but locked at its 5% floor behind FIFO big merges (52% admitted);
    I2: 30% fewer merge bytes (564 against 806 MB) for the same end state, drain 75 s.
  - Core rounds 2 and 3: the same picture as round 1 to within noise (RocksDB leveled
    stalled 120.4 to 120.6 s in each round; strata today drained 150 to 167 s; I2 merged
    537 to 564 MB against 793 to 806). Medians in the note's 4.2.
  - The first many-index rounds (`.scratch/runs/many-v1/`) are a harness artifact: the host
    scheduler's tick was the 4 s refresh and finished merges counted as in flight until
    published, so `global` and `hot` posted 38 merges where today's round robin posted 160.
    Fixed (`publish_finished()`, a 250 ms tick, four queued); rerun queued.
  - Hold incident: my pipelines called `quiet run` without `quiet wait`, and pause pair 2
    started under another agent's strict hold (`scale-threads-vm`, 00:21 to 00:36). Stopped
    at 00:27; every remaining run now sits behind `quiet wait --light`
    (`.scratch/runs/pipeline6.sh`). The host also sped up about 2× over the evening
    (strata's flush per 5 s: 1,911 ms in the calibration, 659 → 512 ms across the core
    rounds), which the rotated rounds absorb for A/B purposes and absolute numbers do not.
- Doing: pipeline6: the pause pairs redone (ABBA), the driver rebuilt, the extras (three
  rounds of feed-forward admission, adaptive refresh, cheapest-first merges), the
  many-index rerun (two rounds).
- Next: the results sections (4.2 to 4.4) and the recommendation (6) from the medians.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: not yet.
