# strata review: commit barriers (scoped durability and concurrency check)

- Date: 2026-10-29.
- Reviewed: completion `perf/commit-barriers` at `completion:3665ee7` (`git diff main...`, merge base
  `completion:ca0889d`: `cdf1cb3`, `5a338d8`, `fdfb94f`, `3665ee7`, plus the README link `89613a7`); strata
  `perf/commit-barriers` at `strata:e471404` (merge base `strata:6d597cf`; main `strata:c8fd5be` differs in docs
  only), seven commits `0765e28`..`e471404`; prospect `prospect:b9b3c02` (the strata pin, `CMakeLists.txt:9`,
  only); Xapiand `phase7a/t11-barriers` at `Xapiand:d5b4a3c21` (the prospect pin, `CMakeLists.txt:780`, plus
  docs). Every pushed head matches its `origin`. The author's argument: Xapiand worktree
  `docs/perf/t11-barriers/README.md` and `e1`..`e3` notes, `docs/status/t11-barriers.md`, completion
  `docs/PLATFORM_FINDINGS.md` section 1. CI: strata run 40858520202 (`cccb2f9`, linux-tsan failed),
  40862778873 (`e471404`, 8 of 8 jobs green), main 40845608517 (`6d597cf`, 8 of 8 green with linux-tsan);
  completion 40861257614 (`3665ee7`, 8 of 8 green).
- Checked against: the campaign floor for durability and concurrency (the architect's scoped check before
  landing); strata's commit contract (`index.hh:877-880`: any failed sync, rename or directory sync fences);
  Apple's `fcntl(2)` (CommandLineTools SDK `man2/fcntl.2`, `.Dd August 12, 2021`, the `F_BARRIERFSYNC` and
  `F_FULLFSYNC` entries) and `fsync(2)`; `docs/reviews/calibration.md` (heads compared with `origin`).
- Checked by: Homebrew clang 23.1.2 TSan builds with CI's flags (`-O1 -g -fsanitize=thread`) of strata head
  `e471404`, pre-fix `cccb2f9` and main `c8fd5be`; 9 concurrency tests each (background_merge and _mapped,
  cold_segment, shared_view and _mapped, shared_read and _mapped, reclaim_bg and _mapped): 9 of 9 pass on all
  three trees, so they do not discriminate on this host (CB-4). completion `3665ee7` Release, Apple clang:
  16 of 16. Probes and mutants in strata's `.scratch/arch-review/` (listed under Probes).
  - Not run: power loss on real hardware; APFS on non-Apple storage (external, Fusion, AppleRAID, virtual
    disks); macOS arm64; Linux and FreeBSD locally (CI only); the full strata suite under TSan locally (CI ran
    58 tests); prospect's suites; Xapiand's build, ctest and `engine_crash_check.sh`; benchmarks and the T11
    numbers (out of scope); fuzzing.

## Verdict

**REJECT: the ordering protocol is sound and is Apple's documented two-phase use of `F_BARRIERFSYNC`, and the
TSan race was introduced and fixed on the branch; two completion items block landing: a failed barrier is
retried into success (CB-1), and no test breaks when the real macOS ordered call loses the directory (CB-2).**

What holds, and reproduces:

- The protocol is the one Apple documents. `fcntl.2`'s `F_BARRIERFSYNC` entry: "execute operations of phase
  one, then fsync(2) each FD and issue a single barrier. Finally execute operations of phase two", with "no
  file can possibly get persisted with the effect of the second phase without the effect of the first one".
  strata's commit writes every file, the temporary and their directory entries (phase one), calls
  `order_files_and_directory` (`index.hh:4472`), renames (phase two), then `sync_dir()`.
- The real call sequence matches it on every commit. The trace probe ran three lifecycle-style workloads (one
  with background merges) on an APFS directory through `PosixIO`: 120 acknowledged manifests, 0 violations of
  P1 (every file the new manifest names and the temporary fsync'd after its last write, before the barrier),
  P2 (every such file's create followed by a directory fsync before the barrier), P3 (a directory fsync then
  `F_FULLFSYNC` between the rename and the commit's return), and P4 (no unlink before the newest manifest is
  durable). One commit, `trace-build-probe-1.log` seq 14-30: four fsyncs, `fsync .`, `barrierfsync seg-1.sx`,
  the rename, `fsync .`, `fullfsync .`.
- The directory folded into the batch (E1) rests on a documented sentence: `F_FULLFSYNC` "drains the entire
  queue of the device", and "data that had been fsync'd on the same device before is guaranteed to be
  persisted when this call returns". Main's `sync_directory` already relies on it (`fsync(dir)` then
  `F_FULLFSYNC` through the owner file, `posix.h:586-595`). strata at `e471404` no longer calls
  `sync_files_and_directory`; it stays an additive completion call.
