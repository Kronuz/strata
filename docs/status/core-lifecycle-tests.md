# core-lifecycle-tests: the core's lifecycle suites on the document layout

- Task (the lead's, 2026-10-20): the lifecycle suites went to prospect with search
  (`docs/status/repo-split.md`, "Next"); strata covered the core only through `core_kv`. Port index,
  lifecycle, reclaim, directory, media, and background merges, with their `_mapped` runs and
  `directory_tmpfs`, to `strata::document_layout`, core headers and `strata::core` only. Drop only
  assertions that are about search. prospect's copies stay.
- Branch: `phase4/core-lifecycle-tests` (worktree `strata-wt/core-lifecycle`), from main `426cc59`.
- Non-sensitive (tests only): no review; lands by fast-forward or merge once CI is green.

## State

Ported and green locally; pushed; CI running (see "CI"). Not landed: the lead lands on green.
Local totals (macOS x86_64): 19 tests pass in RelWithDebInfo, under ASan+UBSan (Homebrew LLVM 23,
Debug -O1), and under TSan (RelWithDebInfo -O1); 9 before the port. Linux adds `directory_tmpfs` (20).

## Done

- `b3c0ea9` six programs on `basic_index<document_layout>` (`index`, `lifecycle`, `reclaim`,
  `directory`, `media`, `background_merge`), their four `_mapped` runs, `directory_tmpfs` on Linux,
  `test/kv_test.hh` (shared helpers), `test/stepping_executor.hh`, the pre-D2 fixture. The commit
  message lists every dropped assertion.
- `14cf51e` live references to `strata_engine` now say `prospect_engine` (the coordinator's
  addition); historical records untouched.

### Mutants (each in a core header, test rebuilt, reverted after)

| Test | Mutant | Result |
|---|---|---|
| media | `read_checked` reads once, no retry | FAILED (transient faults no longer heal) |
| reclaim | `retain()` keeps no pinned manifest | FAILED (pinned manifests missing, 6442) |
| directory | `lock_directory` never takes the lock | FAILED (T5 busy checks, and more) |
| lifecycle | commit skips `sync_files` before the manifest | FAILED (open after power loss: file size differs) |
| background_merge | `install_merge` drops carried deletes | FAILED (digest invariant, twin differs) |
| index | `expected_version` check `!=` becomes `<` | FAILED (version conflict not raised) |

### Changed numbers (the document layout's segments are smaller)

- `merge_rooms`: rooms around admission's reservation for the two-segment index (about 80 KB),
  70,000 to 95,000; with a queued file the window moves down by its size.
- lifecycle disk-full: capacity slack under 400 bytes, and at least 30 of 40 seeds hit a full disk.
- background_merge cancellation: 2,000-byte incompressible records, so stored frames dominate.
- index: `rich` documents carry 100 fields (the 1 KiB-budget run flushes every other transaction).

## CI memory (run 39496090222 was killed on four runners)

`index_test` built a string of `format::max_term_bytes` (4 GiB, termdict's limit) to prove a key
past the limit invalid; with copies it peaked at 12.9 GB. That case is dropped (a key past the
limit is unreachable by a test; the no-prefix and keyless-index invalid cases stay). Peak RSS now,
macOS x86_64, plain / ASan+UBSan: index 400 MB / 978 MB (TSan 850 MB, the 1 MiB-key case), reclaim
12 MB / 453 MB, lifecycle 14 MB / 412 MB, background_merge 23 MB / 224 MB, media 6 MB / 167 MB,
directory 6 MB / 403 MB. No test needs RUN_SERIAL.

## CI

Pushed `phase4/core-lifecycle-tests`; the push starts the 8-job run (5 platform jobs, ASan+UBSan,
TSan, FreeBSD). See the final report for the run id.

## Next

- The lead: land on green (fast-forward or merge into `main`). `repo-split.md`'s "Next" item on
  porting the lifecycle suites is then done.

## Blocked on

- Nothing.
