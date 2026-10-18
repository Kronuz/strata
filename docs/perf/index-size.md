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

### E1 results at 100k and 1M (macOS x86_64, `4c9ebfd`)

One segment at 100k documents (the gate budget never fills), each engine keyed; Lucene's
flushed segment is a compound file, split by its `.cfe` entries:

| Section | strata | Lucene 10.4.0 | Tantivy 0.26 | strata / smaller |
|---|---:|---:|---:|---:|
| Postings (`.doc`, `.idx`) | 15,971,722 | 14,579,635 | 17,102,355 | 1.095× |
| Positions (`.pos`) | 22,446,072 | 22,831,251 | 24,795,243 | 0.983× |
| Terms (`.tim` `.tip` `.tmd`, `.term`) | 4,309,207 | 3,740,646 | 4,414,539 | 1.152× |
| Stored (`.fdt` `.fdx` `.fdm`, `.store`) | 316,696 | 638,182 | 1,005,343 | 0.496× |
| Norms (`.nvd` `.nvm`, `.fieldnorm`) | 100,000 | 100,162 | 200,113 | 0.998× |
| Doc values (`sort_field`) | — | 400,231 | 400,187 | |
| Lengths (excluded, BENCHMARKS) | 187,681 | — | — | |
| `pk_filter` | 118,897 | — | — | |
| Whole directory | 43,493,711 | 42,291,967 | 47,919,012 | 1.028× |

The same 100k documents in N segments (merges off), against one:

| Section | 1 segment | 15 segments | 22 segments |
|---|---:|---:|---:|
| Postings | 15,971,722 | 16,702,996 (+4.6%) | 16,660,918 (+4.3%) |
| Positions | 22,446,072 | 24,293,607 (+8.2%) | 24,708,313 (+10.1%) |
| Terms and term index | 4,309,207 | 9,737,981 (2.26×) | 11,139,101 (2.58×) |
| Everything else | 766,760 | 786,320 | 799,004 |
| Whole | 43,493,711 | 51,521,335 (+18.5%) | 53,307,906 (+22.6%) |

With merges on at the same budgets the tiered policy leaves 8 segments (47.95 and
47.81 MB, +10%), and the directory holds only the manifest's files: at this size the
commit's 64 MiB slice frees every merged-away source.

H2 at 1M documents (`STRATA_BUDGET_MB=490`, merges on, 21 flushes): the manifest names 5
segments, 637,293,248 bytes; 14 merged-away segment files, 511,283,280 bytes, are still on
disk. The directory is 1.80× the index. At the gate budget (2056 MB) the same 1M
documents flush 5 times (186k to 286k documents each, by the budget's estimate of each
document) and nothing merges, so 5M makes about 21 flushes and the policy merges them to
9 segments: H2 applies there.

Findings so far:

1. H1 holds at 100k: splitting costs mostly duplicated dictionaries (each segment holds
   its own copy of the common vocabulary) and positions (more short lists in VByte tails).
2. H2 holds at 1M; its size at 5M is the next measurement.
3. H3 holds: one segment at 100k is 1.028× Lucene's whole; postings (1.095×) and terms
   (1.152×, mostly keys: D12's section in format 13) are the sections above their
   counterparts.
4. The equal-content table has one asymmetry: both baselines store `sort_field` as doc
   values (400 KB here, 20.1 MB at 5M), strata's engine does not index it.

## E2: reclaim at the end of the run

Change (`tools/strata_engine.cc`): after its one commit, `index` calls `ix.reclaim()`,
which frees every queued obsolete file (the commit frees only `reclaim_slice`, 64 MiB),
and prints the bytes freed and the time it took. Lucene's `IndexWriter` deletes
unreferenced files at commit and close (`IndexFileDeleter`), and Tantivy's builder runs
`garbage_collect_files`, both inside their timed runs, so the three directories then hold
only their indexes.

Hypothesis: the directory equals the manifest's files at every size, and the extra time
is under 1 s at 5M (7 to 130 ms per GiB freed, `reclamation.md`). Keep if the bytes fall to
the manifest's total and the run's time does not grow by more than the reclaim's own
printed time; remove otherwise. Bytes at 1M (`STRATA_BUDGET_MB=490`) and 5M (2056); no
hold for bytes.

Library finding, not changed here: `drain()` frees only queued files, and a merged-away
source is queued by `retain()` after the next commit. A storage-budgeted index that
ingests between commits can refuse a transaction (`no_space`) with merged-away sources on
disk. Queueing a source at `install_merge` when no retained manifest names it would let
admission drain it; `index.hh` is under the layers refactor, so this goes to its owner.
