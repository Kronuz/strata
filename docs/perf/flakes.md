# Two flakes: catalog_stress burst_closes, resource_budget tick against failure

Exploration on `explore/flakes` (strata main 3177e491). Driver: `docs/perf/flakes/repeat_load.sh`
(W waves of P copies of a command at once, so the host is oversubscribed).

## resource_budget_test.cc:494 (`CHECK(b.external(D) <= 70)`, TSan)

Cause (code race, `resource_budget.hh` `tick()` against `note_failure()`): `tick()` raises the external cap by
a compare-exchange on its value. A failure whose count equals the cap lowers nothing (`target < e` is false,
no exchange), so the raise succeeds and leaves the cap above the failure's count, though the failure stamped
`external_at_` after the pass read it. The concurrent test reaches that state only in round 0, where the cap
is still 70 from the preceding hook test and the thread's failure is at 70; later rounds start from
`unlimited`, so the failure always lowers. One interleaving in 3000 rounds, and TSan stretches the window.

Deterministic reproduction: the clock hook (called inside `tick()` between the stamp read and the exchange)
posts a failure at the cap's own count (test: "A failure at the cap's own count"), commit 4c34cc0f.

Fix: `tick()` reads the stamp again after a successful raise and takes the raise back when it moved; the
stamp store, the cap load and the raise are sequentially consistent so one side sees the other
(`include/strata/core/resource_budget.hh`, 84c9d7bd).

Jobs: before, deterministic 261111-052543-1799aa, TSan 20x16 261111-052544-b31efa; after, deterministic
261111-052602-2ee5f7, TSan 20x16 261111-052604-cf3b81.

## catalog_stress.cc:701 (`burst_closes`)

Reproduced under load: 11 failures of 320 runs (20 waves of 16 copies, burst mode only), job
261111-052507-6df290 at 423b3a2d. Every failure reads the same: 59 closes, 63 visited, 0 busy, 0 promoted,
0 refused closes, queue 0+5, live 5, opens exactly 64 + 60 per later iteration. The pass ended one close short
with nothing refused.

Cause (code race, `catalog.hh` `closing_over()` against `note()`): the pass's loop asks `closing_over()` after each
close it begins, while the 3 workers end earlier closes. It reads `live_`, then `closing_`; a close's end
lowered `live_` first, then `closing_`. A read between the two lowerings sees the live count after the close
and the closing count before it, one slot too few over `soft_open`, and the pass stops at 59 of 60. The comment
there claimed the read order made a race over-count; it made it under-count.

Fix: `note()` lowers `closing_` before `live_` (release on `live_`, acquire on the read), so a read that sees
`live_` lowered sees `closing_` lowered (`include/strata/core/catalog.hh`, 06a8d72f). This also bears on the
suite's other `closing_over()` readers (a pass ending short leaves the cap over until the next period).

Jobs: after the fix, same load, 261111-053702-bc378b.

## Results

Fill from the jobs above (baseline of the original resource_budget test at main: 261111-053734-7d63a2;
after-fix TSan under load: 261111-052604-cf3b81; 261111-052544-b31efa runs the new deterministic check, so
its failures are that check and not the original flake's rate).
