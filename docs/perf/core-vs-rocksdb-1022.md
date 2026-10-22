# strata core against RocksDB after the week's fixes (2026-10-22)

Quick tier on main `7aad7c4` (format 14 step 1, pressure flush, flush memory, retention 0, the
fusefilter fix, scan API, compact slots, shared reader core, reclaim backlog, streaming merges)
against RocksDB 11.11 `lvl-16k-lz4`, same harness and Mac as `core-vs-rocksdb-0021.md`; five
trials per row, strict holds, run overnight in pieces. Full table: `core-vs-rocksdb-1022/report.md`.

- **Speed:** faster on all 43 rows measured: 1.15–2.5× when data fits, 1.4–6.5× at 2× memory,
  1.2–3.9× at 10× RAM, 1.05–2.3× across value sizes.
- **Disk:** at or below RocksDB on every row (10× RAM 69 vs 67 MB, 64 KiB overwrites 78 vs 82 MB),
  down from 2.6–6.5× on 2026-10-21.
- **Memory:** below RocksDB on every 1 KiB YCSB row that fits (124–186 vs 207–249 MiB) and at the
  value-size ladder; still above it on 100-byte rows that fit (44–60 vs 27–39 MiB) and at 10× RAM
  (182–229 vs 68–149 MiB, down from 353–645). The rest is the key builders' state and the write
  buffer; the write-buffer design replaces the latter.
- **Not run:** synchronous and WAL rows (unchanged code; the turn log is their fix), multi-thread.
