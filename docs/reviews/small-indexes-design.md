# strata review: small indexes, preliminary design review (D1 to D3)

- Date: 2026-10-18.
- Reviewed: `docs/design/small-indexes.md` on perf/small-indexes at `25deedc` (matches origin);
  `docs/perf/small-indexes.md` and `docs/status/small-indexes.md` at `25deedc`; the code those
  decisions move, `include/strata/index.hh`, `files.hh`, `budget.hh`, `merge_policy.hh` at
  `25deedc`; the completion library at strata's pin `5251cf9` (`CMakeLists.txt:30`):
  `include/completion/posix.h`, `io.h`, `docs/PLATFORM_FINDINGS.md`; format 13's design at
  origin `b9cf1b3` (the local branch holds four unpushed commits, `1c76aff`, `5f9d297`, `3cf6bfa`,
  `a83b06e`, and uncommitted edits to `format-13.md`; none touches F2 or F20); `track-c.md` D11
  and T6; design/durability-modes (local only, at main `755a918`, no design file yet).
- Checked against: PLAN ("What waits", the many-index workload, Goal 5), AGENTS.md (one
  implementation per mechanism, limits), REPLICATION.md, the lead's Q1 and Q2 rulings,
  `calibration.md`.
- Checked by: reading the code paths each decision changes; the designer's directory-lock probe
  (`.scratch/dirlock/t.c`). No build, test, or probe was run before the pause.
  - Not run: every build and test (no `quiet check` was needed: nothing compiled); APFS probes of
    a directory descriptor's `F_FULLFSYNC` and two-process `flock`; any NFS probe (no mount);
    faultfs probe of Q2's crash residue; Linux and FreeBSD hosts (shared measurement hosts, not
    touched); benchmarks.

## Interim (paused)

Paused at the coordinator's request (quota). No verdict yet: the rulings on D1, D2, and D3 wait
for the probes listed under "Still unchecked". Findings so far, builds-on first; IDs stay stable.

### Findings

**SI-R1 (D2, builds on format 13).** The three-step migration covers indexes format 13 cannot
have. Format 13 refuses format 12 outright (`format-13.md:243` at `b9cf1b3`) and moves the
manifest family to version 2 (F20, `:1001-1005`); a format-12 binary rejects a version-2 manifest
at `files.hh:118` (`corrupt_index`) before it writes anything. If D2 lands inside format 13
before the freeze, every format-13 index locks its directory from creation, the manifest's
version is the protocol marker, and steps 1 to 3, the protocol field, and the lock file's
retirement are not needed. If D2 misses the freeze, the field needs manifest version 3, and F20
names no such field today. Fix: choose one in the design and, if it is format 13, amend F20
before the freeze. Probe: F2 and F20 at `b9cf1b3`; `files.hh:29, 118`.

**SI-R2 (D2, builds on the completion library).** On macOS the directory barrier uses the owner
lock's descriptor: `sync_directory` runs `fsync(dir)` and then `F_FULLFSYNC` on `owner_fd_`,
throwing `logic_error` when no owner descriptor exists (`posix.h:443-453`). D2-A removes that
file, and the design does not say what carries the device flush. Either `F_FULLFSYNC` on the
directory descriptor works on APFS (not yet probed) or the variant keeps a regular file for it,
and macOS keeps two descriptors. Fix: probe it, then specify the variant's barrier before
anyone designs the completion call.

**SI-R3 (D2).** `create()` is outside the exclusion story. An old binary's create makes
`owner.lock` (`O_CREAT | O_EXCL`, `posix.h:348-358` and `open_file`) and then
`clear_uncreated()` scans and removes `meta-1` and `manifest-1.tmp` (`index.hh:2436-2447`).
Beside a new binary's create, which holds only the directory lock, that deletes the new
create's files mid-write. In a committed D2 index the old create fails `file_exists` but leaves a
stray `owner.lock`. T5 lists no create pair. Fix: T5 gains old and new create-create and
create-open pairs. The new binary also locks `owner.lock` whenever the file exists (it never
creates one), and either holds one during create until `manifest-1` exists, or the design states
that old and new binaries on one root are out of scope.

**SI-R4 (D2).** After step 3 an old binary does write nothing: `acquire_owner(false)` opens
without `O_CREAT`. But it fails as `corrupt_index("no owner lock")` (`index.hh:383-393`), which a
host treats as damage to heal or reinstall, not as a version refusal. The unlink-while-held race
is harmless here only because an old binary cannot write a newer index. An old opener blocked on
the old inode takes its lock once the new binary closes it. Lucene never deletes `write.lock` for
this reason. Fix: state both points, and the error a host should expect.

**SI-R5 (D2, NFS and the fallback).** "Decided at open by the lock call's error" (`:100-101`;
T7, `EOPNOTSUPP` or `EINVAL`) cannot detect NFS:
- Per Linux `flock(2)`, NFS emulates `flock` as a whole-file `fcntl` lock, and an exclusive lock
  needs the file opened for writing, which a directory cannot be. Where directory locks are not
  sent to the server, the call succeeds as a local lock that excludes no other client.
- Either way, `EBADF` or a silent local success, the call escapes T7's list. NFS clients also lose
  the cross-client exclusion that today's `owner.lock` gets from the server.
- Darwin defines `EOPNOTSUPP` as 102 or as `ENOTSUP` (45), depending on the compilation mode
  (`sys/errno.h:144, 154, 251`), so testing one name misses the other.
- Deciding per open lets two hosts on one export choose differently (one the directory, one the
  file), so neither excludes the other.

Fix: decide by filesystem type (`statfs` `f_type` and `f_fstypename`) and refuse network
filesystems by default (NFS, SMB, FUSE without locks, 9p), leaving them to D2-C's host policy.
Or record the lock protocol in the index at create, and have every opener use it or refuse,
never falling back per open.

**SI-R6 (D3, public API, builds on).** `prepare_commit()` and `commit_all(span)` come with no
thread or state contract:
- Index calls run on the owner thread. `commit()` also publishes merges, runs auto merges,
  retention, and reclamation (`index.hh:528-536`), and takes the new manifest only after the last
  directory sync (`:2300`).
- The design leaves open what the span holds, which thread runs `commit_all`, what the owner may
  call between prepare and completion, and where retention runs.

Recommended: keep `commit()` as the one API, and add a node-level commit group in the options,
as `budget` and `background` are. Its leader shares the syncs of commits already waiting
(leader and follower, as Elasticsearch's translog `AsyncIOProcessor` and RocksDB's write group
do). There is then no host API, and durability-modes' checkpoints can use the same object
(SI-R9).

