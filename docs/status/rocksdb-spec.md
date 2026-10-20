# rocksdb-spec: a fairness audit of the KV harness, and a clean-room spec of RocksDB

- Task: (1) audit `core-vs-rocksdb.md`'s harness row by row from RocksDB's own statistics,
  logs, and source; fix the harness where it is unequal; (2) write
  `docs/research/rocksdb-mechanisms.md`, prose and diagrams only, each mechanism's invariant
  and cost, mapped to strata's measured losses.
- Branch / worktree: `research/rocksdb-spec` from main `76e16d0`, at
  `/Users/kronuz/code/Kronuz/strata-wt/rocksdb-spec`. Records:
  `docs/perf/rocksdb-fairness-audit.md`, `docs/research/rocksdb-mechanisms.md`.
- RocksDB: 11.11.1 at `ec93377878cd`, sources `~/.scratch/rocksdb-11.11.fb` (read under
  Apache 2.0), library `~/.scratch/rocksdb-11.11-build/librocksdb.a`. Audit binaries and data
  in `~/.scratch/rocksdb-audit/`, removed when done.

## State

Auditing: counted runs of every RocksDB row done (fits1, twox1, wal1, sync1); the
many-index, group-commit, weak-barrier, and strata barrier runs in flight.

## Done

- 09e34a6: the plan and this ledger.
- This commit: the audit instruments. `kvdriver.hh` marks phase boundaries (an optional
  `on_phase`, and `KV_PHASE_MARKS` lines); `kv_rocksdb` runs a sync row's load at
  durability none (the fix: its puts went into the run's first sync batch), dumps statistics,
  flushes, compactions, and stalls per phase under `KV_ROCKSDB_STATS=1`, and gains the
  `-tblidx` and `-atomic` audit variants; `bench/kv/barriers.c` counts barriers per file.

## Next

1. Audit build with statistics and a barrier counter; one counted trial per row class.
2. Fix the harness asymmetries found; rerun only the rows a fix changes.
3. The mechanisms spec.

## Blocked on

Nothing.
