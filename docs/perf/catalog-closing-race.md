# catalog closing race: a close's end against a policy pass

`catalog_stress` (`burst_closes`) failed under load: a pass ended one close short and left the queues
above `soft_open`. Cause and fix landed in strata:2ea3092b (see `flakes.md`); this note adds the
proof that was missing.

## Cause

`closing_over()` computes `live_ - closing_ - pending_open_ - soft_open`. A close's end lowers `live_`
and `closing_` in two steps. With `live_` lowered first, a pass between the steps reads the open
count after the close and the closing count before it, so it counts the close twice and sees one
slot too few over the cap. Fix: `note()` lowers `closing_` first and `live_` second (both release),
and `closing_over()` reads `live_` then `closing_` (both acquire), so a race only over-counts
(`include/strata/core/catalog.hh` `note()` ~1064-1073, `closing_over()` ~1546-1552). No lock.

## Deterministic test

`note()` calls the hook `note.mid` between the two lowerings of a close's end.
`catalog_policy_test` `pass_between_a_closes_counts` (3 open slots, `soft_open` 1, one close begun)
runs a policy pass there and requires it to begin a close. With the order and loads reverted it fails
(`catalog_policy_test.cc:735 CHECK(ran && closes >= 1)`); with the fix it passes.

## Stress loop

80 waves of 4 concurrent `catalog_stress_test` (320 runs), Release, macOS arm64, `quiet light` with
other holds running (load 9-12 of 16 CPUs), `docs/perf/flakes/repeat_load.sh`.

| Build | Failures |
|---|---|
| order reverted (live lowered first, relaxed loads) | 1 of 320 (`catalog_stress_test.cc:982`, queues above 4 after the burst) |
| main with the fix | 0 of 320 |

The reverted build leaves the booking-release ordering of 2ea3092b in place, so it reproduces only
the `live_`/`closing_` half; the earlier explorer's 11 of 320 (strata:06a8d72f, a heavier load) is the
rate with both halves reverted.

## TSan

`-fsanitize=thread` RelWithDebInfo: `catalog_policy_test` and `catalog_stress_test` pass, no report.
