# Many small indexes

- Status: D1 to D3 ready for preliminary review; F3 parked. Measurements and the kept items
  F1 to F7 are in [docs/perf/small-indexes.md](../perf/small-indexes.md) on perf/small-indexes.
- Why: Xapiand's shape is millions of small indexes per cluster (one per user, group, or
  conversation), thousands to millions per host. What bounds a host is what each open index
  holds and what each small commit costs, not one index's throughput.

## Where it stands (gate VM unless stated; 10,000 indexes open in one process)

| Per idle open index | strata, base | strata, F1 to F7 | Fastest baseline |
|---|---|---|---|
| Heap (1 to 100 documents) | 7.3 to 8.6 KB | 1,964 B on the merged inverter, 2,060 B before it (1.8 KB macOS, FreeBSD) | Tantivy reader: 13 KB |
| Segment pages | 4.7 to 81 KB | none | Tantivy: 25 to 84 KB |
| Mappings | 1 | 0 | Lucene 1, Tantivy 6 |
| Descriptors | 2 | 2 | 0 as readers, 1 as writers |
| Open p50, warm / cold | 90 to 111 µs / 1.0 to 3.4 ms | 49 to 58 µs / about 645 µs | Tantivy 123 µs / Lucene 1.5 to 2.4 ms |

