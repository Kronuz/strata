# Peak RSS at 10x RAM: the transient allocations (explore/rss)

macOS x86_64, Release, `bench/kv` readrandom v100 1M records 12 MiB (plus fillrandom and ycsb_a v1024 200k 24 MiB), `lvl-16k-lz4` settings as in `remeasure-2026-10-26.md`. Strict hold, 3 interleaved trials per cell; A is main behaviour (switch off), B the change. Every get digest is identical between A and B (readrandom `9743db1c4f882e56`, ycsb_a `c33a1d6dd42e3995`; fillrandom has no gets). "secs" is total run time (lower is faster).

## Q1: where the resident arenas come from

An operator-new interposer (`.scratch/rss/allocprof.cc` in the main checkout; allocations >= 8 KiB, by 3-frame site) on readrandom v100. It counts what was requested, so it overstates resident bytes. Peak concurrently live >= 8 KiB: 58 MiB, against 190-220 MiB peak RSS. Churn over the run (each is freed within the flush or merge that made it), by cumulative bytes:

| Site | Cumulative | Avg size | Peak live |
|---|---|---|---|
| `counting_output` / `section_stream::pass` (segment write buffers) | 134 MiB | 166 KiB | 1.3 MiB |
| `fusefilter::filter8::build` from `keys::sorted_encoder::finish` | 116 + 108 MiB | 77-370 KiB | 19 MiB |
| `termdict::builder::cut` | 105 MiB | 97 KiB | 13 MiB |
| `push_record` (flush) | 96 MiB | 69 KiB | 1 MiB |
| `merge_output::write` / `merge_layout` / `keys_plugin::merge` | 90 + 63 + 42 MiB | 0.6-1.1 MiB | 5-12 MiB |

So the freed-but-resident arenas are the flush and merge working buffers (key filter build, term dictionary, segment output, merge output), not read buffers: the readrandom phase allocates almost nothing. All of it is `flush_charge`d but drawn "past the capacity if it must", so `budget_peak` 45 MiB is real concurrent flush plus merge working memory, not a bug in the counter. Blocks of 16 KiB to 128 KiB land in MALLOC_MEDIUM, which libmalloc keeps resident after free.

## Q2: switches tried (readrandom v100, peak RSS MiB, secs)

| Change | A (off) | B (on) | Direction |
|---|---|---|---|
| A/A (control) | 192, 193, 199 | 216, 204, 206 | noise 192-216 (the peak is bursty) |
| `malloc_zone_pressure_relief` every 20 ms from a thread | 204, 201, 180 / 3.4, 3.1, 3.0 s | 131, 132, 170 / 3.6, 3.8, 3.9 s | RSS 20-35% lower, 15-25% slower |
| Same, from flush and merge completion (`STRATA_RELIEF=0`, `relief.hh`) | 184, 222, 190 | 209, 164, 154 | ~15% lower, noisy, 15-40% slower (3.1 to 3.5-4.4 s) |
| Same, rate-limited to 20 ms (`STRATA_RELIEF=20`) | 188, 192, 210 / 3.1, 3.0, 3.2 | 149, 153, 178 / 3.5, 3.5, 3.8 | ~20% lower, ~15% slower |
| `MallocMaxMagazines=1` | 189, 211, 208 | 205, 183, 188 | noise |
| **`MallocSpaceEfficient=1`** | 197, 185, 204 / 3.1, 3.0, 3.1 | 134, 125, 135 / 3.2, 3.3, 3.2 | **RSS 35% lower (tight), ~4% slower** |

`MallocSpaceEfficient=1` (libmalloc, read at process start) on other rows:

| Row | A RSS | B RSS | A secs | B secs |
|---|---|---|---|---|
| fillrandom v100 | 142, 142, 175 | 92, 104, 112 | 2.8, 2.8, 2.9 | 3.2, 3.2, 3.1 |
| ycsb_a v1024 | 233, 224, 241 | 203, 203, 188 | 1.9, 1.9, 1.7 | 1.9, 1.9, 1.9 |

Fill: 30% lower RSS, ~12% slower. YCSB A: 14% lower, ~5% slower on one trial, equal on others. Readrandom 190 to 131 against RocksDB's 122 in the re-measure (1.6x larger before, ~1.07x larger after; RocksDB was not re-run under the same env). It is an environment variable, so it cannot be set from inside a library: the launcher (kv bench, Xapiand) must export it before exec. The explicit relief call is the in-process alternative but costs more time than the env var for less RSS.

## Q3: fairness of mapped segments

From the re-measure (not re-run): strata readrandom RSS 213 MiB against 40.5 MiB physical footprint (dirty); RocksDB 131 against 53.3. ycsb_a v1024: strata 248 against 66.1, of which 117.8 MiB is clean mapped segment pages (evictable under pressure; RocksDB reads through pread and reports 0 mapped). So RSS charges strata for clean file pages RocksDB keeps in the OS page cache outside its RSS; footprint is the fair comparison, and there strata is below RocksDB on readrandom (40.5 against 53.3 MiB) and above on ycsb_a.

## Verdict

Single change that fixes most of it: return freed medium-size arena pages to the OS. The cheapest form is launcher-side `MallocSpaceEfficient=1` (about 30-35% lower RSS on fill and read, 0-12% slower). It does not reach RocksDB on fill (92-112 against 70) or ycsb_a. The remaining gap is real concurrent working memory (45 MiB `budget_peak` on top of the 12 MiB cap), which needs fewer simultaneous flush and merge buffers or streamed filter and term-dictionary builds; that is a design change, hand to the performance engineer. Linux (glibc `malloc_trim`, `M_MMAP_THRESHOLD`) not measured.

Prototype code: `include/strata/core/relief.hh` and two guards in `index.hh` (off unless `STRATA_RELIEF` is set), `KV_RELIEF_MS` thread in `bench/kv/kv_strata.cc`.
