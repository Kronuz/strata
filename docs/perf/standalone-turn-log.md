# Standalone durability through the turn log against RocksDB

The quick tier's durable rows on the standalone turn log (`docs/design/standalone-turn-log.md`),
strata `cc8ba3aa` (the backpressure that bounds the `ack=none` window, D107) against RocksDB 11.11
on its calibrated variants (`lvl-16k-lz4` at 100 B; `lvl-4k-none` at 1 KiB for the `sync1` rows at
`a93f2b59`, `docs/gates/document-profile.md`). Development Mac (Intel i9-9980HK, APFS, `F_FULLFSYNC` barriers),
exclusive holds. `sync1` ran as three one-trial jobs, `wal1` as one three-trial job, and the A/A
(at `193e6e32`) on the sync rows. Each job's log holds the full report (`campaign job log ID`).

## Where it stands

- **Synchronous durability: a tie on every row.** Batch 1, 10 and 100 run at 0.98-1.11× RocksDB's
  throughput at `a93f2b59` (code `cc8ba3aa`; 0.93-1.01× with 1 KiB on `lvl-16k-lz4`, 0.96-1.16× at
  `193e6e32`), against 4.76-5.09× slower on main
  (`core-vs-rocksdb-f14.md`). One barrier per acknowledged batch, the same as RocksDB (M2).
- **WAL tier: 1.68-1.94× faster**, with 7.8-11.9× lower p99 and 3.6-5.4× less CPU per op, but a
  p99.99 113-179× RocksDB's: a put that meets a full record waits for its seal. Backpressure holds
  the `ack=none` window to one barrier plus one write and costs 2.98-3.81× of the unbounded
  throughput (4.48-6.72× faster before it). This tier is not equal durability (D104).
- **Group commit: a tie at K=1 and K=8 on one log, 1.32-1.39× slower at K=4.** strata reaches
  105-106 ops/s at K=4 and 221-222 at K=8, against RocksDB's 140-146 and 222-235. Closing K=4 is
  turn-log-parity's A6.
- **Memory, disk and bytes written are the remaining gaps** on the durable rows: 2.5-3.4× the peak
  RSS on 1 KiB and overwrite rows, 3.3× the disk on batched fills, and 57-91% more written.

## The one-flush fix

Before the fix (`9d7c7710`), `kv_strata` applied each put of a sync batch as its own transaction.
The served loop sealed the open record at the first put and wrote a second turn for the rest, so a
batch cost two barriers against RocksDB's one `WriteBatch` write. The engine now applies a sync
batch as one transaction at `durable()` (`bench/kv/kv_strata.hh`). `test/kv_strata_batch_test.cc`
checks one record, one write and one barrier per batch, and mutant M-r (batching off) fails it:
48 barriers for 32 batches.

| row | before: strata/RocksDB | after: strata/RocksDB (3 trials) | `F_FULLFSYNC` per op, before → after (RocksDB) |
|---|---|---|---|
| fillrandom sync/100 | 0.504 (1.98× slower) | 1.019, 1.008, 1.008 | 0.020 → 0.010 (0.010) |
| overwrite sync/100 | 0.502 (1.99× slower) | 0.984, 1.160, 0.985 | 0.020 → 0.010 (0.010) |
| fillrandom sync/10 | 0.653 (1.53× slower) | 0.993, 0.997, 0.987 | 0.182 → 0.100 (0.100) |
| fillrandom sync/1 | 1.013 | 0.994, 1.048, 0.994 | 1.001 → 1.001 (1.001) |
| ycsb_a sync/1 | 0.988 | 0.988, 1.008, 0.993 | 0.496 → 0.496 (0.494) |
| ycsb_f sync/1 | 0.996 | 0.995, 0.962, 0.987 | 0.495 → 0.498 (0.494) |

Before: `sync1` 261117-033324-78da56, M2 261117-034937-925436. After: `sync1`
261117-055245-3aecbb, 261117-055253-80fcf5, 261117-055301-8578af; M2 261117-055334-a41ea3; batch
test 261117-055212-dc6fc0; M-r 261117-055220-ecd5d0.

## Four axes, after the one-flush fix

At `193e6e32`, before the backpressure; the sync rows at `cc8ba3aa` are in "Where it stands", and
the `wal` rows at `cc8ba3aa` in "The ack=none window". Ranges are over the three `sync1` trials and
the three `wal1` trials. A ratio within 10% is a tie
at this tier. "To durable" counts the settle that makes a WAL row durable.

