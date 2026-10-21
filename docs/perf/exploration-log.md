# Exploration log

A trial from 2026-10-20: benchmarks the lead runs itself and targeted experiments by small
performance agents, without review cycles except where a change touches a format, durability, or
concurrency. Each row records what it cost and what it found, so the mode can be judged against
review-driven days.

Quota is the weekly share used (`quota`), before and after. Agent tokens include cached context.

| Date | Item | Kind | Quota before → after | Agent tokens | Result | Landed |
|---|---|---|---|---|---|---|
| 2026-10-20 | core vs RocksDB rerun on format 14 (quick tier, Mac) | lead benchmark | 81% → | 0 | running | — |

## Baseline for comparison (2026-10-20, review-driven)

Weekly 76% → 81% landed: the merge-stall fix (15.28 s → 0.25 s settle), termdict seek and decode
(1.6–2.4× faster decode), COUNT-union window bits (1.89× faster on its row), format 14 step 1
(point reads 43× faster), the Xapiand-on-strata design, and Phase 0.5's X1. About 2.9M agent
tokens across developers, performance engineers and four reviews.
