# core-vs-rocksdb: strata's storage core against RocksDB, quick tier

- Task: the first measurement of strata's core alone against RocksDB (PLAN, "The `document`
  profile gate"), at the quick tier: db_bench-style rows and YCSB A to F, fits and 2×, the
  durability tiers that exist, one and four threads, the many-index row, all four axes.
- Branch / worktree: `perf/core-vs-rocksdb` from main `68e3d29`, at
  `/Users/kronuz/code/Kronuz/strata-wt/core-vs-rocksdb`. Record: `docs/perf/core-vs-rocksdb.md`.
- RocksDB: 11.11.1, release branch `11.11.fb` at `ec93377878cd` (upstream tags stop at
  v11.8.1), sources `~/.scratch/rocksdb-11.11.fb`, build `~/.scratch/rocksdb-11.11-build`.

## State

Harness written, not yet built (host under another agent's strict hold).

## Done

- Plan (`docs/perf/core-vs-rocksdb.md`), harness (`bench/kv/`, `bench/engines/rocksdb-11.11/`).

## Next

1. Build both drivers; 2k-record dry run of every row, both engines non-empty.
2. Calibrate; A/A; rows fits1, twox1, sync1, wal1, fits4, many.
3. Profile strata's three largest losses; write results.

## Blocked on

Nothing.
