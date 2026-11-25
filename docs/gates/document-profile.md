# The document profile gate

The gate measures strata's lifecycle core, used as a key-value engine, against RocksDB 11.11 on
all four axes (PLAN, "The `document` profile gate (after Phase 4)"; `docs/design/rocksdb-survey.md`
K2). The owner landed it on these numbers (PLAN D143): strata is faster than RocksDB on
throughput in most rows and loses on the rows and axes listed under "Losses against RocksDB",
which `turn-log-parity` A6 closes.

Rows ran on the development Mac (Intel i9-9980HK, APFS, 1 thread, exclusive holds, one trial per
row) at `dd02c63d`, unless marked. The merged head `1bf96fbd` adds main's `catalog.hh`, its test
and notes; no header under `include/` or `bench/kv/` includes `catalog.hh`, so `kv_strata` is the
code `dd02c63d` timed, and no row reran at the merged head. Ratios are strata over RocksDB; a
ratio within 10% is a tie.

## Prerequisites (A1, A2)

| Prerequisite | Where | Evidence |
|---|---|---|
| Standalone durability through the turn log (A1) | `include/strata/core/turn_seal.hh`, `command.hh`, `index.hh` logged mode; `docs/design/standalone-turn-log.md` "Built" | build-test 261119-165623-eec44f; crash, TSan and mutant jobs in the quest journal's Evidence |
| Format 13's ordered `keys` section | `76fa3313` ("Format 13 checkpoint"), an ancestor of `dd02c63d` | quest `format13`, landed |
| Snapshot publication: shared reader views | `23e78bb6` (merge of `phase4/shared-reader-views`) | quest `shared-reader-views`, landed |
| Snapshot publication: write buffer reads | `2f56d64c` (merge of `phase4/write-buffer-reads`) | quest `write-buffer-reads`, landed |
| The manifest log as the checkpoint | `15d39c30` | quest `manifest-log`, landed |

## Rows measured (A3, A4)

