# Code review: `fix/install-own-files` (17e8b3c, ledger ee842c9)

Reviewer: code reviewer, from a fresh context. Worktree `strata-wt/review-iof` (detached at ee842c9, equal to
`origin/fix/install-own-files`). Merge base with main is 48984ab; main has since gained three docs-only commits
(`docs/perf/exploration-log.md`), so the merge is clean.

## Verdict

**APPROVE WITH CONDITIONS.** No blocker. The fix does what the ledger says: in the mapped run, `install()` now leaves the
target mapping only files it wrote, and the original probe's abort is gone. The one condition is process, not code: CI is not
dispatched yet (ledger "State"), and a Kronuz repo lands only on green CI on all platforms (run `tools/preflight.sh` first).
Two design questions go to the architect as non-blocking notes (CR1, CR3).

## Blocking items

None.

## Findings

### CR1. Reopening with `whole=false` installs a copy damaged in the write without verifying it. Acceptable; say so in the ledger. (non-blocking, design note for the architect)

`index.hh:1185-1186` reopens with `load_segment(*io_, theirs, mine, false)`. It checks the size, the footer `toc_crc`, the header
identity, the document count, the manifest's mask and version range, and the live-docs file; it does not CRC the sections
(`index.hh:3493`). The ledger's justification ("the bytes written are the ones `verify_all` passed on the source") covers the
source's bytes, not the copy. A bit flipped in the write is therefore not an install error.

Probe `.scratch/probes/dmg.cc` (shadow `faultfs` in `.scratch/probes/shadow/` stores one flipped bit 3000 bytes into the first
large write of the target; ASan, `out/dmg.log`):

| Build | Install | Reads of the 120 keys | `heal()` | After reopen |
|---|---|---|---|---|
| main, mapped | ok | 120 ok | 0 damaged | 80 ok, 40 throw |
| main, unmapped | ok | 120 ok | 0 damaged | 80 ok, 40 throw |
| branch, unmapped | ok | 120 ok | 0 damaged | 80 ok, 40 throw |
| branch, mapped | ok | 80 ok, 40 throw (`corrupt_segment`) | 1 damaged id | 80 ok, 40 throw |

