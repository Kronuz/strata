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

(filled in as they run)
