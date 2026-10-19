# phase4/dir-lock: code review (small-indexes D2 and D3-C standalone, format 13 F20)

Branch `phase4/dir-lock` at `4b79b7d` (review worktree `strata-wt/review-dir-lock`, detached),
based on main `026ee10`; `origin/phase4/dir-lock` is `4b79b7d` and the developer's worktree is
clean at it, so nothing is unpushed. Scope: `git log 026ee10..4b79b7d` (12 commits: F20's text
`e98fd8f` and `bce5a0b`, the completion pins `582d6dd` and `76ee281` at `60f46ff`, D2 `9e4f2dd`,
D3-C `204daa1`, the keys test `69d8de2`, the FUSE call `f1a3366`, and ledgers). Completion
`60f46ff` is on completion's main (`6e9f148` and `60f46ff` after `6ce490d`).

Read: `docs/design/format-13.md` F20 (`:1124-1153`), `docs/status/format13.md`, and from
`origin/perf/small-indexes` (`39ba962`) `docs/design/small-indexes.md` (D2, D3-C, T5, T5b, T7,
T11), `docs/status/small-indexes.md` (the owner's ruling of 2026-10-19), and
`docs/reviews/small-indexes-design.md` (SI-R17, SI-R18, SI-R21, SI-R22, SI-R25, SI-R27, C4);
`docs/reviews/calibration.md`; completion `posix.h` and `io.h` at `60f46ff`.

Probes, mutants, and logs (all kept, in this worktree's `.scratch/`):

- `bin-asan/lock_probe` (the probe in the ASan build). `build/` (Apple clang, RelWithDebInfo) and `build-asan/` (Apple clang, Debug with `-O1 -g
  -fsanitize=address,undefined -fno-sanitize-recover=all`); `ctest.log`, `ctest-asan.log`.
- `build_one.sh` (a probe or mutant with `directory_test`'s compile command, an include dir first)
  and `build_test.sh` (any test file with its own compile command and a mutant's include dir).
- `probes/lock_probe.cc` (`bin/lock_probe`, output `probes/lock_probe.out`): A, every exception
  path out of `open()`/`create()` releases the lock; B, move, move-assign, `close()`; C, a
  `SIGKILL`ed holder process; D, a forwarding IO over `PosixIO` on a faked NFS under `refuse`;
  E, `create()` with residue against a held lock; F, `open()` with a manifest temporary against a
  held lock.
- `probes/pred2_probe.cc` built against main `026ee10`'s headers (`main-include/`), with
  `probes/mk_d2.cc`: the pre-D2 build opens `test/fixtures/pre-d2` and refuses a D2 index
  writing nothing (`probes/pred2_probe.out`).
- `probes/sizeof_probe.cc`: `sizeof(strata::index)` on the branch and on main
  (`probes/sizeof_probe.out`).
- `mut/` (mine, each `include/` plus `run.log`): `lock-after-clear`, `lock-after-recover` (also
  run under `index_test`, `lifecycle_test`, `concurrency_test`, `reclaim_test` and `lock_probe`),
  `nosync` (SI-R22's setup: no file or directory sync), `nolock-tip`, `d3c-revert-tip`,
  `step2-tip` (the developer's older mutants ported to the tip).
- `mut-dev/`: the developer's tip mutants copied from `strata-wt/dir-lock/.scratch/mut/` and
  rebuilt here (`always-refuse`, `bump`, `fallback2`, `fuse-refused`, `fuse-unlocked`, `keylimit`,
  `no-warning`, `unsupported-refuses`). The developer's `d3c-revert`, `fallback`, `nodirsync`,
  `nolock`, `revert`, `step2` predate the non-local ruling and no longer compile against the
  tip's test; the three that matter are ported above.

## What holds

0. **The suite.** 37 of 37 pass in the RelWithDebInfo build (`ctest.log`) and 37 of 37 under
   ASan+UBSan with no report (`ctest-asan.log`, 898 s; `directory` 16 s, `format13` with the
   4 GiB `PROT_NONE` mapping included). `lock_probe` passes under ASan+UBSan too
   (`probes/lock_probe.asan.out`).
1. **Exclusion (question 1).** `open()` takes the lock before anything else (`index.hh:365-366`)
   and `create()` before clearing residue (`index.hh:2273-2274`). The suite passes 37 of 37, and
   T5 (`directory_test.cc:193-226`) gives busy for all four pairs, as two descriptions and
   against a `posix_spawn`ed process, with the directory unchanged. `lock_probe` adds: every
   exception path out of `open()` and `create()` releases the lock (key prefix mismatch, a
   missing `to`, `file_exists`, a damaged manifest, `index_not_created`); a moved-from index does
   not release the moved-to index's lock; move-assignment releases the old directory and holds
   the new one; `close()` releases; a `SIGKILL`ed holder process makes the parent busy for
   `open()` and `create()` until it dies, and then the open succeeds. The lock is destroyed after
   `group_` (`index.hh:378`, `:2522-2526`), so no background merge outlives it. `nolock-tip` fails
   28 checks; `lock-after-clear` fails T5 (4 checks: the second create reports `file_exists`, not
   busy). One gap, DL1.
2. **The files bump (question 2) is load-bearing.** `pred2_probe` built from main's headers opens
   `test/fixtures/pre-d2` (digest `066bfc9cd60d071f`, files `0x80000004`), so the fixture is a
   real pre-D2 index, and T5b (`directory_test.cc:228-259`) refuses it as `unsupported_index`
   while `owner.lock` is held, nothing written. The `bump` mutant (files back at `0x8000'0004`)
   fails T5b at `:255` (the index opens under the directory lock alone, the two-writer case
   SI-R27 names). The fixtures moved only where they should: `manifest.bin`, `live.bin`, and
   `meta.bin` each differ in byte 9 (the version's low LEB128 byte, `0x84` to `0x85`) and in their
   last 4 bytes (the trailing CRC), nothing else (`cmp -l` against `026ee10`); no segment fixture
   and not `digest.txt` changed; `pre_freeze.txt` changes only the files row. The reverse holds
   too: main's build on a D2 index fails `corrupt_index("no owner lock")` with 4 entries before
   and after.
3. **Deferred create (question 3).** `create()` is `create_deferred()` and then `commit()`, one
   implementation (`index.hh:356-360`, `:2269-2285`). `clear_uncreated` removes only names
   `parse_name` accepts and only when `uncreated()` holds (generation-1 metadata and the first
   manifest's temporary), so foreign files and committed indexes are never removed
   (`index.hh:2289-2295`, `:2316-2334`); the residual for an index that lost every manifest is
   phase 3's, unchanged (`phase-3.md:144-150`). The count is exact: 2 file syncs and 2 directory
   syncs over an empty directory, 3 directory syncs over residue (`directory_test.cc:405-407`);
   `d3c-revert-tip` (always syncing) fails it. `create()`'s comment states SI-R21
   (`index.hh:353-355`).
4. **T11 (question 4) models what it needs and catches the 24.8% case.** faultfs keeps per-sector
   old or new bytes past the last sync and replays each unsynced create, rename, and unlink
   independently (`faultfs.hh:1-15`, `:137-189`), so both unsynced data and unsynced directory
   entries are modelled. Here: 20,007 losses over 18 mutations, 9,469 empty, 1,322 committed,
   9,216 cleared, 0 corrupt. `nosync` (SI-R22's setup: no file or directory sync) gives 7,085
   corrupt of 20,009 (35.4%), and `step2-tip` (commit's first directory sync dropped) 310, both
   failing T11.
5. **The policies (question 5)** behave as ruled, through `PosixIO`'s seams, on every case T7
   lists, and each of the developer's tip mutants fails here as the ledger says: `fuse-refused` 6
   checks, `fuse-unlocked` 8, `no-warning` 14, `always-refuse` 26, `fallback2` 26 and an abort,
   `unsupported-refuses` aborts on warn's create. The `dynamic_cast` fallback is DL2.
6. **The keys test (question 6)** is real: the key sits in `PROT_NONE` pages one byte past
   `max_term_bytes`, borrowed and copied (`format13_test.cc:978-998`); `keylimit` (the check at
   `writer.hh:745` removed) fails at `:994` and dies with a bus error (exit 138), as the ledger
   says.

## Findings

- **DL1 (medium, condition): no test pins "nothing written before the lock" for `open()`.** Moving
  `lock_directory` after `recover()` in `open()` (`mut/lock-after-recover`) passes
  `directory_test`, `index_test`, `lifecycle_test`, `concurrency_test`, and `reclaim_test`. T5's
  "writes nothing" (`directory_test.cc:214-220`) is checked on a quiescent index, where recover
  has nothing to remove. With a writer mid-commit it does: `lock_probe` F puts a
  `manifest-9.tmp` (the holder's commit in flight) beside a held index, and the mutant's second
  `open()` removes it and only then fails busy (`lock_probe.cc:260`, `:263`); recover's
  `reclaim()` would remove the holder's unreferenced new segments the same way. The code at the
  tip is right (`index.hh:365-366`); the property is unguarded. Add F (and E, `create()` with
  residue against a held lock, which the tip passes and `lock-after-clear` fails) to
  `directory_test`, on faultfs, where they cost nothing.
- **DL2 (medium, condition; for the lead and the architect): `refuse` is not enforced through any
  IO but a bare `PosixIO`.** `lock_directory` classifies only through
  `dynamic_cast<PosixIO*>` (`index.hh:2301`); any other IO is taken as local. A host that wraps
  `PosixIO` (metrics, quotas, an allocator context) gets neither the refusal nor the warning:
  `lock_probe` D forwards to a `PosixIO` whose `statfs` says NFS, and `create()` under `refuse`
  succeeds with no `filesystem_warning()`, where the bare `PosixIO` refuses with
  `not_supported`. Under `warn` that is the pre-D2 behavior (no classification), so nothing
  regresses; under `refuse` it fails open, which is what the option exists to prevent. F20 says
  "Only a `PosixIO` classifies" (`format-13.md:1153`) and the ledger lists the follow-up (an
  additive virtual on completion's `IO`, item 5), but the public option's comment
  (`index.hh:326-328`) does not, and `filesystem_warning()`'s comment (`index.hh:567-568`) says
  "warn" only while `refuse` on FUSE sets it too. Condition: the option's comment states that
  only a `PosixIO` classifies; the virtual lands before a host may set `refuse` through a wrapper.
  Smaller, same place: through a non-`PosixIO`, an unsupported lock under `warn` reports
  `local = false` (`index.hh:2311`, `fs && fs->local`) for an IO the code treats as local.
- **DL3 (low, for the architect): a FUSE mount claiming `MNT_LOCAL` is now silent on macOS and the
  BSDs, while Linux always warns on FUSE.** Completion `60f46ff` classifies macOS and the BSDs by
  `MNT_LOCAL` alone (`posix.h:227-229`, `:237-241`), and `directory_test.cc:366-375` asserts that
  a `macfuse` mount claiming it is local, with no warning. SI-R27's residual
  (`small-indexes.md:258-266` at `39ba962`) still says the built classifier "does not trust it
  for FUSE" and refuses `fuse`, `macfuse`, and `osxfuse` by name: no longer true at `60f46ff`.
  macFUSE's `local` option is exactly how sshfs gets `MNT_LOCAL`. `lock_directory` already
  computes `fuse` by name (`index.hh:2302`); using it to set `local = false` for the warning
  would make the platforms agree at no cost. Either that, or the residual's text is restated as
  the new behavior.
- **DL4 (low, for the lead): under `warn`, a failed `fstatfs` refuses the open.** The
  classification runs before the lock under both policies (`index.hh:2301`), and its error
  propagates, so `warn` refuses an index whose lock would work (`directory_test.cc:377-385`
  asserts `EIO` under `warn`). Main had no such failure mode. The ruling covers lock errors
  ("busy and all other lock errors refuse"), not the classifier, which under `warn` is
  diagnostic only. Defensible either way; it should be a ruling, recorded in F20, rather than a
  test's choice. Under `warn` the alternative is a warning naming the failure, lock taken.
- **DL5 (low, for D1): the warning costs 48 bytes per open index.** `sizeof(strata::index)` is
  1,216 on the branch against 1,168 on main (`probes/sizeof_probe.out`, libc++), from
  `std::optional<filesystem_warning>` (`index.hh:2552`), state almost every index never sets.
  With millions of open indexes per host that is 48 MB per million. When D1 splits the idle core,
  hold it behind a pointer or out of the core.
- **DL6 (low, text):** `phase-3.md:44` still names the owner lock `IO::acquire_owner`, and
  `phase-3.md:144-148` describes `create` taking "the owner lock" over `owner.lock`; F20
  supersedes both but they are the documents T1's readers find first. The macOS barrier now
  always runs `F_FULLFSYNC` on the directory descriptor (`posix.h:544-553`, no `owner_fd_` is
  ever held), which the design measured on APFS only; on the non-local mounts `warn` now admits
  it was not run (see "Not run").

## Verdict

**APPROVE WITH CONDITIONS.** The lock excludes a second writer across descriptions and processes
for `create()` and `open()`, nothing is written before it, it lives exactly as long as the index
and every exception path releases it; the files bump is load-bearing and the fixtures moved by
the version byte and CRC alone; the deferred create makes 4 durability calls and T11 catches
the SI-R22 case; the policies follow the owner's ruling and the lead's two calls.

Blocking items: none.

Conditions (before landing unless stated):

1. DL1: `directory_test` gains `lock_probe`'s E and F (on faultfs), so `lock-after-recover` and
   `lock-after-clear` each fail it.
2. DL2: `index_options::nonlocal`'s comment says only a `PosixIO` classifies (another IO is
   taken as local, so `refuse` is not enforced through a wrapper), and `filesystem_warning()`'s
   comment covers `refuse` on FUSE; the completion virtual (ledger item 5) before any host relies
   on `refuse` through a wrapping IO.
3. DL3 and DL4 go to the architect and the lead for a ruling (FUSE claiming `MNT_LOCAL`; a
   failed `fstatfs` under `warn`), recorded in F20 either way. Not blocking the landing.
4. CI run 39270164488 at `4b79b7d`: six of eight jobs green when read (FreeBSD, macOS x86_64 and
   arm64, Linux gcc, clang, arm64), ASan+UBSan and TSan in progress; the lead confirms them green
   before landing.

Not run: Linux, FreeBSD, and tmpfs (the `directory_tmpfs` test and T7's Linux `f_type` cases;
CI's, whose logs I could not fetch); TSan; real NFS, SMB, FUSE, or macFUSE mounts and
`F_FULLFSYNC` on a directory descriptor off APFS (no mounts made on this host); power cuts on
real hardware (T11 is faultfs only); T4 (the descriptor count, the lead's measurement); any
benchmark; Detent's suite against completion `60f46ff`. One slip: for a few minutes four mutant
compiles ran at `nice -n 19` beside the ASan build's 4 jobs under a light hold (`qexp-m3t`),
above the 4-job cap.

## Re-check

Tip `cbcb429` (one commit after the review file's `b878919`), completion pinned at `20a1498`
(confirmed `origin/main` of Kronuz/completion; the build fetched it). Suite 37 of 37 in
RelWithDebInfo (`.scratch/ctest-tip.log`) and 37 of 37 under ASan+UBSan with no report
(`.scratch/ctest-asan-tip.log`, 611 s). `lock_probe` (the review's, unchanged, `probes/lock_probe.cc`)
passes at the tip in both builds (`.scratch/tip/`): E and F pass, and D now prints "error 45
(refused)": the probe's `forwarding_io` does not forward `directory_filesystem`, so it is
unclassified, non-local, and `refuse` refuses it (fails closed, as ruled). New mutants of the tip
are in `.scratch/mut2/` (each `include/` plus `run.log`); each compiles `directory_test` at the tip
with the one edit.

- **DL1: CLOSED.** `directory_test.cc:485-511` adds E (create over `meta-1.kv` residue against a
  held lock: `EWOULDBLOCK`, `f.mutations()` unchanged, residue kept) and F (open beside
  `manifest-9.tmp`: `EWOULDBLOCK`, no mutation, listing unchanged), on faultfs, outside
  `disk_only`. Reverted: `lock-after-recover` (lock after `recover()` in `open()`) fails F at `:509`
  and `:510` (2 checks); `lock-after-clear` (lock after `clear_uncreated()` in `create_deferred`)
  fails E at `:496` and `:497` plus T5's pairs at `:241` and `:243` and one T7 check (7 in all).
  The review's mutant bodies were against `4b79b7d`, so both were re-cut from the tip's `index.hh`.
- **DL2: CLOSED.** `lock_directory` (`index.hh:2303-2324`) calls `io_->directory_filesystem()`;
  no `dynamic_cast` and no `completion/posix.h` include remain in `index.hh`. The base default is
  `{false, "unclassified"}` (`io.h:128` in completion `20a1498`); faultfs forwards through
  `faultfs.hh:410` (`classify()` at `:105` sets another answer, default local "faultfs"); the three
  test wrappers forward (`index_test.cc:107`, `background_merge_test.cc:177`,
  `directory_test.cc:138`). No other `IO` subclass exists in the tree. Comments:
  `index.hh:326-330` (option) and `:567-571` (`filesystem_warning()`, `refuse` on FUSE included),
  F20 `format-13.md:1153-1157`. New tests: `directory_test.cc:430-465` (a forwarding wrapper over a
  PosixIO on injected NFS refuses and warns; a non-forwarding wrapper is "unclassified", refused
  and warned) and `:467-482` (faultfs classified NFS, bare and through a wrapper). Reverted:
  `dyncast` (the old `dynamic_cast`, else local) fails `:446-447` and aborts (the create under
  `refuse` succeeds); `default-local` (base default made `{true, ...}`) fails `:457`, `:458`, `:462`
  (3). The smaller point (`local = false` for an unsupported lock through a non-PosixIO) is gone:
  an unclassified IO is non-local by definition.
- **DL3: CLOSED** (ruling recorded). F20 `format-13.md:1158-1161` states the residual (macOS/BSD
  FUSE flagged `MNT_LOCAL` is local, no warning, Linux warns, `refuse` never refuses FUSE's type),
  and the test at `directory_test.cc:366-375` is unchanged. One line for the lead: the text I flagged
  in `small-indexes.md` (SI-R27's residual, "does not trust it for FUSE") lives on
  `perf/small-indexes`, which this branch does not contain; its owner restates it.
- **DL4: CLOSED.** `index.hh:2306-2312`: under `warn` a failed classification becomes
  `{false, "unknown (<what>)"}` and the open proceeds, locked, with the warning; under `refuse`
  it throws the failure's own `error_code` ("the directory's filesystem is unknown: ..."). Tests
  `directory_test.cc:400-428`: `refuse` open gives `EIO` with "unknown"/"inspect" in the message and
  an unchanged listing; `warn` open warns (not local, locked); `refuse` create gives `EIO` with
  nothing written; `warn` create warns. Reverted: `warn-rethrows` (`throw;` under `warn`) fails
  `:415` and aborts (1 check, then abort); `refuse-ignores` (the failure no longer refuses under
  `refuse`) fails `:408`, `:409`, `:417` (3).
- **DL5: CLOSED.** `docs/status/format13.md:131` (Next item 5) records the 48 bytes per open index
  (1,216 against 1,168) and the D1 instruction (behind a pointer or out of the idle core).
- **DL6: CLOSED.** `phase-3.md:45-46` (tree: no lock file, the directory's own lock, phase 3's
  `lock`/`acquire_owner` named as history), `:148-149` (create clears residue under the directory
  lock), `:34-35` (macOS `F_FULLFSYNC` on the directory handle, APFS only); F20 `format-13.md:1161-1163`
  repeats the APFS-only note and that a non-local mount is untried.

New findings: none that block. One nit for the developer, needing no re-check:

1. `index.hh:20-21` includes `completion/io.h` twice (the `posix.h` line was replaced rather than
   removed). Harmless; drop one.

Verdict: all six closed; nothing blocks landing from this review. The pin at `20a1498` is on
completion's main, so the branch's `CMakeLists.txt` pin resolves as is. CI is the lead's to read.

Not run: Linux, FreeBSD, and tmpfs (CI's); TSan; real NFS, SMB, FUSE, or macFUSE mounts and
`F_FULLFSYNC` off APFS; any benchmark; Detent's suite (the developer reports 214 of 214 with the
replayed chain, not reproduced here); `sizeof(strata::index)` was not re-measured (DL5 is a
recorded follow-up). Builds ran at `nice -n 19` with 4 jobs under a light hold (`qexp-runlevel`)
that began mid-session.
