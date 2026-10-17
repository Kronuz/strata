# Calibration

What a review missed and something later surfaced, with the check that would have caught it.
Every reviewer reads this first and applies its checks.

| Date | Missed by | What | Where it surfaced | The check that would have caught it |
|---|---|---|---|---|
| 2026-10-17 | P1 reordering reviews (`reordering-design.md`) | `bisection.hh` runs its own `std::thread` workers beside Kronuz/threadpool, which strata already links (`pool_executor.hh`): one mechanism twice | The owner asked whether strata reimplements thread pools | For every thread, queue, cache, or encoder a change adds, search strata's dependencies and the Kronuz catalog for an existing one; a second implementation is a finding |
| 2026-10-17 | P1 reordering reviews | `std::jthread` in `bisection.hh` does not exist in libc++ on macOS and FreeBSD, so main failed to compile there | CI on main (run 39060700799) | Check CI on every platform for the reviewed commit, not only the reviewer's local build |
| 2026-10-17 | Phase 3 ratchet review (the architect's own R6 check) | A gate row cited as measured was never measured; format 13's T5 inherited it | Format 13's preliminary review (F13-R15) | For every row a design cites as a baseline, find its raw result file and commit; a row without one is a finding |
| 2026-10-17 | Earlier phase reviews | Rulings addressed to Phase 5 (Phase 1's Levenshtein and wildcard check, Phase 2's D14 seek-to-next) never reached its design | The automata review (AUT-R1, AUT-R2) | Before a design's review, collect every ruling earlier reviews address to its phase and check each is answered |
| 2026-10-17 | The system review | It read `origin` while the automata branch held an unpushed commit (`a29d702`, `expand.hh`) | The automata review | A reviewer compares `HEAD` with `origin` for every branch it reads and names unpushed commits |
