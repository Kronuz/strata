# Index size and merging during indexing

Targets Phase 3's disk row (PLAN, "Disk": the whole index and every section at ≤ 1.0× the
smaller baseline) as T4 measures it, the index each engine leaves after indexing, and the
merging that decides how many segments that index has. Every experiment also reports the
other three axes: indexing time (T4), peak RSS, and write amplification (T3).

## Evidence: the n1017 gate run

5M documents, keyed, at equal memory (`STRATA_BUDGET_MB=2056`, calibrated to Lucene's peak
RSS; raw files on the gate VM, `~/bench/results/index-n1017-t4-{1,8}`, `wamp-t3-*`):

| Engine | Bytes | Segments | Notes |
|---|---:|---:|---|
| strata, 1 thread | 4,850,403,469 | 9 | no merge inside the run |
| strata, 8 threads | 8.02 GB | 14 | |
| Lucene 10.4.0, 1 thread | 2,819,677,322 | 1 | `forceMerge(1)` inside the timed run |
| Tantivy 0.26, 1 thread | 3,066,216,363 | 1 | merges every segment inside the timed run |

T3 at 500k documents, append-only: strata 329.7 MB in one segment (write amplification
1.014), Lucene 320.9 MB (1.498), Tantivy 352.7 MB (2.011). At 20k: strata 1.069, Lucene
1.073, Tantivy 2.166.

What the code says before anything is measured (main at `4c9ebfd`):

1. The engine passes no executor, so merges run inline on the owner thread, and the tiered
   policy (8 per tier, 16 MiB floor, 5 GiB maximum) leaves 9 segments of about 540 MB alone:
   `allowed_segments` gives 9 for 4.85 GB whose smallest is 540 MB.
2. A merged-away segment's file is queued for reclamation only by `retain()` after a commit
   (`index.hh:1869-1925`), and a commit frees at most `reclaim_slice`, 64 MiB
   (`index.hh:395`). The engine commits once, at the end, so every source merged during
   the run stays on disk when the bytes are counted, less 64 MiB.
3. The baselines' bytes are a force-merged single segment; strata's are the segments its
   policy left. The disk row in PLAN is after a full merge, where strata passes at 0.999×
   keyed Lucene `-bp` (`phase-3-ratchets.md`, "Disk after a full merge").

## E1: where the bytes are (measurement, no code change)

Hypotheses, each with what would refute it:

- H1 (per-segment overhead): splitting 100k documents into N segments with merges off costs
  each extra segment a fixed amount (dictionary blocks, partial postings blocks, skip data,
  headers, filters) that, scaled to 5M at N = 9, explains less than a quarter of the
  4.85 − 3.3 GB gap. Refuted if the per-segment cost, scaled, explains more than half.
- H2 (unreclaimed sources): with merges on, the directory holds files no manifest names,
  and at 5M they are most of the gap. Refuted if the directory equals the manifest's files.
- H3 (encoding): one segment of 100k documents is within 5% of Lucene's force-merged 100k
  index in total, but some sections are above their counterparts. Refuted if the whole is
  more than 10% above.

Procedure: `strata_engine` at `4c9ebfd`, Release, macOS x86_64 (bytes do not depend on the
platform: zstd is pinned), the first 100,000 documents of the gate corpus, keyed.

1. One segment: `STRATA_BUDGET_MB=2056`. `sections` on the segment, and every file.
2. N segments, merges off (`STRATA_AUTO_MERGE=0`), budgets that give about 9 and 14
   segments. Per-section totals against step 1.
3. The same budgets with merges on: segments left, the manifest's files against the
   directory.
4. Lucene and Tantivy at 100k on the gate VM (after the T3 run, which counts that disk's
   writes): Lucene by file extension, force-merged and with `NO_MERGE=1`; Tantivy by file
   extension, merged and with `NO_MERGE=1`.

Bytes are exact single measurements; no hold. Kept as the attribution the next experiments
start from; nothing to remove.
