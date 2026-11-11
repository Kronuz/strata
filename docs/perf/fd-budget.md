# Descriptors beyond the budget's count (exploration, explore/fd-budget)

Question: cold-tier-step2's T3 (50,000 indexes, strata:948c0869) saw the kernel hold up to 1,050
more descriptors than the resource budget counted.

Code reading: `counted_io` reserves before, and releases after the kernel object is gone, for the
directory, every `open_existing`/`create_exclusive`/`open_reclaim_candidate`, `acquire_owner` and
`scan_directory`; the inner IO is not forwarded. The budget therefore never trails the kernel.
The T3 sampler (`catalog_tiers.cc` `sampler::now`) reads the kernel's count, then `process_vnodes()`
(one `proc_pidfdinfo` per descriptor, thousands at 8,800 open), then `system_vnodes()`, and only
then the budget's count: the budget can fall in between while the sweep closes.

Probe `bench/fd_budget/fd_budget.cc` (macOS arm64, Release, light jobs; counts, not timings):

1. Quiescent phases, 100 indexes, plain and defer_io (261111-013502-951d00): kernel and budget
   agree to the descriptor in every phase (created and destroyed 0; opened 2 per index, the
   directory and `manifest-1`; after 1 and 30 more commits 2; demoted 1; closed with the IO alive 1;
   IO destroyed 0). The kernel's own list by file name shows only those two kinds.
2. Eight-thread open/write/demote/close storm over 400 indexes, a sampler reading budget, kernel,
   budget (261111-014230-2b0af2): the kernel exceeds both reads by at most 3, the three stdio
   descriptors that precede the budget. No uncounted open under concurrency.
3. T3's sampler order while indexes close steadily (261111-014739-584635): 1,500 held, 3,003 kernel descriptors against 3,000 counted (the 3 stdio); the T3 reading order shows
   the kernel up to 137 beyond the budget (404 samples, closes at about 2,000 a second), the bracketed
   reading 3. The skew is the sampler's order; it grows with the scan's duration and the close rate.

Verdict: no open escapes the budget on main. Fix: in `bench/catalog_tiers/catalog_tiers.cc` read the
budget before and after the kernel's count, and report the kernel's excess over the larger of the
two (`sampler::now`, `fd_gap`); keep the vnode scan out of the bracket.
