> Status: CURRENT (2026-10-28) regression check after the week's landings; raw files in each repo's `.scratch/regress-2026-10-28/`.

# Regression check, 2026-10-28

Question: did the week's landings (write buffer, background deletes, flushmem, the values section,
the field table, cold-tier step 1, the collector fixes) regress strata or prospect? No row got
slower beyond its A/A spread.

## strata kv (macOS, Release, 1 thread, 256 MiB, baseline strata:7aad7c4, main strata:93c96fd)

| Row | Baseline | Main | Change | Verdict |
|---|---|---|---|---|
| fillrandom 100 B | 1,646,200 ops/s | 1,612,294 ops/s | 1.05x slower | same (A/A 0.84–1.03) |
| readrandom 100 B | 773,862 ops/s | 770,394 ops/s | same | same |
| ycsb_a 1 KiB | 548,785 ops/s | pooled 0.989 | same | same (screen 1.17x slower did not reproduce) |
| peak RSS, fill / read / ycsb_a | 49.9 / 57.5 / 184.8 MiB | 43.1 / 47.5 / 180.9 MiB | 14% / 17% lower / same | better |
| disk | 7.21 / 68.88 MB | 7.21 / 68.91 MB | same | same |

## prospect (gate VM, 1M corpus, keyed, 1 thread, baseline strata:8110f1d, main prospect:becfe3c)

| Row | Baseline | Main | Change | Verdict |
|---|---|---|---|---|
| index wall, median of 5 | 29.84 s | 26.86 s | 1.11x faster | faster (A/A 0.2%) |
| indexing peak RSS | 485.7 MiB | 461.9 MiB | 4.9% smaller | smaller |
| serving RSS idle / after queries | 17 / 181 MB | 17 / 181 MB | same | same |
| disk, natural | 641.9 MB | 646.1 MB | 0.65% larger, repeatable | slightly larger (cause not found; gone after merge) |
| disk, merged | 576,286,832 B | 576,287,532 B | +700 B | same |
| TOP_10 / COUNT / TOP_100_COUNT | | | 1.06x / 1.05x / 1.18x faster | faster |
| other query rows | | | 0.97–1.03 | same |

`dump` output identical (same SHA-256 over 5,772 lines).

Against the field, same harness: prospect indexes 3.9x faster than Tantivy 0.26 (103.8 s) and 6.0x
faster than Lucene 10.4.0 (160.4 s) at about a sixth of their indexing RSS (Tantivy 2.8–3.2 GB, Lucene
2.4–2.8 GB); disk Tantivy 682.6 MB, Lucene 624.3 MB; queries 0.83–1.04x Tantivy's time and 0.68–0.93x
Lucene's on every row.

Open: the natural-index +0.65% disk; per-section natural sizes (stale segment files broke the read);
strata query classes and indexing were not in this run.
