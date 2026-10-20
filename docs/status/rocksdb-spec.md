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

Planning: the audit's plan committed; harness reading and source tracing next.

## Done

- This commit: the plan and this ledger.

## Next

1. Audit build with statistics and a barrier counter; one counted trial per row class.
2. Fix the harness asymmetries found; rerun only the rows a fix changes.
3. The mechanisms spec.

## Blocked on

Nothing.