**SI-R7 (D3).** Failure attribution contradicts T10. `files_sync` throws at the first failed
`fsync` without naming the file, and on Linux the throw stops it before the remaining files sync
(`posix.h:173-187`). `IO::sync_files` leaves durability after a failure unspecified
(`io.h:152-155`). So a sync-time EIO in a batch cannot be pinned on its index: either the batch
fences (`:144-146`) or T10's "fails that index's commit only" holds, not both. Fix: T10 lists what
it isolates (write-time ENOSPC and EIO, and per-file `fsync` errors through an additive
completion call that returns per-file results and keeps going) and what fences the batch (a
device flush, which covers every index staged on that device, and directory syncs).

**SI-R8 (D3, T9's contract).** "One that failed leaves its index at its previous manifest" is
false today. `commit_files` renames the manifest (`index.hh:2296`) before the directory sync
(`:2299`). A failed sync fences with the new manifest in place, and a reopen in the same boot
reads it from the page cache as the head. After a failed `fsync`, Linux may report success to a
retry over lost data (fsyncgate; PostgreSQL's `data_sync_retry` defaults to a panic for this
reason). The contract a failed commit actually gives is a crash's: the previous or the failed
manifest, as recovery finds it. Fix: T9 states that, and a fenced index never retries a sync
and then treats it as durable.

**SI-R9 (D3 against Track C's D11 and durability-modes).** Track C already decides between the
same two options by measurement: the per-group journal with batched syncs against the shared
journal (`track-c.md:625`, and T6's decision rule at `:874`). The ruling so far:
- D3-A builds toward durability-modes if it is the engine's one sync batcher below `commit()`,
  which durability-modes' background checkpoints reuse. Exposed as a host API, it becomes a
  second mechanism.
- D3-B leaves this design for durability-modes, which must use D11's turn log (one journal).
- D3 must keep the per-index manifest contract, because journal checkpoint records reference
  manifests (`REPLICATION.md:90-92`), along with per-index completion and no format change.
- To be finished once durability-modes has a file.

**SI-R10 (D3, macOS phases).** The directory phases cannot be batched with today's primitives.
Each index has its own `PosixIO`, and each `sync_directory` pays its own `F_FULLFSYNC`
(`posix.h:443-453`; `PLATFORM_FINDINGS.md`, "Not yet done"). "Each phase one `files_sync` over
the batch" needs an additive cross-IO directory barrier: `fsync` every directory, then one
`F_FULLFSYNC` per device. That is owed to Detent's library. T8's 2x on macOS depends on it.

**SI-R11 (Q2, safety).** As written, Q2 breaks the claim it rests on. D3-C (`:135-138`) writes
the metadata and the first manifest unsynced. `uncreated()` accepts only `meta-1` and
`manifest-1.tmp` (`index.hh:2427-2434`). A renamed but unsynced `manifest-1` can survive a crash
torn or empty (delayed allocation), and `recover()` then throws `corrupt_index`, not
`index_not_created` (`:2471-2486`). Two forms are safe:
- `create()` writes nothing and the first commit writes generation 1 in today's order, so the
  crash residue stays inside `uncreated()`.
- `uncreated()` also accepts a lone generation-1 manifest that fails to decode or names a failing
  file, and `create()` clears it.

Either way, deferred files stay open until the first commit syncs them, as flushed segments do,
so a writeback error surfaces at that sync. The lead's ruling does not say whether Q2 is an
option or the default; the design recommended an option (`:177-178`). As the default it changes
every host's `create()`.

**SI-R12 (Q1, safety).** Safe with one change. `recover()` writes into the options
(`opts_.key_prefix = m_.key_prefix`, `index.hh:2494`); with shared immutable options that write
goes, and the index reads `m_.key_prefix`. The merge policy's finders are `const`
(`merge_policy.hh:58, 111, 155`) and the reorder settings are plain data, so sharing them is
race-free.

**SI-R13 (D1, lifetimes).** `apply()` calls `refresh()` (`index.hh:431`) before `evaluate()`.
Once a flush can free `write_state`, any write-path function holding `write_state&` across a call
that can flush dangles. The mechanical move must fetch `ws()` again after each such call, and
T3 needs an ASan case where a flush releases the state inside one `apply` (the
idle-then-write test does not reach that). Releasing at every flush costs little: `clear_buffer`
already frees `active_` and the docid map (`:2054-2060`).

**SI-R14 (rows).**
- T8's procedure does not say how `many_write.sh` batches commits; one batch of 1,000 meets any
  target.
- No row checks that a lone commit is no slower (`:151-152`).
- T5 lacks create pairs (SI-R3), and T7's errno list is wrong (SI-R5).
- The design's directory-lock probe kept no output and tested no `F_FULLFSYNC`, no tmpfs process
  pair, and no NFS.
- The ledger has no done criteria yet (`docs/status/small-indexes.md:10`).

### Still unchecked

- APFS: `F_FULLFSYNC` and `fsync` on a directory descriptor; directory `flock` across two
  unrelated processes (SI-R2, SI-R5).
- NFS: how directory locks are routed (kernel source or a mount); not settled here.
- A faultfs probe of Q2's torn `manifest-1` (SI-R11). The sanitizer builds and the 12 damage
  tests at `8b834ee` are not rerun.
- FreeBSD and macOS `fsync` error semantics (dirty buffers kept or dropped), and whether UFS
  issues a device flush, for SI-R7 and SI-R8.
- D1: every `ws()` and `ws_if()` call site against the merged inverter (perf/inverter at
  `7179087`; its local branch holds an unpushed `2db1c54`).
- Rulings per step (APPROVE, APPROVE WITH CONDITIONS, or REJECT), conditions, and blockers.

## Completed review

- Date: 2026-10-19.
- Reviewed: `docs/design/small-indexes.md` on perf/small-indexes at `7c23560` (unchanged since
  `25deedc`); main at `be64292` (layers, the inverter, and its follow-ups, C11 at `7bca610`);
  phase4/format13-on-main at `43ee403` (F2, F20, F13-R43, F13-R51); design/durability-modes at
  `37fafe9` with its preliminary review `7ffc16b` (DM-R2, DM-R3, step 2); research/no-stalls at
  `467b5b3` (section 6, item 1); phase-3 D11, D15, D17 at main; the completion library at
  strata's pin `5251cf9` (`posix.h`, `io.h`, `PLATFORM_FINDINGS.md`); Linux `flock(2)` (man-pages,
  "NFS details" and "CIFS details").
- Checked against: the interim above (SI-R1 to SI-R14, and its "Still unchecked" list), PLAN
  ("What waits"), AGENTS.md (one implementation per mechanism, limits), `calibration.md`.
- Checked by: Homebrew clang 23.1.2, x86_64 macOS 15 (Darwin 24.6), APFS. The branch at
  `7c23560` built Debug with ASan and UBSan (`-fno-sanitize-recover=undefined`), and the whole
  suite run under them: 34 of 34 tests passed in 3,717 s at four jobs (`ctest-asan.log`), among
  them index (the 12 damage cases of `8b834ee`), lifecycle and lifecycle_reordered (3,269 and
  3,229 s, 10,000 crash points each), the mapped variants, faultfs, and both fuzz replays. Probes, all in `.scratch/si-review/` of the detached
  worktree `strata-wt/review-small-indexes` (outputs beside them): `apfs/dirprobe.c`
  (`apfs.out`, `ramdisks.out`), `apfs/dirsync.c` (`dirsync.out`), `q2/q2_deferred_create.cc`
  (`q2.out`), `fence/failed_commit_head.cc` (`failed_commit_head.out`), `d1/d1_sizes.cc`
  (`d1_sizes.out`, built against main in the detached worktree `strata-wt/review-si-main` and
  against the branch), `ws_sites.py` (`ws_sites.out`, main's write-state uses by function). The
  APFS timings ran behind `quiet run` light holds (`si-review-apfs`, `si-review-apfs2`), one
  thread, three minutes at most; builds and tests at `nice -n 19`, four jobs, under the light
  holds `qexp-e1-*` that were live.
  - Not run: any NFS, SMB, or FUSE mount (none here, and making one needs root); Linux and
    FreeBSD (shared measurement hosts, not touched), so ext4, tmpfs, XFS, and UFS directory
    locks rest on the designer's 2026-10-18 probe, which kept no output; power-cut tests of
    APFS's `F_FULLFSYNC` on a directory (timing is the evidence); writeback-error semantics on
    macOS and FreeBSD; TSan; benchmarks (T1, T2, T8); the MS-DOS RAM disk (`diskutil` refused
    to format it).

### Verdict

**D1: APPROVE WITH CONDITIONS. D2: APPROVE WITH CONDITIONS, preferably inside format 13 before
the freeze, without the migration, and with no per-open fallback. D3: REJECT as written (a host two-phase API and
its own shared journal); APPROVE WITH CONDITIONS restated as durability-modes' background
checkpoint batcher. Q1: yes, after D1. Q2: D3-C as written is unsafe (a quarter of power losses
leave a directory neither `open()` nor `create()` accepts); yes in SI-R11's first form, as the
default wherever a journal records the creation.**

What holds, and reproduces:

- **A directory descriptor carries the macOS device flush.** `F_FULLFSYNC` on an `O_RDONLY`
  directory descriptor succeeds and costs what it costs on a file: 20.5 ms p50 after a staged
  write, against 20.4 ms on the file and 2 µs for `fsync(dir)` (`apfs.out`, 60 rotated rounds).
  So D2-A on macOS needs no regular file for the barrier (SI-R2).
- **One flush covers many directories.** Sixteen index directories, each with a create and a
  rename: `fsync` each and one `F_FULLFSYNC` takes 20.8 ms p50; today's barrier per directory
  takes 325 ms, 15.7x (`dirsync.out`). On APFS `fsync(dir)` alone costs 4 µs with a dirty
  namespace, so the device flush is the whole cost (that one flush also
  commits the other directories' namespaces is the assumption completion's `files_sync` already
  makes for files; no power cut tested it): a batch's floor is about 20 ms per phase
  per device whatever its size, and SI-R10's cross-IO barrier is what T8's macOS gain rests on.
- **A directory `flock` excludes on every local macOS filesystem probed.** APFS, HFS+, and exFAT
  (RAM disks): a second description in one process and an unrelated process (`posix_spawn`, no
  inherited descriptors) both get `EWOULDBLOCK` (35); the lock follows the inode across a rename
  of the directory; a `SIGKILL`ed holder releases it (`apfs.out`, `ramdisks.out`). All three
  report `MNT_LOCAL`.
- **A failed sync fences and is never retried in-process** (main `index.hh:502-511` through
  `guarded`, `:988-996`), so SI-R8's fsyncgate concern is about reopening, not retrying (SI-R20).
- **SI-R12 holds on main:** the one options write is `index.hh:2322`; `install` writes only
  `m_.key_prefix` (`:824`).
- **SI-R13 holds on main:** `apply` refreshes before `evaluate` (`index.hh:405`).

What blocks (each before implementation of its step, none before another step starts):

- D1: SI-R15 (T1's target is unreachable on main), SI-R16 (the write state is the freeze's
  unit).
- D2: SI-R17 (land in format 13; drop the migration), SI-R18 (a refusal rule, no fallback).
- D3: SI-R19 (the batcher checkpoints a named generation, off the owner thread), SI-R20 (a failed
  checkpoint is a failed barrier), with SI-R7 and SI-R8 folded into its rows.
- Q2: SI-R11, now measured (SI-R22).

### Status of SI-R1 to SI-R14

- **SI-R1: superseded by SI-R17.** Format 13 now has an index-level attribute list with a
  required-tag rule (`format-13.md:1116-1122`, F13-R51), so missing the freeze no longer costs
  files version 3; inside the freeze, no field at all.
- **SI-R2: closed by probe.** `F_FULLFSYNC(dir)` is a device flush (above). The variant's
  barrier is `fsync(dir)` then `F_FULLFSYNC(dir)`, and `sync_directory`'s `logic_error` without
  an owner descriptor (`posix.h:443-453`) becomes that path.
- **SI-R3, SI-R4: closed by SI-R17 if D2 lands inside format 13;** open otherwise. Create pairs
  stay in T5 for two binaries of the same version (SI-R17).
- **SI-R5: closed by rule** (SI-R18). Not probed on NFS; the rule does not depend on how a
  client routes a directory lock.
- **SI-R6: closed.** Durability-modes takes SI-R6's form: no host API, `commit()` the one call,
  one node-level group (`durability-modes.md:667-673`). SI-R19 corrects its thread.
- **SI-R7: open,** carried into D3's restated T10 (SI-R19).
- **SI-R8: partially closed, narrowed by probe.** Only a failure of the last directory sync
  leaves the failed commit's manifest renamed, after every file it names, the manifest
  included, synced (`write_new` syncs, `index.hh:2151`). A same-boot reopen takes it as the head
  in 200 of 200 runs, and a power loss then reverts it in 147 of 200 (`failed_commit_head.out`).
  Failures at the three earlier syncs reopen at the previous manifest, 200 of 200 each. So the
  data is safe and T9's sentence is wrong: the contract after a failed commit is "the previous
  manifest or the failed one". Under durability-modes it matters more (SI-R20).
- **SI-R9: closed.** Durability-modes' D1 makes D3-A its checkpoint batcher and replaces D3-B
  (`durability-modes.md:145-167, 667-677`).
- **SI-R10: closed for the design; owed by the library.** It is durability-modes' step 2, which
  may start now (`durability-modes-design.md:566`); `dirsync.out` sizes it.
- **SI-R11: open, measured** (SI-R22).
- **SI-R12: open until Q1 is built** (holds, above).
- **SI-R13: open,** a condition on D1.
- **SI-R14: partially closed.** T5 and T7 are rewritten by SI-R17 and SI-R18; T8 is replaced
  by SI-R19's rows; the lone-commit row is SI-R19's T8c. The ledger still has no done criteria
  (`docs/status/small-indexes.md:10`).

### Findings

**SI-R15 (D1, T1): the split saves 208 B on main, under T1's 220 B.** The design lists the
branch's members (`:33`, 232 B, 224 saved). Main has no `valid_terms_` and has
`max_run_bytes_` (`index.hh:2447`), so the move is 216 B with the test hook and 208 without,
and the saving after the `unique_ptr` is 208 or 200 B (`d1_sizes.out`, libc++: `sizeof(index)`
1,056 on main, 1,048 on the branch). T1's "at least 220 B" fails on main as built, on macOS;
libstdc++'s figures are unmeasured. `options` is 232 B on libc++, not 240 (`:45`). Fix: D1 lists
main's members (`max_run_bytes_` moves: it bounds a run of the buffer), and T1's target is the
moved members' size less 8, stated per standard library from a `sizeof` probe, at least 200 B on
libc++.

**SI-R16 (D1, against no-stalls item 1 and phase-3 D11, D15, D17): the write state must be what a
background flush freezes.** No-stalls ranks moving the flush off the owner thread first
(`no-stalls.md:668-673`: freeze the buffer at refresh, flush it on the pool, the owner continues
into a fresh buffer, the budget holds two); phase 3 already decides it (`phase-3.md:270-273`,
`:325-326`). Then:
- `ws_if() == nullptr` no longer means "an empty buffer" (`:53`). While a frozen buffer is in
  flight, a key or docid in it must still resolve: `locate_docid`, `key_slot`,
  `buffered_docid`, `buffered_key`, `buffered_version`, `buffer_size`, `digest`, `map_bytes`,
  `charge` (main `index.hh:1570-1850`, `638`, `881-890`; 28 functions use these members,
  `ws_sites.out`). The read paths consult the owner's state and every frozen one.
- The release rule becomes the freeze itself: the freeze moves the buffer's part to the flush,
  the owner's slot empties, and the next `apply` makes a fresh one. Done as written, a busy index
  pays the write state's first growth (the buffer's floor alone is 1,002 B, 2,898 B through
  `apply` with the version map, `reviews/inverter-followups.md:53, 73`) at every refresh.
Fix: split `write_state` in two before the move: the buffer and its version map (frozen with
the flush, charged on its own permit until it settles, as D17 says), and the transaction
scratch (`pending_docs_`, `terms_in_`, `undo_`, owner-only, released after one refresh interval
with no `apply`). State which read paths see frozen states, and add to T2 a 1,000 x 100 row
with a refresh per second, where any per-refresh allocation would show. D1 may land before the
background flush, but its struct boundary is the one the flush uses, so the code moves once.

**SI-R17 (D2, format 13): land D2 inside format 13 and drop the migration.** Format 13 refuses
format 12 (`format-13.md:253`: rule 3), every pre-freeze build refuses another's files
(`files::pre_freeze`, F20 and C17), and strata has shipped no release (no tag but two
`format13-*` bookmarks). So no binary a D2 binary must coexist with exists. If D2 lands before
the freeze, bumping `files::pre_freeze`:
- every format-13 index locks its directory from creation, and steps 1 to 3 (`:86-99`), the
  protocol field, `owner.lock`'s retirement, T5's old-binary pairs, and T6 are removed;
- the design states that binaries from before D2 on one root are out of scope; a pre-D2
  pre-freeze build fails at `corrupt_index("no owner lock")` (main `index.hh:367`) and writes
  nothing, which SI-R4's note covers for development builds;
- T5 keeps the pairs of one version: open-open, create-create, and create-open on one
  directory, each busy and nothing written, on Linux, macOS, and FreeBSD.
If it misses the freeze, the protocol is a required (odd) index-level tag (F13-R51), not a
version, and the three steps stay with the tag as their marker. Recommended: before the freeze.
It needs only the completion library's additive directory-lock call (SI-R18).

**SI-R18 (D2, closes SI-R5 by rule): refuse network filesystems; never fall back per open.**
Linux emulates `flock` over NFS as a whole-file byte-range lock that needs the file open for
writing, or treats it as local under `local_lock` (`flock(2)`, "NFS details"); over SMB it is a
mandatory byte-range lock since Linux 5.5, under which other I/O fails `EACCES` ("CIFS details"). A
directory cannot be opened for writing, so on any network filesystem a directory lock either
fails with an error T7 does not list or succeeds locally and excludes no other client. Neither
outcome is detectable from the lock call. The rule:
- The completion library's additive call (`acquire_owner` on the directory handle) checks the
  filesystem before locking: on macOS and FreeBSD `statfs` with `MNT_LOCAL` set (APFS, HFS+, and
  exFAT probed here all set it); on Linux an allow-list of `f_type` (ext2/3/4, XFS, Btrfs,
  tmpfs, F2FS, ZFS, bcachefs, overlayfs; ZFS and overlayfs unprobed, so CI decides them). Anything else refuses at `create()` and `open()` with a
  named error, the filesystem's name in its message.
- There is no lock-file fallback: `owner.lock` retires with format 13 (SI-R17), so a fallback
  would be a second lock mechanism. A host that must run on a network filesystem takes D2-C (its
  own root lock, which the engine never sees).
- T7 becomes: a `statfs` the test injects (NFS, SMB, FUSE, 9p, an unknown type) refuses at create
  and open, and nothing is written; Linux CI runs the lock tests on tmpfs as well as ext4.
  Detent keeps the file-lock call (additive only).

**SI-R19 (D3, restated as durability-modes' checkpoint batcher; builds on DM-R3 and no-stalls).**
D3-A's two-phase host API (`:125-130`) is withdrawn for SI-R6's form, which durability-modes
adopts. Three things the batcher must specify that neither design does:
- **Its unit is a checkpoint of a named generation.** DM-R3 requires a checkpoint of the newest
  refresh generation at or below `commit_durable`, with that generation's files retained
  (`durability-modes-design.md:224-245`). So a batch entry is (index, generation, its files and
  manifest snapshot), taken by the owner when it submits, and `commit_files` (branch
  `index.hh:2215-2305`) works from that snapshot, not from `segs_` at phase time. Standalone
  `commit()` is the same entry for the current generation, a batch of whatever waits: one
  implementation.
- **It runs on the pool, never on an owner thread.** SI-R6 proposed a caller-thread leader. With
  the flush off the owner thread (no-stalls item 1), a leader on an owner thread would stall that
  index's `apply` for the batch: four device flushes, about 80 ms here (`dirsync.out`), whatever
  the batch's size. The batcher is a task on the pool's background class; `commit()` blocks only
  its caller on the entry's completion; retention and reclamation run on each owner after its
  completion (`index.hh:508-509`), not in the batch.
- **Its rows.** T8 is replaced: T8a is durability-modes' T1 (`durability-modes.md:736`: at most 4
  durability calls per index per checkpoint, at most 4 `F_FULLFSYNC` per device per batch), with
  the batch sizes `many_write.sh` produced recorded (SI-R14); T8b is M2 at 1,000 x 100 on the Mac
  and the gate VM, macOS at least 2x, Linux at least 0.98x; T8c is a lone commit on an idle host
  at least 0.98x today's. T10 takes SI-R7's split: write-time ENOSPC and EIO and per-file `fsync`
  errors (through an additive per-file result) fail one index; a device flush and the directory
  phase fail the batch.

**SI-R20 (D3, safety): a failed checkpoint is a failed barrier.** In durability-modes a
checkpoint record lets the log truncate (`durability-modes.md:153-154`). After a shared phase
fails, `failed_commit_head.out` shows what an index's reopen does: it takes the highest manifest,
which after a failed last directory sync is the failed checkpoint's, not durable. A later
checkpoint from that base re-references the failed generation's files, whose names (or, after a
failed file sync, bytes) then rest on a retried sync, which is what SI-R8 and DM-R2 rule out; its
record then truncates the log over them. Fix:
- a checkpoint record names only checkpoints whose every phase completed, and the log never
  truncates past an index's last completed one;
- an index whose checkpoint failed fences its checkpoints and is reopened at the generation its
  last checkpoint record names (`recover(to)`, which commits it again as the head), never at the
  highest manifest on disk;
- the replica follows DM-R2's rule for a failed barrier: it never retries the sync as success,
  and is forgetful until it re-learns.
T9 becomes: a crash at every point of a batch over 3 to 100 indexes leaves each index at its last
recorded checkpoint or the batch's, and the log holds everything above it; a standalone failed
`commit()` leaves the previous manifest or the failed one (SI-R8).

**SI-R21 (D3-C and the create contract, pre-existing, not introduced): nobody syncs the parent
directory.** `create()` makes the index's own files durable, but the index directory's entry in
its parent is never synced: `PosixIO` opens an existing directory (`posix.h:330-339`), and
strata, completion, and the tools have no parent sync (searched). On ext4 a power loss can drop
a new directory whose parent was not synced. With millions of index directories this is the
host's to batch (one parent sync covers many creates). Fix: `create()`'s comment and the host
guide state that the directory's entry is the host's; the batcher may later dedupe parent syncs
per parent. Not blocking.

**SI-R22 (Q2, measures SI-R11).** D3-C replayed on faultfs: `owner.lock`, `meta-1.kv`, and
`manifest-1` written as today's create writes them, nothing synced, then 20,000 seeded power
losses (`q2.out`):

| `open()` reports | Count | Example residue |
|---|---:|---|
| `index_not_created` | 14,971 | none; `meta-1.kv`; `owner.lock` |
| `corrupt_index: no owner lock` | 2,522 | `manifest-1` without `owner.lock` |
| `corrupt_index: manifest: magic` | 1,870 | `manifest-1` empty, or its 53 B zeroed |
| `corrupt_index: missing file: meta-1.kv` | 315 | `manifest-1` alone |
| `corrupt_index: file size differs` | 153 | `meta-1.kv` empty |
| `corrupt_index: metadata differs` | 95 | `meta-1.kv` torn |
| opens | 74 | all three whole |

4,955 of 20,000 (24.8%) end in `corrupt_index`, and `create()` then refuses every one of them
(`file_exists`): the directory needs removing by hand. Every one's residue holds only create's
own names. So the design's premise (`:136-138`, "which recovery already treats as not created")
is false today, and both of SI-R11's forms would be safe. Ruling: the first form (create writes
nothing; the first commit or checkpoint writes generation 1 in today's order). Under D2 it
leaves no residue at all, since the directory lock writes no file, and it is what
durability-modes' "create's 6 fold into the first checkpoint" needs (`durability-modes.md:162-163`):
a journal's creation record must find the directory uncreated, never corrupt. Without a journal,
`create()` keeps today's synced protocol.

### Rulings

| Step | Ruling | Before implementation | At landing |
|---|---|---|---|
| D1 (A) The idle core and the write state | APPROVE WITH CONDITIONS | SI-R15, SI-R16 in the design | SI-R13's ASan case; T1 to T3 as restated |
| D1 (C), Q1 Shared options | Yes, after D1 | SI-R12's change | A test of two indexes sharing one `options` under TSan |
| D2 (A) The owner lock on the directory handle | APPROVE WITH CONDITIONS | SI-R17, SI-R18, SI-R2's barrier in the design; the completion call (additive, Detent's suite) | T4, T5 and T7 restated, on Linux, macOS, and FreeBSD; full design review, since it changes the on-disk protocol |
| D2 (B), (C) | B rejected (as the design says); C a host policy | | |
| D3 (A) as written | REJECT | | |
| D3 (A) as durability-modes' checkpoint batcher | APPROVE WITH CONDITIONS | SI-R19, SI-R20; durability-modes' re-check of DM-R3 for the generation rule (its step 5); completion's step 2 barrier | T8a to T8c, T9, T10 as restated |
| D3 (B) A shared journal | Replaced by durability-modes' D1 and D3 | | |
| D3 (C), Q2 Deferred create | REJECT as written; APPROVE in SI-R11's first form, the default where a journal records the creation | SI-R22 | The create crash points rerun with the deferral, zero `corrupt_index` |

