# Peak memory on the 100k x 100 B fillrandom row

Question: where does the 94-99 MiB peak RSS (RocksDB 27 MiB) go, and what one change cuts the most?
Row: `kv_strata --workload fillrandom --records 100000 --value-size 100 --threads 1 --memory-mb 256`,
Release, dev Mac (macOS, Intel), under `quiet run explore-mem` (light), main `8af7d92`.

## Attribution

- Run phase ends at ~46 MiB RSS: the buffer is 29-32 MiB live (100k nodes of 112 B for the value
  strings, a 9 MiB `buffered` vector, two 4 MiB hash-slot tables). It is heap, not retention.
- Settle (flush) takes RSS to ~99 MiB. malloc-zone peak in-use rises to 62-73 MiB during
  `flush_documents`: the writer holds a second copy of every value (`stored_`, doubling growth,
  realloc spike) and builds the whole segment into a `std::string` (doubling growth) before
  writing it. Freed pages stay resident, so the high-water mark is what counts.
- The mapped segment file is 7 MiB; `vmmap` footprint at the end is 29 MiB while peak RSS is ~99.

## Experiments (peak RSS KiB, six interleaved trials each; output files byte-identical, same sha)

| Variant | Median | Range |
|---|---|---|
| base A (A/A, two runs of six) | ~100k | 89k-113k |
| 1. reserve hints: stored copy and output string sized up front (`KV_RESERVE`) | ~85.5k (-13%) | 78k-94k |
| 2. + stored records borrowed from the buffer, no copy (`KV_BORROW`) | ~80k (-20%) | 70k-86k |
| 3. + single run streams to the segment file, no output string (`KV_STREAM`) | ~73k (-27%) | 64k-83k |

A/A spread is about 10% of the median (the medians of two base sets differ by 3%); variants 2 and
3 sit outside it, variant 1 only just. Throughput is unchanged (run phase 1.12-1.29 M ops/s in
all arms); total wall time 0.37-0.39 s against 0.40-0.41 s base. Direction: lower is better.

## Verdict

Worth finishing: cumulative -27% peak RSS (about 100 -> 73 MiB) with no format change. The
remaining gap to RocksDB is the buffer itself (~32 MiB for 11.6 MB of data, ~290 B per record
against the design's ~177) plus the writer's key and doc structures; shrinking the buffered node
(112 B value allocations, 72 B `buffered` slots, two hash tables) is the next item.

Prototype caveats: env-gated (`KV_RESERVE`, `KV_BORROW`, `KV_STREAM`) in
`include/strata/core/index.hh` and `writer.hh`; the stream path covers the single-run flush only;
no test suite was run; the borrow relies on the buffer outliving the writer (true in
`flush_documents`).
