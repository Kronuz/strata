# Merges that stream (perf/merge-stream)

Targets: peak RSS and the memory budget's peak on big1 (fillrandom and readrandom at 100 B,
1M records, M = 12 MiB; ycsb_a at 1 KiB, 200k records, M = 24 MiB), without moving fits1.
Origin: the explorer's `docs/perf/bigmem.md` (explore/bigmem, 7eb2c11): a merge builds its
output sections in memory, charged to the budget without waiting, so working memory grows with
the merged segment (budget peak 96-419 MiB against a 3-6 MiB share).

Goal: a merge's working memory is a few blocks plus the key builders' state, not the merged
segment; segment bytes identical to main's for the same inputs; no format change.

Harness: `.scratch/ms/abm.py` (interleaved trials of run.py's rows, A/A first, six trials),
kv_strata Release, macOS arm64, `quiet run merge-stream` (strict). Identity: `KV_GET_DIGEST`
on every run, and `test/merge_stream_test.cc` (digests of merged files pinned from main).

## Profile (main, trace of `flush_charge` held/peak per merge entry)

The stored plugin's merge dominates. On ycsb_a a merge that writes a 40 MB segment holds
141 MB after its stored entry and peaks at 219 MB; keys hold 1-2 MB per 10k docs. The cause is
`stored_builder`: every record is copied into `stored_` (doubling growth), the whole section is
encoded into one string (doubling again), then `append_built` writes it, and the charge is not
returned until the merge ends. On fillrandom (100 B) stored is again the bulk (80 of 98 MiB
at 22 MB of output); keys add ~30 MB per 300k docs (the filter's `64 n + 1 MiB` build set).

## Experiments

### 1. Stream the stored section (kept)

Hypothesis: gathering one block of records at a time and writing each frame through the
section stream, keeping only the 16-byte index entries, removes the copy and the encoded
string, so the stored entry holds a block. Bytes cannot change: the block-closing rule
(count at the target, or bytes past it) is applied record by record instead of by lookahead.
Flushes use the same frame writer, so their encoded string is gone too (fits1's ycsb_a at
1 KiB, which flushes 20 MB sections, fell too).
Kept if peak RSS and budget peak fall, throughput does not drop, and the digests match.

### 2. A key-ordered merge's plan without the docid sort, and sized key builders (kept)

Hypothesis: a merge given an order (every key-ordered merge) built a docid-sorted list of every
live document (16 B each, an n log n sort) only to discard it for the order's own, and copied
the order into `refs`; the keys builder grew its versions and hashes by doubling. Numbering the
documents straight from the order (a sorted copy of the docids only for the duplicate check, freed
at once), using the order as the output order, reserving the builders, and returning the plan's
charge when it frees `docs` cut the budget peak and the CPU.

## Results

kv_strata Release, macOS x86_64, `quiet run merge-stream` (strict), six interleaved trials per
arm in `.scratch/ms/abm.py`; medians with [min-max]. `main` and `main2` are two copies of the
main binary (the A/A); `e1` is experiment 1 alone, `e2` both (the branch). Memory in MiB.

big1 (12 MiB; ycsb_a 24 MiB):

| row | arm | peak RSS | budget peak | run ops/s | total s | CPU s | disk MiB |
|---|---|---|---|---|---|---|---|
| fillrandom 1M x 100 B | main | 334.6 [291-367] | 96.4 [96-117] | 401k | 3.42 | 5.49 | 189 |
| | main2 (A/A) | 342.0 [318-373] | 113.2 [96-116] | 398k | 3.42 | 5.55 | 190 |
| | e1 | 183.7 [169-197] | 53.9 [52-59] | 445k | 3.10 | 4.93 | 188 |
| | e2 | 159.4 [146-178] | 38.3 [33-52] | 467k | 2.86 | 4.62 | 176 |
| readrandom 1M x 100 B | main | 364.8 [335-405] | 106.1 [97-116] | 379k | 3.71 | 5.78 | 194 |
| | main2 (A/A) | 364.2 [307-387] | 113.1 [96-116] | 379k | 3.74 | 5.87 | 194 |
| | e1 | 198.4 [187-241] | 53.7 [47-88] | 383k | 3.27 | 5.11 | 183 |
| | e2 | 192.0 [181-206] | 50.3 [31-52] | 378k | 3.19 | 4.92 | 178 |
| ycsb_a 200k x 1 KiB | main | 674.6 [554-741] | 210.5 [109-212] | 207k | 2.58 | 3.15 | 272 |
| | main2 (A/A) | 644.9 [609-882] | 212.3 [210-419] | 209k | 2.62 | 3.24 | 281 |
| | e1 | 238.2 [215-245] | 14.3 [14-22] | 226k | 1.94 | 2.24 | 259 |
| | e2 | 230.4 [219-237] | 11.3 [10-15] | 233k | 1.92 | 2.13 | 233 |

fits1 (256 MiB, 100k records; the arm that must not regress):

| row | arm | peak RSS | budget peak | run ops/s | total s | CPU s |
|---|---|---|---|---|---|---|
| fillrandom 100 B | main / main2 | 62.4 / 60.2 | 42.7 | 1.39M / 1.40M | 0.39 / 0.39 | 0.19 |
| | e2 | 48.7 | 42.0 | 1.38M | 0.38 | 0.19 |
| readrandom 100 B | main / main2 | 69.3 / 65.2 | 42.7 | 656k / 629k | 0.54 / 0.55 | 0.35 |
| | e2 | 54.8 | 42.0 | 645k | 0.53 | 0.34 |
| ycsb_a 1 KiB | main / main2 | 312.1 / 314.9 | 104.9 | 397k / 401k | 1.29 / 1.28 | 0.95 |
| | e2 | 190.4 | 72.6 | 398k | 1.15 | 0.82 |

Attribution. The A/A medians differ by 2% (RSS) on the big rows, and the arms' ranges do not
overlap main's on any big1 RSS or budget-peak row. Peak RSS falls 2.1x on fillrandom, 1.9x on
readrandom, 2.9x on ycsb_a (fits1 ycsb_a 1.6x); the budget peak falls 2.5x, 2.1x, and 18.6x.
Experiment 1 is most of it; experiment 2 adds 13% RSS and 29% budget peak on fillrandom (it
touches the plan of every merge), little on the others. Throughput is flat or up (the merge's CPU
is 15-32% lower: no copy, no whole-section string, no docid sort); the fits1 rows sit inside the
A/A. Disk is the run's own noise (merge timing decides how many dead segments exist at the end).
Every digest (`KV_GET_DIGEST`: readrandom 9743db1c4f882e56 hits 100000, ycsb_a c33a1d6dd42e3995
hits 50259, and fits1's) is identical in every arm. Segment bytes: `merge_stream_test` pins the
merged files of five shapes (zstd, lz4 and raw frames, empty records, records past the block
target, deleted documents, keyless ones) to main's.

What it saves: a streamed merge holds a block, so the working set of a merge no longer grows
with its output. On the 1 KiB row a host that ran this row's M = 24 MiB budget used 675 MiB at
the peak and now uses 230 MiB; in the many-indexes target (millions of indexes, merges running
in a few) the saving is the merge working set per concurrently merging index, up to ~440 MiB
here, not a per-index cost. No dollar figure: it depends on the host's index count.

Correctness: Release, ASan+UBSan and TSan (Homebrew LLVM, -O2 -g) full ctest 38/38 each, no sanitizer reports.

## What remains

The 160-230 MiB left on big1 is not the merge's sections. A footprint sample of fillrandom
shows ~15 MB dirty and ~105 MB freed-but-resident medium malloc pages; on ycsb_a, 126 MB of
mapped segment files (reclaimable file pages), ~55 MB of buffer and ~40 MB freed pages. The
freed pages are the key builders' transients at the largest merge (about 87 B a key at the
end: the filter's build set ~50 B, its hashes 8, versions 8, the plan 20). Shrinking that
needs library changes, not this repository's: fusefilter's `build` copies and sorts its input
(`unique`, 8 B/key) and keeps its peel order as 16-byte pairs (12 would do), and termdict's
builder holds the whole keys section until `finish`; a block sink on it would stream the keys
section too. A partitioned filter would bound the build set but is a format change. Not done:
Linux and FreeBSD (glibc returns large frees, so RSS there should follow the budget peak more
closely), the mapped-pages residency measure, and the reclaim/retain work (another developer's).
