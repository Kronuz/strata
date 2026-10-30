> Status: HISTORY (2026-10-19) code not on main; its attribution predates the prospect query wins, verify before citing.

# Run-level A/B scoring with an A/A band

The lead's ruling of 2026-10-19, after the query-gap experiments found D14's 1.03 guard inside
the host's run-to-run noise: a same-binary A/A on S′ read up to 1.074 on the reordered merged
index and 1.129 on the 16-segment index (`query-gap-m3.md`), while `ab.py`'s interval, a
bootstrap over queries of each query's best time over all rounds, carries none of that drift.

## The method

1. **Runs.** `bench/quick/ab.sh` already interleaves the two sides round by round (A then B
   in odd rounds, B then A in even ones). Each round is a run.
2. **A run's value per row.** For a row (a class or stratum tag, at one command), the median
   over the row's queries of B's time over A's in that run. The median keeps one slow query
   from moving a run.
3. **The row's ratio and interval.** The geometric mean of its runs' values, with a 95%
   t-interval on their logarithms (degrees of freedom: runs − 1). The interval is over runs,
   so drift between runs widens it.
4. **The A/A first.** Every session runs A against itself first, the same way and with the
   same number of runs, and scores it the same way. Its per-row interval is the row's noise
   band for that session.
5. **Verdicts.** A row counts as a **loss** only when its lower bound clears both 1.03 and the
   A/A band's upper bound. It counts as a **win** only when its upper bound is below both 1.0
   and the A/A band's lower bound, with its ratio at most 0.97 (D14 item 1's level). Anything
   else is **neither**: not established either way at this session's noise.
6. **Keep or remove.** D14 reads these verdicts: an item needs a win on a named target row and
   no loss on any set or class row of both indexes (under both presets where D14 asks), and a
   row of neither stays neither, never a pass or a failure by itself.

## The tooling

`bench/quick/ab.py OUT --aa AA_OUT` prints the run-level table (row, runs, ratio and
interval, the A/A's ratio and interval, the verdict); without `--aa` it prints the query-level
tables as before (`32d598a`, brought onto this branch). `bench/quick/ab_test.py` adds three
cases: a run's median ignoring one slow query and the t-interval over runs computed by hand;
every verdict boundary (inside the band, below the guard, above 0.97); and the table's
markdown. `ruff format`, `ruff check`, and `pyrefly check --preset strict` are clean. Sessions
run as `.scratch/e1/session.sh OUT SCHEME A_BIN A_DIR B_BIN B_DIR [ROUNDS]`: the A/A, then the
A/B, on S′, one thread pinned, each phase timed.

## What it costs

About 40 s per round on S′ for the reordered merged index under `bm25`: an A/A of eight
rounds and an A/B of eight take about 11 minutes, under a light hold.
