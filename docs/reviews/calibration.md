# Calibration

What a review missed and something later surfaced, with the check that would have caught it.
Every reviewer reads this first and applies its checks.

| Date | Missed by | What | Where it surfaced | The check that would have caught it |
|---|---|---|---|---|
| 2026-10-17 | P1 reordering reviews (`reordering-design.md`) | `bisection.hh` runs its own `std::thread` workers beside Kronuz/threadpool, which strata already links (`pool_executor.hh`): one mechanism twice | The owner asked whether strata reimplements thread pools | For every thread, queue, cache, or encoder a change adds, search strata's dependencies and the Kronuz catalog for an existing one; a second implementation is a finding |
| 2026-10-17 | P1 reordering reviews | `std::jthread` in `bisection.hh` does not exist in libc++ on macOS and FreeBSD, so main failed to compile there | CI on main (run 39060700799) | Check CI on every platform for the reviewed commit, not only the reviewer's local build |
