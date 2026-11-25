# Sorted runs

How the merge policy keeps a segment that overlaps no other segment out of the tiers, so a
sequential load is written once by its flush and never rewritten by size (rocksdb-parity D9), and
how a lookup finds the one run that can hold a key. The code is `merge_policy::settled`,
`cap_runs` and `find_merges` in `include/strata/core/merge_policy.hh`, `basic_index::run_merges`
in `include/strata/core/index_merge.hh`, and `key_ranges` in `include/strata/core/key_ranges.hh`.

## The rule

A segment is a run when its key range overlaps no other segment's, whatever its size. A run is no
candidate for a size merge; it still joins a merge that reclaims deletes past the allowed
fraction. RocksDB's trivial move does the same for an SST that overlaps nothing in the next level;
strata's keys section and filter are per segment, so a run needs no rewrite.

Past `max_runs` runs (64), the smallest key-adjacent runs merge: a window of up to the merge
factor of runs with no other segment between them in key order, the one writing the fewest bytes
per segment it removes, repeated down to the cap. The output spans only its runs' keys, so it is a
run again. `max_runs = 0` keeps no runs: the tiers alone, as before D9. The cap bounds what every segment costs whatever its size: a query's per-segment setup
(query-gap H20), a mapping and its record. 64 is twice what the tiers hold at their largest (8 a
tier over four tiers up to the 2.5 GiB half-maximum), and RocksDB holds every flushed SST with no
cap (103 files after `tenx1` fillseq).

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

## Lookups

`key_ranges` holds the mapped segments' ranges: the runs sorted by first key, and the rest (ranges
that overlap, and unmapped segments, whose range is unknown) by position. A key inside a run's
range is in no other mapped segment's, so `lookup_key` (every upsert's read) and `get` probe that
run, found by binary search, and the rest, newest first, instead of every segment. The index
rebuilds it right after each change to its segment list (flush, merge publication, checkpoint
install, open, close, warm twins, heal) and after a merge decision maps segments; a list of another
length than the one it was built for, or a rebuild that threw, probes every segment. Snapshots and
shared readers' views walk their segments as before.

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

- A scan still opens every segment whose range reaches past its start, runs included: a short scan
  over many runs seeks in each, where it needs only the run holding its start and the next ones.
- Snapshots and shared readers' views walk every segment for a get.
- A reopened index finds no run until a merge decision or a change to its segment list maps the
  segments; until then a lookup probes every segment, as before D9.
- A sequential load past 64 runs rewrites its bytes in key-adjacent merges (1.8x write
  amplification over 200 flushes in `merge_policy_test`'s simulation, against none at the cap's
  value), where RocksDB writes each SST once and keeps every one.
- The cap trades search cost for bytes written: an index of text documents keyed in order holds up
  to 64 segments where the tiers alone held about a quarter of that.
