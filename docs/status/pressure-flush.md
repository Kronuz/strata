# Pressure flush (phase4/pressure-flush)

Branch `phase4/pressure-flush` from main `b1c3be9`; worktree `strata-wt/pressure-flush`. Finishes the
explorer's win in `docs/perf/write-p99.md` (branch `explore/write-p99`). Touches only `needs_flush()`
and a new option; `phase4/flush-memory` owns the writer and the flush body.

## State

Done and pushed; waiting for the lead's measurement. No reviewer (small fix, outside sensitive areas).

## Done

1. `165985b` Option `pressure_flush_divisor` (default 4; 0 = flush on any pressure). Under budget pressure
   `needs_flush()` waits for buffer + version map >= min(buffer_share, budget capacity) / divisor. The
   capacity cap keeps the gate live when `buffer_share` is unset (its default is unlimited).
2. `165985b`, next commit: regression test `pressure_flush_bounded` in `test/background_merge_test.cc`: the budget
   held past its capacity, 2000 applies of 200 bytes, share 256 KiB: 14 segments by default, 1999 with
   divisor 0.
3. Ab tooling: `bench/kv/ab.py` gains the `pressure` and `pressure_more` row sets, and records a failed run
   (`failures.jsonl`) instead of ending the set.

## Proof

1. Fails with the fix reverted (gate replaced by `return true`): `CHECK(gated ...)` fails, "1999 segments gated".
2. Mutant (threshold share/(4*250), i.e. share/1000): same check fails, 1999 segments.
3. Full ctest 23/23 (RelWithDebInfo). ASan+UBSan (llvm@22, -O1, Debug) ctest 23/23.
4. A/A (main vs main, twox1 rows + fits fillrandom, 5 trials each, one thread, 100k ops, `quiet run
   pressure-flush`): every row a tie (B/A 0.75 to 1.09, CIs include 1; fillseq is the widest, [0.69, 1.12]).
5. A/B main (A) vs branch (B), same rows, 5 interleaved trials, ops/s and p99 us (A/B):

   | row | B/A [95% CI] | p99 A/B | peak RSS MiB A/B | disk MiB A/B |
   |---|---|---|---|---|
   | twox1 fillseq | 1.80 [1.74, 2.37] | 5.54 / 1.42 | 94 / 84 | 6.5 / 6.5 |
   | twox1 fillrandom | 2.25 [1.60, 2.42] | 154.6 / 2.38 | 100 / 71 | 6.9 / 6.8 |
   | twox1 overwrite | 1.71 [1.43, 1.81] | 207.9 / 8.51 | 144 / 141 | 14.6 / 14.4 |
   | fits fillrandom | 1.03 [0.96, 1.05] tie | 2.45 / 2.42 | 98 / 92 | 6.9 / 6.9 |

6. The explorer's untested rows, one trial each (A/B): sync fillrandom batch 100 0.98 (tie); wal fillrandom
   1.03 (tie); many readrandom (1000 indexes) 1.23, p99 2.61/2.35; twox1 ycsb_a 1 KiB 1.64, p99 21.1/8.3, peak
   RSS 635/610. One trial: a direction, not a measurement. The key-order scan hash was not rerun here (the
   explorer's KV_DIGEST is not on this branch); the test checks every key survives.

## Finding: fusefilter fence on main (given to a separate agent; not chased here)

`strata::index_fenced: strata: merge failure: fusefilter: construction failed` killed the driver twice, both
times on the A binary (main, no fix), both inside `bench/kv/ab.py` runs under host load: twox1 fillseq (A/A
attempt 1, side b) and twox1 overwrite (attempt 2, side a). Not reproduced standalone: 120 runs of the main
binary (40 each of fillseq, overwrite, fillrandom) and 120 of the branch binary, all ok; the branch binary had
no failure in about 50 harness runs either. The failure is timing dependent, so it needs load or a merge
interleaving, not a seed (the driver seed is the default 42).

Command (twox1 = 5 MiB budget, 100-byte values; run many times, concurrently with other load):

    kv_strata --dir D --workload overwrite --records 100000 --ops 100000 --value-size 100 --threads 1 \
      --durability none --batch 1 --memory-mb 5 --max-seconds 20

(also `--workload fillseq`). Loop used: `.scratch/rep.sh BIN WORKLOAD N` (in this worktree, untracked).
Failed runs and their stderr go to `failures.jsonl` of an `ab.py` OUT directory.

## Next

1. The lead measures on main after landing; CI on the branch (not watched).
2. Cause of the fusefilter fence: separate agent.

## Blocked on

Nothing.
