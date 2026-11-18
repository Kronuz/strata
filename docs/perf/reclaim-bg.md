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

## Linux x86_64

Measured 2026-10-26 on the strata Linux guest (qemu x86_64, 8 vCPU, 16 GiB, clang 18.1, Release `kv_strata`), each round's three runs under one `quiet exclusive` hold from the host. A = `45e6b32^1`, B = `45e6b32`, A2 = a second copy of A's binary (the A/A), built from separate worktrees. Command: `reclaim-bg-ab.sh`'s `run` loop with the doc's row (`ycsb_a`, 2,000 records, 20,000 ops, 64 KiB values, `--memory-mb 256`, `--threads 1`, `--durability none`, `--background 2`, `KV_GET_DIGEST=1`), 8 interleaved rounds in the order A, A2, B.

Run-phase ops/s, higher is better:

| Arm | median | per round |
|---|---|---|
| A | 13,410 | 13,430 13,507 13,164 13,401 10,543 13,419 13,576 13,386 |
| A2 | 13,370 | 13,409 13,416 12,725 13,371 13,368 13,349 13,492 13,308 |
| B | 14,441 | 10,755 14,488 14,138 14,169 14,443 14,500 14,439 14,477 |

B is 1.077x A by medians (7.7% faster; per round 0.80x, 1.07x, 1.07x, 1.06x, 1.37x, 1.08x, 1.06x, 1.08x), against 1.16x on macOS. A/A: A2 is 0.997x A. B's first round (10,755) is the one slow B run, probably a cold cache; the other seven B rounds are 14,138 to 14,500 and every A round but one is 13,164 to 13,576, so the gap is about 1.06x to 1.08x and consistent in direction (B ahead in 7 of 8 rounds). The 1.37x round is A's outlier (10,543). Means: A 13,053, B 13,926 (1.067x). Total time (mean) 2.06 s for A, 1.98 s for A2, 1.90 s for B; settle 0.28, 0.23, 0.21 s.

Disk after settle and digest: get digest `a46c3543d251394d` in all 24 runs, equal to macOS. B and A2 end at 81,654,028 bytes in 10 files in every round (macOS 81,654,036 bytes in 10 files; the 8 bytes are platform); A ended at 81,654,028 in six rounds but at 85,339,537 (10 files) in round 2 and 89,350,036 (11 files) in round 5, so main sometimes still holds obsolete files when settle returns (round 5 is also its 10,543 ops/s outlier), while the branch never did in these 8 rounds. A2, the same binary as A, did not show it, so it is a race, not a fixed difference; 2 of 8 rounds is too few to rate it.

Verdict on Linux x86_64: same disk and digest as macOS; the gain is real but smaller than on macOS (1.08x against 1.16x), at a row where the deletes are a few tens of milliseconds of a 1.5 s run. Not measured: arm64 Linux and FreeBSD.