| row | throughput | to durable | p99 | CPU/op | peak RSS | disk | bytes written |
|---|---|---|---|---|---|---|---|
| fillrandom sync/100 v100 | tie (1.01-1.02) | tie | 0.92× to 2.05× lower, noisy at µs | 1.27-1.49× less | 29-51% more | 3.3× | 57% more |
| overwrite sync/100 v100 | tie (0.98-1.16) | tie | 3.18× lower to 1.66× higher | tie to 1.14× less | 2.5-3.0× | 73-79% more | 90-91% more |
| ycsb_a sync/1 v1024 | tie (0.99-1.01) | tie | tie to 1.40× lower | tie to 1.21× less | 2.8-3.4× | 15% more | 88-89% more |
| ycsb_f sync/1 v1024 | tie (0.96-0.99) | tie | tie to 1.36× higher | 1.12-1.17× less | 2.5-3.0× | 15% more | 88-89% more |
| fillrandom sync/1 v100 | tie (0.99-1.05) | tie | 1.95× lower to 1.65× higher | tie to 1.34× more | 42-44% less | 37.8-39.5× | tie |
| fillrandom sync/10 v100 | tie (0.99-1.00) | tie | tie | tie to 1.12× more | 32-36% less | 7.7-7.8× | tie |
| fillseq wal v100 | 5.53× faster | 1.28× faster | 8.85× lower | 5.15× less | 2.3× | 3.1× | 58% more |
| fillrandom wal v100 | 6.72× faster | 1.30× faster | 10.13× lower | 6.12× less | 2.6× | 3.7× | 89% more |
| overwrite wal v100 | 4.48× faster | 1.13× faster | 7.39× lower | 4.01× less | 2.8× | 2.6× | 2.2× |

`wal1`: 261117-055310-7264db. On batched rows a sync p99 is a put that does not wait for the
barrier, and it moves by 2× between trials for both engines. p99.9 sits at one barrier for both
(21.6-22.2 ms). The `wal` rows' p99.99, from 100k ops (10 samples), was 2.4-5.9× RocksDB's here
(fillseq 428 vs 72 µs, fillrandom 465 vs 106, overwrite 194 vs 81).

`sync1` at `a93f2b59` (code `cc8ba3aa`), RocksDB at 1 KiB on `lvl-4k-none` (261118-202755-3d88cf,
261118-202758-1425d5, 261118-202802-fb1582): fillrandom sync/100 0.998-1.114, overwrite sync/100
0.976-1.006, ycsb_a 0.977-0.998, ycsb_f 0.979-0.989, fillrandom sync/1 0.983-1.000, sync/10
0.991-1.001. The new pin leaves RocksDB's 1 KiB disk uncompressed (101.5 MiB against strata's
66.5, 34% less for strata) and its peak RSS at 76-84 MiB (3.2× and 2.8× for strata). At
`cc8ba3aa` on `lvl-16k-lz4` (261117-134818-6068ec, 261117-134824-199dc8, 261117-134834-f21397)
the same rows ran 0.929-1.011. The backpressure does not touch
these rows: each batch is one record and one turn.

The disk on small sync rows is the host's preallocated 4 MiB turn-log segment against 0.1-0.6 MiB
of data, a fixed cost per host rather than per index. Peak RSS on 1 KiB rows is the core's own
memory gap (`core-vs-rocksdb-f14.md`, loss 2), not the log's.

## A/A

Each engine against itself, alternating trials as sides, on `syncaa` (fillrandom sync/100,
ycsb_a sync/1, fillrandom sync/1). Jobs 261117-055320-5ca39d and 261117-055327-074989.

| engine | fillrandom sync/100 | ycsb_a sync/1 | fillrandom sync/1 |
|---|---|---|---|
| strata, b/a | 0.978, 1.000 | 0.998, 1.004 | 0.998, 1.019 |
| RocksDB, b/a | 0.997, 0.926 | 1.004, 0.988 | 1.002, 0.998 |

The spread was 0-2.3% for strata and 0.2-8.0% for RocksDB. The ties above sit inside that noise.

## Group commit (the open gap)

K writers, each on its own index, on L turn logs, against RocksDB's K threads on one DB.
fillrandom sync/1, two trials: job 261117-134842-7d024d at `cc8ba3aa` (before:
261117-034940-b2b9c3 at `7db7a919`).

| K | strata L=1 ops/s | strata L=K ops/s | RocksDB ops/s | strata p50 (L=1) | RocksDB p50 | L=1 at `7db7a919` |
|---|---|---|---|---|---|---|
| 1 | 48.3, 47.9 | - | 47.4, 47.9 | 20.3-20.6 ms | 20.8 ms | 48.5, 47.6 |
| 4 | 106.1, 105.0 | 63.4, 70.8 | 145.7, 140.4 | 40.6-41.2 ms | 21.1 ms | 98.8, 99.7 |
| 8 | 222.0, 221.2 | 122.0, 116.1 | 234.9, 222.3 | 40.1-40.6 ms | 40.1-41.2 ms | 185.8, 186.0 |

