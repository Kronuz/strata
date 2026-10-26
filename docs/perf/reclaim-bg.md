# reclaim-bg: deletes on the host's executor, A/B

Raw result: `reclaim-bg-ab.log`; the command: `reclaim-bg-ab.sh` (run as `quiet strict NAME 15 "what" -- reclaim-bg-ab.sh 8`).

- Arms: A = main `1d76408`'s headers, B = `phase4/reclaim-bg`'s, the same driver (`bench/kv/kv_strata.cc`, which waits for the posted
  deletes in settle when the index has `wait_deletes()`) and flags (clang 23, `-O2 -g -DNDEBUG`), built side by side.
- Row: `ycsb_a`, 2,000 records, 20,000 ops, 64 KiB values, 256 MB, 1 thread, durability none, `--background 2`, eight
  interleaved rounds, macOS x86_64.
- Run-phase ops/s: A 10,619 (mean of 10,380 to 10,825); B 12,319 (12,025 to 12,586); B is 1.16x A
  (per round 1.13x to 1.20x; slowest B round 1.11x the fastest A round). Total time 2.75 s to 2.47 s.
- Disk after settle: 81,654,036 bytes in 10 files in both arms; get digest `a46c3543d251394d` in all 16 runs.
- The delete CPU moves to the pool; it does not vanish.
