# Status

strata is under active development and its API changes between phases. Phases 0
through 2 have passed their gates. Phase 3 (index lifecycle) has reported its
durability rows, and its performance rows are being measured. On 2026-10-20 the search side
moved to prospect with its history; this repository keeps the core.
[PLAN.md](PLAN.md) has the phases, and [gates/](gates) has the core's
measured rows.

## Owed at the next completion repin

- completion's read-only IO virtuals (the sync-files change) are pure: `CountedIO`
  (`include/strata/core/counted_io.hh`) and faultfs (`include/strata/core/faultfs.hh`) must
  forward the four read-only opens in the same commit that moves strata's completion pin.
