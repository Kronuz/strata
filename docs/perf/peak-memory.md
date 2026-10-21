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

## Landed (`phase4/flush-memory`)

The three changes are real code, no env gates, for every flush shape:

1. `basic_segment_writer::reserve(docs, stored_bytes)` sizes the stored builder, its offsets, and the docids before the first add.
2. The stored builder keeps views into the buffer instead of copying the records. The buffer is a `borrowable_vector` (`core/borrow.hh`): every mutator throws while a lease is held, a builder takes a record only from a part inside its lease's range, and it keeps the lease as long as its views. The flush pins the buffer for each run's writer, so it cannot change under the writer, whatever else runs (background merges never touch it; a flush runs on its caller's thread and its run threads are joined before it returns).
3. Each run writes straight to its own segment file through `file_sink`, for one run or many. Files are created in run order, every run is written before any segment is published, and a failure removes them all (ENOSPC is a `space_error`); the commit's syncs, rename, and manifest are untouched. The single-segment inverter flush shares the new `streamed_segment` publish step.

Segment bytes are unchanged: the format 14 fixtures pass, and `flush_memory_test` pins the digest of every segment file a one-run and a three-run flush write, taken from the previous commit (`55453ae`).

Measurement: 100k x 100 B fillrandom, `kv_strata --threads 1 --memory-mb 256`, Release, macOS x86_64, main `55453ae` code against this branch, `quiet run flush-mem` (strict), `peak_rss` from the driver, six trials each.

| Arm | Median | Range |
|---|---|---|
| A/A: main binary, run 1 | 101.1 MiB | 93.4-106.7 |
| A/A: main binary, run 2 | 102.3 MiB | 92.2-112.5 |
| A: main, interleaved with B | 95.5 MiB | 92.4-101.3 |
| B: this branch | 70.7 MiB | 64.2-71.6 |

A/A medians differ by 1.2%; B against A is -25.9%, and B's range does not overlap A's. The segment files have the same size (7,207,937 bytes) in every run.

What is left: the stored section is still built whole before it is appended (`encode_stored`), and the buffer itself (~290 B per record) is the larger share.
