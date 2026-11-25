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

## Limits

- A get walks the segments newest first and skips each by its key range; with many settled runs
  the walk is linear in their count, where RocksDB binary-searches a level's files. The runs'
  range index and the cap on the run count (D9, N13) are the next steps of task 7.
- Segments under `run_bytes` still merge once before they settle, so a sequential load writes its
  bytes about twice where RocksDB's writes them once.
