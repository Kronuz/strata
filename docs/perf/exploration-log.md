# Exploration log

A trial from 2026-10-20 (since then also comparing models: explorers on sonnet at medium, and haiku on narrow tasks, some given the same task side by side): benchmarks the lead runs itself and targeted experiments by small
performance agents, without review cycles except where a change touches a format, durability, or
concurrency. Each row records what it cost and what it found, so the mode can be judged against
review-driven days.

Lesson (2026-10-20): take targets from a fresh measurement, not an older attribution doc; one explorer spent its run rediscovering a fix already on main.

Quota is the weekly share used (`quota`), before and after. Agent tokens include cached context.

| Date | Item | Kind | Model | Quota before → after | Agent tokens | Result | Landed |
|---|---|---|---|---|---|---|---|
| 2026-10-20 | core vs RocksDB rerun on format 14 (quick tier, Mac) | lead benchmark | — | 81% → 81% | 0 | strata wins 27 of 34 rows; six losses named (`core-vs-rocksdb-f14.md`) | results, 8af7d92 |
| 2026-10-20 | explorer: per-index open cost (85 ms vs 22 ms) | explorer | sonnet, medium | 81% → 81% | 88k | create is 4 F_FULLFSYNCs (~21 ms each), not CPU; one fewer barrier makes it 1.35× faster (`explore/open-cost` b093ded); cross-index batching is the big win | no: durability change, folds into the turn log |
| 2026-10-20 | explorer: peak memory (94 vs 27 MiB) | explorer | sonnet, medium | 81% → 81% | 102k | flush holds a second copy of every value and grows its output by doubling; sizing, borrowing and streaming cut peak RSS 27% with identical bytes (`explore/peak-memory`); the buffer at ~290 B/record vs 177 B designed is next | finishing |
| 2026-10-20 | explorer: YCSB E scans 1.21× slower | explorer | sonnet, medium | 81% → 81% | 95k | the driver copied and sorted ~3n keys per scan; a lazy k-way merge over a new `keys::reader::range` cursor is 1.52× faster (21.7 vs RocksDB's 26.1 µs/op), identical digest (`explore/scan`) | finishing as the core scan API |
| 2026-10-20 | explorer: write p99 at 2× memory | explorer | sonnet, medium | 81% → 81% | 85k | merges hold more than the budget, so the writer flushed near-empty buffers every ~170 docs; gating the pressure flush at share/4 makes 2× fills 1.6–2.2× faster with p99 3–5 µs, identical scan hash (`explore/write-p99`) | finishing |
| 2026-10-20 | explorer: disk on 1 KiB values | explorer | sonnet, medium | 81% → 81% | 83k | two retained predecessor manifests pin merged-away inputs (32 of 102 MB); retention 0 gives 66 MiB, 14% below RocksDB; block size moves 1%, zstd_merged halves 2× reads (`explore/disk-1k`) | owner: measure retention first |
| 2026-10-20 | explorer: prospect TOP_100_COUNT unions | explorer | sonnet, medium | 81% → 82% | | the loss was already fixed on main (query-wins row 1; the attribution doc was stale); one 7-line tweak keeps the seed floor for counted disjunctions, 5–15% faster on union 3 on a loaded host, byte-identical (`explore/topk-count`); rebuilt format 14 indexes kept in ~/.scratch/prospect-idx | parked until a quiet-host A/B |
| 2026-10-20 | explorer: prospect intersection_union | explorer | sonnet, medium | 81% → 82% | | the hypothesis was already on main (stale doc again); found a real gap anyway: the optional side never got its own minimum; one edit makes TOP_10 with >3 tokens 1.45× faster (bm25) and 1.81× (bm25_xapian), byte-identical, no row loses (`explore/req-opt`) | finishing |
| 2026-10-20 | explorer: termdict forward seek within the open block (paired A) | explorer | haiku, medium | 81% → 82% | 117k | built the fast path, measured honestly: 1.24× faster at 1–16 steps but 1.32× slower at 1–300 and 1.17× slower on random seeks (only 7% stay in the block); identity thorough; stopped without trying a gate (`explore/forward-seek-a`) | no |
| 2026-10-20 | explorer: termdict forward seek within the open block (paired B) | explorer | sonnet, medium | 81% → 82% | 93k | hit the same random-seek loss, then added a gate: ~2× faster at 1–16 steps, 21–27% at ≤256, random unchanged; the gate is a heuristic to replace (`explore/forward-seek-b`) | finishing |
| 2026-10-20 | explorer: what manifest retention buys (recovery under damage) and costs | explorer | sonnet, medium | 81% → | | running | — |
| 2026-10-20 | explorer: termdict exact-size truncation sweep (test) | explorer | sonnet, medium | 82% → 82% | 77k | no bug: every truncation and byte flip returns or throws corrupt under ASan; a broken load is caught | landed, termdict a91e9b6 |
| 2026-10-20 | explorer: SR4, merge over a damaged source fences the index (fix) | explorer | sonnet, medium | 82% → 82% | 94k | logic_error in a merge now marks the damaged source via the needs-heal path; test fails without (index fenced) and passes with, three executors (`explore/sr4-merge-damage`) | review with SR2 |

## Baseline for comparison (2026-10-20, review-driven)

Weekly 76% → 81% landed: the merge-stall fix (15.28 s → 0.25 s settle), termdict seek and decode
(1.6–2.4× faster decode), COUNT-union window bits (1.89× faster on its row), format 14 step 1
(point reads 43× faster), the Xapiand-on-strata design, and Phase 0.5's X1. About 2.9M agent
tokens across developers, performance engineers and four reviews.
