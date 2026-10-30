> Status: SUPERSEDED (2026-10-20) landed as basic_index::scan, strata:89f9fd7 (exploration log).

# YCSB E scan: lazy k-way merge (explore/scan)

Question: why is strata 1.21x slower than RocksDB on ycsb_e (1 KiB values, fits: 31.6 vs 26.1 CPU us/op)?

## Profile (macOS sample, run phase, 1 thread, main 55453ae)

The driver's scan (bench/kv/kv_strata.cc) asked every segment for up to n keys through the callback
scan, copied each key into a std::string, sorted all hits, uniqued, trimmed to n. With 3 segments that is
about 3n keys materialized and sorted for n returned. Active (non-idle) samples: memcmp 21%
(sort and unique comparisons), introsort and insertion sort 17%, std::string allocation/free
about 3%, LZ4 block decode 18%, residency verify 8%, key_slot 6%. So roughly 40% is sort plus key copies, not
value decode: no per-record block decode (a stored frame is decoded once per frame per scan) and no per-record value copy
beyond the sink append.

## Change

keys::reader::range (keys.hh), a lazy cursor over termdict's cursor::seek, exposed as keys_plugin reader range(from).
The driver merges segment ranges and the buffered key set by pulling heads only as far as n outputs need.
No key copy per hit, no sort. No format bytes.

## Results (ycsb_e, 100k records, 100k ops, 1 KiB, none durability, 1 thread, interleaved ABBA, quiet run light)

| row | old CPU us/op (median, min-max) | lazy CPU us/op | ops/s old -> lazy |
|---|---|---|---|
| fits 256 MB, 12+12 runs | 33.0 (32.7-35.7) | 21.7 (20.9-22.9) | 30.3k -> 46.0k |
| 2x 49 MB, 8+8 runs | 51.2 (32.4-76.3, noisy) | 27.7 (27.4-32.3) | 19.5k -> 36.0k |

Lazy is 1.52x faster (34% less CPU) on fits. A/A (even vs odd runs of the same mode): old 33.1/33.0, lazy 21.9/21.7.
At 21.7 us/op strata beats the report's RocksDB 26.1 on this row.

## Identity

KV_SCAN_DIGEST=1 hashes every returned key and value: old and lazy both give 81bb0f0b7fe8d333 (seed 42, 100k ops),
and the same value-bytes total (5088720). Ties resolve to buffer then highest segment; ycsb_e has no duplicate keys,
so a duplicate-key case (update then scan) is untested here and needs a unit test when finished.

## Not done

Core API (scan lives in the driver; K3). Scan across a segment boundary mid-merge, deletes, and updates need tests.
KV_SCAN_OLD=1 keeps the old path in the driver for A/B only.