### Blocking items / Required changes

1. SI-R15: D1 lists main's members; T1's target is the measured size less 8 per standard
   library, at least 200 B on libc++.
2. SI-R16: `write_state` split into the frozen buffer part and the owner's scratch; read paths
   over frozen states; the release rule; T2's refresh row.
3. SI-R17: D2 inside format 13 before the freeze, with `files::pre_freeze` bumped; the migration,
   T5's old-binary pairs, and T6 removed; one-version create pairs in T5. Or, missing the
   freeze, a required index-level tag.
4. SI-R18: the filesystem rule in the completion call; no fallback; T7 rewritten.
5. SI-R19: the batch entry as (index, generation, snapshot); the batcher on the pool; T8a to T8c
   and T10.
6. SI-R20: checkpoint records only for completed checkpoints; reopen at the recorded generation;
   T9 rewritten.
7. SI-R22: Q2 answered in SI-R11's first form, and D3-C's text corrected.

### Conditions

1. **C1. Done criteria.** The ledger has none. The lead writes them before D1 starts: T1 to T3
   as restated; T4, T5, T7; T8a to T8c, T9, T10; ASan and UBSan on every touched test, TSan on
   the concurrency, background-merge, and Q1 tests; CI green on all jobs.
2. **C2. Sequencing.** D1 first, on main, with SI-R16's boundary; D2 with format 13's freeze
   candidate (it then goes through format 13's full review); D3 after durability-modes' re-check
   clears DM-R3 and its step 2 lands in completion.
3. **C3. For durability-modes' designer, through the lead:** SI-R19 and SI-R20 belong in its D1
   and "How small-indexes' D3 relates" (`durability-modes.md:667-677`), and SI-R22's rule in its
   T1.
4. **C4. For the format 13 designer, through the lead:** D2's place in the freeze (SI-R17); no
   field is needed if it lands inside.
5. **C5. SI-R21** goes into `create()`'s comment and Xapiand's host guide.

### Verdict and required changes, in one line each

**D1 and D2 APPROVE WITH CONDITIONS; D3 REJECT as written, APPROVE WITH CONDITIONS as
durability-modes' checkpoint batcher; Q1 yes; Q2 in SI-R11's first form.**

1. SI-R15: T1's 220 B is unreachable on main; restate it from main's members.
2. SI-R16: the write state is what a background flush freezes; split it once.
3. SI-R17: D2 inside format 13, no migration.
4. SI-R18: refuse network filesystems by type; no fallback.
5. SI-R19: the batcher checkpoints a named generation, on the pool.
6. SI-R20: a failed checkpoint is a failed barrier; reopen at the recorded generation.
7. SI-R22: deferred create corrupts 24.8% of power losses; write nothing until the first
   commit.

Not run: NFS, SMB, FUSE mounts; Linux and FreeBSD hosts; APFS power cuts; writeback-error
semantics on macOS and FreeBSD; TSan; benchmarks (T1, T2, T8).

## Re-check

- Date: 2026-10-19.
- Reviewed: `docs/design/small-indexes.md` and `docs/status/small-indexes.md` on perf/small-indexes
  at `058bf1c` (scope `git log 5bb3797..058bf1c`: `25deedc`, perf only, and `058bf1c`, the
  revision); main at `be64292` (unchanged since the completed review); phase4/format13-on-main at
  `a7516f9` (`format-13.md` unchanged since `f983012`; its ledger now lists the directory lock as a
  pre-freeze item; `files.hh:42` `pre_freeze = 0x8000'0004`); design/durability-modes at `1544d82`
  (what the revision read) and at its committed tip `cd34ba1` (2026-10-19 05:38, revision 1), both
  read with `git show`, never the worktree's uncommitted edits; the completion library and the
  threadpool library at strata's pins (`pool_executor.hh`, `task_group.hh:70-71, 223-228`).
- Checked against: the completed review's SI-R1 to SI-R22, rulings, blocking items 1 to 7, and
  conditions C1 to C5; the designer's two flagged items (the journaled-create residue rule; T1 at
  200 B); `calibration.md`.
