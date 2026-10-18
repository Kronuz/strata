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
