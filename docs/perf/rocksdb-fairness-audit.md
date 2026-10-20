# Fairness audit of the key-value harness against RocksDB

Status: plan, 2026-10-20. Audits `core-vs-rocksdb.md` (main `b8a2cb6`): for each row, what
RocksDB 11.11.1 actually did, from its own statistics, its info log, a barrier count, and its
source (read under Apache 2.0). Companion: `docs/research/rocksdb-mechanisms.md`, the
clean-room spec of the mechanisms this audit observes.

## Targets

Every row class of `core-vs-rocksdb.md`: the fill rows (`fillseq`, `fillrandom`, `overwrite`)
at fits and 2×, the read rows (`readrandom`, `readmissing`, `readseq`, `seekrandom`, YCSB B to
E), the mixed rows (YCSB A, F), the durable rows (`sync1`), the WAL row (`wal1`), four threads
(`fits4`), and the many-index row.

## Hypotheses (each kept or removed by evidence)

- **H1, background work.** At fits, RocksDB runs no flush or compaction inside a timed fill
  phase (the 32 MiB memtable holds the 11.6 MB load), so its fill rates are pure memtable
  inserts and its flush lands in the settle; at 2× (1 MiB memtables, a 1.25 MiB write buffer
  manager) flushes and L0 compactions run inside the timed phase and may slow or stop writes.
- **H2, sync rows.** `sync=true` reaches `F_FULLFSYNC` once per write group on macOS; one
  writer gets no group commit; concurrent sync writers share one barrier per group.
- **H3, `none`.** `disableWAL` plus the settle's flush makes everything durable with three
  barriers per flush (table file, directory, manifest), against strata's five per commit.
- **H4, read sources.** At fits, after the first touch every RocksDB get is a block cache hit;
  at 2× most are block cache misses served by the OS page cache, not the device.
- **H5, setting asymmetries.** Settings that differ between the engines in a way that favors
  either side: memory accounting, cache contents, the strata frame cache the driver does not
  use, compression, the sync rows' load phase, and the many-index create and settle.

## Procedure

1. Read the driver and harness for asymmetries; trace each durability path in RocksDB's
   source.
2. An audit build of `kv_rocksdb` (`KV_ROCKSDB_STATS=1`): RocksDB `Statistics` and the
   `rocksdb.stats` property dumped to stderr at each phase boundary (the settle's entry and
   exit), so the timed phase's tickers are separated from the settle's; the info log kept.
3. A barrier counter: an interposed `fcntl`/`fsync` counter (`DYLD_INSERT_LIBRARIES`) that
   prints, per file kind, how many `F_FULLFSYNC` calls each phase issued, for both drivers.
4. One trial per row with statistics on (statistics cost CPU, so these runs give counts, not
   rates); rates are rerun with statistics off only where a fix changes a row, interleaved,
   three trials, under a light hold.

A finding stays if the counters or the source show it; a ratio is restated only from a rerun.