- Checked by: Homebrew clang 23.1.2 and GCC 16.2.0 (libstdc++), x86_64 macOS 15, APFS, at
  `nice -n 19` under the light hold `qexp-e2-m9` that was live (`quiet check` exit 2). New probes,
  all in `.scratch/si-review/` of this worktree, outputs beside them:
  - `d1/d1_sizes_gcc.cc` (`d1_sizes_main_stdcxx`; the earlier `d1_sizes.cc` with libstdc++'s
    headers included before its access hack, built against main): libstdc++'s layout.
  - `residue/stale_incarnation.cc` (`stale_incarnation.out`, ASan and UBSan, on faultfs): the
    journaled-create rule against another incarnation's committed index.
  - `batcher/owner_waits_on_pool.cc` (`owner_waits_on_pool.out`, ASan and UBSan): owners on the
    node's pool waiting on a batch task posted to that pool through `pool_executor`.
  - `dm-1544d82.md`, `dm-cd34ba1.md`, `f13-a7516f9.md`: the committed texts compared.
  - Reused, not rerun: `fence/failed_commit_head.out`, `q2/q2.out`, `apfs/*.out`,
    `ws_sites.out`, `d1/d1_sizes.out`.

### Verdict

**D1: APPROVE WITH CONDITIONS (SI-R15 and SI-R16 closed; SI-R23 before implementation). D2:
APPROVE WITH CONDITIONS; nothing in this design blocks its format-13 step (SI-R27's three
conditions go with steps 1 and 2). D3: APPROVE WITH CONDITIONS as durability-modes' checkpoint
batcher, with two new blocking items before the batcher is built (SI-R24, the thread rule;
SI-R25, the journaled-create rule). D3-C standalone: APPROVE (step 3 may go with step 2). D3-C
journaled: REJECT as written (SI-R25). Q1 yes, after D1.**

