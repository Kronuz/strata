# Two flakes: catalog_stress burst_closes, resource_budget tick against failure

Both flakes are code races. Driver: `docs/perf/flakes/repeat_load.sh` (W waves of P copies of a
command at once, so the host is oversubscribed; exits 1 when any run failed).

## catalog_stress.cc:701 (`burst_closes`)

Symptom under load: a policy pass ends one close short with nothing refused (59 closes of 60, 63
visited, 0 busy, 0 promoted, live 5, queue 0+5).

Cause (`catalog.hh` `closing_over()` against `note()`): the pass asks `closing_over()` after each
close it begins while workers end earlier closes. It computes `live - closing - booked - soft_open`.
Each counter pair one event changes in opposite directions must never be seen half-done in the
direction that lowers the result:

| Event | Counters | Lowering half | Raising half |
|---|---|---|---|
| a close ends (`closing` to `free`) | `live_` - 1, `closing_` - 1 | `live_` | `closing_` |
| a due close begins (B2 booking) | `closing_` + 1, `pending_open_` - 1 | `closing_` | `pending_open_` |

The writer does the raising half first and publishes the lowering half with a release RMW; the
reader loads the lowering half's counter with acquire before the raising half's. A reader that sees
the lowering half then sees the raising half, so a race only over-counts (a pass may begin one extra
close, never one too few). Before the fix both writers did the lowering half first, all relaxed.

- `note()`: `closing_` changes before `live_`; `live_`'s RMWs and `closing_`'s rise are release
  (every write to either is an RMW, so a later value read still carries the release sequence);
  `closing_`'s fall is relaxed, since seen alone it only over-counts.
- Every close begin (`begin_close`, `begin_bound_close`, reopen, replace, shutdown) releases the
  slot's due booking (`release_due`, `pending_open_` - 1) before `note()` raises `closing_`.
- `closing_over()` loads `live_` (acquire), then `closing_` (acquire), then `pending_open_`.

Not covered, by design of the measure: an open that lands after the pass read `live_` is counted by
the next pass, not this one.

## resource_budget_test.cc:494 (`CHECK(b.external(D) <= 70)`, TSan)

Cause (`resource_budget.hh` `tick()` against `note_failure()`): `tick()` raises the external cap by a
compare-exchange on its value. A failure whose count equals the cap lowers nothing (`target < e` is
false, no exchange), so the raise succeeds and leaves the cap above the failure's count, although the
failure stamped `external_at_` after the pass read it. The concurrent test reaches that state only in
round 0, where the cap is still 70 from the preceding hook test and the thread's failure is at 70.

Fix: after a successful raise, `tick()` re-reads the stamp and takes the raise back (a compare-exchange
from the raised value to the loaded one) when the stamp moved. A failure that lowered the cap below the
raise makes the take-back fail and keeps its own lower value.

Orderings. The failure stores the stamp, then loads the cap; the pass raises the cap, then loads the
stamp. That is the store-buffering shape: with acquire/release each side may miss the other, and the
raise survives. All four are `seq_cst`, so in the single total order either the pass's re-read follows
the stamp (the raise is taken back) or the failure's load follows the raise (it sees the raised cap and
lowers it when its count is below). `acq_rel` would suffice only if both stamp accesses were RMWs (the
re-read a `fetch_add(0)`), which writes a shared line on every pass for no gain over `seq_cst`. The
other accesses keep their weaker orders: the cap's lowering is release and the pass's first cap load
acquire (a pass that sees a lowered cap sees its stamp and waits out the quiet period); the pass's
first stamp load is relaxed (a stale stamp only leads to a raise the re-read takes back); the take-back
is relaxed (only the cap's value matters, and `mu_` orders the next pass).

Two edges are deliberate. A failure that read the clock at the same value as the previous stamp and
stalled a full quiet period before storing it leaves the stamp unchanged; its failure time is then
already a quiet period old, so the raise is what the policy asks. A changed descriptor limit still
clears the descriptor cap unconditionally: a failure under the old limit says nothing of the new one,
and the next failure lowers it again. Between the raise and its take-back, `soft()` shows the raised cap
for a moment; the pass ends with `over_soft()` and posts if anything is over.

Deterministic reproduction: `tick_against_failure` sets the clock hook, which `tick()` calls between
its stamp read and the raise, to post a failure at the cap's own count, and checks the hook ran and the
cap stays at 70.

## Descriptor and vnode relief

`closing_over()`'s descriptor and vnode terms have the same shape with `share()` (from the budget's
`used_` and the catalog's counts) as the lowering half and the booked relief as the raising half. A
close books what its slot holds when it begins, but the slot lets go of descriptors and vnodes at many
points before the close ends: a holder's drain between the begin and the replace chore, the close
chore's preparation (`commit_merge_output`, the warm close's `demote()` and `reclaim(0)`, the bound
commit, `reclaim()`), `close()` and `retire`. A booking that outlives those counts makes the pass
subtract relief for counts already gone, and a pass under a descriptor or vnode cap can end short.

