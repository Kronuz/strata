> Status: CURRENT (2026-11-04) perf/wbuf-inline, developer: the write buffer's records hold their key and stored bytes inline for every layout; functional results, throughput by A/B with an A/A, nothing landed on main.

# Write buffer records: key and stored bytes inline, one shape

Finishes `docs/perf/wbuf-bytes.md` (explore/wbuf-bytes, strata:bec2c09), which prototyped the inline record for `document_layout` behind `STRATA_WBUF_INLINE`. Here there is no switch: every layout's record is `[header][u32 key len][u32 stored len][key][stored]`, a document the inline form cannot hold stays a `document` in the record (the fallback), and tombstones and inverting layouts' records hold the key alone. How it works: `docs/mechanisms/write-buffer.md`. Raw data: `docs/perf/wbuf-inline/`.

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
3. The buffer's memory estimate (the flush threshold) is unchanged, so flush points match base; charging real strides would flush later.
