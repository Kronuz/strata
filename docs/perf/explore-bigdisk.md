> Status: SUPERSEDED (2026-10-21) the reclaim backlog it found is fixed on main by the background reclaim (docs/perf/reclaim-bg.md, docs/perf/reclamation.md).

# big1 disk after settle (explore/bigdisk)

Hypothesis: the 2.6-2.9x disk gap at 10x RAM is obsolete segment files still queued for reclaim when the row ends, not live data.

Confirmed on main d1bbc42 (Release, kv_strata, durability none, one commit at the end):

| row | after settle, baseline | reclaim drained | live segments |
|---|---|---|---|
| fillrandom v100, 1M, 12 MiB | 184-208 MB, 193-210 files pending in reclaim | 72.3 MB, 7 files | 72 MB, 0 dead |
| ycsb_a v1024, 200k, 24 MiB | 292-295 MB | 135-139 MB | 141 MB (13% dead docs, about 18 MB) |

Cause: with durability none no commit happens during the run, so merged-away segments are queued only at the end; commit() reclaims reclaim_slice (64 MiB) per call and settle makes at most two commits. Settle never calls reclaim().

Prototype: bench/kv/kv_strata.cc, settle() calls ix->reclaim() when KV_SETTLE_RECLAIM is set; KV_SEGREPORT prints per-segment docs/dead/section bytes and reclaim_pending.

A/A/B/B (A = baseline, B = drained), digests identical (fillrandom get digest 1ac557eb7ac4608e, hits 1000000; ycsb_a c33a1d6dd42e3995, hits 50259):
- disk: A 189/202 MB vs B 72.3/72.4 MB (fillrandom); A 292/295 vs B 135/139 (ycsb_a). A/A spread about 7%, B/B under 3%.
- run-phase throughput unchanged (fillrandom 392k/376k vs 394k/386k ops/s; ycsb_a 216k/221k vs 224k/226k).
- settle cost: the drain adds a few hundred ms (unlinks), well inside the timed settle.

Fix to finish: the driver's settle (and any "settled" state a user sees) should drain the queue; consider whether commit()'s slice should scale with queued bytes. Live data is already at RocksDB's level (72 MB vs 67-116). Remaining ycsb_a gap, if any, is the 13% dead docs plus no compression; not pursued.
