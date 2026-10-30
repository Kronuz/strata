> Status: HISTORY (2026-10-18) code not on main; measured before formats 13 and 14, verify before citing.

# Reorder speed: a faster bisection for the reordering merge

**Target.** The reordering merge (docs/design/reordering.md D4). On the 5M quick-tier
calibration a force merge with renumbering takes 2,678 s on one thread and 846 s with
`reorder_threads` at 8 (355% CPU, 6.03 GB peak RSS), against 404 s in docid order; Lucene
`-bp`'s whole merge takes 595 to 652 s at about 177% CPU (phase-3-ratchets.md, T3).
Renumbering buys ranked queries 1.10 to 1.13× of the best reference instead of 1.37 to
1.45×, so the order must stay; its cost must shrink.

**Where the time goes.** `sample` over a one-thread 1M force merge (base, `d8d219c`):
457 s, of which bisection 376.8 s (82%), the forward index 11.2 s, and the rest of the
merge 69.4 s. Inside bisection, 93% of the samples sit in `bisect_part`'s gain loop (the
inlined `move_gain`: four fixed-point logarithms per edge per iteration) and 5% in the
two `std::sort` calls. The 1M graph: 4,587 graph terms (min_df 4096, as Lucene), 92.3M
edges, about 1.07 bytes an edge in the forward index; 15 levels down to 32 documents,
and nearly every part runs all 20 iterations (18 to 20 on average at every depth).

## Procedure

- Corpus: the first 1M documents of the 5M Wikipedia corpus, indexed once by the base
  binary (7 segments, 1.2 GB), cloned (APFS `cp -c`) for each run.
- The merge: `strata_engine merge DIR` (force merge to one segment, reordering), timed
  with its phases (`reorder::phase_times`: forward index, bisection, the rest).
- Bisection alone, for quick iteration: a scratch tool (`bisect_bench`) that writes the
  forward index of the same segments into memory and runs `bisection::order`, printing
  the times and an FNV-1a hash of the order.
- An exact change keeps the order's hash and the merged segments' bytes; then postings
  size and query speed are unchanged by construction. An inexact one is measured on
  postings bytes (exact, `strata_engine sections`) and on query speed (quick sample,
  `bench/quick/ab.sh`) against the docid-ordered index of the same size.
- One thread under a light hold; eight threads under a strict one. Interleaved rounds.

## Hypotheses, in order

