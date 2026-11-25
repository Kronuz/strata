# Sorted runs

How the merge policy keeps a segment that overlaps no other segment out of the tiers, so a
sequential load is written once by its flush and never rewritten by size (rocksdb-parity D9). The
code is `merge_policy::settled` and `find_merges` in `include/strata/core/merge_policy.hh`, and
`basic_index::run_merges` in `include/strata/core/index_merge.hh`.

## The rule

A segment is a settled run when its live bytes are at least `run_bytes` (4 MiB) and its key range
overlaps no other segment's. A settled run is no candidate for a size merge; it still joins a merge
that reclaims deletes past the allowed fraction. Segments under `run_bytes` merge by size as
before, so a run of flushes smaller than that merges once into a segment that settles. RocksDB's
trivial move does the same for an SST that overlaps nothing in the next level; strata's keys
section and filter are per segment, so a settled run needs no rewrite.

## Where the ranges come from

Each merge decision (`run_merges`) maps every candidate, a map point handled as `schedule()`
handles it: a segment that fails its checks is marked damaged, and a host error drops the round.
A candidate passes its keys' `min_key()` and `max_key()`. A segment outside the candidates, one
already merging or damaged, still occupies its key space: a candidate meeting its range is marked
`overlapped` and does not settle, and a damaged segment whose range is unknown overlaps every
candidate. A candidate with no range (a layout without keys, a segment without keys) overlaps
every other, so nothing settles.

The policy recomputes the runs at every decision: a segment that a later flush overlaps stops
being a run and merges by size with what overlaps it.

## Measured

Exclusive A/B on the development Mac (job 261124-231350-d66cf1), A = strata `7823c51a` (main's
code), B = `19ddcbd0`, `bench/kv/ab.py`'s `runs` set, 3 interleaved trials, medians:

| Row | write amp A/B | CPU µs/op A/B | peak RSS MiB A/B | ops/s B/A [95% CI] |
|---|---|---|---|---|
| `tenx1` fillseq | 2.08/1.31 | 2.84/2.10 | 134/89 | 1.07 [1.00, 1.24] |
| `tenx1` fillrandom | 2.10/2.09 | 3.69/3.78 | 149/144 | 0.97 [0.90, 1.00], tie |
| `twox1` fillseq | 1.62/1.62 | 2.05/1.88 | 36/36 | 1.08 [0.85, 1.39], tie |
| `twox1` fillrandom | 1.71/1.22 | 2.48/2.47 | 38/29 | 1.01 [0.91, 1.14], tie |
| `tenx1` readrandom | 2.11/2.09 | 2.49/2.59 | 185/176 | 0.96 [0.92, 1.19], tie |

The same job's A/A spreads 3.3-10.5% for A and 4.5-37.5% for B on throughput, so only the write
amplification, CPU and memory of `tenx1` fillseq move: 37% fewer bytes written, 1.35× less CPU per
operation, 34% less peak RSS. Random keys settle nothing; their differences are merge timing.
After a fillseq, readall returns the same get digest on A and B (eb2a3ad93b05aafe, 1,000,000 hits).

## Limits

- A get walks the segments newest first and skips each by its key range; with many settled runs
  the walk is linear in their count, where RocksDB binary-searches a level's files. The runs'
  range index and the cap on the run count (D9, N13) are the next steps of task 7.
- Segments under `run_bytes` still merge once before they settle, so a sequential load writes its
  bytes about twice where RocksDB's writes them once.
