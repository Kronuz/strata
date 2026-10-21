# strata core against RocksDB on format 14 step 1

Quick tier, rerun on main `75d1e7e` (format 14 step 1: key-ordered document segments, 4 KiB LZ4
blocks, `get(key, f)`) against RocksDB 11.11 on its calibrated `lvl-16k-lz4`, the same harness and
rows as `core-vs-rocksdb.md`. Development Mac (Intel i9-9980HK, APFS), one thread, five trials per
row (three for durable and WAL), A/A first, strict hold. Full table: `core-vs-rocksdb-f14/report.md`;
raw lines: `raw.jsonl.gz`.

## Where strata stands

- **Wins on 27 of 34 rows.** Fits: fillrandom 2.05× faster, point reads 1.35× faster, absent keys
  2.68× faster, YCSB A–D and F 1.83–2.48× faster. At 2× memory: point reads 5.25× faster, YCSB B–D
  3.10–3.68× faster. WAL durability 5.94–9.06× faster. 1,000 indexes: point reads 1.68× faster with
  66% less memory and 71% less disk.
- **Against the format 13 report:** point reads went from 12–32× slower to 1.35–5.25× faster;
  scans from 45–220× slower to within 1.21×.

## Losses (the targets)

1. **Synchronous durability: 4.76–5.09× slower** on every row, with 2.5–10× the CPU per op. The
   known cause is five barriers per commit against RocksDB's one; the turn log (durability step 1)
   is the fix.
2. **Peak memory: 1.5–4.3× RocksDB's** on every single-index row (94 MiB against 27 MiB on 100-byte
   fills that fit).
3. **Opening 1,000 indexes: 85 s against 22 s** (median open phase), about 85 ms per index.
4. **Scans (YCSB E): 1.21× slower** that fit, 1.12× slower at 2×.
5. **Tail latency at 2× memory:** p99 39–79× higher on fills and overwrite (140–177 µs against
   2–4 µs), the writer stalling on flush.
6. **Disk on 1 KiB values:** 27% more that fit, 2.0× at 2× on YCSB A and F.

A/A spread was 1–17% for strata and 6–26% for RocksDB; ratios inside 10% are ties.