Writing at once (M2b, on perf/inverter's kept items through E32): 8 indexes of 100,000
documents at 1.92x Tantivy's throughput, 1,000 of 100 at 5.13x, the latter half system time:
creating and committing one small index costs 10 fsyncs (Lucene 6, Tantivy 19).

The note's directions: measure first (done: M0 to M2); open lazily (done: F1, F5, F7) and
close idle (F3, parked below); pack small indexes together and a small-segment profile
(format 13: notes N5 to N8 in the perf note); share the machinery (F2 for merges; D3 for
commits); share schemas (not measured). What is left per open index is the `index` object
itself (1,160 B with libstdc++, 1,048 B with libc++), its file table and manifest (about
600 B), and two descriptors. D1 to D3 are the three steps that change safety or durability.

## D1. The idle core and the write state

The object holds 232 B of write state an idle index never uses: the buffer and its
transaction scratch (`buffer_`, `active_`, `pending_docs_`, `terms_in_`, `valid_terms_`),
the version map (`buffer_docids_`, `key_slots_`), the undo log, the buffered bytes, and the
budget permit. Options:

- **A. A lazily made `write_state`** (recommended): one `std::unique_ptr<write_state>`, made
  at the first `apply`, released by the `commit` (or `flush`) that leaves the buffer empty, so
  an index that wrote once and went idle is small again. Internal; 224 B less per idle index,
  for one allocation on the first write and, after a release, the buffers' first growth
  again, which a commit's syncs dwarf.
- B. A reader handle and a writer handle in the public API, as Lucene's `DirectoryReader` and
  `IndexWriter`: the same bytes saved, an API change for every host, and two owners of the
  index's state to keep consistent. Not worth it for 224 B.
- C. Also share the options: the index copies its 240 B `options`; a host that opens many
  indexes with one configuration could pass `std::shared_ptr<const options>` (additive
  overload). Independent of A; worth 232 B more, so it is Q1.

The interface the inverter's write path needs, so its code moves mechanically: `write_state`
holds the members above as they are; the write path reaches them only through
`write_state& ws()`, which makes the state (and opens the budget permit) on first use; the
read paths that consult the buffer (key and docid lookups, the digest, `buffered_count`) go
through `const write_state* ws_if() const`, null when idle, meaning an empty buffer;
`tuple_buffer.hh` does not change. Sequencing: after perf/inverter
lands on main, as one mechanical commit on main's write path, because E31 and E32 rewrote
exactly these members and its later items would conflict with any earlier move.

| Row | Target | Procedure |
|---|---|---|
| T1 Idle heap | An idle open index's heap at least 220 B under the build before the split, on Linux, macOS, and FreeBSD, at 1 and 100 documents; after a write and a commit, the same | open-many, 10,000 indexes, five rotated rounds against the build before; a variant that writes and commits each index once first |
| T2 One index's speed | Phase 3's one-thread indexing row and M2 at 8 x 100k: at least 0.98x the pre-split build by the interval's lower bound | `bench/ab.sh` five trials; `bench/many_write.sh` |
| T3 Correctness | The index, lifecycle (crash points), media, faultfs, reclaim, background-merge, and concurrency tests green under ASan, UBSan, and TSan; a test that writes, commits, goes idle, and writes again matches a never-idle run's digest | CI on every platform |

## D2. One descriptor per open index

An open index holds its directory handle (every file operation is relative to it, so a
rename or a symlink swapped in above it cannot redirect them) and the owner lock (an
exclusive `flock` on `owner.lock`, held while the index is open). An exclusive, non-blocking
`flock` on the directory handle itself refuses a second open description of the directory on
APFS, ext4, tmpfs, and UFS, and a second process on APFS, ext4, and UFS (a two-descriptor and
two-process probe on each host, 2026-10-18), so the handle can carry the lock.

- **A. The owner lock on the directory handle** (recommended): one descriptor, the
  directory's safety kept. Needs an additive `IO::acquire_owner` variant in the completion
  library (Detent's: additive only) and a manifest field, so it goes through format 13.
- B. Path-based I/O, keeping the lock file: one descriptor, but every operation resolves the
  path again, so a directory renamed or replaced while open sends writes to another
  directory, whose own lock file the writer does not hold: two owners. It trades a property
  a multi-tenant host needs for one descriptor. Rejected.
- C. One lock per host root (one process owns every index under it): no lock per index, the
  directory handle as today. A host policy (Xapiand's node owns its root), compatible with A
  later; not the engine's default.

Two binaries on one index under A: an old one locks only `owner.lock`, a new one only the
directory, so neither excludes the other, both would act as owner, and the index would be
corrupted. A version check closes it, in three steps:

1. A new binary opening an index whose manifest predates the protocol takes both locks; an
   old binary holding `owner.lock` makes that open fail busy, so no upgrade runs beside it.
2. Its first commit writes the manifest with the protocol field (lock = directory). An old
   binary that then takes `owner.lock` reads a manifest version it rejects (readers reject
   unknown versions) and fails the open without writing.
3. While any older manifest remains (retained predecessors, pinned checkpoints), an old
   binary could open one by recovery's `--to`; so the new binary keeps `owner.lock` locked
   until retention has removed the last one, then removes `owner.lock` durably (unlink, then
   a directory sync) and closes it. From then on an old binary fails at its first step
   (no lock file), and the index costs one descriptor. An index that pins an older
   checkpoint keeps two until the pin is released.

A filesystem where `flock` on a directory fails (some network filesystems) keeps the lock
file and two descriptors, decided at open by the lock call's error.

| Row | Target | Procedure |
|---|---|---|
| T4 Descriptors | One per idle open index after step 3, on Linux, macOS, and FreeBSD; two before it | open-many's descriptor count |
| T5 Exclusion | Every pair fails clean (busy or an unknown version, nothing written): new and new; old then new; new then old before and after step 2; old opening a retained older manifest before step 3; old after step 3 (no lock file) | a test with the old protocol emulated by a lock-file-only opener and a pinned old-format fixture, on all three platforms in CI |
| T6 Crash points | Each crash point across steps 2 and 3 recovers with the right locks: the lock file kept while an older manifest exists | the lifecycle crash-point harness with the upgrade as a workload |
| T7 Fallback | A directory lock failing with `EOPNOTSUPP` or `EINVAL` falls back to the lock file and two descriptors | flaky IO in index_test |

## D3. Group commit across indexes

What one sync covers today: `fsync` covers one file (on Linux, concurrent ones may share a
filesystem journal commit); a directory `fsync` covers that directory's entries, one per
index directory; on macOS, `fsync` stages a file on the drive and one `F_FULLFSYNC` per
device flushes everything staged before it, which the completion library's `files_sync`
already does for a set of files; Linux's `syncfs` covers a whole filesystem, other tenants'
dirty data included, so its cost has no bound. So across indexes in separate directories,
one device flush can cover many files on macOS; on Linux the files and directories still
need their own syncs unless the commits share a journal.

The contract per index does not change: `commit()` returns after its manifest, and every
file it names, is durable; a commit that fails is reported to that index alone.

- **A. Batched syncs over today's files** (recommended first): an additive two-phase commit
  (`index::prepare_commit()` returns its files to sync and its rename; `commit_all(span)` runs
  today's phases for many indexes at once: the new files, their directories, the manifests'
  renames, their directories, each phase one `files_sync` over the batch), so macOS pays one
  drive flush per device per phase of a batch instead of one per file, and Linux issues the
  same syncs together. The order within each index is today's; no format change.
- B. A shared journal per volume (the note's direction 5 and C2's shared journal): one
  `fdatasync` covers every commit record in a batch, small segments' bytes ride in the
  record and become files at a checkpoint, and recovery replays each index's records. The
  real group commit on Linux, but a new on-disk structure (format 13 and the journal
  library, with Detent), so it needs its own design.
- C. Fewer syncs at create: write the owner lock, metadata, and first manifest unsynced and
  let the first commit sync them in order, about 5 of a new index's 10 syncs. It changes
  `create()`'s contract (an index created and never committed can vanish in a crash, which
  recovery already treats as not created), so it is Q2.

Failure cases under A (and B):

- A write or a sync that fails for one index before the shared phase fails that commit
  alone; the others go on.
- A shared sync that fails fails every commit in its batch, and those indexes fence: after a
  failed `fsync`, Linux may have dropped the dirty pages, so a retried sync can report success
  over lost data.
- A crash before a batch's syncs complete leaves each index at its previous manifest, with
  orphans removed at its next open, as a lone commit does today; a crash after them and
  before `commit_all` returns leaves the commits durable and unacknowledged, as today.
- Indexes on several devices take one sync per device per phase (`files_sync` groups them).
- Latency: a lone commit does not wait for a batch; batching happens only among commits
  already waiting, so an idle host's single index commits as fast as today.
- Under B also: a damaged journal record must fence only its index (each record carries its
  index and a checksum), and a damaged journal must not hide indexes whose state is in
  their own files (checkpoints bound what lives only in the journal).

| Row | Target | Procedure |
|---|---|---|
| T8 Many small commits | M2 at 1,000 x 100: macOS throughput at least 2x today's with A, Linux no slower (at least 0.98x), every engine beside it | `bench/many_write.sh` on the gate VM and the Mac; syncs counted by `strace -c` and `dtruss` |
| T9 Contract | A commit that returned survives a crash at every later point; one that failed leaves its index at its previous manifest; a failed shared sync fails and fences every index in its batch | lifecycle crash points and faultfs sync failures with `commit_all` over 3 to 100 indexes |
| T10 Isolation | One index's ENOSPC or EIO during a batch fails that index's commit only | faultfs per-index faults |

## Parked

F3, closing idle indexes behind handles, is a host policy above the engine (the lead's
ruling, 2026-10-18), for Xapiand to decide. Measured: with Zipf accesses over 10,000
one-document indexes and a cache of 1,000 open, a mean access costs 2.1x the all-open one on
Linux (49.8 against 23.3 µs, a miss about 110 µs) and 23x on macOS (3,475 against 149 µs, a
miss about 14 ms), for an eighth of the descriptors and mappings and a sixth of the heap.

## Open questions

- Q1. D1-C: share an index's options across indexes (an additive overload), for 232 B more
  per open index? Recommendation: yes, after D1-A, with the same lifetime rule `budget` and
  `background` already have.
- Q2. D3-C: may `create()` return before the new index is durable, its first commit making
  it so? Recommendation: yes for hosts that create and fill at once, as an option, not the
  default.
