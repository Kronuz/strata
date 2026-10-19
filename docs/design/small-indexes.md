# Many small indexes

- Status: designing (revised for the architect's re-check, SI-R23 to SI-R27; ready for another
  re-check), 2026-10-19. Rulings (`docs/reviews/small-indexes-design.md`, "Completed review" and
  "Re-check"): D1 APPROVE WITH CONDITIONS (SI-R23 before implementation); D2 APPROVE WITH
  CONDITIONS (SI-R27 in steps 1 and 2); D3 APPROVE WITH CONDITIONS as durability-modes'
  checkpoint batcher, with SI-R24 and SI-R25 before the batcher is built; D3-C standalone
  APPROVE, D3-C journaled REJECT as written (SI-R25); Q1 yes, after D1; Q2 in SI-R11's first
  form. F3 parked. Measurements and the kept items F1 to F7 are in
  [docs/perf/small-indexes.md](../perf/small-indexes.md) on perf/small-indexes.
- Why: Xapiand's shape is millions of small indexes per cluster (one per user, group, or
  conversation), thousands to millions per host. What bounds a host is what each open index
  holds and what each small commit costs, not one index's throughput.
- Read at: main `be64292` (the merged inverter and its follow-ups; `include/strata/index.hh`,
  `budget.hh`); phase4/format13-on-main `a7516f9` (`format-13.md` F2, F20, F13-R51; `files.hh:42`;
  unchanged since `f983012`); design/durability-modes at its committed tip `cd34ba1` (revision 1;
  read with `git show cd34ba1:docs/design/durability-modes.md`, never the worktree's uncommitted
  edits; D1, D5, T1, "How small-indexes' D3 relates", "First steps"; its review's DM-R2, DM-R3);
  research/no-stalls `467b5b3` (section 6, item 1); the completion library at strata's pin
  `5251cf9` (`include/completion/posix.h`, for the lines the earlier revision cited) and at its
  main `6ce490d` (`cabfd78`'s `acquire_directory_owner()`, step 1, built); the threadpool library at
  strata's pin `628679a` (`task_group.hh`); the architect's probes in
  `strata-wt/review-small-indexes/.scratch/si-review/` (`d1_sizes.out`, `d1_sizes_main_stdcxx`,
  `ws_sites.out`, `apfs.out`, `dirsync.out`, `q2.out`, `failed_commit_head.out`,
  `batcher/owner_waits_on_pool.out`, `residue/stale_incarnation.out`). Line numbers below are
  main's unless a file says otherwise. No build, test, or measurement was run for this revision.

## Changes after the re-check

| Finding | Change | Where |
|---|---|---|
| SI-R23 | The owner part is also released at `commit()` and at `close()` (and at a checkpoint's completion on the owner), since nothing refreshes an idle index; T1's variant writes and commits with no `refresh()` | D1, T1, T3 |
| SI-R24 | Replaces SI-R19's thread rule: the waiting caller runs the batch on its own thread; only background checkpoints nobody waits on go to the executor; T8d (N owners on fewer than N workers) | D3, T8c, T8d |
| SI-R25 | The creation record carries the index UUID; replay decides from the log; a manifest with another UUID refuses and nothing is removed; no format change; T11's journaled half restated with the stale-incarnation probe's two cases | D3-C, T11 |
| SI-R26 | Every durability-modes citation re-pinned to `cd34ba1`; the failed-checkpoint bullet follows `cd34ba1`'s restart and same-boot rule instead of "forgetful until it re-learns" | header, D3, D3-C, Changes to other documents |
| SI-R27 | A lock error other than `EWOULDBLOCK` refuses with that error, no fallback (T7 injects one); why the `files::pre_freeze` bump is load-bearing (T5b); the macFUSE case is closed by name in the built classifier, the residual restated | D2, T5b, T7 |
| SI-R15, SI-R16 (C6) | libstdc++'s 200 B measured; the 29 functions and 104 uses corrected to 28 and 94; `for_each_buffer` visits the owner's current `buffer_state` | D1, T1 |
| Step 1 | Done: completion main `6ce490d` has `acquire_directory_owner()` | D2, First steps |

## Changes after the completed review

| Finding | Change | Where |
|---|---|---|
| SI-R15 | D1 lists main's members (216 B on libc++, `max_run_bytes_` included); T1 restated at 200 B on libc++, with what else closes the gap to the old 220 B | D1, T1 |
| SI-R16 | The write state is split once, into the buffer part a flush freezes and the owner's part; the read paths visit every buffer part; T2 gains a refresh row | D1, T2 |
| SI-R13 | Write paths fetch the buffer part again after any call that can flush; T3's ASan case | D1, T3 |
| SI-R17 | D2 lands inside format 13 before the freeze; the migration, the protocol field, `owner.lock`'s retirement, the old-binary pairs, and T6 are gone; the fallback if it misses the freeze is a required index-level tag | D2, T5 |
| SI-R18, SI-R2 | Network filesystems refused by type, no lock-file fallback; the macOS barrier on the directory descriptor; T7 rewritten | D2, T7 |
| SI-R19, SI-R6, SI-R7, SI-R10 | D3 is durability-modes' checkpoint batcher: no host API, a batch entry is a checkpoint of a named generation, the batcher runs on the pool's background class (corrected by SI-R24, above); T8a, T8b, T8c, T10 | D3 |
| SI-R20, SI-R8 | A failed checkpoint is a failed barrier; reopen at the recorded generation; T9 rewritten | D3 |
| SI-R21 | The index directory's entry in its parent is the host's | D3-C, steps |
| SI-R22, SI-R11 | `create()` writes nothing until the first commit or checkpoint; Q2 answered; T11 | D3-C, T11 |
| SI-R12 | Q1's change stated; Q1 answered | Q1 |
| SI-R14 | T8's batching recorded (T8b), the lone-commit row (T8c), T5 and T7 rewritten | D2, D3 |

## Where it stands (gate VM unless stated; 10,000 indexes open in one process)

| Per idle open index | strata, base | strata, F1 to F7 | Fastest baseline |
|---|---|---|---|
| Heap (1 to 100 documents) | 7.3 to 8.6 KB | 1,964 B on the merged inverter, 2,060 B before it (1.8 KB macOS, FreeBSD) | Tantivy reader: 13 KB |
| Segment pages | 4.7 to 81 KB | none | Tantivy: 25 to 84 KB |
| Mappings | 1 | 0 | Lucene 1, Tantivy 6 |
| Descriptors | 2 | 2 | 0 as readers, 1 as writers |
| Open p50, warm / cold | 90 to 111 µs / 1.0 to 3.4 ms | 49 to 58 µs / about 645 µs | Tantivy 123 µs / Lucene 1.5 to 2.4 ms |

Writing at once (M2 on a quiet host, on perf/inverter's kept items through E32): 8 indexes of
100,000 documents at 1.75× Tantivy's throughput, 1,000 of 100 at 5.72× Tantivy's, the latter
half system time: creating and committing one small index costs 10 fsyncs (Lucene 6, Tantivy 19).

The note's directions: measure first (done: M0 to M2); open lazily (done: F1, F5, F7) and close
idle (F3, parked below); pack small indexes together and a small-segment profile (format 13:
notes N5 to N8 in the perf note); share the machinery (F2 for merges; D3 for commits); share
schemas (not measured). What is left per open index is the `index` object itself (1,056 B on
main with libc++, 1,200 B with libstdc++: `d1_sizes.out`, `d1_sizes_main_stdcxx`), its file table and manifest (about
600 B), and two descriptors. D1 to D3 are the three steps that change safety or durability.

## D1. The idle core and the write state

An idle index holds 216 B of write state it never uses (main `index.hh:2443-2453`; sizes on
libc++ from `d1_sizes.out`, and the same 216 B on libstdc++, every member 8-aligned,
`d1_sizes_main_stdcxx`):

| Member | Bytes | Part |
|---|---:|---|
| `buffer_` (documents kept whole for a multi-thread flush) | 24 | buffer |
| `active_` (the inverting buffer) | 8 | buffer |
| `max_run_bytes_` (bounds a run of the buffer; tests lower it) | 8 | buffer |
| `buffer_docids_` (docid to buffer index) | 40 | buffer |
| `key_slots_` (the version map) | 40 | buffer |
| `buffer_bytes_` | 8 | buffer |
| `permit_` (the buffer's budget permit) | 16 | buffer |
| `pending_docs_` (the transaction's documents) | 24 | owner |
| `terms_in_` (inversion's scratch) | 24 | owner |
| `undo_` (the transaction's undo log) | 24 | owner |

Options:

- **A. The write state behind pointers, split once** (recommended). Two parts, each made at
  first use and held by a `std::unique_ptr`:
  - `buffer_state`, 144 B: the buffer and its version map, the piece a flush freezes. Phase 3
    already decides that a refresh freezes the buffer and flushes it on the pool while the owner
    continues into a fresh one, each frozen buffer charged on its own permit until it settles
    (`phase-3.md:270-273`, `:325-326`, D15, D17), and no-stalls ranks it first (`no-stalls.md`
    section 6, item 1). So the struct boundary is the one that background flush moves, and the
    code moves once.
  - `owner_state`, 72 B: the transaction's scratch, owner-only. When the background flush
    lands, the head of its list of frozen buffers lives here too, so it costs an idle index
    nothing.
  - Saving per idle index: 216 B less two pointers, **200 B on libc++ and on libstdc++**
    (arithmetic over the measured member sizes; the architect measured both: `sizeof(index)`
    1,056 B and 1,200 B, `options` 232 B and 240 B, `d1_sizes_main_stdcxx` with GCC 16.2.0 on
    macOS, whose LP64 layout Linux shares, which no Linux build has confirmed). 200 B is 10.2% of
    an idle open index's 1,964 B of heap (derived).
- B. A reader handle and a writer handle in the public API, as Lucene's `DirectoryReader` and
  `IndexWriter`: the same bytes saved, an API change for every host, and two owners of the
  index's state to keep consistent. Not worth it for 200 B.
- C. Also share the options: the index copies its `options`, 232 B on libc++ and 240 B on
  libstdc++ (`d1_sizes.out`, `d1_sizes_main_stdcxx`); a host that opens many indexes with one
  configuration could pass `std::shared_ptr<const options>` (an additive overload), saving 216 B
  more on libc++ and 224 B on libstdc++ (the options less the 16 B `shared_ptr`). Independent
  of A. Q1, answered yes, after D1.

Lifetimes:

- **The buffer part's release is the freeze.** A flush takes the owner's `buffer_state` (moves
  it out of the slot), writes it, and frees it when it settles; the next `apply` makes a fresh
  one. Until the background flush exists, the flush runs inline and the same rule holds inside
  `flush()`. A flush that fails with ENOSPC before its sync puts the part back in the owner's
  slot, keeping today's "a failure keeps the buffer" (`index.hh:1921-1922`, the contract at
  `:498-501`). What a busy index pays again per flush is one 144 B allocation and `buffer_`'s
  growth: today's flush already frees `active_` and both hash maps (`clear_buffer`,
  `index.hh:1895-1901`; `hash_slots::clear` swaps its vector away, `:236-239`; `key_slots_`
  cleared at `:1915`). T2's refresh row bounds it.
- **The owner part is released at `commit()`, at `close()`, and at a refresh with no `apply`
  since the previous one, each only with no frozen buffer in flight** (SI-R23). The engine has
  no refresh timer: `refresh()` runs only when the host calls it (`index.hh:445-450`), and a host
  with millions of idle indexes does not call it on them, so a release that waited for two
  refreshes would never come for an index that wrote, committed, and went idle. `clear()` keeps
  a vector's capacity, and `clear_buffer` never touches `terms_in_` or `undo_`
  (`index.hh:1895-1901`; `terms_in_` is cleared only inside the inverter, `:1870`), so the
  capacity of `terms_in_`, `undo_`, and `pending_docs_` would stay for the index's life.
  - `commit()` releases the owner part when it returns (after `reclaim`, `index.hh:508-509`):
    a transaction lives inside one `apply` (`:401-441`), so none is open between calls, and a
    commit pays four durability calls, so regrowing scratch on the next write is noise. Under
    durability-modes, where checkpoints replace `commit()`, the same release runs on the owner
    after a checkpoint's completion, where retention and reclamation already run.
  - `close()` (`index.hh:383-394`) releases both parts; the object may outlive its close in the
    host's container, and today it keeps every capacity.
  - The refresh rule stays for a host that refreshes without committing. The flag that says so
    lives in `owner_state`, not in the index.
  - An index with documents applied and not yet flushed holds them by definition; its buffer
    part goes at the commit's flush (the freeze, above).
- **Every write-path pointer is fetched again after any call that can flush** (SI-R13).
  `apply` calls `refresh()` before `evaluate()` (`index.hh:405`), so a `buffer_state&` held
  across that call dangles once a flush frees the part.

The interface, so the inverter's write path moves mechanically:

- `buffer_state& buf()` and `owner_state& own()` make their part on first use; `buf()` opens
  the part's permit from `options::budget` (free: `byte_budget::open`, `budget.hh:67`).
- The read paths that consult the buffer go through `for_each_buffer(f)`, which visits the
  owner's current `buffer_state` (not `owner_state`), if any, then every frozen one, newest
  first; until the background flush there are none. Those paths are the lookups (`locate_docid`, `key_slot`, `buffered_docid`,
  `buffered_key`, `buffered_version`; `index.hh:1570`, `1684`, `1837-1848`), the counts
  (`buffer_size`, `active_count`, `buffered_bytes`; `:1834-1836`, `885`), the accounting
  (`map_bytes`, `charge`; `:881`, `886`), and the digest (`:638`). `ws_sites.out` lists 28
  functions and 94 uses that move (its summary line counts the member declaration
  `load_segment:2399`, 10 uses, as a function; the completed review's figures hold).
- `tuple_buffer.hh` does not change. Sequencing: one mechanical commit on main's write path,
  the boundary above, before the background flush and independent of it.

| Row | Target | Procedure |
|---|---|---|
| T1 Idle heap | An idle open index's heap at least 200 B under the pre-split build on both libc++ and libstdc++ (216 B moved less two pointers; `d1_sizes_main_stdcxx` measured both), on Linux, macOS, and FreeBSD, at 1 and 100 documents; after a write and a `commit()` with no `refresh()` call anywhere in the run, the same (SI-R23); after a write and a `close()`, the same | The `sizeof` and `offsetof` probe (`d1_sizes.cc`'s method, and `d1_sizes_main_stdcxx` for libstdc++) on both standard libraries, its output kept beside the run; open-many (it holds the indexes in one reserved `std::vector<strata::index>`, `tools/strata_engine.cc:387-389`, so the object's size shows directly), 10,000 indexes, five rotated rounds against the pre-split build; the variants write and commit (or close) each index and never refresh: the run counts `refresh()` calls and fails on a nonzero count, so the row cannot pass on a test-driven refresh; a separate variant that refreshes twice with no write checks the refresh rule alone |
| T2 One index's speed | Phase 3's one-thread indexing row, M2 at 8 x 100k, and M2 at 1,000 x 100 with refreshes: each at least 0.98× the pre-split build's throughput by the interval's lower bound | `bench/ab.sh` five trials; `bench/many_write.sh`; the refresh row with `build-small` refreshing every second and, since 100 documents rarely span a second, also every 10 documents |
| T3 Correctness | The index, lifecycle (crash points), media, faultfs, reclaim, background-merge, and concurrency tests green under ASan, UBSan, and TSan; a test that writes, commits (both parts released, with no refresh), and writes again matches a never-idle run's digest; an ASan case where the flush inside one `apply` frees the buffer part (SI-R13) | CI on every platform |

What closes the gap to the previous revision's 220 B: that figure was this branch's member list
(232 B with `valid_terms_`, which main no longer has). On main, A alone saves 200 B on libc++ and
libstdc++; one pointer instead of two would save 208 B and cost a second dereference on every
write path, so it is not taken (a judgment at 0.4% of the idle heap, not a measurement; T2
guards the chosen form). Q1's shared options add 216 B on libc++ and 224 B on libstdc++, so A
with Q1 saves 416 B and 424 B per idle open index (21.2% of the heap on libc++; arithmetic over
`d1_sizes.out` and `d1_sizes_main_stdcxx`, not measured as heap).

## D2. One descriptor per open index

An open index holds its directory handle (every file operation is relative to it, so a rename or
a symlink swapped in above it cannot redirect them) and the owner lock (an exclusive `flock` on
`owner.lock`, completion `posix.h:348-358` at the pin `5251cf9`, held while the index is open). An exclusive,
non-blocking `flock` on the directory handle itself excludes a second open description and a
second, unrelated process on APFS, HFS+, and exFAT (`apfs.out`, `ramdisks.out`: `EWOULDBLOCK`,
the lock following the inode across a rename of the directory, a `SIGKILL`ed holder releasing
it), and on ext4, tmpfs (one process), and UFS by the designer's 2026-10-18 probe, which kept no
output; T5 is the evidence on Linux and FreeBSD.

- **A. The owner lock on the directory handle, inside format 13 before the freeze**
  (recommended; APPROVE WITH CONDITIONS). One descriptor, the directory's safety kept.
- B. Path-based I/O, keeping the lock file: one descriptor, but every operation resolves the
  path again, so a directory renamed or replaced while open sends writes to another directory
  whose own lock file the writer does not hold: two owners. Rejected.
- C. One lock per host root (one process owns every index under it): no lock per index. A host
  policy (Xapiand's node owns its root), and the path for a host on a network filesystem; not
  the engine's default.

**Inside format 13 (SI-R17).** Format 13 refuses format 12 (`format-13.md:253`, rule 3), every
pre-freeze build refuses another's files (`files::pre_freeze`, F2 and F20, `:1061-1064`), and
strata has shipped no release, so no binary a D2 binary must coexist with exists. So:

- Every format-13 index locks its directory from creation. No `owner.lock` is created or read,
  and the manifest gains no field.
- The change bumps `files::pre_freeze` (`0x8000'0004` at `a7516f9`, `files.hh:42`) to the next
  number and records it in `test/fixtures/pre_freeze.txt` (F2's check). **The bump is
  load-bearing** (SI-R27): the directory lock and `owner.lock` are different locks, so without
  it a D2 binary would open a pre-D2 pre-freeze index under the directory lock alone while a
  pre-D2 binary holds `owner.lock` on that index, and two writers would run. With it the D2
  binary refuses the index as `unsupported_index` before it writes; T5b tests it.
- Binaries from before D2 on one root are out of scope. A pre-D2 pre-freeze build opening a D2
  index finds no lock file and fails `corrupt_index("no owner lock")` (`index.hh:362-367`),
  writing nothing. Its `create()` in a D2 directory creates a stray `owner.lock` and then refuses
  a committed index (`file_exists`, `:2270`); in an empty one it is not excluded. Both are
  development builds only.

**The completion call** (step 1, done: completion main `6ce490d`, code in `cabfd78`, additive;
Detent keeps the file-lock call and links the new one, and its suite passed 167 of 167 on it, as
the lead reported; not rerun here). `IO::acquire_directory_owner()` (`io.h:127`, the base class
throws `logic_error`; `PosixIO` at `posix.h:422-437`, line numbers at `6ce490d`) locks the
directory handle, writes nothing, and:

1. checks the filesystem first, through `fstatfs`: on macOS and FreeBSD `MNT_LOCAL` set and the
   type not a FUSE one by name (`fuse`, `macfuse`, `osxfuse` prefixes, `posix.h:226-229`,
   `:240`); on Linux an `f_type` allow-list (ext2/3/4, XFS, Btrfs, tmpfs, F2FS, ZFS, bcachefs,
   overlayfs; ZFS and overlayfs unprobed, so CI decides them) with NFS, SMB, FUSE, 9p, and
   others named as refusals, and any unknown type refused (`:196-221`). Anything else refuses
   with `std::errc::not_supported` (one name on every platform, since Darwin's `EOPNOTSUPP` and
   `ENOTSUP` differ by compilation mode, SI-R5), the filesystem's type name in the message, and
   strata passes it through at `create()` and `open()` with nothing written;
2. takes `flock(LOCK_EX | LOCK_NB)` on the directory descriptor, retrying `EINTR`. `EWOULDBLOCK`
   is busy, as today's lock reports it; `EOPNOTSUPP` and `ENOTSUP` refuse as `not_supported`; **any
   other error (`ENOLCK`, `EINVAL`, `EBADF` on a filesystem the classifier allowed) throws that
   error and nothing else** (`:428-432`), and strata passes it through at `create()` and
   `open()` with nothing written and no fallback to any other lock (SI-R27); T7 injects one;
3. carries the macOS barrier on the directory descriptor (SI-R2): `sync_directory()` runs
   `fsync(dir)` then, on macOS, `F_FULLFSYNC` on the owner file if held and otherwise the
   directory descriptor, replacing the `logic_error` the earlier pin threw (`posix.h:525-534`;
   `file_sync` is `F_FULLFSYNC` on macOS, `:139-147`; the pin's `:443-453` was the old form).
   `F_FULLFSYNC` on an `O_RDONLY` directory descriptor costs what it costs on a file, 20.5 ms
   against 20.4 ms p50 (`apfs.out`), which a power cut has not tested.

There is no lock-file fallback (SI-R18). Over NFS, Linux emulates `flock` as a whole-file lock
that needs the file open for writing, or keeps it local under `local_lock`; over SMB it is a
mandatory byte-range lock (`flock(2)`, "NFS details", "CIFS details"). A directory cannot be
opened for writing, so on a network filesystem the lock either fails with an error no list
names or succeeds locally and excludes no other client, and the lock call cannot tell. A host
that must run there takes C. The refusal is by filesystem type at every open, so two hosts on
one export never choose different protocols.

**The residual** (SI-R27): `MNT_LOCAL` is set by the filesystem, and macFUSE sets it under its
`local` mount option, so a network filesystem mounted through macFUSE would pass a classifier
that trusted the flag. The built classifier does not trust it for FUSE: it refuses `fuse`,
`macfuse`, and `osxfuse` types by name even with `MNT_LOCAL` set (`posix.h:226-229`), and Linux
refuses `fuse` and every type off its allow-list. What remains is a network filesystem mounted
under a non-FUSE type that sets `MNT_LOCAL`: the kernel flag is the only evidence the classifier
has, and the lock then excludes only local processes. That is an operator's choice, like D2-C,
not a gap in the rule; the host guide says so.

**If D2 misses the freeze**, the protocol is a required (odd) index-level tag (F13-R51,
`format-13.md:1116-1120`), not a field or a files version. A format-13 binary without D2 refuses
an index carrying it as `unsupported_index`, writing nothing. A D2 binary opening an index
without the tag takes both locks, so a binary without D2 holding `owner.lock` makes it fail
busy; its first commit writes the tag; it keeps `owner.lock` until no retained manifest lacks
the tag, then removes it durably (unlink, then a directory sync). After that, a binary without
D2 fails `corrupt_index("no owner lock")`, which a host must read as a version refusal, not
damage (SI-R4). SI-R3's cross-version create pairs and the previous revision's old-binary pairs
in T5 and its T6 crash points come back with it. Recommended: before the freeze.

| Row | Target | Procedure |
|---|---|---|
| T4 Descriptors | One per idle open index on Linux, macOS, and FreeBSD | open-many's descriptor count |
| T5 Exclusion | Within one version, on one directory: open then open, create then create, create then open, each as two descriptions in one process and as two processes; the second fails busy and writes nothing; on Linux (ext4 and tmpfs), macOS (APFS), and FreeBSD (UFS) | index_test with a second description and a `posix_spawn`ed opener holding no inherited descriptor; CI on all three |
| T5b Old pre-freeze index | A fixture index at the pre-D2 `files::pre_freeze` (`0x8000'0004`), opened by a D2 build, refuses as `unsupported_index` and writes nothing, with another description holding the fixture's `owner.lock` the whole time (the pre-D2 binary's stand-in) so a wrongly accepted open would show as a second writer (SI-R27) | index_test with the fixture from `test/fixtures` and a held `owner.lock` |
| T7 Refusal | A `statfs` result the test injects (NFS, SMB, FUSE, 9p, an unknown type) refuses at `create()` and `open()` with `not_supported`, naming the type, and writes nothing; a lock call that fails with an error other than `EWOULDBLOCK` (`ENOLCK`, `EINVAL`, `EBADF`) on an allowed filesystem refuses with that error, writes nothing, and takes no other lock (SI-R27); Linux CI runs the lock tests on tmpfs as well as ext4 | the completion call's classifier tests in its own suite (`replace_statfs`); index_test through an IO whose filesystem answer and lock result the test sets |

T6 (the migration's crash points) is removed with the migration.

## D3. Commits across indexes: durability-modes' checkpoint batcher

What one sync covers today: `fsync` covers one file (on Linux, concurrent ones may share a
filesystem journal commit); a directory `fsync` covers that directory's entries, one per index
directory; on macOS, `fsync` stages a file on the drive and one `F_FULLFSYNC` per device flushes
everything staged before it, which the completion library's `files_sync` already does for a set
of files. Linux's `syncfs` covers a whole filesystem, other tenants' dirty data included, so its
cost has no bound. On APFS, sixteen index directories each with a create and a rename take
20.8 ms p50 with an `fsync` each and one `F_FULLFSYNC`, against 325 ms with today's barrier per
directory, 15.7× faster (`dirsync.out`; that one flush also commits the other directories'
namespaces is the assumption `files_sync` already makes for files, untested by a power cut).

The previous revision's A (a host two-phase API, `prepare_commit` and `commit_all`) is
withdrawn (REJECT, SI-R6); its B (a shared journal) is replaced by durability-modes' D1 and D3 on
Track C's turn log (`durability-modes.md:237-276`, `1103-1113`, at `cd34ba1`). What remains is A
in SI-R6's form, which durability-modes adopts as its checkpoint batcher
(`durability-modes.md:243-247`, `1105-1110`):

- **One mechanism.** A node-level commit group in the options, as `budget` and `background`
  are. `commit()` stays the one call; there is no host API. It is the engine's one sync batcher
  below `commit()`, used by a standalone `commit()` and by durability-modes' background
  checkpoints alike.
- **A batch entry is a checkpoint of a named generation** (SI-R19). The owner submits (index,
  generation, that generation's files and manifest snapshot), and `commit_files` works from the
  snapshot, not from `segs_` at phase time. A standalone `commit()` flushes as today and submits
  the current generation. A durability-modes checkpoint submits the newest refresh generation
  whose position is at or below `commit_durable`, with that generation's files, merge inputs
  included, retained until a later checkpoint covers them (DM-R3, `durability-modes.md:248-261`;
  the finding is `docs/reviews/durability-modes-design.md:224`, both at `cd34ba1`).
- **Whoever waits leads** (SI-R24, replacing SI-R19's thread rule, which deadlocks a shared
  pool). The engine owns no threads: the host passes one pool per node
  (`pool_executor.hh:1-4`), and `TaskGroup::wait()` blocks without running queued work
  (`task_group.hh:221-229`, threadpool `628679a`). Owners that run on that pool (Xapiand offloads
  blocking calls to its worker pool) and commit at once would take every worker, and a batch task
  posted to the same pool, which their `commit()` calls wait for, would never run. The probe
  (`owner_waits_on_pool.out`): a pool of 2 with one owner committing returns; with two owners, 0
  of 2 return within 3 s. So the batcher is a leader and followers, as RocksDB's write group and
  Elasticsearch's `AsyncIOProcessor` do:
  - A `commit()` that finds no batch running runs the batch on its own thread, with every entry
    already submitted. Followers wait for it or lead the next batch. The leader is always a
    thread that was going to wait for the batch anyway, so no owner stalls for work it was not
    waiting on, which was SI-R19's concern (four device flushes, about 80 ms on the development
    Mac, `dirsync.out`).
  - Only checkpoints nobody waits on, durability-modes' background ones, are posted to the
    executor as background work. A caller that comes to wait while such a batch is queued and
    has not started claims it and runs it on its own thread; the posted task finds it claimed
    and returns. A batch already running on a pool worker completes without needing another
    worker, so waiting for it cannot deadlock.
  - Retention and reclamation run on each owner after its completion (`index.hh:508-509`), not
    in the batch.
- **Phases, in today's order per index** (`commit_files`, `index.hh:2060-2149`):
  every entry's new files synced together; the directories; the
  manifests' temporaries; the renames, then the directories again. Each phase is one call over
  the batch: files through `files_sync` with per-file results (below), directories through the
  cross-IO directory barrier (SI-R10: `fsync` each directory, then one `F_FULLFSYNC` per device on
  macOS), which is durability-modes' step 2 in completion. Under D2 every index's IO holds a
  directory descriptor, so that barrier needs no owner file.
- **Batching takes only what waits.** The batcher starts a batch with every entry already
  submitted; one submitted during a batch waits for the next. An idle host's single commit runs
  as a batch of one on its own thread, with no task hop (T8c).
- **Several devices** take one device flush per device per phase.

Failure, by where it happens (SI-R7, SI-R20):

- A write-time ENOSPC or EIO, or a per-file `fsync` error, fails that entry's index alone; the
  others go on. Per-file errors need an additive completion call that syncs every file, keeps
  going past a failure, and returns each file's result: today's `files_sync` throws at the first
  failed `fsync` without naming the file and, on Linux, before the rest sync
  (`posix.h:173-187` at the pin).
- A device flush or a directory-phase failure fails every entry in the batch on that device: a
  device flush covers every index staged there.
- Every failed index fences and never retries a sync in-process (`index.hh:502-511` through
  `guarded`, `:988-996`).
- **A standalone `commit()` that failed leaves its index at the previous manifest or the failed
  one, as recovery finds it** (SI-R8). Only a failure of the last directory sync leaves the
  failed manifest renamed, after every file it names was synced: a same-boot reopen took it as
  the head in 200 of 200 runs, and a power loss then reverted it in 147 of 200
  (`failed_commit_head.out`); failures at the three earlier syncs reopened at the previous
  manifest, 200 of 200 each.
- **Under durability-modes, a failed checkpoint is a failed barrier** (SI-R20). A checkpoint
  record names only checkpoints whose every phase completed, and the log never truncates past an
  index's last completed one. An index whose checkpoint failed fences its checkpoints and is
  reopened at the generation its last checkpoint record names (`recover(to)`, which commits it
  again as the head, `index.hh:2311-2313`, `2328-2330`), never at the highest manifest on disk;
  so retention keeps that generation's manifest until a later record supersedes it. A failed
  checkpoint is not a failed journal barrier: the journal is intact, so the index fences and
  reopens from it and replays what the record does not cover, and the voter does not become
  forgetful for it. Forgetfulness belongs to the journal's own barrier, which durability-modes'
  D5 decides and this design does not restate (`durability-modes.md:367-381` at `cd34ba1`): a
  failed barrier restarts the process, and the same-boot rule decides, forgetful only after a
  torn turn past S.
- A crash before a batch's syncs complete leaves each index at its previous manifest, with
  orphans removed at its next open, as a lone commit does today; a crash after them and before
  the callers return leaves the commits durable and unacknowledged, as today.

### D3-C. Create writes nothing until the first commit (Q2, answered; SI-R22)

Measured on faultfs, today's create files written unsynced and 20,000 seeded power losses leave
4,955 directories (24.8%) that `open()` reports as `corrupt_index` and `create()` then refuses
(`q2.out`), so the previous revision's premise ("which recovery already treats as not
created") was false. The form the review approves:

- `create()` takes the directory lock and writes nothing (under D2, no file at all).
- **Standalone** (no journal), `create()` then commits generation 1 before it returns, in
  today's order (the metadata file, the directory, the manifest's temporary, the rename and the
  directory), so its crash residue stays inside `uncreated()` (`index.hh:2255-2262`). This is
  today's protocol less the lock file's sync and, when `clear_uncreated` removed nothing, its
  directory sync (`:2274`): 4 durability calls for create where durability-modes measured 6
  (`durability-modes.md:226` at `cd34ba1`), derived, not measured. One implementation: the standalone create
  is the deferred create followed by the first commit.
- **Where a journal records the creation** (strata::group under durability-modes), `create()`
  returns without committing, and the first checkpoint writes generation 1 with its segments,
  so creation folds into it (`durability-modes.md:267-268` at `cd34ba1`). Flushes before that
  checkpoint write segment files into a directory with no manifest, which `recover()` reads as
  `corrupt_index("no manifest")` (`index.hh:2305-2307`) and `uncreated()` does not accept.
  **The replay of a creation record decides from the log and the index's UUID, never from the
  directory alone** (SI-R25). A manifest's presence proves neither "its first checkpoint
  completed" (a first checkpoint that failed at its last directory sync leaves `manifest-1`
  renamed, and a same-boot reopen takes it: 200 of 200 runs, `failed_commit_head.out`) nor "this
  index's manifest" (a directory holding another incarnation's committed index, as when a host
  re-creates an index at one path and the `rmdir` was not yet durable, which SI-R21 leaves to the
  host; `stale_incarnation.out`: with the new incarnation's log positions from 1, all three of
  its writes replay as `skipped`, `index.hh:407-410`, and the index holds only the old
  incarnation's documents; with positions continuing past the old manifest's, all three are
  accepted on top of its five documents, last docid 8 against 3 alone). The rule:
  - The creation record carries the UUID `create()` already takes (`index.hh:335-344`), which
    every manifest stores (`files.hh:63`; at `a7516f9` `:95`). No manifest format change; the
    record's field is durability-modes' (C3).
  - A checkpoint record of the index after the creation record: open the index at its
    recorded generation (SI-R20).
  - None: the log holds every write since the creation. If every manifest present carries the
    record's UUID, **clear the strata-named residue and replay**: one rule, decided by the log,
    consistent with SI-R20's "never at the highest manifest on disk", at a cost of at most one
    checkpoint interval of replay. That includes a renamed `manifest-1` from a failed first
    checkpoint.
  - A manifest carrying another UUID **refuses with a named error and removes nothing**; names
    that are not strata's stay untouched, as `clear_uncreated` leaves them (`index.hh:2272`).
  - The invariant, as truncation by records gives it: the log keeps an index's creation record
    and every record after it until a checkpoint record of that index lands.
  Every other `open()` and `create()` keeps today's narrow `uncreated()`, so damage that removes
  a standalone index's manifests stays `corrupt_index`.
- **The index directory's entry in its parent is the host's** (SI-R21, pre-existing). `create()`
  makes the index's own files durable, but nothing syncs the parent: `PosixIO` opens an existing
  directory (completion `posix.h:330-339` at the pin), and strata, completion, and the tools have no parent
  sync. On ext4 a power loss can drop a new directory whose parent was not synced. A host
  creating many index directories batches one parent sync over them; `create()`'s comment says
  so. The batcher may later deduplicate parent syncs per parent.

| Row | Target | Procedure |
|---|---|---|
| T8a Syncs per checkpoint | durability-modes' T1 (`durability-modes.md:1206` at `cd34ba1`): at most 4 durability calls per index per checkpoint, at most 4 `F_FULLFSYNC`s per device per batch on macOS; the batch sizes recorded | `bench/durability/count_syncs` at 1, 10, and 1,000 indexes; `syncspy` on macOS, `strace -c -f` on Linux, `truss -c` on FreeBSD; the batcher's own count of entries per batch |
| T8b Many small commits | M2 at 1,000 x 100: macOS throughput at least 2× today's, Linux at least 0.98× today's, every engine beside it; the distribution of batch sizes the run produced recorded with it (one batch of 1,000 meets any target, so the row states what it measured) | `bench/many_write.sh` on the gate VM and the Mac (the lead's, on main); `build-small` reports the batcher's entries per batch |
| T8c A lone commit | On an idle host, one index committing one document at a time: at least 0.98× today's commit throughput, p50 and p99 reported | a one-index commit loop, 1,000 commits, five interleaved trials against the pre-batcher build, on the Mac and the gate VM |
| T8d Owners on a small pool | N owners running on a pool of fewer than N workers (2 workers with 2, 4, and 1,000 owners), all committing at once through the batcher, and each also with background checkpoints queued on the same pool: every commit returns, none within a timeout fails | `owner_waits_on_pool.cc`'s method as a test over the real batcher: today's 0 of 2 must become 2 of 2 and N of N (SI-R24); under ASan, UBSan, and TSan |
| T9 Contract | A crash at every point of a batch over 3 to 100 indexes leaves each index at its last recorded checkpoint or the batch's, and the log holds everything above it; a standalone failed `commit()` leaves the previous manifest or the failed one; a fenced index never retries a sync and then treats it as durable | lifecycle crash points and faultfs sync failures at each phase with the batcher over 3 to 100 indexes; the recorded-generation reopen through strata::group once durability-modes' step 5 exists |
| T10 Isolation | Write-time ENOSPC and EIO and per-file `fsync` errors fail that index's commit only, the others durable; a device-flush or directory-phase failure fails and fences every index of the batch on that device | faultfs per-index and per-phase faults |
| T11 Deferred create | Standalone: zero `corrupt_index` over SI-R22's 20,000 seeded power losses of a create and its first commit; every outcome opens or is cleared by `create()`. Journaled, replayed through the creation record: (a) the same 20,000 seeded power losses over a create, a flush, and the first checkpoint, zero `corrupt_index`, every outcome the index at its recorded generation or the replay over cleared residue, its digest equal to a never-crashed run's; (b) the stale-incarnation probe's two cases, another incarnation's committed index in the directory and the new incarnation's positions from 1 (arm 0) or continuing past the old manifest's (arm 1): each refuses with the named error, removes and writes nothing, and the old index's digest is unchanged, where the earlier open-if-a-manifest-exists rule silently held the wrong documents (`stale_incarnation.out`); (c) a first checkpoint that failed at its last directory sync (`manifest-1` renamed, no record), reopened in the same boot and after a power loss: the replay clears and replays, and the digest equals a never-failed run's | `q2_deferred_create.cc`'s method as a faultfs test in CI for the standalone half and (a); `stale_incarnation.cc` as the test for (b) and the failed-sync injection for (c); the journaled half once durability-modes' step 5 exists |

## Parked

F3, closing idle indexes behind handles, is a host policy above the engine (the lead's ruling,
2026-10-18), for Xapiand to decide. Measured: with Zipf accesses over 10,000 one-document
indexes and a cache of 1,000 open, a mean access is 2.1× slower than all-open on Linux (49.8
against 23.3 µs, a miss about 110 µs) and 23× slower on macOS (3,475 against 149 µs, a miss
about 14 ms), for an eighth of the descriptors and mappings and a sixth of the heap.

## Open questions

- Q1. D1-C: share an index's options across indexes (an additive overload), for 216 B more per
  open index on libc++? **Answered: yes, after D1** (the completed review). SI-R12's change
  comes with it: `recover()`'s one write into the options (`opts_.key_prefix = m_.key_prefix`,
  `index.hh:2322`) goes, and the index reads `m_.key_prefix`, which `install` already writes
  alone (`:824`). The merge policy's finders are `const` and the reorder settings plain data, so
  sharing them is race-free; a TSan test of two indexes sharing one `options` checks it. The
  options follow the lifetime rule `budget` and `background` already have.
- Q2. D3-C: may `create()` return before the new index is durable? **Answered: in SI-R11's first
  form** (create writes nothing; the first commit or checkpoint writes generation 1), deferred
  past `create()`'s return by default wherever a journal records the creation; standalone,
  `create()` commits generation 1 before it returns. No question is open.

## Findings' status

- SI-R1, SI-R3, SI-R4, SI-R5: superseded or closed by SI-R17 and SI-R18 (D2 inside format 13,
  refusal by type); SI-R3 and SI-R4 return only under D2's fallback.
- SI-R2: closed by the architect's probe; the barrier is in D2's completion call.
- SI-R6, SI-R9, SI-R10: closed in the review; SI-R10's barrier is owed by completion
  (durability-modes' step 2).
- **SI-R7: folded into D3** (failure attribution and T10); open until the per-file completion
  call exists.
- SI-R8: folded into D3's standalone contract and T9.
- **SI-R11: closed in the design by SI-R22's form** (D3-C), with the journaled residue rule
  above; open until T11 passes.
- **SI-R12: stated in Q1;** open until Q1 is built.
- **SI-R13: stated in D1** (fetch again after a call that can flush; T3's ASan case); open
  until D1 lands with that case.
- SI-R14: T5, T7, and T8 rewritten (T8a to T8c); the ledger's done criteria are the lead's (C1).
- SI-R15 to SI-R22: folded in as the tables at the top say. The re-check closed SI-R15 and
  SI-R16 (with its two corrections, made here, C6), and left SI-R14 partly open (C1, the lead's).
- **SI-R23: folded into D1** (release at `commit()`, `close()`, and a checkpoint's completion;
  T1's variant without refreshes); open until D1 lands.
- **SI-R24: folded into D3** (the waiting caller leads; T8d); blocks the batcher.
- **SI-R25: folded into D3-C** (the creation record's UUID, the log decides, a foreign UUID
  refuses; T11's journaled half); blocks D3-C's journaled half.
- **SI-R26: folded in** (citations re-pinned to `cd34ba1`; the failed-checkpoint bullet).
- **SI-R27: folded into D2** (lock errors, the bump's reason and T5b, the residual); open until
  steps 1 and 2 pass T5b and T7. Step 1 is built; the first condition's refusal is tested in
  strata at step 2.

## Changes to other documents

Each is the owning document's to make, through the lead (the review's C3 to C5).

| Document | Change |
|---|---|
| `format-13.md` F20 (`phase4/format13-on-main`) | The index directory carries the owner lock; no `owner.lock`; the files family's pre-freeze number bumps with D2; if D2 misses the freeze, the tag table gains its required tag (C4) |
| `durability-modes.md` D1 and "How small-indexes' D3 relates" (`:237-276`, `:1103-1113` at `cd34ba1`, which still carries none of these) | SI-R19's entry; SI-R24's thread rule (the waiting caller leads, only unwaited checkpoints go to the executor); SI-R20's failed-checkpoint rule; SI-R25's replay rule (the creation record carries the UUID, the log decides, a foreign UUID refuses) and its invariant; SI-R22's rule in its T1 (`:1206`) (C3, widened) |
| Xapiand's host guide | The parent directory's entry is the host's (SI-R21, C5); a node on a network filesystem takes D2-C |

## First steps

In order; each item's rows run in CI on Linux, macOS, and FreeBSD.

1. **D2, the completion call: done.** Completion main `6ce490d` (code `cabfd78`, docs `6ce490d`)
   adds `acquire_directory_owner()`: the directory `flock`, the known-local check
   (`errc::not_supported`, FUSE types refused by name), and the macOS barrier on the directory
   descriptor; lock errors other than `EWOULDBLOCK` throw as they are (SI-R27's first condition).
   Detent's suite passed 167 of 167 on it (the lead's report; not rerun here). Nothing is left
   of this step but the pin.
2. **D2 in format 13** (on `phase4/format13-on-main`, with the format 13 designer, before the
   freeze). Pin completion `6ce490d` or later; `create()` and `open()` take the directory lock
   through `acquire_directory_owner()`; no `owner.lock`; `files::pre_freeze` bumped and recorded;
   T4, T5, T5b, T7 (SI-R27's three conditions). It goes through format 13's full review, since it
   changes the on-disk protocol, and that review checks the bump's reason. Waits for C4 (F20
   carries the directory lock; at `a7516f9` only its ledger does). Starts now otherwise.
3. **D3-C standalone** (with step 2): `create()` writes nothing and then commits generation 1;
   `clear_uncreated` syncs only when it removed something; `create()`'s comment states SI-R21;
   T11's standalone half.
4. **D1 on main**: the `buffer_state` and `owner_state` split as one mechanical commit, the
   read paths through `for_each_buffer`, the owner part's release at `commit()`, `close()`, and
   the refresh rule (SI-R23), SI-R13's ASan case, the `sizeof` probe as a test output on both
   standard libraries; `build-small` gains its refresh settings for T2. Waits for the lead's
   done criteria (C1).
5. **Q1** after D1: the shared-options overload, SI-R12's change, the TSan test.
6. **D3, the batcher**: waits for durability-modes' re-check to clear DM-R3 (its step 5),
   completion's cross-IO directory barrier (durability-modes' step 2, `:1267` at `cd34ba1`) and
   the per-file sync call (additive, in completion with that step), and for SI-R24 and SI-R25 in
   the design (done here; durability-modes takes them through C3). The leader-and-follower
   batcher with T8d comes first; T11's journaled half, with SI-R25's replay rule, comes with
   strata::group (durability-modes' step 5, `:1270`).