What holds, and reproduces:

- **D1's arithmetic holds on both standard libraries.** libstdc++ moves the same 216 B
  (every member 8-aligned, same sizes), `sizeof(index)` 1,200 B against libc++'s 1,056 B, and
  `options` 240 B against 232 B (`d1_sizes_main_stdcxx`; `d1_sizes.out`). So A saves 200 B on
  both, Q1 adds 216 B on libc++ and 224 B on libstdc++, and A with Q1 saves 416 B and 424 B.
  The moved members are interleaved with `meta_` (main `index.hh:2443-2453`), and all are
  8-aligned, so the object shrinks by exactly the bytes moved less the two pointers.
- **Every main citation in D1 to D3 checked resolves** (`index.hh:236-239, 273-274, 405, 498-511,
  1895-1901, 1915, 1921-1922, 2255-2262, 2274, 2305-2307, 2311-2313, 2322, 2328-2330,
  2443-2453`; `budget.hh:67`; `files.hh:42` at `a7516f9`; `format-13.md:253, 1061-1064,
  1116-1120`).
- **D2 inside format 13 is coherent:** a pre-D2 binary fails at the lock before it reads anything
  (main `index.hh:358-367`), and the files bump makes a D2 binary refuse a pre-D2 index before it
  writes (SI-R27 states why that matters). The fallback (`small-indexes.md:205-213`) carries
  SI-R3's and SI-R4's rules back with it.
