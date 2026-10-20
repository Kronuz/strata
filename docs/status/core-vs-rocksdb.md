# core-vs-rocksdb: strata's search layout as a key-value store against RocksDB, quick tier

- Task: the first key-value measurement against RocksDB (PLAN, "The `document` profile gate"),
  quick tier. Relabeled by the owner and lead: "strata's search layout used as a key-value
  store", not the core comparison; the real run follows strata::core's format-14 KV read
  path and turn log, with this harness.
- Branch / worktree: `perf/core-vs-rocksdb` from main `68e3d29`, at
  `/Users/kronuz/code/Kronuz/strata-wt/core-vs-rocksdb`. Record: `docs/perf/core-vs-rocksdb.md`,
  raw data in `docs/perf/core-vs-rocksdb/`.
- RocksDB: 11.11.1, release branch `11.11.fb` at `ec93377878cd` (upstream tags stop at
  v11.8.1), sources `~/.scratch/rocksdb-11.11.fb`, build `~/.scratch/rocksdb-11.11-build`.

## State

Done and stopped, as the lead asked. Every row measured on the macOS host; the rows the
unheld disk cleanup overlapped (fits1, A/A, twox1) rerun and quoted from the reruns; the top
three losses profiled and written as inputs to the core's KV design.

## Done

- 881fc41: plan and harness (`bench/kv/kvgen.hh`, `kvdriver.hh`, `kv_strata.cc`, `run.py`;
  `bench/engines/rocksdb-11.11/`).
- c16f57e: `search_layout` fallback (`basic_index<document_layout>` does not compile on main),
  section bytes, capped runs.
- e671bea: labeled reruns, heap pause.
- This commit: results, raw data, the perf index row, this ledger.

## Next

1. After strata::core lands its KV read path and turn log: point `kv_strata` at the core's
   index (the driver's test hook for gets and scans goes, replaced by the public API), rerun
   calibrate, A/A, and every set; add the cgroup-capped 2× rows on the Linux VM at the gate
   tier's sizes, and FreeBSD.
2. Rows not yet run: `deleterandom`, `multireadrandom`, `readwhilewriting`, jemalloc (Q5).

## Blocked on

Nothing. The `document_layout` index (L12) waits on the core/search split, not on this branch.
