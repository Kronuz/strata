# strata review: retention default 0 (phase4/retention, scoped durability check)

- Date: 2026-10-21. Tier: durability (scoped architect check, which also serves as the code
  review).
- Reviewed: `phase4/retention` at `770866d` (worktree `strata-wt/retention`, base main
  `6105148`; `origin/phase4/retention` is `770866d`, nothing unpushed), `git log
  main..770866d` (2 commits: `195d854` ledger, `770866d` the change). The owner's decision of
  2026-10-20 and its evidence, `docs/perf/retention.md` and `test/retention_test.cc` on
  `origin/explore/retention` at `74c3d6b`. `docs/design/track-c.md` on this branch;
  `docs/design/durability-modes.md` on `origin/design/durability-modes` at `95357d9` (D1,
  DM-R3, DM-R23, DM-R43, T16); `docs/design/media-faults.md`; prospect `cb36499`'s mirror
  suites.
- Checked against: `docs/design/phase-3.md` D3 (`:135-150`) and D4 (`:155-196`), the
  design's own rule that a file is truncated only after its last mapping is released
  (`:193`), the phase 3 gate's T1 row (`docs/gates/phase-3-lifecycle.md:18`),
  `docs/reviews/calibration.md`.
- Checked by: Homebrew LLVM clang 23.1.2, Debug, `-fsanitize=address,undefined
  -fno-sanitize-recover=undefined` (`.scratch/review/build-asan`); full ctest 24 of 24 pass
  in 1,636 s (`.scratch/review/ctest.log`; `retention` 0.8 s, `lifecycle` 1,015 s,
  `lifecycle_mapped` 125 s, `reclaim` 391 s, `reclaim_mapped` 159 s, `index` 57 s,
  `background_merge` 20 s). GitHub CI run 39563008642 for `770866d`: all 8 jobs green
  (Linux ASan+UBSan, TSan, gcc x86_64 and arm64, clang; FreeBSD; macOS x86_64 and arm64).
  Probes, all kept in `.scratch/review/` and listed below, built with retention_test's
  compile command (`build_probe.sh`).
  - Not run: benchmarks or any timing (none needed; disk numbers re-scored from the
    explorer's raw table only); TSan locally (CI's TSan job is green); Linux or FreeBSD
    locally; a real-filesystem SIGBUS reproduction (faultfs's mapping model stands in);
    prospect's suites against this commit.

Probes and outputs (`.scratch/review/`):

- `merge_reclaim_race.cc` (`bin/merge_reclaim_race KEEP EXTRA`, `STRATA_FAULTFS_MAP=1`):
  a background merge queued on the inline executor, every document of one source deleted
  and committed, then EXTRA more commits and `reclaim()`. Outputs
  `out-race-keep{0,1,2}-extra*-mapped.txt`, `out-race-0_0-unmapped.txt`, and against the fix
  sketch `out-race-fix-*.txt`.
- `crash_keep0.cc`: power loss at every mutation of a flush commit with deletes and
  metadata, a merge commit with reclaim, `checkpoint()`, and a merge then `release()` of a
  pin, 64 crash seeds per point, at the default; then a pinned checkpoint through merges and
  reclaim. `out-crash_keep0.txt`, `out-crash_keep0_mapped.txt`.
- `lifecycle_keep0.cc`, `index_keep0.cc`, `reclaim_keep0.cc`, `media_keep0.cc`: the branch's
  suites with `retained_predecessors` back at the default, 2 kept only in the blocks that need
  a predecessor (marked "review probe"). `out-lifecycle_keep0*.txt`, `out-index_keep0*.txt`,
  `out-reclaim_keep0*.txt`, `out-media_keep0.txt`.
- `fallback_crash.cc` (`bin/fallback_crash KEEP`): power loss at every mutation of
  `open(io, opts, to)`, 16 seeds each. `out-fallback_crash_keep{1,2}.txt`.
- `mut-fix/include` (the RET-1 fix sketch, `build_probe_fix.sh`): the race probe,
  `retention_test_copy.cc` and `background_merge_copy.cc` built against it
  (`out-background_merge_fix_mapped.txt`).
- `mut-discarded/include` (`build_probe_discarded.sh`): `discarded = newest - to`, run
  against `retention_test_copy.cc`.
- `perf-retention.md`: the explorer's note, copied from `74c3d6b` for the RET-3 re-score.

## Verdict

**REJECT: the default of 0 makes a merge source get truncated under its live mapping in
the same commit (RET-1), and the crash suites no longer exercise the default (RET-2). Both
fixes are small; the commit protocol at 0 is sound.**

What holds, and reproduces:

- Q1, no window without an openable manifest at 0. `commit_files()` syncs the data files,
  the directory (`index.hh:2297`), the manifest temporary, renames it (`:2302`) and syncs the
  directory again (`:2305`) before `commit()` calls `retain()` (`:617`). `retain()` removes
  old manifests and syncs the directory (`:2373`) before it queues a data file for
  `reclaim()`. faultfs keeps any in-order subset of the namespace operations since the last
  directory sync (`faultfs.hh:172-191`), so every barrier ordering is in reach. At the
  default:
  - `crash_keep0`: 5,760 crashes (86 crash points, 64 seeds each), 0 unopenable and 0 with a
    digest other than before or after. The same holds with mapping on.
  - `lifecycle_keep0`: T1's 10,000 crash points over 32 workloads, 558 of them inside
    recoveries, plus 2,000 with background merges, 171 inside recoveries: ok. The mapped run
    (1,000 + 200 points) is also ok.
  - `reclaim_keep0`: 3,852 crash points, 11,170 manifests checked: ok; mapped, 1,633 points:
    ok.
  - The explorer's cell A at keep 0: 288 crashes, all opened the head.
- Q2, pins at 0. `retain()` keeps `m_.pinned` and every file a pinned manifest names
  (`index.hh:2330`, `:2345-2353`). `checkpoint()` writes the pin into the next manifest
  before any retention can drop the checkpoint (`:835-842`). `crash_keep0`'s pin case:
  generation 4 was pinned, followed by 4 commits, a full merge and a full reclaim. Its
  manifest stayed and `open(to = 4)` reached the checkpoint's digest. Orphans still leave
  only after a successful open (`:2560-2563`). `index_keep0`'s orphan block, re-run at 2
  only because it also tests `recover --to`, is unchanged.
- Q4, `fallback()` covers every path that opens an older generation. That path is
  `recover(to)` with `to` below the newest manifest (`index.hh:2557-2559`). `heal()`
  re-reads segments and never changes the generation. `install()` replaces the content with
  another index's checkpoint, which is not a fallback. No automatic fallback exists. The
  recovery tool does not exist yet (RET-4).
- `retention_test` covers its claims. The default is 0. At 1 the fallback is reported with
  `to`, `newest` and `discarded == 1`. At 0 the same damage is refused. Opening the head by
  name reports nothing.

What blocks: RET-1 and RET-2.

## Findings

**RET-1 (blocking). At 0, `reclaim()` truncates a background merge's source under the
merge's live mapping in the same commit.**

- The path. `commit_files()` drops a segment whose documents are all deleted
  (`index.hh:2218`) whether or not a pending merge holds it. `retain()` then references
  only manifests and `segs_` (`:2345-2356`), so with no predecessor nothing names the file.
  It is queued, and the same `commit()` reclaims it (`:618`). A file larger than
  `reclaim_step` is truncated in steps (`:633-646`) while `merge_job::sources`
  (`:1047`) still maps it. The merge then reads past the cut: on a real disk that is
  SIGBUS on the node.
- This breaks the design's rule that "a file is truncated only after its last mapping is
  released" (`phase-3.md:193`). `install_merge()` already expects a source to vanish
  (`:1506-1535`, "otherwise the output's file is reclaimed"), so this is a reachable
  sequence, not a contract violation by the host.
- Evidence (`merge_reclaim_race`, mapped, `reclaim_step = 256`):

  | keep | extra commits | result |
  |---|---|---|
  | 0 | 0 | exit 134, "faultfs: a file truncated below a live mapping" |
  | 1 | 0 | survives |
  | 1 | 1 | exit 134 |
  | 2 | 0 or 1 | survives |
  | 2 | 2 | exit 134 |

  Without mapping it survives, because the merge reads its own copy of the bytes.
- Attribution: the bug predates the branch. At keep k it fires once k more commits land
  before the merge publishes. With background merges of large segments (minutes) and
  per-second commits, it is reachable at 2 today. The branch's default removes the last
  delay, so it fires on the first commit after the delete.
- Fix: in `retain()`, beside the flushed segments, reference the sources of every pending
  merge:
  `for (const auto& job : pending_) for (const auto& src : job->sources) referenced.insert(index_detail::segment_name(src->ref.id));`
  After publication, the next round queues them. The sketch (`mut-fix/include`) passes the
  race at keep 0, 1 and 2, `retention_test`, and `background_merge` mapped.
- Test: add the race to `background_merge_test`, or to `retention_test` under a `_mapped`
  ctest. Use an inline executor, a source made all-dead, a commit, and
  `reclaim(UINT64_MAX)` before the merge runs, at keep 0 and at keep 2 with 2 commits.

**RET-2 (blocking). The crash suites no longer test the default.**

- `lifecycle_test.cc:103`, `index_test.cc:79`, `media_test.cc:34` and `reclaim_test.cc:107`
  now pin 2 for the whole suite, so T1's 10,000 points, the reclaim suite's 3,852 and
  index's 30 run only at a setting users no longer get. RET-1 is the kind of defect that
  slips through this way.
- Only three places in those suites need a predecessor, found by running them at 0:
  1. `lifecycle_test`'s `recover --to` block (`:375-433`).
  2. `index_test`'s retention and `recover --to` block (`:1021-1047`), its condition 14
     pin-listing block (`:1508-1543`), and the binding-check fixture that reuses an older
     live-docs generation (`:1671-1700`).
  3. `media_test`'s final block, which recovers from a predecessor (`:132-163`).
- Everything else passes at 0: lifecycle 10,000 + 2,000 points, index (mapped too), reclaim
  3,852 + 1,633, and media's 738 transient and 738 sticky faults.
- Fix: set 2 only in those blocks and leave each suite's options at the default. Or run
  the suites twice: a `STRATA_RETAINED_PREDECESSORS` environment variable with a ctest
  variant at 0 for `lifecycle`, `reclaim` and `index`. `reclaim_test` needs no 2 at all;
  its comment (`:2`) should name the setting it runs at.

**RET-3 (non-blocking, doc). The disk cost in D4 is understated, and its source is not on
the branch.**

- `phase-3.md:162` says predecessors "cost 30-45% more settled disk where merges run".
  The explorer's raw table (`perf-retention.md` §2, re-scored) shows keep 2 holds 47% to
  108% more settled disk than keep 0 on every row with merges. Put the other way, keep 0 is
  32% to 52% smaller:
  - ycsb_a 1 KiB fits: 97.1 vs 66.2 MiB.
  - overwrite 1 KiB 2x: 122.7 vs 59.0 MiB.
  - overwrite 100 B fits: 13.8 vs 6.9 MiB.
- Write "keep 0 holds 32-52% less settled disk where merges run", from the table.
- `docs/perf/retention.md` exists only on `origin/explore/retention` (`74c3d6b`). Land it
  with this branch, or cite the commit.

**RET-4 (non-blocking, doc). The repair paths the text names do not exist for a standalone
index at 0.**

- `phase-3.md:163` and `index.hh:358` say damage is "repaired from a replica or the
  recovery tool". The tool is planned "after the engine is complete" (`PLAN.md:694-716`),
  so today a standalone index at 0 with a damaged head stays refused.
- `media-faults.md:68-71` still says standalone repair is "from a retained predecessor
  that does not name the damaged file (`recover --to`)". Its test list (`:87-88`) still
  describes a recovery that now needs a non-default 2.
- Fix: state what exists today, which is a replica, or opting into 1 or 2. Name the tool as
  planned and update `media-faults.md`'s Repair bullet.

**RET-5 (non-blocking; a condition on the durability work). Under groups, nothing now
covers the checkpoint-record window that 2 covered by accident.**

- Track C and durability-modes do not rely on `retained_predecessors`. DM-R43
  (`durability-modes.md:862-879` at `95357d9`) names it "load-bearing for nothing here" and
  specifies a recorded-generation keep in `retain()` instead. T16's mutant is
  `retained_predecessors` 0, which is now the default.
- That keep is not implemented, and nothing on main opens strata under a journal today, so
  nothing breaks now. Until DM-R43 lands, though, a group host at the default would have no
  local copy of the recorded generation in the window. At 2 that window was covered for up
  to two checkpoints.
- `phase-3.md:169-170` (the journal passes the pinned set at open) is also still
  unimplemented, unchanged by this branch.

**RET-6 (non-blocking). D4's "reported as data loss" is wrong for group recovery.**

- `phase-3.md:141-142` and DM-R43 open a group's recorded generation with `recover(to)`,
  usually below a newer manifest on disk. That sets `fallback()`, while the journal replays
  every entry above it, so nothing is lost.
- D4 (`:164-167`) and `index.hh:676-678` tell the host to report it as data loss.
- Fix: scope the sentence to standalone opens, or have the durability work's recovery
  record that the journal covers the discarded generations.

**RET-7 (non-blocking). The fallback report lives in memory only.**

- `fallback_crash`, at keep 1 and at keep 2: of 144 crashes inside `open(io, opts, to)`, 52
  reopen with the older content and an empty `fallback()`. The report died with the
  process, and the commit made the chosen content the newest manifest
  (`index.hh:2557-2559`). The host asked for `to`, so it is not silent to the host, but how
  many newer manifests were discarded is lost.
- Fix: state the host contract in D4 (record the intent and the newest generation before
  calling `open(..., to)`). Or carry the report in the new manifest; formats stay open until
  release.

**RET-8 (non-blocking). `discarded` is weakly tested and loosely described.**

- The mutant `discarded = newest - to` (`mut-discarded`) passes `retention_test`, because
  its generations are consecutive.
- The text says "how many newer ones are gone" (`phase-3.md:166`), which reads as commits.
  `discarded` counts manifests on disk: `crash_keep0`'s pin restore reports to 4, newest 10,
  discarded 1, while six generations are gone.
- Fix: add a case with a gap (a pinned checkpoint under later commits), and either say
  "manifests found" or report `newest - to` beside it.

**RET-9 (non-blocking, cross-repository). prospect's mirror suites rely on 2.**

- prospect pins strata at `75d1e7e`. Its `index_test.cc:152`, `lifecycle_test.cc:124` and
  `media_test.cc:64` take the default and assert `recover --to` predecessors
  (`index_test.cc:1392`, `:1778`, `:1858-1885`; `lifecycle_test.cc:404-458`).
- The pin bump that takes this change must make the same edits, or RET-2's narrower ones.

## Blocking items

- L1 (RET-1): `retain()` references the sources of pending merges, with a regression test
  of the race that fails without the fix at keep 0, and at keep 2 with 2 extra commits.
- L2 (RET-2): the crash suites (`lifecycle`, `reclaim`, `index`) run at the default in CI,
  with 2 confined to the blocks that need a predecessor.

## Conditions

- C1 (RET-3, RET-4): D4's disk figure re-scored from the raw table, the perf note landed or
  cited by commit, and `media-faults.md` and the "recovery tool" wording corrected before the
  next gate.
- C2 (RET-5, RET-6): the durability implementation lands DM-R43's recorded-generation keep
  before any group host opens strata, runs T16 at the default, and settles how `fallback()`
  reads under journal replay.
- C3 (RET-7, RET-8): the host contract for `open(..., to)` stated in D4, and `discarded`
  tested with a gap.
- C4 (RET-9): prospect's strata pin bump carries the predecessor settings into its mirror
  suites.
- C5: gate rows that report settled disk are re-baselined at the new default and say so.

Not run: benchmarks or timing of any kind, local TSan, Linux and FreeBSD locally (CI covers
them), a real-mmap SIGBUS reproduction of RET-1, prospect's suites, fuzzing.

## Re-check

Scope: RET-1 and RET-2 at `983eaf8`. Built with Homebrew LLVM 23, ASan+UBSan
(`-fsanitize=address,undefined -fno-sanitize-recover=all`), RelWithDebInfo, in
`.scratch/review/build-asan`. The original probes were deleted; the race probe is now
`retention_test`.

**RET-1: CLOSED.**

- The fix. `retain()` adds every pending merge's sources to `referenced`
  (`index.hh:2360-2363`), before `obsolete` is computed (`:2368`), so a source dropped by
  `commit_files()` (`:2218`) is not queued, and the same commit's `reclaim()` (`:620`) cannot
  truncate it. Once the merge publishes, `pending_` no longer holds it and the next `retain()`
  queues it (`install_merge`, `:1508-1555`).
- The test. `retention_test.cc:197-237`: six segments, a queued merge, one victim segment
  deleted wholesale and committed, `keep` further commits, `reclaim(UINT64_MAX)`, then the
  merge runs and publishes; `needs_heal()` and `check()` after. It runs at 0, 1 and 2, and for
  six victims each. ctest `retention` and `retention_mapped` (`CMakeLists.txt:115-119`,
  `STRATA_FAULTFS_MAP=1`) both pass under ASan+UBSan.
- Revert. With the three-line block removed from a copy of `include/`
  (`.scratch/review/mut-revert/include`, built by `build_probe.sh` into `probe_revert`;
  `retention_probe.cc` is the test with `PROBE_KEEP` selecting one keep), the mapped run aborts
  "faultfs: a file truncated below a live mapping" (exit 134) at keep 0, 1 and 2. The unmapped
  run survives, as the review predicted. With the fix (`probe_fixed`) all six runs exit 0.
  Outputs: `out-probe_{fixed,revert}-m{0,1}-k{0,1,2}.txt`.
- Other paths that delete or truncate a file a pending merge maps:
  - `reclaim()` (`:625-665`) only works on `reclaim_`, which only `retain()` and the merge's own
    output (`:1525`, `:1583`) fill; the output is `reset()` first. A source is never
    queued while a pending merge holds it, as `retain()` now stands.
  - `heal()` (`:700-730`) replaces a `segs_` entry with a fresh load and deletes no file; the
    job keeps its old `open_segment`, publication then finds no match and reclaims the
    output (`:1529-1549`).
  - `install()` (`:880-924`): `abandon_merges()` (`:1575`) calls `group_->cancel()`, which
    waits for running work (`executor.hh:27-29`) and drops queued work, then clears `pending_`;
    the sources are released before the trailing `retain()` queues them.
  - `close()` (`:495`) ends the group (joins) and removes nothing. `recover()` runs only from
    `open` (`:484`) with `pending_` empty. The other `remove` calls (`:1142`, `:2105`,
    `:2194`, `:2503`, `:2570`) remove a partial new file, an uncreated index's files, or a
    manifest temporary.
  - The live-docs files of a source are not held by the merge (it reads a copy of `deleted`),
    so they are rightly not referenced.

**RET-2: CLOSED.**

- `lifecycle_test.cc`, `index_test.cc`, `media_test.cc` and `reclaim_test.cc` no longer set
  `retained_predecessors` suite-wide (diff of `:103`, `:79`, `:34`, `:107`); `reclaim_test.cc:1-2`
  names the setting it runs at.
- 2 (media: 1) is set by a block-local shadow of `keyed` only in: lifecycle's `recover --to`
  block (`lifecycle_test.cc:376-378`, ends `:434`), index's retention block
  (`index_test.cc:1020-1023`, ends `:1049`), its condition-14 block (`:1509-1512`, ends
  `:1545`), the binding-check fixture (`:1681-1683`, via `kept`), and media's last block
  (`media_test.cc:133-135`, to the end). These are the blocks the review named. The one
  deviation is media at 1, not 2; its block needs one predecessor and passes.
- Run at default under ASan+UBSan (`out-default-suites.txt`): `media`, `reclaim`,
  `reclaim_mapped`, `lifecycle` (189 s), `lifecycle_mapped`, `index`, `index_mapped`,
  `background_merge`, `background_merge_mapped`: 9 of 9 pass.

New findings: none. Outside scope, one line: `retention_test` skips the segment-damage
cases in the mapped run (`:131`) because mapped reads bypass media faults; the unmapped run
covers them.

Not run: benchmarks, TSan, the other ctest targets (files, faultfs, core_kv, format14,
get_alloc, boundary checks, `media_mapped`), prospect's suites, a real-mmap SIGBUS
reproduction, fuzzing, Linux and FreeBSD.
