# Write-buffer bake-off (step 0 of `docs/design/write-buffer.md`)

Branch `perf/lanebuf` from `design/write-modes` (`acdc908`). Prototype: `bench/kv/proto/lanebuf.hh`,
`lanebuf.cc` (the three candidates from one source), `lanebuf_work.hh` (workloads shared with the
RocksDB adapter `lanebuf_rocksdb.cc`). Benchmark code only; nothing outside `bench/kv/proto` and the
`STRATA_KV_PROTO` block of `CMakeLists.txt` changes.

## Owner's answers (2026-10-21, folded in before the run)

1. Q1: a reader sees a write only once it is acknowledged at the index's durability level. The
   prototype has no log (durability `none`), so a write is acknowledged and visible when its group's
   stage 1 ends; V is stored then.
2. Q2: search reads at a freeze (coalesced); gets and scans read live. The prototype measures gets and
   scans only.
3. Q3 (stricter than the designer's): keep the multi-writer mode for Xapiand only if, on the
   Xapiand-shaped load (T11), it gives **at least 1.5x the fallback's requests per second or at most
   0.5x its request p99**, while costing **at most 3% at one writer** (T1) and **at most 5% more CPU
   per request**.

## Candidates, one source

| Name | Writers | Order for scans |
|---|---|---|
| C | write group (queue, leader runs stage 1), per-writer lanes | shared sorted runs (main + delta + private tail) |
| A' | same | concurrent skip list, tower inside the record, links by the writers, V after the last link |
| F (fallback, D14) | exclusive lease per transaction, one lane | shared sorted runs |

Common: lanes with 32-bit references, one hash version map (8-byte atomic slots, 7/8 full, no
erase, growth with epoch-stamped retirement), leader-filled 24-byte headers, tombstones and `prev`,
the visible position V, freeze, a minimal view with frozen buffers and stub segments.

## Hypotheses and keep/remove rules

H1 (C vs A'): C's single-writer insert is cheaper than A' (no tower links) and A' wins only on scans
beside writers. Rule from the design: a finalist that misses T1, T2, T4, or T7 is out; A' beats C
only if its T3 p99 is more than 20% better at 4 writers while C misses T3, and A' stays within 10%
of C at one writer.

H2 (multi-writer vs fallback): on request-shaped load over many small indexes the leader handoff
costs more than the lease serialization saves. Kept only if Q3's bar above is cleared.

Tuning experiments (at most three, each one hypothesis, run after the candidates): chosen from what
the data shows; recorded below with the numbers whether they win or lose.

## Procedure

Interleaved trials (at least five, spread stated), strict holds in pieces of at most 20 minutes
(`quiet run bakeoff-<n> 20 ...`), builds as light holds at 4 jobs. RocksDB 11.11.1 through the same
workload code (`lanebuf_rocksdb.cc`) with memtables large enough that nothing flushes, WAL off,
`allow_concurrent_memtable_write` at its default, and through the kv harness rows (fits1, fits4,
fits8) for context. Host: Intel i9-9980HK, 8 cores / 16 threads, macOS x86_64 only; the other
platforms are not measured (caveat).

## Results

(filled in as pieces complete)
