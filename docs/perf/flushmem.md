# Flush and merge working memory (perf/flushmem)

Target: `budget_peak` at 10x RAM (kv bench, Release, macOS x86_64: fillrandom and readrandom v100 1M records 12 MiB; ycsb_a v1024 200k 24 MiB, `lvl-16k-lz4`). Origin: `rss-transient.md` (45 MiB `budget_peak` against a 3 MiB share) and the hunt-list row. Goal: the peak is the budget plus one segment's bounded scratch, segment bytes identical, no throughput loss.

Method: an operator-new interposer (`.scratch/ap.cc`, allocations of 8 KiB or more, 4-frame sites, every allocation and free logged and replayed to the moment of the highest live total), kv_strata on the rebased main (`39c9cc1`, no RSS prototype), under `quiet light` (attribution is not a timing measurement). A/Bs below are under `quiet strict`, interleaved, 3 trials, with an A/A of two copies of the main binary.

## Q1: attribution of the peak

The peak is one large merge finishing, not several merges at once. At the highest live total (read row 59 MiB, of which 7.6 MiB is the bench's own value stream) the output is a 495k-document segment (segment sizes at the end: 495k, 268k, 167k, ... documents; sections 36 MB for the largest). Per output key of that merge, `keys_builder::finish` and the merge around it hold:

| Holder | MiB at 495k keys | Bytes per key | Scales with |
|---|---|---|---|
| `fusefilter::filter8::build` working set | 18.3 | 38 | output keys |
| its input copy (`unique`) | 3.8 | 8 | output keys |
| the caller's hashes (`sorted_encoder::hashes_`) | 3.8 | 8 | output keys |
| `versions_` | 3.8 | 8 | output docs |
| `termdict::builder` output string (`out_.blocks`, doubling capacity) | 8.3 | 17 | output keys |
| merge order (`merge_ref`), docids, ordinals | 3.8 + 3.8 + 1.9 | 20 | output docs |
| segment write buffers (`counting_output`, 64 KiB gather, 1 MiB chunks) | 2 | constant | n/a |

About 100 bytes per output key at the peak, 49 MiB for the largest merge, matching `budget_peak` 53 MiB (the charges lag the allocations: the termdict output was never charged and the filter's charge was `64 n`). Other churn from `rss-transient.md` (segment write buffers 134 MiB, `push_record` 96 MiB, `merge_output::write`) is freed within the call and lives at a few MiB.

## Experiments

Identity for every arm: get digests equal (readrandom `9743db1c4f882e56`, ycsb_a `c33a1d6dd42e3995`), and a forced compaction (`bulkload`) writes the same segment file bytes on main and on the branch (sha of `seg-*.sx`): 300k x 100 B `43ee00f01a033b6a`, 1M x 100 B `1b2fb45c4d91137d`, 100k x 1 KiB `97aa0129946b8423`. The digest, flush, merge-stream and format 14 tests pin the bytes from main; the full suite passes (ctest, 53 tests).

### 1. The key filter takes its hashes over (kept, fusefilter 755b392)

Hypothesis: `filter8::build(span)` copies the hashes (8 B/key) and holds a 16 B/key peel order next to a 8 B/key counting-sort array that is dead by then. Sorting the caller's vector in place and storing the peel order in the dead array (8 B/key hash plus a 1 B slot choice) gives the same filter. Additive: a `build(std::vector<uint64_t>&&)` overload; the span overload forwards to it. Result: serialized filters equal the old code's for 13 sizes from 0 to 1M keys (with duplicates); 445k keys build in 51 ms against 62 ms (n=100k: 9.3 against 11.9 ms; 1M: 127 against 132 ms); peak with the caller's vector 56 to 33 B/key (interposer, `mem.cc`).

### 2. The key dictionary streams its blocks (kept, termdict 8367263)

Hypothesis: `termdict::builder` builds the whole blocks string (doubling capacity: 8.3 MiB for 495k keys, 17 B/key, never charged). An optional sink receives each block as it closes; `keys_builder` opens the keys section on the segment sink and the encoder writes into it (merge: while keys arrive; flush: while `encode` walks the sorted entries), so the keys section never exists in memory. Additive constructor argument; the sink sees the same blocks and index as the whole-string build (test on the random key sets). The charge for the filter build is now `32 n + 1 MiB` (was `64 n + 1 MiB`), against a measured 23 B/key plus the serialized filter and its copy.

Experiments 1 and 2 were built and profiled together: at the peak moment of the read row the filter fell from 18.3 + 3.8 (copy) to 11.0 MiB and the dictionary from 8.3 to 0; the strata total live at the peak from 49 to 29 MiB (interposer, one run each).

### 3. Versions freed before the filter (kept, neutral on `budget_peak`)

A streamed (key-ordered) merge encodes its small versions section first and frees the 8 B/key vector before the filter build. 3.7 MiB less live at the peak in the interposer; inside the noise of `budget_peak` (read 26.4 vs 27.9 MiB, fill 33.2 vs 30.6, ycsb 9.9 vs 9.6, 3 trials, B without it and C with it). Kept: it removes a deterministic 8 B/key from the worst moment and costs nothing.

### 4. Policy when the budget is short (no code, loser as a policy)

Hypotheses: waiting for room, smaller merges, or serializing builds hold the cap. Evidence:

1. Waiting was removed on purpose (`docs/status/merge-wait.md`): a merge sleeping for room that never came took the 2x settle from 0.25 s to 15.3 s.
2. Serializing: kv_strata fillrandom v100 1M with `--background 1` (merges one at a time) against `--background 4`, budget peak 42.5 / 38.8 MiB (main / branch, bg=1) and 41.3 / 36.8 (bg=4): no effect. The peak is one merge finishing, not several overlapping. A 64-index run (`--indexes 64`, 1M records) peaks at about 20 MiB on both binaries and is the write buffers (many small draws, 64 KiB floor per index), with under 6 MiB in allocations of 8 KiB or more.
3. Smaller merges: a merge's scratch is 61 B per output key; a 3 MiB share holds a 50k-key merge, so a cap would mean no segment over ~50k keys, which multiplies segments (and the read path's per-segment cost) for every index. Not tried: it trades the disk and read axes for the memory one.

So admission cannot hold the cap on this workload; only a smaller per-merge scratch can. The charge is now accurate (budget peak 32 MiB against 29 MiB live in the interposer on the fill row; before, the dictionary was uncharged and the filter charged at twice its need), so `pressure()` sees the real draw.

## Results

kv_strata Release, macOS x86_64, 16 CPUs, `quiet strict`, 5 interleaved trials per arm; median [min-max]. A is main `39c9cc1`, A2 a second copy of it (the A/A), C the branch (experiments 1 to 3). Memory in MiB, throughput in operations per second (run phase), lower is better for memory and seconds.

| Row | Arm | `budget_peak` | peak RSS | dirty footprint | secs | run ops/s |
|---|---|---|---|---|---|---|
| fillrandom v100 1M, 12 MiB | A | 36.0 [35.4-45.7] | 152 [140-196] | 22 [20-27] | 2.92 | 437,253 [436,594-444,268] |
| | A2 | 47.8 [35.7-50.1] | 183 [134-201] | 26 [23-35] | 2.96 | 439,344 [430,777-449,253] |
| | C | 25.3 [22.9-30.8] | 158 [130-174] | 19 [17-28] | 2.83 | 453,414 [445,396-476,210] |
| readrandom v100 1M, 12 MiB | A | 38.1 [35.5-50.5] | 188 [179-205] | 20 [16-30] | 3.04 | 383,899 [362,979-386,466] |
| | A2 | 41.5 [35.5-47.6] | 188 [173-198] | 20 [17-30] | 3.15 | 377,279 [362,447-386,319] |
| | C | 23.8 [22.8-29.9] | 171 [163-190] | 21 [20-25] | 3.11 | 380,096 [358,250-396,209] |
| ycsb_a v1024 200k, 24 MiB | A | 11.3 [11.2-14.7] | 243 [218-247] | 59 [57-64] | 1.98 | 231,919 [226,974-251,885] |
| | A2 | 11.5 [10.5-13.0] | 234 [227-238] | 63 [61-64] | 1.94 | 236,736 [231,035-242,852] |
| | C | 9.7 [9.3-12.3] | 233 [226-243] | 60 [55-64] | 1.87 | 230,380 [221,819-236,486] |

Ratios (branch against main, medians): `budget_peak` 30% smaller on fill (25.3 against 36.0), 38% smaller on read (23.8 against 38.1), 14% smaller on ycsb_a (9.7 against 11.3). The A/A spread on fill and read is 36-48 and 38-42 MiB (merge timing moves the peak), and the branch's ranges (22.9-30.8, 22.8-29.9) do not overlap main's on those two rows; ycsb overlaps slightly. A forced compaction of one 1M-key segment (bulkload, one merge, no timing noise): 101.0 to 61.0 MiB (40% smaller); 300k keys: 31.0 to 19.0 MiB (39% smaller); 100k x 1 KiB: 11.0 to 8.2 MiB. That is 101 to 61 B per output key.

Other axes: throughput is unchanged (run phase equal within the spread; the fill run is 3.7% faster, inside A2's drift, and the filter build itself is 0 to 18% faster); disk is unchanged (same bytes); indexing time unchanged (load rate 441,921 against 448,264 ops/s on read). Peak RSS and the dirty footprint do not move (macOS keeps the freed medium blocks resident, as `rss-transient.md` found), so this change shrinks the charged working memory and the transient allocation sizes, not the RSS of this bench.

What it saves: no CPU or power (the same work, a little less of it). The memory is transient: about 40 B per key of every concurrent large merge, 20 MiB for the largest merge here, so a host merging four 500k-key segments at once holds about 80 MiB less at the peak. At the many-indexes scale the effect is on host sizing headroom, not on the steady state, and in dollars it is negligible on its own.

## What is left (Next)

1. The cap still does not hold: one merge needs 61 B per output key (hashes 8, merge order and docids 16, ordinals 4, the filter build 24, section buffers 2 MiB), 29 MiB for the 495k-key segment against a 3 MiB share. Bounding it needs a smaller filter scratch: partition the key filter by hash prefix into filters of at most 64k keys (the scratch drops to the 8 B/key hash list plus a constant; about 4% more filter bytes on 64k-key partitions; a format change, so the bytes change and it needs its own proof), and build it at the tail after the merge order is released (saves 16 B/key more, an API change in `section_sink`).
2. Linux (glibc, x86_64 and arm64) and FreeBSD are not measured. The change is allocation-size only, so the budget numbers should hold; the RSS effect (glibc returns freed large blocks) is the open question.
3. Detent links fusefilter and termdict: both changes are additive; its pins stay until it bumps them.
