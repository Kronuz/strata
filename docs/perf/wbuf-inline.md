> Status: CURRENT (2026-11-09) quest/xapiand/wbuf-inline, developer: the write buffer's records hold their key and stored bytes inline for every layout that opts in (`inline_kind`); functional results, throughput by A/B with an A/A; the landing A/B against main is the last section.

# Write buffer records: key and stored bytes inline, one shape

Finishes `docs/perf/wbuf-bytes.md` (explore/wbuf-bytes, strata:bec2c09), which prototyped the inline record for `document_layout` behind `STRATA_WBUF_INLINE`. Here there is no switch: every layout's record is `[header][key len varint][stored len varint][key][stored]` (a u32 pair before the review fixes, strata:0b760d8), a document the inline form cannot hold stays a `document` in the record (the fallback), and tombstones and inverting layouts' records hold the key alone. How it works: `docs/mechanisms/write-buffer.md`. Raw data: `docs/perf/wbuf-inline/`.

The tables up to "Proof" are the first round (u32 lengths, estimate unchanged); "After the review fixes" has the review round's numbers; "Landing A/B" has the numbers of the code that lands, against current main.

Host: dev Mac (macOS x86_64), Homebrew LLVM clang++, Release. Base is main 9e344d8 built from `git archive`; after is perf/wbuf-inline. Every timed or measured run was under `quiet strict wbufin 20`.

## Resident bytes per record

`kv_proto_bufmem`, 300k records, nothing flushed, RSS(300k) - RSS(100k) over 200,000 records, 16 B keys; 5 interleaved rounds of base, base again (A/A twin), after. The layouts:

1. `document`: `document_layout`, 100 B values.
2. `carry`: a layout whose stored-derived part carries an extra member (a field-carrying part), empty in every document: inline through the plugin's `inline_part`.
3. `carry-extra`: the same layout, the extra member set: the document stays whole.
4. `document`, 1500 B values: past a 1 KiB chunk, so whole.

| Layout | Base B/record (range) | A/A twin | After | Change |
|---|---:|---:|---:|---|
| document, 100 B | 231.0-236.6 | 231.0-236.4 | 198.3-198.6 | 14% smaller |
| carry, 100 B | 255.0-260.6 | 255.0-260.3 | 198.3-203.9 | 22% smaller |
| carry-extra, 100 B | 255.2-260.5 | 255.1-260.6 | 255.1-260.6 | unchanged (whole) |
| document, 1500 B | 1660.8-1666.0 | 1660.9-1666.3 | 1660.8-1666.2 | unchanged (whole) |

The A/A twins agree with the base ranges (the 5 B steps are page granularity). Heap bytes per record (malloc size in use) move the same way: document 237.8-243.1 to 204.5-207.1.