- **D3-C standalone is safe:** create writes nothing, then commits generation 1 in today's order,
  so a crash leaves only `meta-1` and `manifest-1.tmp` (inside `uncreated()`, `index.hh:2255-2262`),
  or a renamed `manifest-1` whose content was synced before the rename (`write_new`); `q2.out`'s
  corruption came from unsynced writes this form never makes.
- **D3 matches durability-modes** at `1544d82` and, in substance, at `cd34ba1`: D1 takes D3-A as
  its checkpoint batcher (`cd34ba1` `durability-modes.md:237-276`), checkpoints the newest refresh
  generation at or below `commit_durable` with that generation's files retained (`:248-261`,
  DM-R3), and "How small-indexes' D3 relates" is unchanged (`:1103-1113`); its T1 is unchanged
  (`:1206`). The differences are SI-R26's.

What blocks: SI-R24 and SI-R25, each before its own step (D3's batcher, step 6; D3-C's journaled
half). Neither blocks D1, D2, or D3-C standalone.

### Status

- **SI-R1: closed** (superseded by SI-R17; `small-indexes.md:167-179`).
- **SI-R2: closed** (`:192-195`): `fsync(dir)` then `F_FULLFSYNC(dir)` in the completion call.
- **SI-R3, SI-R4: closed** inside format 13 (`:175-179`); they return under the fallback, which
  says so (`:205-213`).