Each set is the harness's row definition (`bench/kv/run.py`); both engines get an equal memory
limit. RocksDB's pin per value size (A5) is `lvl-16k-lz4` at 100 B and `lvl-4k-none` at 1 KiB, the
fastest at `dd02c63d` (`docs/perf/rocksdb-fairness-audit.md`, "RocksDB's pin per value-size
class"; jobs 261119-165657-597c2b and 261119-165659-de5c70).

| Set | Memory | Rows | Throughput, strata over RocksDB | Job |
|---|---|---|---|---|
| `fits1`, tier none | fits | db_bench fills, reads, seeks (100 B) and YCSB A-F (1 KiB) | faster on all 13: 1.09-2.95× (fillseq 1.09×, overwrite 1.12×, YCSB A 1.36×, YCSB E 1.58×) | 261119-165709-5e305c |
| `twox1`, tier none | 2× (half the memory) | the same 13 | faster on all 13: 1.91-6.97× | 261119-165712-a6c262 |
| `big1`, tier none | 10× (1M records at 12 MiB; YCSB A 200k at 24 MiB) | fillrandom, readrandom, seekrandom, YCSB A | faster on all 4: 1.60-4.03× | 261119-165716-6bc7ac |
| `tenx1`, tier none | 10×, as `big1` | `fits1`'s 13 rows at that scale | faster on all 13: 1.49-3.96× (readseq 1.49×, seekrandom 1.71×, overwrite 2.01×) | 261119-165720-85290b |
| `sync1`, sync/100, /10, /1 | fits | fillrandom, overwrite, YCSB A, F | tie on every row (0.98-1.05×); one barrier per batch, as RocksDB | 261119-165700-a330b1 |
| `wal1`, `ack=none` | fits | fillseq, fillrandom, overwrite | 1.33-1.69× faster; the window is bounded at one barrier plus one write (D107); not equal durability (D104) | 261119-165704-63dd29 |
| group commit | fits | K=1, 4, 8 writers, fillrandom batch 1 | tie at K=1 (47.5 against 47.8 ops/s) and K=8 on one log; losses below | 261119-165705-96e179 |

Variance: the `723a5e97` trials of the same engine source (261119-054043-5ab1f3,
261119-054047-bffba3, 261119-054051-012889, 261119-054055-fde881, 261119-054058-2c1749) and the
`tenx1` dry run (261119-054339-e1a2d2) are the record. Over four trials every `none` throughput
row stays above 1.0; the lowest single trial is `big1` seekrandom at 1.03×, `fits1` readrandom at
1.06× and `fits1` fillseq at 1.09×. The A/A on the sync rows (261117-055320-5ca39d,
261117-055327-074989) spreads 0.2-0.4% for sync/1 and 2.3% for strata at sync/100, 0.2-8.0% for
RocksDB across rows.

## Losses against RocksDB (A4)

Every loss the rows above measured, each with its magnitude. `turn-log-parity` A6 is where they
close; the gate does not hide or re-measure them.

Throughput and tails:

| Loss | Magnitude | Source |
|---|---|---|
| `fits1` overwrite, time to durable | 1.14× slower in the reported trial; 0.79-0.96× over four trials (throughput itself is 1.12× faster) | 261119-165709-5e305c |
| `fits1` YCSB A, to durable | tie (0.93×); trials span 0.92-1.07× | same |
| Group commit, K=4 writers on 1 log | 1.39× slower (103 against 143 ops/s, two trials) | 261119-165705-96e179 |
| Group commit, K=4 on 4 logs; K=8 on 8 logs | 2.1× and 1.9× slower (68 against 143; 118 against 221 ops/s) | same |
| sync/1 fillrandom and YCSB F | 1-2% slower, outside the A/A band (0.98-1.00× and 0.98-0.99× over four trials) | 261119-165700-a330b1 |
| p99, tier none | 1.19-2.08× higher on `tenx1` fillseq, fillrandom, overwrite; 1.64× higher on `twox1` overwrite | 261119-165720-85290b, 261119-165712-a6c262 |
| p99, sync/100 | 1.30× higher on fillrandom, 1.13× on overwrite | 261119-165700-a330b1 |
| p99.99, `wal` | 130-193× higher (9.5-17.2 ms against 73-92 µs) | 261119-165704-63dd29 |
| `wal1` in the Linux and FreeBSD VMs | 1.15-1.38× slower, p99 1.42-1.87× higher | VM rows below |
| 1 KiB fillrandom, tier none | 0.78× RocksDB at 256 MiB: strata flushes inline on the writer (profile 8c6f59, quest journal); a background flush is an engine change | quest journal |

Memory, CPU and disk:

| Axis | Loss and magnitude | Where |
|---|---|---|
| Peak memory | up to 3.4× on sync/1 YCSB A, 3.1× on YCSB F, 2.6× on sync/100 overwrite, 2.2-2.9× on `wal1`; 2.3× on `big1` fillrandom and `tenx1` fillseq and fillrandom, 2.1× on `tenx1` overwrite; 23-87% more on the other `big1` and `tenx1` rows; 11-73% more on `fits1` fills, reads and overwrite; up to 69% on `twox1` | sync1, wal1, big1, tenx1, fits1, twox1 |
| CPU per operation | 1.97× more on `tenx1` fillseq, 1.42× more on `tenx1` overwrite, 1.15× more on sync/1 fillrandom | tenx1, sync1 |
| Disk | up to 39.5× on sync/1 fillrandom, 7.9× on sync/10, 3.3× on sync/100 fillrandom, 3.7× on `wal1` fillseq and fillrandom, 2.6× on `wal1` overwrite, 74% on sync/100 overwrite | sync1, wal1 |
| Bytes written | 3.2× on `tenx1` fillseq, 2.5× on `twox1` fillseq, 82% to 2.2× on `wal1`, 36-92% on the sync rows | tenx1, twox1, wal1, sync1 |

Wins on the same axes: scans (YCSB E 1.58-3.50×, seekrandom 1.60-2.39×) where the earlier
`75d1e7e` rows lost by 1.12-1.21×; CPU per operation on every read row (1.49-6.79× less); disk 33-54%
less and bytes written 52-78% less on YCSB at fits, 2× and 10×. The lanebuf scan tail at full
write speed (1.5-1.8× RocksDB's p99.9, accepted by the owner, 2026-10-26) is
`docs/perf/lanebuf-scan-tail.md`'s.

### VM rows (A6, VM-virtio, D114)

qemu with HVF on this Mac, virtio disk with `cache=none` on APFS: the barriers measure qemu's, not
a device's, so these rows check the platform, not the gate's ratios. Ubuntu 24.04 (Linux 6.8,
4 vCPU, 6 GiB) and FreeBSD 14.4 (4 vCPU, 3 GiB), at `dd02c63d`, one trial each, RocksDB on the
1 KiB pin `lvl-4k-none`.

| Platform | Dry run (every set, 2,000 records) | sync1 | wal1 | Jobs |
|---|---|---|---|---|
| Linux x86_64 | passed | 1.36-2.02× faster on all six rows | 1.15-1.35× slower, p99 1.42-1.87× higher, CPU 1.40-1.62× more | build 261119-165632-5fe923, dry 261119-165632-dc968f, rows 261119-165632-8f14ec |
| FreeBSD x86_64 | passed | 1.21× faster to 1.12× slower (sync/1 fillrandom 1.12× slower; the other rows tie or win) | 1.31-1.38× slower, p99 1.50-1.53× higher, CPU 1.51-1.59× more | build 261119-165633-50f1f8, dry 261119-165633-c6bdbf, rows 261119-165633-528b91 |

In the VMs strata's peak memory runs up to 2.5× RocksDB's and its disk up to 13.8× (Linux sync/1
fillrandom). FreeBSD has no per-process write counter, so its bytes-written column reads zero and
is unmeasured. wal1, faster on the Mac, is slower in both VMs, where strata spends 1.4-1.6×
RocksDB's CPU per write; why is not measured. arm64: CI's linux-arm64 job builds and runs the
tests; its rows are owed before Phase 3R's next gate (D105), and no arm64 host exists beyond
GitHub's CI (D36).

## Scale

Each claim holds at the scale its rows measured, and no further:

- **Rows:** 100 B and 1 KiB values; `fits1` under a 256 MiB budget, `twox1` at half the memory,
  `big1` and `tenx1` at 1,000,000 records of 100 B (200,000 of 1 KiB) at 12-24 MiB; one thread
  and one index. Sync rows are capped at 15 s, about 700-70,000 acknowledgements. Group commit:
  up to 8 writers on 1-8 logs. The many-index row (1,000 indexes) belongs to main `75d1e7e` and
  did not rerun.
- **Limits** (each from a mechanism): a logged transaction is at most 64 MiB, consensus's command
  ceiling (D3, Q5). Checkpoints are due every 64 MiB of log. A host runs 1-4,096 loops, one turn
  log each, with 64 MiB segments by default. Group ids are 64-bit and never reused. The benchmark
  host uses 1 MiB commands and 4 MiB segments (`bench/kv/kv_strata.hh`).
- **Platforms:** macOS x86_64 on APFS carries the gate's rows; Linux and FreeBSD x86_64 run in
  qemu VMs (sync and wal1 rows, one trial each, VM-virtio).
- **Not measured:** more than 8 writers, millions of indexes on one host, arm64, Linux or FreeBSD
  on a device rather than a VM, and more than one trial per row beyond the variance record.

## Checks (A7)

At `dd02c63d`, 75 CPU-minutes (3 workers × 25 minutes, ASan and UBSan) of fuzzing per decoder, all
exit 0: `stored` (format 14) 261119-165634-78c0da, `manifest_log` 261119-165634-438cd8,
`manifest_log_file` 261119-165633-0f74fd, `command` 261119-165634-abafdb. The CPU-hour per changed
decoder is deferred (D143); these runs fall short of it. Readers reject unknown versions, and the
fixtures decode: 261119-165635-a1dd1c. Coverage: 261119-165634-90ce4e. Build-test:
261119-165623-eec44f. ASan, UBSan and TSan run on CI's Linux jobs at the landing commit.

## Status (A8)

Every row above names its commit, platform and jobs; the quest journal's Evidence table maps each
criterion to them. The gate landed on these numbers, with the losses above as `turn-log-parity`
A6's inputs.
