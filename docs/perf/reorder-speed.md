# Reorder speed: a faster bisection for the reordering merge

**Target.** The reordering merge (docs/design/reordering.md D4). On the 5M quick-tier
calibration a force merge with renumbering takes 2,678 s on one thread and 846 s with
`reorder_threads` at 8 (355% CPU, 6.03 GB peak RSS), against 404 s in docid order; Lucene
`-bp`'s whole merge takes 595 to 652 s at about 177% CPU (phase-3-ratchets.md, T3).
Renumbering buys ranked queries 1.10 to 1.13× of the best reference instead of 1.37 to
1.45×, so the order must stay; its cost must shrink.

**Where the time goes.** `sample` over a one-thread 1M force merge (base, `d8d219c`):
457 s, of which bisection 376.8 s (82%), the forward index 11.2 s, and the rest of the
merge 69.4 s. Inside bisection, 93% of the samples sit in `bisect_part`'s gain loop (the
inlined `move_gain`: four fixed-point logarithms per edge per iteration) and 5% in the
two `std::sort` calls. The 1M graph: 4,587 graph terms (min_df 4096, as Lucene), 92.3M
edges, about 1.07 bytes an edge in the forward index; 15 levels down to 32 documents,
and nearly every part runs all 20 iterations (18 to 20 on average at every depth).

## Procedure

- Corpus: the first 1M documents of the 5M Wikipedia corpus, indexed once by the base
  binary (7 segments, 1.2 GB), cloned (APFS `cp -c`) for each run.
- The merge: `strata_engine merge DIR` (force merge to one segment, reordering), timed
  with its phases (`reorder::phase_times`: forward index, bisection, the rest).
- Bisection alone, for quick iteration: a scratch tool (`bisect_bench`) that writes the
  forward index of the same segments into memory and runs `bisection::order`, printing
  the times and an FNV-1a hash of the order.
- An exact change keeps the order's hash and the merged segments' bytes; then postings
  size and query speed are unchanged by construction. An inexact one is measured on
  postings bytes (exact, `strata_engine sections`) and on query speed (quick sample,
  `bench/quick/ab.sh`) against the docid-ordered index of the same size.
- One thread under a light hold; eight threads under a strict one. Interleaved rounds.

## Hypotheses, in order

| # | Change | Kind | Keep if |
|---|---|---|---|
| 1 | Graph terms by df cutoffs (Lucene's) | — | already there: min_df 4096, max_df 1.0 |
| A | A term's move gain once per iteration and side, not per edge | exact | faster, same bytes |
| E2 | Degree steps from a table; nl and nr's logarithms hoisted | exact | faster, same bytes |
| E3 | Prefetch rows ahead (offset, then the row) | exact | faster, same bytes |
| E4 | Sort the gain lists only as far as the swaps reach | exact | faster, same bytes |
| 2 | Fewer iterations per part | inexact | faster; postings and queries within noise |
| 4 | Top levels' gain loops on all threads | exact | faster at 8 threads, same bytes |
| 3 | Start from the sources' order when they are reordered | inexact | not measured here (see below) |
| 5 | Forward index layout | exact | folded into E3 (see below) |

## Results
