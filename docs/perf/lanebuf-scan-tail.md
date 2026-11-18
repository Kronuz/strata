> Status: HISTORY (2026-10-26) the bake-off and the scan-tail experiments; the owner accepted C and the write buffer is built on it (docs/design/write-buffer.md).

# C's scan tail (plan, written before the experiments)

Item: candidate C of the write-buffer bake-off (`bench/kv/proto/lanebuf.hh`, branch `perf/lanebuf`) misses T3
(seek + 10 nexts beside writers at full speed): scan p99 5.6 to 24 ms against RocksDB's 15 to 31 us and A''s
12 to 19 us. Results go to the "Scan tail" section of `write-buffer-bakeoff.md`; the ledger is `docs/status/lanebuf.md`.

Footprint: scan latency beside 1/2/4/8 writers (readers 4, 200,000 loaded keys, 100 B values, 2 s runs, buffer
growing to several million records), `bakeoff.py reads ... tune`. Guard rows: T1 (fillrandom 100 B, 1 and 4
writers), T2 (get p99), T6 (bytes per record), model check, cases.

Profile of the baseline (4 writers, 4 readers): readers spend about 65% of their time in `runs_at` sorting the
private tail (`tail_avg` 1,500 to 4,700 entries per scan, `tail_max` over 10^6); the single pool builder is busy
90% of the time (about 380 ns per entry built, merges of the main run included) and cannot keep up with 2 to 3.5M
writes/s, so every scan sorts a long tail.

Target: scan p99.9 at most 1.0x RocksDB's at 1/2/4/8 writers (design T3); T1 and T2 within 10% of today.

## Experiments (one hypothesis each; a loser's code is removed)

1. **E1**: runs of prefixed entries in a binary counter. Wins big, not enough.
2. **E2**: the builder stays on the buffer (fixed a use-after-free of the installed run set). Tie; removed.
3. **E3**: the scan selects the smallest tail entries instead of sorting the tail. Tie on E1; removed, retried on E5 and kept.
4. **E4**: the leader writes the prefix entry into the position index; the tail pass reads sequentially. Wins about 40% on p99.
5. **E5** (replaces the plan's "publish before the cascade"): a fresh job and a compactor job, applied as splices. Wins by 100x on p99 and 4 orders on p99.9.
6. **E6**: the scan's merge compares key prefixes and finds the minimum in one pass. Wins, small.
7. **E7**: counter ratio 8 instead of 2. Wins, small.

Procedure: tuned binaries from one source (`-D` switches, later made the only implementation), interleaved with C (the unmodified source
at 737948f, built as `e0`), C2 (the A/A twin), A' and RocksDB, five trials or more, inside `quiet strict`. A difference counts only beyond
the twins' spread. Identity: `check` (model, scans compared with the model), `cases`, TSan and ASan on every kept variant.

Outcome and numbers: `write-buffer-bakeoff.md`, "Scan tail". T3 is met at 2 MB/s and missed by 1.5 to 1.8x (p99.9) at full write speed.
