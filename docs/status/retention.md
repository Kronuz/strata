# Retention default 0 (phase4/retention)

Branch `phase4/retention` from main, worktree `strata-wt/retention`. Owner decision
(2026-10-20): `retained_predecessors` defaults to 0; damage to the current manifest or
newest segment is refused and reported; 1 or 2 stay an option, and any open that falls back
to a predecessor reports it as data loss. Measurements: `docs/perf/retention.md` on
`explore/retention`. Landing: durability policy, so one code review and a scoped architect
check come first.

## State

Review round 1 (docs/reviews/retention.md: REJECT, RET-1 and RET-2 blocking) folded in at
the second commit; waiting for the re-check. Not merged.

## Done

- `770866d`: default 0; `fallback()` report; D4 text; `test/retention_test.cc`.
- Round 1 fix (see the git log):
  - RET-1: `retain()` references every pending merge's sources; the race is a test in
    `retention_test` (ctest `retention` and `retention_mapped`) at 0, 1 and 2 predecessors.
  - RET-2: lifecycle, index, reclaim and media run at the default; 2 (media: 1) is confined to
    the recover --to block, the retention block, the condition-14 block and the binding-check
    fixture.
  - RET-3: D4 says 47-108% more settled disk and cites `docs/perf/retention.md`, now on the branch.
  - RET-4/RET-6/RET-7: D4 and `index.hh` say the report is about local manifests (a journal that
    replays above `to` loses nothing), the recovery tool is planned, and the report is in memory.
  - RET-8: a test with a gap (pinned checkpoint under later commits) kills `discarded = newest - to`.
- Evidence (ASan+UBSan, LLVM 22): full ctest 25 tests, all pass (media re-run after fixing its
  block; the lifecycle, reclaim and index crash suites run at the default). Each fix reverted
  alone fails the new test: RET-1 without the pending-merge sources aborts "a file truncated
  below a live mapping" in `retention_mapped` at keep 0, 1 and 2 (run one keep at a time);
  `discarded = newest - to` fails the gap test; media's block at 0 throws "no such manifest".

## Follow-ups (not in this branch)

- `fallback()` is in memory only: a crash inside `open(io, opts, to)` loses the report (RET-7;
  52 of 144 such crashes reopen with it empty). A host records its intent and the newest
  generation first, or the report moves into the new manifest (formats stay open until release).
- C2: the group host's recorded-generation keep (DM-R43) must land in `retain()` before any
  group runs at 0; T16 runs at the default; settle how `fallback()` reads under journal replay.
- C4: prospect's mirror suites (`index_test`, `lifecycle_test`, `media_test`) need 2 where
  they assert `recover --to` when its strata pin moves (RET-9).
- C5: gate rows that report settled disk are re-baselined at the new default and say so.

## Blocked on

Nothing.