No run returned a wrong answer. Main already installs a silently damaged durable copy and serves the verified in-memory bytes
until the next open. On the branch the mapped target serves the copy it owns, so the damage surfaces at first read (the
per-region CRC on first use) and `heal()` reports it, which is earlier than main and matches `media-faults.md` ("never a wrong
answer", damage found by the one retry, then reported). The unmapped path is unchanged. This is the same rule an open follows
(`recover` opens with `whole=false`, `index.hh:3362`), and flush and merge outputs are not read back either (`streamed_segment`,
`written_segment`, `index.hh:1717-1733`), so a whole-segment verify here would make install stricter than every other write path.

If the owner wants an install to refuse a copy damaged in the write (so the host retries rather than finds a damaged shard at
first read), the cost is `whole=true` on the reopen: one CRC pass over the installed bytes, which are page-cache hot right after
`write_new`'s sync. I did not time it (no timing was asked). Recommendation: do not require it for this fix; ask the architect
whether D5 ("verifies each whole") is meant to include the written copy, and fix the ledger sentence either way: it should say
that damage in the copy is found at first read, `heal()` or open, as for every other written file.

### CR2. `install_own` (unmapped) passes without the fix; only `install_own_mapped` guards it. (non-blocking)

Confirmed the contributor's revert claim. `.scratch/rt.sh` compiles `test/install_own_test.cc` against `origin/main`'s `index.hh`
(`.scratch/mut/revert/`): unmapped rc 0, `STRATA_FAULTFS_MAP=1` rc 134, "faultfs: a file truncated below a live mapping"
(`out/revert_1.err`). So the test fails without the code in the one configuration that can see the bug, and the unmapped run is a
control for content (deletes, guard, view, reopen), not a regression guard. The ledger says as much ("unmapped passes (it never
mapped)"). Fine.

Mutants, both mapped, both rc 134 (the test is not satisfied by a partial fix): `.scratch/mut/nolive` (reopen only segments with no
live-docs file) and `.scratch/mut/nolast` (skip the last segment). The contributor's third mutant (live ref cleared) I did not rerun.

Gap: no test covers the new failure site (CR4). `faultfs` has no silent-write-damage knob, which is why I added one in my shadow header.

### CR3. A host error (not EIO) from the reopen fences the writer. (non-blocking, design note)

`read_checked` rethrows a `std::system_error` that is not EIO (`index.hh:3416-3422`); the reopen runs inside `guarded`, which
turns every `system_error` into `fenced_ = true` plus `index_fenced` (`index.hh:1703-1710`). An EMFILE or ENOMEM on the extra
`open_existing` or `map()` per mapped segment (new in this fix) therefore fences the target, where before the fix install opened
nothing after the pre-`guarded` load, and a failed load there left the index "as it was, unfenced" (the doc comment at
`index.hh:1136-1141`). Writes inside the same lambda already fence on every error but ENOSPC, so this is consistent, and a later
reopen of the index clears the fence. Read from the code only: `faultfs` cannot inject an `open` error, so there is no probe. The
architect may prefer the reopen outside the fenced region (before the first `write_new` is impossible, since the files must exist;
so it would be a try/catch that classifies as `space_error` does). Not worth a fix round alone.

### CR4. Failure after some segments are reopened and before the swap: clean. (verified, no finding)

Probe `.scratch/probes/failmid.cc` (ASan, `out/failmid.log`): the target's 1st or 2nd segment copy gets a flipped bit in the header
identity, so segment 0 (and, with skip 1, segment 1) is already reopened from the target's own file when the next reopen throws.
Mapped run: `install` throws `corrupt_index` ("segment identity differs from its manifest entry"); the target's old content is
intact (`kold` readable, nothing of the checkpoint visible); not fenced; `segs_`, `m_` and the published view are untouched
(`got.segments` is local, `segs_ = std::move(...)` comes after the loop, `index.hh:1189`); the orphan `seg-2.sx` and `seg-3.sx`
files are tracked in `files_` and are queued and removed by the next commit and reclaim (directory listings in the log); a retry
install succeeds, and a reopen reads all 120 keys. ASan and UBSan report nothing (no leak: the half-reopened mapped objects die
with `got`). Unmapped control: the damaged copy is not read, install succeeds with the verified bytes (same as CR1).

Counters: `m_.next_segment` and `next_live` advance on a failed install; names are never reused anyway (open takes the max on
disk), same as the pre-existing ENOSPC path.

### CR5. Views, guards and merge sources: none can hold a source-mapped object after the swap. (verified, no finding)

Read of every path between the copy and the swap (`index.hh:1165-1189`): the loop calls `write_new`, `track`, `s->bytes()`,
`load_segment`, and assigns into `got.segments`, which is a local `vector<shared_ptr<open_segment>>` until `segs_ = std::move(...)`.
Nothing in the loop calls `changed()`, `publish_point()`, `free_views()`, `retain()` or `reader()`; those run after the swap
(`index.hh:1207-1214`), when `segs_` holds only reopened objects. `abandon_merges()` (`index.hh:1161`) runs first, so no pending
merge job keeps a source of the old content or captures `got`. Readers on other threads acquire only the published view, which
until `publish_point()` is the target's old content (its own files, kept by `retain()` through D7). The test covers the three
observable cases: a guard held across the install (`before`, old content, still readable after the source's reclaim), a view
taken after (`after`), and the target itself. The segment's other state (`deleted`, `dead`, `damaged`, `unsynced`, `ref`) is
rebuilt by `load_segment` from `mine`, whose `live_count`/`live_length` are the source's and so agree with the live-docs file it
checks. I found no threading question, so no TSan build (the contributor's TSan run on the touched tests stands).

### CR6. Process conditions. (non-blocking)

1. CI on all platforms, with `tools/preflight.sh` first. The ledger says "CI not dispatched". A mapped faultfs test depends on
   `STRATA_FAULTFS_MAP=1` through `set_tests_properties`; confirm `install_own_mapped` runs with it on every CI job.
2. Ledger: reword the "Extra bytes" sentence per CR1; record the architect's answer on CR1 and CR3.

## What I ran

ASan+UBSan (Homebrew LLVM, `build-asan`, via `.scratch/cfg.sh`): `install_own`, `install_own_mapped`, `index`, `index_mapped`,
`lifecycle`, `lifecycle_mapped`, `media`, `shared_view`, `shared_view_mapped` pass (`out/ctest_asan.log`, 10 of 10 selected).
`core_kv` and `background_merge` were built or in the contributor's set; `core_kv` was built but not run by me.

## Kept in `.scratch/`

- `cfg.sh`, `pb.sh`, `rt.sh`: configure, probe build (branch or main header), test-against-shadow-header build.
- `probes/dmg.cc` (CR1), `probes/failmid.cc` (CR4), `probes/shadow/strata/core/faultfs.hh` (faultfs plus `damage_writes`,
  `dmg_skip_`), `probes/shadow-main/` (origin/main's `index.hh`).
- `mut/revert/` (main's header), `mut/nolive/`, `mut/nolast/`; `out/*.log`, `out/*.err`.

## Not run

TSan (no threading question found). Other platforms (Linux, FreeBSD, CI). A real filesystem's SIGBUS (faultfs aborts instead).
Timing of a `whole=true` reopen (not asked). An EMFILE or ENOMEM injection on the reopen (faultfs cannot; CR3 is by reading). A
self-install (target and source on the same IO). The full ctest (only the touched tests). The contributor's live-ref-cleared
mutant. Fuzz.