Identity in the same runs: the scan digest of the buffer (FNV over every key and value) and the index digest (`ix->digest()`, which asks every plugin for its digest records, through `materialize()` for inline records) are identical in all 15 runs per layout: `15a6928fd4ce9082` / `5b88e1845a0a0466` (document), `5b88e1845a0a0466` (both carry layouts, no `scan`), `c6a65f2d591cf21e` / `ececbcbe4a67ded3` (1500 B). Not measured: an inverting layout (prospect's; no inverter in this repository), whose records now hold the key alone instead of a key-only `document`.

## Linux

Ubuntu 24.04 in the local QEMU VM (glibc, clang, `tools/vm/aws-linux.sh run` with `STRATA_AWS_VM_ALLOW` unset, so no AWS). Tree shipped from `git archive`. Release ctest 70/70 and ASan+UBSan ctest 70/70 (`docs/perf/wbuf-inline/linux-ctest-*.log`). Resident bytes per record, 3 rounds (base, base again, after; `linux-bufmem.txt`), RSS only (no malloc in-use count on Linux):

| Layout | Base B/record | After | Change |
|---|---:|---:|---|
| document, 100 B | 284.1 | 188.1 | 34% smaller |
| carry, 100 B | 316.1 | 188.1 | 40% smaller |
| carry-extra, 100 B | 316.1 | 300.1 | 5% smaller (whole records; not explained, macOS shows no change) |

Scan and index digests are the macOS ones in all runs. No throughput was run on Linux.

## Throughput

`bench/kv/ab.py rows wbuf 11`: kv_strata built from base (A) and the branch (B), 11 interleaved trials per row (the order alternates each trial), 100,000 records, one thread, no durability. The first six rows have 256 MB of memory (nothing flushes before the settle), the last four are `twox1` rows at about half the data size in memory, which flush under pressure. A/A is the same run with both sides the base binary (`docs/perf/wbuf-inline/ab-aa-report.txt`). Ratio is after over base operations per second, 95% bootstrap interval.

| Row (value) | memory | B/A [95% CI] | A/A [95% CI] |
|---|---|---|---|
| fillrandom (100) | fits | 1.054 [0.982, 1.108] tie | 0.975 [0.939, 1.002] tie |
| readrandom (100) | fits | 1.005 [0.973, 1.032] tie | 0.976 [0.917, 1.037] tie |
| readseq (100) | fits | 1.075 [0.919, 1.205] tie | 1.057 [0.962, 1.117] tie |
| seekrandom (100) | fits | 0.993 [0.921, 1.041] tie | 0.958 [0.886, 1.028] tie |
| ycsb_a (100) | fits | 1.019 [0.969, 1.057] tie | 1.003 [0.966, 1.101] tie |
| ycsb_a (1024) | fits | 1.031 [0.989, 1.077] tie | 1.018 [0.989, 1.045] tie |
| fillrandom (100) | 2x | 1.032 [0.990, 1.077] tie | 1.037 [1.002, 1.091] 1.04x faster |
| readrandom (100) | 2x | 0.987 [0.957, 1.068] tie | 0.983 [0.930, 1.047] tie |
| ycsb_a (100) | 2x | 1.013 [0.962, 1.040] tie | 1.014 [0.976, 1.043] tie |
| ycsb_a (1024) | 2x | 1.000 [0.941, 1.134] tie | 0.979 [0.965, 1.032] tie |

No row regresses beyond the A/A spread: every B/A interval contains 1.0, and the A/A itself ranges 0.958 to 1.057 in the point estimates (one A/A row reads "1.04x faster" for identical binaries). The B/A point estimates range 0.987 to 1.075. This says there is no throughput regression visible at 11 trials; it does not show a gain.

## Scan digests

`KV_SCAN_DIGEST` on readseq, seekrandom and ycsb_e, base against after, 21 configurations (`docs/perf/wbuf-inline/digests.txt`): values of 100 B (memory 5, 16 and 256 MB, so flushes under pressure and none), 1024 B (24 MB), 1500 B and 4000 B (16 MB, and 1500 B at 256 MB): the fallback sizes. All 21 identical. The digests are taken after the load's settle, so most scanned data comes from flushed segments; the flush of inline and whole records is what they cover. The unflushed buffer's scan is the bufmem digest above.

## Proof

Release ctest 69/69; ASan+UBSan 69/69; TSan 69/69 (clang from Homebrew LLVM, RelWithDebInfo). Mutants, each caught: (1) `lane_fits` ignoring the operations' stored bytes: `write_buffer_test` aborts with `length_error: chunk directory is full` in `chunk_limit_sized_transactions`; (2) the flush dropping the last byte of an inline record's stored bytes: `write_buffer_test` and `core_kv_test` fail (31 CHECK lines each, the cap). Linux: see the ledger.

## Limits

1. A value past about 900 B (a record past the 1 KiB first chunk) stays whole: no bytes saved. Chunks double, so a larger inline bound would need the record to know its chunk.
2. A layout with a part the segment writer reads beyond the stored bytes keeps whole documents (`carry-extra`, prospect's whole-document mode with `flush_threads > 1`). Inline needs a serializer per plugin; the owner's call.
3. Since strata:0b760d8 the estimate charges inline records their stride, so flush points move (below).

## After the review fixes (strata:0b760d8)

Same host and method; raw data `docs/perf/wbuf-inline/bufmem-cadence-rounds.txt` (script beside it).

Resident bytes per record, 5 interleaved rounds (main / u32 pair at strata:6f26aa2 / varints):

| Layout | main | u32 pair | varints |
|---|---:|---:|---:|
| document, 100 B | 231.4-237.3 | 198.4-204.0 | 190.6-196.2 |
| document, 500 B | 639.2-644.8 | 598.6-599.1 | 590.6-591.2 |
| carry, 100 B | (no layout on main) | 203.7-204.3 | 190.7-196.3 |
| carry-extra, 100 B | | 255.5-260.7 | 255.2-260.8 |
| document, 1500 B | 1660.8-1666.2 | 1660.9-1661.1 | 1660.7-1666.3 |

Varints are the smaller shape (about 8 B per record, a lane stride step), so the code uses them, as D3 has.

Records per flush (`flush_cadence.cc`: 400,000 key-ordered writes, one flush thread, no budget pressure; counts are deterministic, 3 rounds identical; index digests identical main against branch):

| Value, share | main | branch | Change |
|---|---:|---:|---|
| 100 B, 8 MiB | 32,264 | 55,189 | 1.71x more |
| 100 B, 32 MiB | 129,056 | 220,753 | 1.71x more |
| 500 B, 8 MiB | 12,711 | 15,197 | 1.20x more |
| 500 B, 32 MiB | 50,841 | 60,788 | 1.20x more |
| 1024 B (whole), 32 MiB | 28,340 | 28,340 | unchanged |

Peak resident bytes of the same runs rose at 500 B values (8 MiB share: main 74-80 MiB, branch 95-110 MiB; 32 MiB: 87-92 against 133-141), with merges on or off, and at 100 B with an 8 MiB share (59-65 against 69-72); unchanged at 1024 B. Explained in `wbuf-inline/peak-rss.md`: freed lane chunks the macOS allocator keeps resident (clean pages; the footprint peak is unchanged), and beneath it the lane's open chunk, which the document layouts' estimate does not charge (a three-line fix proposed there).

Throughput of the rows that flush under memory pressure (`ab.py rows OUT wbufw 11`, main as A, the branch as B, then main against itself; `ab2-*-report.txt`): fillrandom 100 B 1.06x faster [1.015, 1.115], ycsb_a 100 B 1.10x faster [1.025, 1.147], readrandom 100 B a tie (1.042 [0.943, 1.139]), ycsb_a 1024 B (whole records) a tie (1.003 [0.945, 1.029]); the A/A rows are ties from 0.993 to 1.013. The driver's peak RSS is the same on both sides (30/30, 43/43, 172/173 MiB). Branch numbers, not a gate row.

## Landing A/B (strata:bb2f58d against main strata:70df28d)

The code that lands (the lane charge, chunk growth by ruling (c), the test-only fixes after it),
rebased on main, against main. Both arms built by `wbuf-inline/landed-ab.sh build` (Release,
AppleClang, Mach-O x86_64; flush_cadence compiled and linked as kv_strata is, against each arm's
headers), job 261109-024405-35d764. Host: dev Mac (macOS x86_64).

Records per flush (`landed-ab.sh cadence`: `flush_cadence`, merges on, 400,000 key-ordered writes,
one flush thread; counts are deterministic, 1 trial), job 261109-024406-5881b6, raw
`wbuf-inline/landed-cadence.txt`. A is main, B the branch. Max RSS is one noisy trial per cell and
not a claim. The counts equal the chunk round's A and G columns.

| value, share | flushes A/B | records/flush A | B | B/A | max RSS MiB A/B | digest |
|---|---|---:|---:|---:|---|---|
| 100 B, 64K | 1581/1007 | 253 | 397 | 1.57x | 126/101 | same |
| 100 B, 96K | 1055/661 | 379 | 605 | 1.60x | 121/102 | same |
| 100 B, 256K | 396/240 | 1,009 | 1,666 | 1.65x | 101/118 | same |
| 100 B, 1 MiB | 99/60 | 4,033 | 6,663 | 1.65x | 125/120 | same |
| 100 B, 8 MiB | 12/7 | 32,264 | 53,748 | 1.67x | 92/67 | same |
| 100 B, 12 MiB | 8/4 | 48,396 | 80,178 | 1.66x | 68/76 | same |
| 100 B, 32 MiB | 3/1 | 129,056 | 214,641 | 1.66x | 92/89 | same |
| 200 B, 64K | 2185/1632 | 183 | 245 | 1.34x | 108/112 | same |
| 200 B, 96K | 1459/1072 | 274 | 373 | 1.36x | 127/123 | same |
| 200 B, 256K | 548/393 | 729 | 1,017 | 1.40x | 105/111 | same |
| 200 B, 1 MiB | 137/98 | 2,913 | 4,072 | 1.40x | 101/102 | same |
| 200 B, 8 MiB | 17/12 | 23,302 | 32,917 | 1.41x | 97/107 | same |
| 200 B, 12 MiB | 11/8 | 34,953 | 49,114 | 1.41x | 115/74 | same |
| 200 B, 32 MiB | 4/3 | 93,207 | 131,524 | 1.41x | 92/118 | same |
| 500 B, 64K | 3999/3738 | 100 | 107 | 1.07x | 151/127 | same |
| 500 B, 96K | 2684/2453 | 149 | 163 | 1.09x | 121/142 | same |
| 500 B, 256K | 1005/892 | 398 | 448 | 1.13x | 120/165 | same |
| 500 B, 1 MiB | 251/221 | 1,589 | 1,804 | 1.14x | 122/131 | same |
| 500 B, 8 MiB | 31/27 | 12,711 | 14,747 | 1.16x | 97/112 | same |
| 500 B, 12 MiB | 20/18 | 19,066 | 22,019 | 1.15x | 121/147 | same |
| 500 B, 32 MiB | 7/6 | 50,841 | 59,037 | 1.16x | 87/136 | same |
B/A records per flush: 1.07-1.67x; digests identical across arms: True

readrandom 100 B (`landed-ab.sh rr`: `bench/kv/ab.py rows OUT wbufrr 41`, 41 interleaved trials per
pair, a `strict` job), job 261109-063604-6b6841, raw `wbuf-inline/landed-rr-*`:

| Pair | A ops/s | B ops/s | B/A [95% CI] | Direction | p50 µs A/B | p99 µs A/B | CPU µs/op A/B | peak RSS MiB A/B | disk MiB A/B |
|---|---:|---:|---|---|---|---|---|---|---|
| main / branch | 510,981 | 543,905 | 1.064 [1.042, 1.091] | 1.06x faster | 1.70/1.61 | 2.86/2.70 | 1.96/1.84 | 32/34 | 6.9/6.8 |
| branch / branch | 554,729 | 542,287 | 0.978 [0.959, 1.003] | tie | 1.59/1.62 | 2.61/2.77 | 1.80/1.84 | 35/34 | 6.8/6.8 |

The branch reads 1.06x faster than main, clear of the B/B band (0.959-1.003), with 6% less CPU per
read. The chunk round measured a tie against its main (1.020 [0.988, 1.054], main strata:9e344d8);
main has moved 86 commits since, so the two baselines differ. The driver's peak RSS is 2 MiB more
on the branch (32 against 34 MiB); the B/B pair spans 34-35 MiB, so that gap is within one
binary's run-to-run spread and not attributed. Disk is the same.