- **SI-R5: closed by rule** (`:184-189`, `197-203`), with SI-R27's condition on the lock call's
  other errors.
- **SI-R6: closed.** Its thread is corrected by SI-R24.
- **SI-R7: closed in the design** (`:270-276`, T10 at `:335`); open in code until completion's
  per-file sync call exists.
- **SI-R8: closed in the design** (`:279-284`, T9 at `:334`).
- **SI-R9: closed.** D3 is durability-modes' one batcher (`:240-243`).
- **SI-R10: closed for the design** (`:258-262`); owed by completion (durability-modes' step 2,
  "Now" at `cd34ba1` `:1267`).
- **SI-R11: closed for the standalone create** (`:304-311`); the journaled form is SI-R25.
- **SI-R12: closed in the design** (Q1, `:348-354`); open until Q1 is built with its TSan test.
- **SI-R13: closed in the design** (`:117-119`, T3 at `:139`); open until D1 lands with the ASan
  case.
- **SI-R14: partially closed.** T5, T7, T8a to T8c are rewritten (`:218-219`, `331-333`); the
  ledger still has no done criteria (`docs/status/small-indexes.md:13`), which is C1, the lead's.
- **SI-R15: closed** (`:61-76`, `90-94`, `137`, `141-145`). The libstdc++ figure is now measured:
  200 B (above); T1 can state 200 B on both.
