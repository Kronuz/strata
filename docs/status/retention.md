# Retention default 0 (phase4/retention)

Branch `phase4/retention` from main, worktree `strata-wt/retention`. Owner decision
(2026-10-20): `retained_predecessors` defaults to 0; damage to the current manifest or
newest segment is refused and reported; 1 or 2 stay an option, and any open that falls back
to a predecessor reports it as data loss. Measurements: `docs/perf/retention.md` on
`explore/retention`. Landing: durability policy, so one code review and a scoped architect
check come first.

## State

Implemented and tested; waiting for the code review and the scoped architect check
(durability policy) before landing. Not merged.

## Done

- Default 0 (`include/strata/core/index.hh`); `fallback()` reports an open at a `to` older
  than the newest manifest (`fallback_report`: generation opened, newest found, newer
  manifests discarded). No automatic fallback exists; the host's `open(..., to)` is the
  fallback, and it is now never silent.
- `docs/design/phase-3.md` Retention (D4) and I14: default 0, reporting rule.
- `test/retention_test.cc` (ctest `retention`): default leaves one manifest and only the
  head's segment after a merge settles (1 and 2 keep predecessors and segments); with 1,
  a flipped head manifest or newest segment falls back and the report names the
  generation and 1 discarded; with 0 the same damage is refused (no predecessor to open).
- Suites that assumed 2 predecessors (media, index, reclaim, lifecycle) set it explicitly.
- Evidence (ASan+UBSan, Homebrew LLVM 22, x86_64): full ctest 24 of 24 pass. Reverts/mutant
  against retention_test, each fails: default back to 2; silent fallback (report not set);
  retain() keeps at least 1 (refusal and disk checks); retain() keeps 0 regardless (the
  retention-1 fallback aborts with "no such manifest").

## Next

Code review, scoped architect check, then land. Not run: TSan (no concurrency changed).

## Blocked on

Nothing.
