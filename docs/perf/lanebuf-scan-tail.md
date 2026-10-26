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

1. **E1**: runs of prefixed entries in a binary counter. A build sorts only the backlog, merges amortize, and the
   comparisons use registers. Keep if scan p99 at 4 writers falls by more than the A/A spread.
2. **E2**: the builder stays on the buffer and rebuilds as soon as its result is installed (the source has a
   use-after-free of the installed run set, fixed by building from the job's own last output).
3. **E3**: the scan takes the tail through a lazy heap (selection of the n smallest) instead of a full sort.
4. **E4**: the leader writes the prefix entry into the position index, so the tail pass reads sequentially and
   dereferences no record.
5. **E5**: publish the fresh sorted run before the cascade of merges (a merge of a multi-million-entry level
   blocks the publication of the backlog behind it).

Procedure: tuned binaries from one source (`-DLANEBUF_E=n`), interleaved with C (E0), C2 (the A/A twin), A' and
RocksDB, five trials or more, inside `quiet strict`. A difference counts only beyond the twins' spread. Identity:
`check` (model, scans compared with the model) and `cases` on every kept variant.
