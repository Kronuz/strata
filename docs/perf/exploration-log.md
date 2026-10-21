# Exploration log

A trial from 2026-10-20: benchmarks the lead runs itself and targeted experiments by small
performance agents, without review cycles except where a change touches a format, durability, or
concurrency. Each row records what it cost and what it found, so the mode can be judged against
review-driven days.

Quota is the weekly share used (`quota`), before and after. Agent tokens include cached context.

| Date | Item | Kind | Quota before → after | Agent tokens | Result | Landed |
|---|---|---|---|---|---|---|
| 2026-10-20 | core vs RocksDB rerun on format 14 (quick tier, Mac) | lead benchmark | 81% → 81% | 0 | strata wins 27 of 34 rows; six losses named (`core-vs-rocksdb-f14.md`) | results, 8af7d92 |
| 2026-10-20 | explorer: per-index open cost (85 ms vs 22 ms) | explorer | 81% → 81% | | create is 4 F_FULLFSYNCs (~21 ms each), not CPU; one fewer barrier makes it 1.35× faster (`explore/open-cost` b093ded); cross-index batching is the big win | no: durability change, folds into the turn log |
| 2026-10-20 | explorer: peak memory (94 vs 27 MiB) | explorer | 81% → 81% | | flush holds a second copy of every value and grows its output by doubling; sizing, borrowing and streaming cut peak RSS 27% with identical bytes (`explore/peak-memory`); the buffer at ~290 B/record vs 177 B designed is next | finishing |

## Baseline for comparison (2026-10-20, review-driven)

Weekly 76% → 81% landed: the merge-stall fix (15.28 s → 0.25 s settle), termdict seek and decode
(1.6–2.4× faster decode), COUNT-union window bits (1.89× faster on its row), format 14 step 1
(point reads 43× faster), the Xapiand-on-strata design, and Phase 0.5's X1. About 2.9M agent
tokens across developers, performance engineers and four reviews.
