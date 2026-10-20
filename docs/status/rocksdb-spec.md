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

Done. Audit findings A1–A6 in `docs/perf/rocksdb-fairness-audit.md` with raw data; the spec in
`docs/research/rocksdb-mechanisms.md`. Branch pushed, not merged. Audit binaries and data under
`~/.scratch/rocksdb-audit/` removed.

## Done

- 09e34a6: the plan and this ledger.
- 531c113: audit instruments (`on_phase`, `KV_PHASE_MARKS`, `KV_ROCKSDB_STATS`, the barrier
  counter, `-tblidx` and `-atomic` variants) and A1's fix (a sync row's load out of the run).
- fb02183: the clean-room mechanisms spec.
- This commit: A2's fix (strata's settle commits again only for pending merges), A3's fix
  (`-atomic` for multi-index rows), the audit record and raw data, pointers from
  `core-vs-rocksdb.md` and the perf index.

## Next

1. The lead decides whether to rerun the affected rows as a set (`sync1@fix`, `many`, the fits
   "to durable" columns) and restate them in `core-vs-rocksdb.md`.
2. Linux and FreeBSD at the gate tier: A4 predicts the 2× fill rows change direction there.

## Blocked on

Nothing.
