# Disk bytes at 1 KiB values (explore/disk-1k)

Question: why strata is 97 vs RocksDB 77 MiB on ycsb_a at 256 MB (fits) and 118 vs 58 MiB at 49 MB (2x).
Prototype knobs (env, bench only): KV_RETAIN (retained_predecessors), KV_BLOCK, KV_ZSTD (zstd_merged), in kv_strata.cc and the document policy's block size.

## Byte split, fits, ycsb_a, settled (base)

Disk 101.8 MB (97.1 MiB) in 4 data files: seg-1 32 MB, seg-2 29 MB, seg-3 18 MB, seg-4 23 MB. The index lists 3 live segments whose sections sum to 69.4 MB: stored 67.4 MB, keys 1.17 MB, key index 0.2 MB, everything else under 0.5 MB. seg-1 is a merged-away input still on disk: `retained_predecessors = 2` keeps the two previous manifests, and each names its segments, so retention holds the inputs. Dead versions inside live segments are small here.
So: values 66%, keys 1.7%, retained obsolete segments 31%. Per-block overhead is in the stored section (ratio below).

## Results (fits, mb=256, one run each, single run noise ~10% on ops/s)

| config | disk MiB | ycsb_a ops/s | cpu us/op |
|---|---|---|---|
| base | 97.1 | 339k | 2.92 |
| retained_predecessors=1 | 97.1 | 385k | 2.58 |
| retained_predecessors=0 | 66.2 | 373k | 2.66 |
| 8 KiB blocks | 96.4 | 344k | 2.89 |
| 16 KiB blocks | 95.8 | 309k | 3.21 |
| zstd_merged | 93.2 | 374k | 2.65 |
| retain 0 + zstd_merged | 62.3 | 315k | 3.12 |

Block size moves disk by 1-1.4% only (values are half-compressible, each 1 KiB value already holds its own repeat). zstd_merged: 4%. Retention: 32%, and 66.2 MiB beats RocksDB's 77 MiB by 14% (RocksDB figure from the report, not rerun).

## 2x (mb=49), four interleaved trials, ycsb_a disk MiB

Bimodal and timing dependent (background merges race the 20 s run cap; "files" in the hundreds means obsolete files still queued at the end):
- base: 118.4, 145.7, 118.3, 117.1 (A/A base2: 117.8, 118.3, 118.6, 146.6)
- retain 0: 81.4, 136.3, 142.5, 145.1
- retain 0 + zstd_merged: 59.7, 107.4, 58.8, 69.6
- zstd_merged: 98.9, 107.5, 99.1, 99.2
ycsb_a ops/s spans 35k-200k in every config, so no throughput claim is possible at 2x from this harness.
ycsb_c (read only) at 2x: base 58.2 MiB (equals RocksDB's 58), zstd_merged 49.9 MiB, but throughput 120-150k vs 245-365k ops/s (about 2x slower, cpu 8.2-9.5 vs 2.7-4.1 us/op).

## Findings

1. The fits gap is retained manifests, not compression. retained_predecessors 2 -> 0 gives 97 -> 66 MiB at no measured read or write cost.
2. At 2x the same cause plus unreclaimed obsolete files at the end of the run: roughly 118 MiB when settled cleanly, 145 when many files are queued; with retention off and zstd_merged it reaches 58-70 MiB in 3 of 4 trials.
3. 8 and 16 KiB blocks: not worth it (1-1.4% on disk); 16 KiB at 2x was 124-136 MiB with the highest cpu.
4. zstd_merged halves ycsb_c throughput at 2x: it trades read speed, so it fails the "without losing read speed" bar.
5. Settle does not guarantee the final disk number: the benchmark should report disk after reclaim() and without retained predecessors, or both, so the table compares live bytes.

## Not checked

Read-back identity was not verified: the driver has no dump or digest, and the knobs only change retention, the codec and the block target (existing, tested options). A verify pass belongs in the finishing work. retained_predecessors is a recovery policy (I14), so lowering it is a design decision for the owner, not a bench setting; a settled-state `trim` that drops predecessors only after a quiet commit is the likely shape.