At K=4, strata is 1.32-1.39× slower with twice RocksDB's median: a writer that arrives while a
barrier is in flight waits for that barrier and then the next. At K=8 it ties (0.94-1.00×). One
log per core is 1.5-1.9× slower than one shared log, on macOS, where `F_FULLFSYNC` calls from
several logs coalesce at a longer latency (D104). K=8 on one log moved from 186 to 221 ops/s
between the two commits; batch-1 rows put one record per turn, so the backpressure is not the
cause, and the commits between were not bisected. Closing K=4 is turn-log-parity's A6.

## The ack=none window

The `wal` tier returns at `apply`, before its turn's barrier (D104). A process crash loses up to one
barrier plus one write after `apply`: about 21 ms here. Journal seals one record of a group per
turn, so without a bound a writer faster than the barrier piled up closed records: the durability
review's probe saw the log 34 records (13.4 s) behind apply at 1M puts. `apply` now waits before
closing a record while the unsealed ones fill a turn (D107), so the log stays within two records
of apply (one turn in flight, one open record), and an index holds at most one command unsealed.

| probe, `wal`, 16 B keys, 100 B values | max lag (records) | apply to durable p99 | ops/s |
|---|---|---|---|
| before (261117-082815-50cdbe, `12f59669`), 1M / 3M puts | 34 / 31 | 13.4 / 12.0 s | 35,322 / 30,021 |
| after (261117-134801-f716c7, `cc8ba3aa`), 1M / 3M puts | 2 / 2 | 2.0 / 1.9 s | 45,729 / 49,900 |

The probe runs under a light hold on a loaded host, so its barriers are slower than a row's; the
record counts are the host-independent part. The apply-to-durable p99 counts the probe's own
sampling, not only the log. Test `lag` (2 ms barriers) holds the same bound, and mutants M-s (no
backpressure) and M-t (one closed record allowed behind the open one) fail it.

The cost, on the rows (`wal1`, three trials, 261117-134809-8409da at `cc8ba3aa`, against
261117-055310-7264db at `193e6e32`):

| row | strata ops/s before → after | RocksDB ops/s | strata/RocksDB before → after | p99 µs s/r | p99.99 µs s/r |
|---|---|---|---|---|---|
| fillseq wal v100 | 913,115 → 244,152 | 144,914 | 5.53 → 1.68 | 2.96/23.17 | 16,908/95 |
| fillrandom wal v100 | 924,436 → 242,326 | 124,882 | 6.72 → 1.94 | 2.42/28.80 | 16,450/96 |
| overwrite wal v100 | 636,146 → 213,209 | 126,801 | 4.48 → 1.68 | 3.79/30.34 | 12,911/114 |

The cap costs 2.98-3.81× of strata's unbounded `wal` throughput: one 1 MiB record (about 9,000 puts)
per turn. It stays 1.68-1.94× faster than RocksDB, with 7.8-11.9× lower p99 and 1.20-1.42× faster to
durable, but its p99.99 is 113-179× RocksDB's: the put that meets a full record waits for the loop
to seal it, up to one barrier. Journal's packing (a sealer called again with the room left, asked of
Detent) raises the cap to a turn's room; it lands as a follow-up measured on these rows.

The M4 floor is 4 KiB pwrite plus `F_FULLFSYNC` at 48.4/s, with p50 20.2 ms and p99 36.2 ms
(261117-034943-316356). Until that barrier, a writer does not see its own acknowledged write (D103,
Q3's read-your-writes gap). The `wal` rows above are throughput under that contract, never a claim
of equal durability. Everything whose turn's barrier returned survives power loss.

## The honest gap (D103, D104)

1. **Group commit:** 1.32-1.39× slower at K=4 on one log, a tie at K=8 (turn-log-parity A6).
2. **`ack=none` tail:** p99.99 113-179× RocksDB's on `wal` rows while one record per turn caps
   the writer (journal's packing, D107).
3. **`ack=none` reads:** a writer misses its own write for up to one barrier (D103 Q3).
4. **`log_lost`:** a host that lost its log refuses the index rather than reopening it (D103 Q4).
5. **Command limit:** 64 MiB per transaction on a logged index (D103 Q5).
6. **Memory and disk:** 2.5-3.4× peak RSS on 1 KiB and overwrite rows, 3.3× disk on batched fills,
   57-91% more written. These are the core's gaps (`core-vs-rocksdb-f14.md`); on small rows the
   disk is the host's preallocated segment.

D103 assigns items 3-5 to turn-log-parity after document-gate. At one writer, the turn log's
synchronous rows now match RocksDB's throughput and barrier count.
