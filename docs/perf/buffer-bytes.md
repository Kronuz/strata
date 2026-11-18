> Status: SUPERSEDED (2026-10-21) the 8-byte slots landed as strata:8fcd4e3 (exploration log).

# Buffer bytes per record: 290 B against the 177 B design

Question: why does the write buffer hold ~290 B per record against format 14's 177 B, and what one change cuts the most?
Row: `kv_strata --workload fillrandom --records 100000 --value-size 100 --threads 1 --memory-mb 256`, Release, dev Mac (macOS, x86_64), main `1abca41`, `document_layout`.

## Attribution (confirmed on main)

Live allocations at the start of settle (operator new/delete counted by `malloc_size`; driver patch in `.scratch/explore-buffer/hist-driver.patch`): 100,002 allocations of 112 B plus three large blocks, 29.0 MB for 100k records.

| Piece | Bytes per record | Why |
|---|---:|---|
| Value heap string (`stored`, 100 B) | 112 | one malloc per value; the 16-byte key is SSO |
| `buffered` vector | 94 | 72 B element (docid, version, key string, stored string, dead) x capacity 131,072 (22 B of doubling slack) |
| Key version map | 42 | 16 B slots (64-bit hash + value), at most half full: 262,144 slots |
| Docid map | 42 | same slots; its hash is the docid |
| Total | ~290 | matches 29.0 MB / 100k |

The undo log is not retained (cleared per commit), so it is not a share.

## Experiment 1: 8-byte slots, 7/8 full (`hash_slots`)

32-bit folded hash + 32-bit value, grow at 7/8. The docid map's lookups compare the stored docid (`docid_is`) instead of trusting the hash, since 32 bits no longer identify a 64-bit docid. Tables drop from 8.4 MB to 2.1 MB (-63 B/record); live heap 29.0 -> 22.7 MB.

Interleaved, strict hold, fillrandom then readrandom, base x2 per round (A/A) and the change once, six rounds:

| Arm | Peak RSS median | Range | Run-phase ops/s median |
|---|---:|---:|---:|
| base (12 runs, both A/A copies) | 78.0 MiB | 69.0-84.5 | 1.05 M |
| 8-byte slots (6 runs) | 70.7 MiB | 65.9-72.4 | 1.22 M |

Peak RSS is 9% lower (lower is better); the A/A copies' spread is wide (about 20%) but the change's range sits below the base median. Run-phase throughput is 16% higher (higher is better): the maps are a quarter of the cache footprint. Identical: the get digest (`20d487ed1b591925`, 100,000 hits) and the sha of all files are the same in every run. Not run: the unit tests and sanitizers (prototype).

## Verdict

1. The loss is the design not being built: the buffer is a `std::string` per value and a 72 B element per record (206 B), plus 16 B hash slots (84 B). The arena (24 B header + key + value in 1 MiB chunks, no vector slack) would cost ~140 B for records and cut ~75 B per record: the single largest cut, but a large change (writer borrow, undo, flush all see `buffered`). Not prototyped here; an estimate.
2. Slot compaction is the cheaper one: -63 B/record (-22% of the buffer), faster, a ~20-line change. Worth finishing first (the docid map could also disappear for sequential docids, which is further).
3. Order: slot compaction, then the arena; together ~290 -> ~150 B, near the design's 161 B.