| # | Change | Kind | Keep if |
|---|---|---|---|
| 1 | Graph terms by df cutoffs (Lucene's) | — | already there: min_df 4096, max_df 1.0 |
| A | A term's move gain once per iteration and side, not per edge | exact | faster, same bytes |
| E2 | Degree steps from a table; nl and nr's logarithms hoisted | exact | faster, same bytes |
| E3 | Prefetch rows ahead (offset, then the row) | exact | faster, same bytes |
| E4 | Sort the gain lists only as far as the swaps reach | exact | faster, same bytes |
| 2a | Fewer iterations per part | inexact | faster; postings and queries within noise |
| 2b | Leave larger parts (64, 128 documents) | inexact | the same |
| P | Top levels' gain loops on all threads | exact | faster at 8 threads, same bytes |
| 3 | Start from the sources' order when they are reordered | inexact | not measured here (see below) |
| 5 | Forward index layout | exact | prefetching instead (E3; see below) |

**Verdicts.** Kept: A, E2, E3, E4, P (exact, the base's order) and parts of 64 (2b).
Removed: fewer iterations (2a: +0.11% to +0.28% postings, above the noise floor) and
parts of 128 (2b: top-10 queries 1.05× to 1.12× slower). Idea 1 was already in place.

## Results

Host: the Mac (i9-9980HK, 8 cores, 32 GB, macOS 15), Release, `-march=native`.

### Bisection alone, one step at a time

`bisect_bench` on the 1M segments; the order's hash is the base's (`b337cec109e2ff6f`)
through P. A to E4 are single runs taken while another agent's one-thread VM job ran
(15:28 to 16:08), so they compare with each other; P and F are interleaved later runs.
E3's run prefetched in the gain loop only; the degree loop's prefetch came with E4's.

| Step | Commit | 1 thread | 8 threads | Order |
|---|---|---:|---:|---|
| base | `d8d219c` | 376.8 s in the profiled merge | | |
| A: gain once a term and side | `068b9ac` | 142.1 s | | same |
| E2: step table, hoisted logs | `12add4e` | 135.5 s | | same |
| E3: prefetch rows | `e4636ea` | 105.0 s | | same |
| E4: partial sorts | `69c6fa1` | 80.0 s | 16.55, 13.61, 13.98 s | same |
| P: top levels on all threads | `ba8ec0e` | 55.2, 53.5, 54.5 s | 13.37, 13.11, 12.70 s | same |
| F: parts of 64 | `ce011a7` | 47.8, 45.6, 48.1 s | | new |

After E4 the per-depth profile is flat: about 4.7 s a depth at depths 0 to 10 (gain
loop 3.7 s, two nanoseconds an edge, decode-bound), rising to 9.0 s at depth 14 where the
swaps (18 a part-iteration in parts of 61) and the per-term gains dominate.

P's eight-thread gain is small (5 to 10%): the top three levels were about 6.6 s of
serial-ish work and now run split, but the counts, the swaps, and the laptop's all-core
clocks (the same work runs 20 to 30% slower per thread with eight busy) stay.

### Inexact candidates: postings bytes at 1M

Merged at eight threads (the order does not depend on the thread count). Postings are
exact bytes; their spread among near-equal settings (18, 19, 20 iterations) is
-0.001% to +0.047%, the noise floor used here.

| Setting | Postings | vs default | Positions | Bisection (8 threads) | Kept |
|---|---:|---:|---:|---:|---|
| docid order | 218,774,896 | +14.7% | 367,665,001 | | |
| 20 iterations, parts of 32 (base) | 190,781,607 | | 348,090,074 | 9.3 s | |
| 19 iterations | 190,872,219 | +0.047% | 348,165,103 | | |
| 18 iterations | 190,780,377 | -0.001% | 348,073,680 | | |
| 16 iterations | 191,073,321 | +0.153% | 348,198,921 | 11.2 s | no |
| 10 iterations | 191,193,583 | +0.216% | 348,080,526 | 6.0 s | no |
| 6 iterations | 190,999,712 | +0.114% | 348,115,647 | 4.5 s | no |
| 4 iterations | 191,321,805 | +0.283% | 348,140,472 | 4.9 s | no |
| parts of 64 | 190,816,346 | +0.018% | 348,102,616 | | **yes** |
| parts of 128 | 190,702,940 | -0.041% | 348,190,679 | | no (queries) |

(Bisection times in this table come from back-to-back runs on a warming laptop and are
only indicative; the kept series below is interleaved.)

Fewer iterations is a real trade, not noise: six iterations save about half the
bisection's CPU (user 62 s against 99 s for the whole merge at eight threads) for
+0.11% postings, above the noise floor, so by this item's rule it is removed. It is the
owner's call whether +0.1% postings buys half the bisection.

### Query speed: the quick sample at 1M

`bench/quick/ab.sh`, 6 rounds, 181 gate queries (`sample_queries.py`, 40 a class) and
the 200 stratified terms; B/A is the geometric mean of the best times, with its 95%
bootstrap interval. A is the base order (20 iterations, parts of 32).

| Command | Set | A/A | Parts of 64 | Parts of 128 |
|---|---|---:|---:|---:|
| TOP_10 | queries | 1.003 | 1.002 | 1.047 (1.033–1.059) |
| TOP_10 | terms | 1.003 | 1.003 | 1.121 (1.110–1.131) |
| TOP_100 | queries | 1.003 | 1.003 | 0.965 |
| TOP_100 | terms | 1.004 | 0.999 | 1.005 |
| TOP_1000 | queries | 0.997 | 1.000 | 0.991 |
| TOP_1000 | terms | 0.997 | 1.005 | 1.038 |
| TOP_100_COUNT | queries | 1.000 | 0.999 | 1.005 |
| TOP_100_COUNT | terms | 1.002 | 1.005 | 1.035 |
| COUNT | queries | 1.001 | 1.000 | 1.001 |
| COUNT | terms | 1.005 | 1.009 | 1.002 |

Parts of 64 stay within the A/A's spread (COUNT terms 1.009 against the A/A's 1.005,
intervals overlapping); parts of 128 lose top-10 speed, so the order inside a postings
block matters at that scale even though the bytes do not.

### The kept series: 1M force merges, interleaved

`strata_engine merge`, base (`d8d219c`'s bisection with `9be7931`'s timers) against F
(`ce011a7`), three rounds each, sides alternating; docid order from F's binary with
`STRATA_REORDER=0`. Every run's merged segment is byte-identical to its side's others,
at either thread count; docid-order segments from the base and F binaries are
byte-identical (`cmp`).

| Mac | Merge | Forward | Bisection | The rest | User CPU | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| base, 1 thread | 311.9, 311.8, 312.0 s | 6.1 s | 266.1, 265.3, 263.3 s | 40.8 s | 304.2 s | 1.52–1.53 GB |
| F, 1 thread | 94.8, 95.2, 87.4 s | 6.1 s | 48.2, 48.1, 42.5 s | 40.2 s | 85.3 s | 1.53–1.54 GB |
| base, 8 threads | 103.0, 104.7, 104.6 s | 5.3 s | 57.0, 57.4, 58.0 s | 41.4 s | 290.8 s | 1.54–1.56 GB |
| F, 8 threads | 53.1, 54.5, 55.5 s | 5.3 s | 7.6, 7.7, 8.5 s | 41.1 s | 88.4 s | 1.55–1.56 GB |
| docid order | 38.5, 39.2, 39.2 s | | | 39.0 s | 29.8 s | 1.46–1.48 GB |

Bisection is 5.7× faster on one thread and 7.2× on eight; the merge 3.4× and 1.9×. The
reordering's cost over docid order falls from 273 s to 54 s on one thread and from 65 s
to 15 s on eight, and its CPU from 261 s to 59 s at eight threads. (The profiled base run
in the first section, 457 s, ran under `sample` while another agent's VM job ran.)

### The gate VM against Lucene `-bp`, 1M

`tools/vm/linux.sh` (Ubuntu 24.04, 8 vCPUs under HVF on the same Mac), clang Release
`-march=native`, a 1M strata index of 10 segments built there, one run each under a
strict hold. Lucene is `lucene-10.4.0-st` with `KEYED=1 BP=1` (the reference `-bp`
builder's settings); its merge runs from the "Merge" line to the build's end, its CPU
from `/proc` at that line to the end.

| Gate VM, 1M | Merge | Bisection | CPU | Peak RSS | Output |
|---|---:|---:|---:|---:|---:|
| strata base, 8 threads | 115.8 s | 61.8 s | 347.7 s | 1.37 GB | 579,583,160 B |
| strata F, 8 threads | 58.3 s | 10.8 s | 117.2 s | 1.33 GB | 579,636,400 B |
| strata F, 1 thread | 94.7 s | 46.8 s | 94.5 s | 1.32 GB | 579,636,400 B |
| Lucene `-bp`, 256 MB buffer (5 sources) | 103.1 s | | 164.1 s | 1.81 GB (whole build) | 574,890,519 B |

At its default 2,047 MB buffer Lucene holds all 1M documents in one in-memory segment:
"Merge" finds no flushed segment, so its 24.6 s is a flush with neither a merge nor
reordering (624 MB, unreordered). The 256 MB buffer gives it 4 flushed segments and the
buffer, a real reordering merge. Against it, strata's merge is 0.57× its wall time on
eight threads and 0.92× on one, at 0.71× and 0.58× its CPU. The VM's sizes match the
Mac's to the byte.

### At the project's scale

The 5M merge was not rerun. The base's own scaling from 1M to 5M (its reordering cost,
2,274 s on one thread and about 390 s on eight at 5M on the gate VM, against 266 s and
62 s at 1M) is about 8× on one thread and 6× on eight. Applied to F's 1M costs on the VM
(52 s and 15 s), a 5M force merge should take about 830 s on one thread and 500 s on
eight, from 2,678 s and 846 s, under Lucene's 595 to 652 s. These are projections for
the gate's T3 row to measure. The CPU saved, about 1,800 CPU-seconds per 5M reordering
merge on one thread, is half a CPU-hour: about 2 US cents at $0.04 a vCPU-hour, or 5 Wh
at 10 W a core. Negligible per index; it matters where merges run all day under query
load, which is the owner's point.

### Not done here

- **Start from the sources' order** (idea 3). It applies when the sources are already
  reordered (the `merges` mode, repeated force merges); the measured case merges
  docid-ordered flushes, so it needs its own benchmark. Untested.
- **Forward index layout** (idea 5). Prefetching (E3) took the stalls that the layout
  causes; what is left is the LEB128 decode, about 2 ns an edge. Fixed 16-bit rows would
  remove the decode branch but double the forward index (99 MB to 185 MB at 1M, file
  pages that count in RSS). Untested.
- **The executor** (idea 4's "use strata's existing executor"): see the ledger's
  "Blocked on".
