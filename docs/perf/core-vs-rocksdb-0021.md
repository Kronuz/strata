# strata core against RocksDB after the overnight fixes (2026-10-21)

Quick tier on main `89f9fd7` (format 14 step 1 plus the pressure flush, flush memory, retention 0,
the fusefilter fix and the scan API) against RocksDB 11.11 `lvl-16k-lz4`, same harness and Mac as
`core-vs-rocksdb-f14.md`. Five trials per row, A/A first, strict hold. The run was stopped after
the first synchronous row to free the host for agents; the sync and WAL rows are unchanged code
since the last report. Full table: `core-vs-rocksdb-0021/report.md`.

| Loss on 2026-10-20 | Then | Now |
|---|---|---|
| Scans (YCSB E), fits | 1.21× slower | 1.54× faster |
| Scans, 2× memory | 1.12× slower | 2.12× faster |
| Write p99 at 2× (fillrandom) | 140 µs vs 3.6 | 2.1 µs vs 3.7 |
| Fills at 2× | 2.65–3.71× faster | 5.98–6.25× faster |
| Disk, 1 KiB YCSB A/F fits | 97 vs 77 MiB | 66 vs 77 MiB (14% less) |
| Peak RSS, 100 B fills that fit | 94–131 vs 27–37 MiB | 59–94 vs 27–35 MiB |
| Synchronous commits | 4.76–5.09× slower | 4.70× slower (one row); the turn log |
| Opening 1,000 indexes | 85 s vs 22 s | unchanged; the turn log |

strata now wins every non-durable row measured (26 of 26) and loses only synchronous durability.
Peak memory is still 2.2–3.5× RocksDB's on small-value rows: the write buffer (about 290 B per
record against 177 designed) is next.