- **SI-R16: closed** (`:79-89`, `105-131`; T2's refresh row at `:138`). One correction, mine:
  `ws_sites.out`'s "29 functions, 104 uses" counts the member declarations as a function
  (`load_segment:2399`, 10 uses); it is 28 functions and 94 uses, as the completed review said.
  `:130-131` copies the inflated figure. `for_each_buffer`'s "the owner's part" (`:126`) means the
  owner's current `buffer_state`, not `owner_state`; say so.
- **SI-R17: closed** (`:158-179`, T5 at `:218`, T6 removed at `:221`). Format 13's F20 does not yet
  carry it (`format-13.md` at `a7516f9`; only its ledger does): C4 stays open.
- **SI-R18: closed** (`:181-203`, T7 at `:219`), with SI-R27.
- **SI-R19: partially closed.** The entry as (index, generation, snapshot) and the rows are in
  (`:244-250`, `263-266`, T8a to T8c at `:331-333`). The thread rule (`:251-255`) deadlocks a
  standalone `commit()` on a shared pool: SI-R24.
- **SI-R20: closed in the design** (`:285-292`, T9 at `:334`); its third bullet follows
  durability-modes' superseded text (SI-R26).
- **SI-R21: closed in the design** (`:322-327`); C5 is the implementation.
- **SI-R22: closed for the standalone create** (`:299-311`, T11 at `:336`); the journaled half is
  SI-R25.

### Rulings on the designer's two flagged items

**1. The journaled-create residue rule (`small-indexes.md:312-321`): not safe as written; safe
with SI-R25's change.** What holds: the log keeps every write since the creation until a
checkpoint record of the index lands (SI-R20's first bullet), so clearing strata-named residue
and replaying loses nothing, and keeping every other `open()` on today's narrow `uncreated()` is
right. What fails is the branch "opens the index when a manifest exists":

- A manifest's presence does not mean "its first checkpoint completed". A first checkpoint that
  fails at its last directory sync leaves `manifest-1` renamed, and a same-boot reopen takes it
  (`failed_commit_head.out`: 200 of 200 for a commit's last directory sync; the other three
  failure points leave no new manifest). That case is benign, since every file such a manifest
  names was synced before the failing sync, but it contradicts D3's own rule two bullets earlier
  ("never at the highest manifest on disk", `:288-289`), and the design should say which holds.
- A manifest's presence does not mean "this index's manifest". A directory holding another
  incarnation's committed index (a host that deleted and re-created an index at one path, whose
  `rmdir` was not yet durable, which SI-R21 makes the host's to sync; or a host path collision)
  is opened as the new index. `stale_incarnation.out`: with the new incarnation's log positions
  starting at 1, all three of its writes replay as `skipped` (`index.hh:407-410`) and the index
  holds only the old incarnation's documents; with positions continuing past the old manifest's,
  all three are accepted on top of the old incarnation's five documents (last docid 8 against 3
  alone). Either way silently wrong, where today's synchronous `create()` refuses the directory
  (`file_exists`, `index.hh:2270`).

**2. T1 at 200 B: honest; load-bearing once SI-R23 is fixed; the single-pointer rejection
accepted.** The 200 B is reproduced on libc++ and now libstdc++ (above), so T1 can drop "as the
probe prints it" and state 200 B on both. It is 10.2% of the idle open index's 1,964 B heap, about
200 MB per million open indexes, and 21.2% (416 B, libc++) with Q1. The bare `sizeof` delta can
fail only by a layout slip; what makes T1 load-bearing is its last clause ("after a write, a
commit, and two refreshes with no write, the same"), and that clause passes in the test only
because the test calls `refresh()`, which an idle index in a host never receives (SI-R23). The
single-pointer variant would save 8 B more (0.4% of the idle heap) for a dependent load on every
write path; the rejection is a judgment, not a measurement, which is acceptable at that size, and
T2 guards the chosen form.

### Findings

**SI-R23 (D1, the owner part's release has no trigger on an idle index).** The owner part is
released "at a refresh with no `apply` since the previous one" (`small-indexes.md:114-116`), but
the engine has no refresh timer: `refresh()` runs only when the host calls it (main
`index.hh:445-450`), and a host with millions of idle indexes does not call it on them. So an
index that wrote, committed, and went idle keeps its `owner_state` and the capacity of
`terms_in_`, `undo_`, and `pending_docs_` (`clear()` keeps capacity; `clear_buffer` never touches
`terms_in_`, `index.hh:1895-1901`) indefinitely, and T1's variant passes only because it
refreshes each index twice. Fix: also release the owner part at `commit()` when no transaction is
open (a commit pays four syncs, so regrowing scratch afterwards is noise) and on `close`; T1's
variant writes and commits with no refresh. Before D1's implementation; not blocking anything
else.

**SI-R24 (D3, blocking before the batcher; corrects my SI-R19): a standalone `commit()` that
waits on a batch task on the pool deadlocks the pool.** "It runs on the pool's background class,
never on an owner thread ... `commit()` blocks only its caller, on its entry's completion"
(`:251-255`). The engine owns no threads; the host passes one pool per node
(`pool_executor.hh:1-4`), and `TaskGroup::wait()` blocks without running queued work
(`task_group.hh:223-228`). Owners that run on that pool (Xapiand offloads blocking calls to its
worker pool) and commit at once take every worker, and the batch task they wait for never runs.
`owner_waits_on_pool.out`: a pool of 2, one owner committing returns; two owners, 0 of 2 return
within 3 s. T8b's 1,000 indexes committing at once is exactly this load. Fix: whoever waits leads.
A `commit()` that finds no batch running runs the batch on its own thread with every entry already
waiting (the leader and follower form SI-R6 first proposed, as RocksDB's write group and
Elasticsearch's `AsyncIOProcessor` do); followers wait for it or lead the next. Only checkpoints
nobody waits on, durability-modes' background ones, are posted to the executor. SI-R19's concern
still holds for those: the leader is always a thread that was going to wait for the batch anyway,
so no owner stalls for another's work. Add a row to T8c or T3: N owners on a pool of fewer than N
workers, all committing, all return.

**SI-R25 (D3-C journaled, blocking before that half): decide by the log and the index's UUID,
never by the directory alone.** Evidence and cases are in ruling 1 above. Fix, for the replay of a
creation record:
- The creation record carries the UUID `create()` already takes (`index.hh:335-344`), which every
  manifest stores (`files.hh:63`; `a7516f9` `files.hh:95`); no format change.
- A checkpoint record of the index after the creation record: open at its generation (SI-R20).
- None: the log holds every write since the creation. If every manifest present carries the
  record's UUID, either clear the strata-named residue and replay (recommended: one rule, decided
  by the log, consistent with `:288-289`, and its cost is at most one checkpoint interval of
  replay) or open the highest such manifest, stating why that is safe (the only manifest that can
  exist without a record is one whose every named file was synced). A manifest carrying another
  UUID refuses with a named error, removing nothing; non-strata names stay untouched, as
  `clear_uncreated` leaves them (`index.hh:2272-2273`).
- State the invariant as "the log keeps an index's creation record, and every record after it,
  until a checkpoint record of that index lands", which is what truncation by records gives.
- T11's journaled half adds both arms of `stale_incarnation.cc` (expecting the refusal) and the
  failed-last-directory-sync first checkpoint, reopened in the same boot and after a power loss.

**SI-R26 (D3 against durability-modes at its committed tip; not blocking).** The revision read
`1544d82`; the branch's committed tip is `cd34ba1` (revision 1, 26 minutes later). The substance
agrees (above). Three things differ:
- Every `durability-modes.md` line D3 cites moved: `:145-165` is now `:237-276`, `:667-677` is
  `:1103-1113`, `:736` is `:1206`, `:134` is `:226`, `:162-163` is `:267-268`.
- SI-R20's third bullet (`:290-292`, "is forgetful until it re-learns") restates `1544d82`'s D5
  (`:241-242`). At `cd34ba1` a failed barrier restarts the process, and the same-boot rule
  decides: forgetful only after a torn turn past S (`:378-381`). A failed checkpoint is not a
  failed journal barrier either; it fences the index and reopens it at its recorded generation
  from a journal that is intact. Say that, and point at durability-modes' D5 for the journal's
  barrier instead of restating it.
- Durability-modes at `cd34ba1` still carries none of SI-R19's entry and thread, SI-R20's
  failed-checkpoint rule, or D3-C's residue rule (C3 open), and SI-R24 and SI-R25 now join that
  list.

**SI-R27 (D2, three conditions for steps 1 and 2; not blocking).**
- The completion call names only `EWOULDBLOCK` (`:190-191`). Any other error from the lock
  (`ENOLCK`, `EINVAL`, `EBADF` on a filesystem the classifier allowed) must refuse with that error,
  nothing written and no fallback; T7 injects one.
- The files bump (`:173-174`) is load-bearing, and the design should say why: without it a D2
  binary would open a pre-D2 pre-freeze index under the directory lock alone while a pre-D2 binary
  holds `owner.lock` on it, two writers. With it the D2 binary refuses the index as
  `unsupported_index` before writing. A refusal test opens a fixture at `0x8000'0004` and expects
  that, nothing written.
- `MNT_LOCAL` is set by the filesystem, and macFUSE sets it under its `local` mount option, so a
  network filesystem mounted through macFUSE that way passes the classifier and gets a lock that
  excludes only local processes. State it as a residual (an operator's choice, like D2-C), not
  a gap in the rule.

### New blocking items

1. SI-R24: the waiting caller leads its batch; only unwaited background checkpoints go to the
   executor; a row with more committing owners than pool workers. Before D3's batcher (step 6).
2. SI-R25: the creation record carries the UUID; replay decided by the log, a foreign UUID
   refused; T11's journaled half extended. Before D3-C's journaled half (with durability-modes'
   step 5).

### Anything blocking D2's format-13 step

Nothing in this design. Its preconditions are sequencing: step 1 (the completion call, with
SI-R27's first condition, Detent's suite green at its commit) landed; format 13's F20 amended to
carry the directory lock (C4, the format 13 designer's through the lead; at `a7516f9` only its
ledger records it); and format 13's full review of the step, which should check SI-R27's second
condition there. D3-C standalone (step 3) may go with it.

### Conditions

1. **C1 (carried, open).** The lead's done criteria (`docs/status/small-indexes.md:13`: none).
2. **C2 (carried).** Sequencing as the design's "First steps" (`:390-410`); D3's batcher also waits
   for SI-R24 and SI-R25 in the design.
3. **C3 (carried, open), widened.** Durability-modes takes SI-R19's entry, SI-R20, SI-R24's thread
   rule, and SI-R25's replay rule, and small-indexes re-pins its citations to `cd34ba1` or later
   (SI-R26).
4. **C4 (carried, open).** F20 carries the directory lock before step 2.
5. **C5 (carried).** SI-R21 in `create()`'s comment and Xapiand's host guide.
6. **C6 (new).** Before D1's implementation: SI-R23's release at `commit()` and `close`, T1 at
   200 B on both standard libraries and its variant without refreshes, and `:126`, `:130-131`
   corrected (SI-R16's status).
7. **C7 (new).** SI-R27's three items in steps 1 and 2.

Not run: any build or test of the branch (a docs-only revision; the completed review's ASan and
UBSan suite at `7c23560` was not rerun); TSan; NFS, SMB, and FUSE mounts (none here); macFUSE's
`local` option (read from its documentation, not mounted); Linux and FreeBSD hosts (shared
measurement hosts, not touched), so libstdc++ was measured with GCC 16 on macOS, whose LP64 layout
Linux shares but which no Linux build confirmed; power cuts; the journaled-create rule itself (no
replay code exists; the probe emulates it with `open()` and positioned `apply`); benchmarks (T1,
T2, T8).
