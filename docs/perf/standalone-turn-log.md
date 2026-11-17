# Standalone durability through the turn log against RocksDB

The quick tier's durable rows on the standalone turn log (`docs/design/standalone-turn-log.md`),
strata `193e6e32` against RocksDB 11.11 on its calibrated `lvl-16k-lz4`. Development Mac (Intel
i9-9980HK, APFS, `F_FULLFSYNC` barriers), one thread, exclusive holds. `sync1` ran as three
one-trial jobs, `wal1` as one three-trial job, and the A/A ran first on the sync rows. Each
job's log holds the full report (`campaign job log ID`).

## Where it stands

- **Synchronous durability: a tie or better on every row.** Batch 1, 10 and 100 run at 0.96-1.05× RocksDB's
  throughput, against 4.76-5.09× slower on main (`core-vs-rocksdb-f14.md`). One barrier per
  acknowledged batch, the same as RocksDB (M2).
- **WAL tier: 4.48-6.72× faster**, with 7.4-10.1× lower p99 and 4.0-6.1× less CPU per op. This tier
  runs `ack=none` on the log (D104): it is not equal durability (see "The ack=none window").
- **Group commit is 1.2-1.45× slower.** With K writers on one log, strata reaches 99 ops/s at K=4
  and 186 at K=8, against RocksDB's 121-144 and 228-247. This is turn-log-parity's A6, not fixed
  here.
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

## Four axes, after the fix

Ranges are over the three `sync1` trials and the three `wal1` trials. A ratio within 10% is a tie
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
(21.6-22.2 ms).

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
fillrandom sync/1, two trials (job 261117-034940-b2b9c3 at `7db7a919`). That commit predates the
batch fix, which leaves batch-1 rows unchanged.

| K | strata L=1 ops/s | strata L=K ops/s | RocksDB ops/s | strata p50 (L=1) | RocksDB p50 |
|---|---|---|---|---|---|
| 1 | 48.5, 47.6 | - | 47.8, 45.1 | 20.3-20.8 ms | 20.8 ms |
| 4 | 98.8, 99.7 | 63.7, 61.7 | 144.1, 121.3 | 40.6-41.7 ms | 21.1-23.5 ms |
| 8 | 185.8, 186.0 | 122.9, 118.7 | 227.8, 246.7 | 41.2-41.7 ms | 40.1-40.6 ms |

At K=4, strata is 1.2-1.45× slower with twice RocksDB's median: a writer that arrives while a
barrier is in flight waits for that barrier and then the next. One log per core is slower still
on macOS, where `F_FULLFSYNC` calls from several logs coalesce at a longer latency (D104). Closing
this gap is turn-log-parity's A6.

## The ack=none window

The `wal` tier returns at `apply`, before its turn's barrier (D104). A process crash loses up to one
barrier plus one write after `apply`: about 21 ms here. The M4 floor is 4 KiB pwrite plus
`F_FULLFSYNC` at 48.4/s, with p50 20.2 ms and p99 36.2 ms (261117-034943-316356). Until that
barrier, a writer does not see its own acknowledged write (D103, Q3's read-your-writes gap). The
`wal` rows above are throughput under that contract, never a claim of equal durability. Everything
whose turn's barrier returned survives power loss.

## The honest gap (D103, D104)

1. **Group commit:** 1.2-1.45× slower at K=4 to 8 on one log (turn-log-parity A6).
2. **`ack=none` reads:** a writer misses its own write for up to one barrier (D103 Q3).
3. **`log_lost`:** a host that lost its log refuses the index rather than reopening it (D103 Q4).
4. **Command limit:** 64 MiB per transaction on a logged index (D103 Q5).
5. **Memory and disk:** 2.5-3.4× peak RSS on 1 KiB and overwrite rows, 3.3× disk on batched fills,
   57-91% more written. These are the core's gaps (`core-vs-rocksdb-f14.md`); on small rows the
   disk is the host's preallocated segment.

D103 assigns items 2-4 to turn-log-parity after document-gate. At one writer, the turn log's
synchronous rows now match RocksDB's throughput and barrier count.