- Acknowledged commits are durable: the final `sync_directory` is unchanged and follows the rename
  (`index.hh:4476`). Removing it (mutant S4) fails lifecycle with 31 CHECKs ("inconsistent open after power
  loss").
- No file misses the batch. `commit_files` passes every unsynced segment, flushed or merged (`index.hh:4377`),
  each live-docs file, the metadata file, the field table or its appended block (`written`, `index.hh:4424`)
  and the temporary (`index.hh:4470-4471`). The trace's P1 confirms it with merges. Leaving the live-docs files
  out (S1) or the metadata file out (S2) aborts lifecycle and index on `corrupt_index` (for example "file size
  differs from the manifest: seg-12.7.live").
- The cross-device fallback is unreachable from strata and correct where it applies. `open_file` refuses a
  file on another device (`posix.h:717`), so one index's files share the directory's device. completion's model
  (`sync_files_test.cc:134`, "mixed") checks one barrier on the directory's device and a full flush on the
  other.
- State after the durable point is safe on failure. A space error comes only from `write_unsynced`, before the
  ordered call; every other failure fences (`guarded`, `index.hh:2507`), and a fenced index runs no chores and
  no retention (`index.hh:2583-2585`, `close` at `index.hh:626`). Retention and reclaim run after
  `commit_files` returns (`index.hh:884-893`).
- Linux and FreeBSD are unchanged in strength (CB-6).

What blocks: CB-1 (the barrier's fallback fails open on an I/O error), CB-2 (the real macOS ordered path has
no dying mutant).

## Findings

**CB-1 (blocking). A failed barrier is retried into success.** `DeviceSyncCalls::barrier` (`posix.h:138-144`)
retries with `F_FULLFSYNC` after any `F_BARRIERFSYNC` error, not only when the filesystem lacks the call.
Probe: the tracer with `TRACE_FAIL_BARRIER=EIO` fails every barrier with `EIO`. All 38 commits are
acknowledged; `trace-eio.log` seq 9-11 shows `barrierfsync meta-1.kv -1 5`, `fullfsync meta-1.kv 0`, then the
rename, 49 times in the run. Main fails closed on any flush error (`file_sync`, `posix.h:147-155`; strata
fences), and the comment beside `sync_directory` says so ("Unsupported barriers fail closed").
`F_BARRIERFSYNC` begins with an `fsync`. If that `fsync` fails after the kernel has dropped the dirty pages (as
Linux does; XNU's behavior is undocumented), the retried `fsync` has nothing to write and succeeds, and a
commit naming lost bytes is acknowledged. The fix: fall back to `F_FULLFSYNC` only for the errors a filesystem
without the call returns (`ENOTSUP`, `EOPNOTSUPP`, `ENOTTY`; `EINVAL` only if XNU uses it for an unknown
`fcntl`); fail closed on everything else. Test it through CB-2's seam: `EIO` from the barrier must throw, and
`ENOTSUP` must fall back.

**CB-2 (blocking). No test breaks when the real macOS ordered call loses the directory.** Mutant C5
(`mutants/c5.py`: `sync_staged` passes no directory to `files_sync_devices` in ordered mode,
`posix.h:613`) passes completion's 16 tests (`mutants/build-completion-c5`). By construction it also passes
strata's crash suites, because faultfs implements its own `order_files_and_directory`
(`faultfs.hh:538`), and strata's `PosixIO` test (directory_test) has no crash points on a real directory. The
trace probe catches it: 113 P2 violations on 39 commits (`trace-run/trace-build-probe-c5-1.log`, for example
"meta-1.kv created at 2, no directory fsync before the barrier at 8"), against 0 on the branch. Mutant F2 shows
the protocol depends on the directory entries being ordered: with faultfs ordering staged bytes but not the
namespace changes before the mark, lifecycle and index abort on "missing file: meta-1.kv" and
"missing file: seg-2.sx". completion's model covers the template `files_sync_devices` (`sync_files_test.cc:134`).
Its real path is untested: `sync_staged` (`posix.h:600-614`: which files reach the batch, the directory and
owner descriptors, the empty-batch branch), `order_files_and_directory` (`posix.h:579`; the test calls only
`sync_files_and_directory`, `sync_files_test.cc:226`), and `DeviceSyncCalls::barrier`. The fix: add a seam on
`PosixIO` that injects the sync calls, as `replace_flock` and `replace_statfs` do (`posix.h:479`, `483`). Then
drive `order_files_and_directory` and `sync_directory` on a real directory under the model and assert: every
file and the directory fsync'd, one barrier on the directory's device, nothing reported durable before
`sync_directory`, a directory-only call still ordered, and CB-1's errno cases. C5 must fail that test. The
tracer (`probe/trace.c`, `check_trace.py`) can also become a macOS CI check.

**CB-3 (condition). The barrier's dependence on hardware is understated.** Apple documents `F_BARRIERFSYNC`'s
ordering where the hardware supports it: "It requires hardware support, which Apple SSDs are guaranteed to
provide." It documents `F_FULLFSYNC` for any drive that honors a cache flush. The README ("that the drive
honors a barrier (as the old one trusted it to honor a flush)") and PLATFORM_FINDINGS ("The barrier is only as
strong as the drive's honoring of it, as a full flush is") treat the two as equal. They are not. On external
or third-party drives, Fusion and AppleRAID sets, and virtual disks, what APFS does with the call is
undocumented. A web search found no source for a fallback to a full flush there. If APFS degrades it to a
plain `fsync`, power loss can make a torn commit visible. strata then refuses to open the index
(`corrupt_index`, what mutant F1 produces) rather than serving wrong data, and acknowledged commits stay
durable through the final `F_FULLFSYNC`. The condition: state this caveat in PLATFORM_FINDINGS section 1,
completion's ARCHITECTURE and strata's durability notes, replacing the two sentences quoted. Before the first
release, give hosts an opt-in that makes `order_files_and_directory` flush in full (a `PosixIOOptions` field
selecting the durable mode). Q1 asks the owner for the default.

**CB-4 (closed by `e471404`). The race was introduced by this branch.** `strata:d01a1c4` added the whole-ref
copy `segs_[i]->ref = m_.segments[i]` (`index.hh:4480` at `cccb2f9`). CI run 40858520202 failed linux-tsan on
background_merge, background_merge_mapped, cold_segment and shared_view_mapped. Each report shows the write in
`segment_ref::operator=` from `commit_files` at that line and the read in `run_merge` (`index.hh:2985`,
`doc_count`) or in `open_body_of` (the identity, from a reader's lazy map). Main's linux-tsan is green at
`6d597cf` (run 40845608517). Main's `commit_files` writes only `ref.live`, `ref.live_count` and
`ref.live_length` (`src-main` `index.hh:4429-4437`), which no other thread reads. `e471404` is green
(run 40862778873).

The fix is complete for today's readers. On a shared record the commit now writes only those three fields
(`index.hh:4481-4483`), plus `unsynced`, which only the owner reads. The other threads that read a shared
record's `ref` are `run_merge` (`doc_count`) and `open_body_of` (`id`, `size`, `footer_crc`, `identity`,
`doc_count`, `index.hh:4919`). No other thread reads the live fields: the only uses are `install`
(`index.hh:1471-1494`), `commit_files` and `read_live`, all on the owner or at load. Non-blocking: the branch
now writes the live fields of every record on every commit, and the rule protecting them is a comment ("a
merge reads the others"). Keeping the commit-mutable counts out of the shared `ref` would make the race
impossible by construction.

Local macOS TSan cannot confirm or refute this race class. It misses races whose write is a struct copy
(`__tsan_memcpy`): `tsan-sanity/memcpy_race_raw.cc` and `mr_{id,size,doc_count}.cc` report nothing, while
`order_race.cc` with plain stores reports. `race_probe`'s merge part on `cccb2f9` logs the same address written
by the commit and read by the merge afterwards (`race-prefix-debug.log`) with no report. The Linux TSan job is
the authority for this class; this belongs in `calibration.md`.

**CB-5 (non-blocking). faultfs's ordering model is faithful for this commit, with one optimistic case.** My
mutants die: S1 and S2 (above), S4 (lifecycle; index_test does not catch it), F1 (faultfs models no ordering:
lifecycle aborts on "file size differs from the manifest: seg-1.sx", index on "corrupt segment: header
magic"), and F2. lifecycle ran them at 10,000 crash points and 2,000 with background merges; the
unmutated tree passes at 3,000 and 600 (`mutants/run-base-*.log`), and CI passes the default counts.
Logs: `mutants/run-*.log`. The author's mutants are the same kind and agree. The optimistic case: once the staged
state survives, a write to a staged file after the ordering point becomes durable in full
(`faultfs.hh:179-182` copies `current`, later writes included). `probe/faultfs_staged_probe.cc`: 197 of 400
crashes kept a 4 KiB post-mark write whole and none kept part of it, where a device may keep any subset. The
commit writes no staged file between the ordered call and `sync_dir()` (`index.hh:4472-4476`), so today's
results stand. A protocol that appends after a barrier would be over-trusted. Fix: snapshot the staged bytes
in `stage()` and treat later writes like any unsynced write in `crash()`. Scope note: the test IO wrappers in
index (`flaky_io`), background_merge (`gate_io`), flush_memory, format14 and cold_segment do not forward
`order_files_and_directory`, so those paths take the base default (full sync before the rename). The ordering
coverage comes from lifecycle's T1 (faultfs directly, background merges included) and the direct faultfs
cases in index and directory. The README's list of suites that "enumerate crash points through commits"
reads broader than that; reclaim, retention and fields were not checked here.

**CB-6 (no action). Linux and FreeBSD keep the old strength.** Off macOS, `order_files_and_directory` is
`sync_files(files)` then `sync_directory()` (`posix.h:579-584`). Every data file, the temporary and the
entries are durable before the rename, then the rename and a directory fsync: main's durable states, with the
temporary's entry also durable before the rename (harmless). Wrappers that forward only `sync_files` and
`sync_directory` get the same through the base defaults (`io.h:231-240`). CI at `e471404` is green on
linux-x86_64 gcc and clang, arm64-gcc, asan-ubsan, tsan and freebsd.

## Blocking items

- L1 (CB-1): `DeviceSyncCalls::barrier` falls back to `F_FULLFSYNC` only on the unsupported-call errors, and
  fails closed on every other error; a test shows `EIO` from the barrier throws and `ENOTSUP` falls back.
- L2 (CB-2): a `PosixIO` seam for the sync calls and a test of `order_files_and_directory` and
  `sync_directory` through it on a real directory, which fails under mutant C5
  (`.scratch/arch-review/mutants/c5.py`) and under a `barrier` that ignores CB-1's errno rule.

## Conditions

- C1 (CB-3): the hardware caveat stated in completion's PLATFORM_FINDINGS section 1 and ARCHITECTURE, strata's
  durability notes and the T11 README, replacing the two sentences quoted; an opt-in full-flush ordering for
  hosts before the first release.
- C2 (CB-4): the macOS TSan blind spot recorded in `calibration.md`; concurrency fixes keep needing the Linux
  TSan job green.
- C3 (CB-5): faultfs keeps post-mark writes to a staged file unordered before any protocol writes after a
  barrier.
- Q1 (owner): on storage other than an internal Apple SSD, should strata order with the barrier and warn
  (the current behavior), or flush in full unless the host opts in? The architect's pick is the barrier with
  the warning and an opt-in, per the "warn, don't babysit" rule.

## Probes

All under `/Users/kronuz/code/Kronuz/strata-wt/commit-barriers/.scratch/arch-review/`, kept:

- `probe/trace.c` (a dyld interposer logging fsync, `F_FULLFSYNC`, `F_BARRIERFSYNC`, creates, renames,
  unlinks, writes and truncates under a root; `TRACE_FAIL_BARRIER=EIO` fails every barrier),
  `probe/commit_trace.cc`, `probe/check_trace.py` (P1-P4), `probe/CMakeLists.txt`,
  `probe/configure-probe.sh`, `run-trace.sh`; traces in `trace-run/` (`trace-build-probe-1.log`, `-2bg.log`,
  `-3.log`, `trace-eio.log`, `trace-build-probe-c5-1.log`).
- `probe/race_probe.cc` (relaxed-handoff race probe, merge and reader parts), `build-race-probe.sh`,
  `run-race-probe.sh`, `race-*.log`, `race-prefix-debug.log`; `tsan-sanity/` (`race.cc`, `relaxed.cc`,
  `order_race.cc`, `memcpy_race*.cc`, `mr_*.cc`).
- `configure.sh`, `build-tsan.sh`, `run-tsan.sh`, `tsan-{head,prefix,main}.log`, trees
  `build-tsan-{head,prefix,main}`, exports `src-main`, `src-prefix`, `completion-7734544`;
  `ci-tsan-40858520202.log`.
- `build-completion.sh`, `build-completion/` (16 of 16).
- `mutants/strata_mutants.py` (S1, S2, S4, F1, F2), `mutants/run-strata-mutant.sh`, `mutants/run-*.log`,
  `mutants/c5.py`, `mutants/completion-c5/`, `mutants/build-completion-c5/`, `mutants/build-probe-c5/`,
  `probe/faultfs_staged_probe.cc`.

A re-check reruns `run-trace.sh` (with and without `TRACE_FAIL_BARRIER=EIO`), the new completion test against
`mutants/c5.py`, and `mutants/run-strata-mutant.sh` for each mutant.