Fix: the booking falls with the counts, at the one place they drop. `resource_counts::booked` holds a
slot's booking of descriptors and vnodes and, on the catalog's counts, their sum (32 bits each; the
slot stays at D4's 192 B). `resource_reservation::release()` drops the slot's count, lowers the slot's
booking to it, then a release fence (whether or not this drop lowered), then the catalog's count and
the budget's `used_`. `closing_over()` reads both shares, an acquire fence, then the sums, so a reader
that sees a drop sees the booking it lowered. The booking stays while `close()` runs and goes when the
close ends (`finish_close`), is refused (`end_at`) or sticks (`after_close`); `retire`'s drops, the
IO's directory descriptor included, lower it like any other.

Every fall of a booking (`lower()`, `unbook()`) subtracts from the sum first, then moves the slot's
booking by a release compare-exchange, and adds the sum back when the exchange loses. Two drops of one
slot race: the one whose lowering the other already did finds nothing to lower, but its load of the
booking (seq_cst) synchronizes with that exchange, so its fence carries the other's subtraction to any
reader of its catalog drop. With the sum lowered after the exchange, or the fence taken only by the
drop that lowered, a reader saw the catalog's count fall with the sum still above it (review probe: 25
to 78 bad reads in 42-46 M per run); `unbook()`'s exchange to 0 followed by its subtraction has the same
window against a drop.

A booking and a drop that race: `book()` stores the booking, then loads the count; `release()` drops
the count, then loads the booking; all four are `seq_cst`, so one sees the other and lowers it. The
sum is added after that lowering, so it may dip below the slots' bookings for a moment (an over-count;
it is signed and read as 0 below zero), never above. Not covered: a reservation and a close's begin that
both land between the pass's read of the share and its read of the sum book a count the pass has not
seen yet (not one already dropped); the next pass counts it.

Not covered either: a demotion's maps relief (`pend_maps`, the catalog's `pending_maps_`, read by
`pass()`'s maps term) keeps the old shape, booked at the demotion's begin and released at its end, so
maps the demotion drops before it ends stay booked until then and a maps pass can end short. It is
outside `closing_over()`; moving it onto the same booking changes demotion pacing (campaign hunt list).

Deterministic check: `test/catalog_relief_test.cc` (suite test `catalog_relief`) samples, at every
catalog hook site and host commit while a close runs, that the booked descriptors and vnodes never
exceed what the closing slot still holds: a sweep close, and a healthy reopen whose draining holder
merges, commits and reclaims before the replace chore (before the fix: 4 vnodes booked, 3 held, from
the holder's reclaim to `chore.replace`). The close chore's own preparation has no hook site between
its drops and `close()`; it drops through the same `release()`. `test/resource_budget_test.cc`
`booking_falls` races drops against bookings, and `two_droppers` two drops of one booked slot, under a
reader that reads as the pass does (`BOOKING_ONLY=1 ROUNDS=N` runs those two alone).

## Results

| Run | Commit | Result | Job |
|---|---|---|---|
| burst_closes 20x16 under load, before | 423b3a2d (main + diagnostics) | 11 of 320 | 261111-052507-6df290 |
| burst_closes 10x16 under load (strict), before | main 3177e491 | 7 of 160 | 261111-062817-1fabdf |
| burst_closes 2 x 10x16 under load (strict), after | fb15069a | 0 of 160, twice | 261111-065359-06bdf8, 261111-065400-8620b9 |
| resource_budget TSan 20x16, before (original test) | main 3177e491 | 3 of 320 | 261111-053734-7d63a2 |
| resource_budget TSan 20x16, after | b7bd5251 | 0 of 320 | 261111-061320-6fa85e |
| resource_budget TSan 2 x 10x16 (strict), after | fb15069a | 0 of 160, twice | 261111-065354-020159, 261111-065355-a0477c |
| deterministic hook test, main's `tick()` | b7bd5251 with main's header | fails at resource_budget_test.cc:482 | 261111-062645-154f0d |
| deterministic hook test, fixed | b7bd5251 | passes | 261111-061317-5cd220 |
| catalog_relief, catalog.hh before the relief fix | fb15069a with cf76b825's header | fails at `close.end`: 2 descriptors and 2 vnodes booked, 0 held | 261111-065033-c4aeb6 |
| catalog_relief, fixed (suite) | fb15069a | passes | 261111-065032-2e5b27 |

The burst runs replace `test/catalog_stress_test.cc` in the job's checkout with the exploration's
(strata `26dc030e`), which adds the `burst` mode and diagnostics.
