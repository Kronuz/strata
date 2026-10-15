# Phase 3 design: index lifecycle

This document specifies what Phase 3 builds on the Phase 2 segment: an index of many
segments with one writer, keys, versions, updates and deletes, near-real-time
snapshots, durable commits through manifests, tiered merges, metadata, atomic
transactions, a node-wide indexing memory budget, the deterministic core interface
(`apply`, `checkpoint`, `install`), a logical-content digest, and `faultfs`. Phase 3R
puts consensus groups on top of this core; nothing here talks to a network. The
architect's review (`docs/reviews/phase-3-design.md`) settles D1–D20 and its re-check
R1–R9; each section names the rulings it implements. PLAN's Goals and "Ratchets" set
the gate's performance rows.

## Conventions

- An **index** is one shard: one directory, one writer, one owner core.
- **Docids** are the shard's ids: positive, at most one live copy each. The engine
  assigns `lastdocid + 1`; the host may also address a docid explicitly (D7).
- **Ordinal order is docid order in every flushed segment** (D15): flushes sort by
  docid. A merge writes docid order, or reorders its output for compression and
  pruning (Track P1, R2); the segment's docids kind records which.
- Files are written once and never modified. Integers in new files are LEB128
  (`varint::leb128`) unless a table says otherwise; fixed-width fields are
  little-endian; every file ends in a CRC-32C of its preceding bytes.
- All index I/O goes through `kronuz::journal::IO` (`cluster/completion/io.h`, used
  unchanged at a pinned commit, D19). strata registers with the Detent work as a
  consumer of `completion/io.h`, so changes to it are coordinated (D20).

## Directory

```
index/
  seg-<id>.sx            immutable segments (format 12)
  seg-<id>.<gen>.live    live-docs generations
  meta-<gen>.kv          metadata generations
  manifest-<N>           commit points; manifest-<N>.tmp while being written
  lock                   the owner lock (IO::acquire_owner)
```

Every counter (manifest generation, segment id, live-docs and metadata generations)
only increases, and at open it starts above the highest value on disk (D3), so no name
is ever reused.

## Files

### Manifest (D6)

`manifest-N`: magic `STRMAN01`, then LEB128 fields, then a CRC-32C:

- format version; index UUID (16 bytes); N; revision; `lastdocid`; next segment id;
  next live-docs and metadata generations;
- applied position: the pair (log index, transactions applied from that entry); (0, 0)
  standalone (I11, R3);
- schema version (D7); the host's key-term prefix (D16);
- metadata generation, size, and CRC;
- document count and total length including deleted documents (scoring, D13); live
  count and live total length (reported to the host);
- pinned checkpoints: their manifest generations (D4);
- per segment: id, file size, footer CRC, document count, live count, live total
  length, live-docs generation (0 for none), its size and CRC.

### Live docs (D6)

`seg-<id>.<gen>.live`: magic `STRLIV01`, format version, segment id, generation,
document count, live count, live total length, the encoding (0: a bitset over
ordinals; 1: sorted dead ordinals as LEB128 deltas, when smaller), the bytes, and a
CRC. A reader rejects an unknown version and checks the live count against the bits.

### Segment format 12 (D6, D16, R1, R2)

Phase 2 ended at format 11. Format 12 carries every Phase 3 format decision and freezes
before merge code; format 11 segments are rejected with a version error, and a fixture
pins format 12:

- `pk_filter`: a `fusefilter::filter8` (pinned at `b4f4ce1`) over the 64-bit hashes of
  the segment's key terms; the hash is xxHash64 of the term bytes with seed 0 (I12).
- `versions`: one version per ordinal, bit-packed from the segment's minimum.
- `docids`, in one of two kinds, recorded in the section:
  - monotone (D15): bit-packed deltas with a sparse index every 128 ordinals (I6);
    docid → ordinal is a binary search over the index, then a block decode;
  - permuted (a merge's reordered output, P1): ordinal → docid bit-packed, and the
    docids sorted with their ordinals for docid → ordinal.
  At open the section is validated: strictly increasing for monotone, a permutation of
  distinct docids for permuted.
- The stored section records its largest frame, which bounds a frame's allocation
  (review C9).
- The term dictionary's redesign (Track P12), the postings change the disk row needs,
  and the verification region size (Track P15), which the table of contents records.

The 16-byte header id holds the 64-bit segment id and the index UUID's first 8 bytes,
checked against the manifest entry at open (I13).

### Metadata (I8)

`meta-<gen>.kv`: a plain sorted file of (key length, key, value length, value) in
LEB128 lengths, then a CRC. Values of any size (Xapiand's schema) fit.

## Commit and recovery

### Commit (D1, D2)

1. Write each new data file (segments, live-docs, metadata) under its final name
   through `IO::create_exclusive` and `File::sync` it. A segment flushed earlier, at a
   refresh or by the budget, keeps its `File` handle open until the commit that covers
   it syncs it through that handle (R9).
2. Sync the directory.
3. Write `manifest-(N+1).tmp` and sync it.
4. `IO::replace` it to `manifest-(N+1)`.
5. Sync the directory. The commit is acknowledged here; retention runs only after this
   barrier succeeds.

Failures:

- A failed `sync` of any file in a commit fences the writer: nothing is retried and
  nothing acknowledged; reopening selects the last durable manifest. A failed or
  uncertain rename or directory sync fences the same way.
- ENOSPC from a write before any sync is recoverable: the flush keeps its buffer, the
  partial file is removed, and the write is retried once space returns (T5).
- A commit costs two directory barriers; the gate report states commit latency beside
  the VM's fsync latency.
- Revision increments on every commit that applies at least one transaction. A commit
  that only installs merges or changes pins keeps it, since Xapiand's WAL replay keys
  on revision (I9).

### Recovery (D3)

- Recovery selects the highest final-named `manifest-N`; `.tmp` files are never
  candidates. If its CRC fails, or a file it names is missing or differs in size or
  CRC, the open fails closed and changes nothing on disk. Under the commit protocol,
  power loss alone never produces that state.
- Opening an older manifest (`recover --to N`, or group recovery at a checkpoint's
  manifest) commits its content as a new generation through the commit protocol, so
  power loss leaves the old head or the chosen content. Deleting the higher manifests
  instead is not atomic: unsynced removals survive a crash independently, which can
  leave a manifest between the two as the newest (found by T1's `recover --to` test).
- A directory whose only strata files are `meta-1.kv` and `manifest-1.tmp` is what an
  interrupted `create` leaves. `open` reports it as `index_not_created`, distinct from
  `corrupt_index`, and `create` runs again over it, removing those files however it took
  the owner lock. Any other strata file makes the directory an index: segments without a
  manifest stay corrupt, and `create` refuses it, with or without its lock. The residual:
  an index that never changed its metadata and lost every manifest, segment, and
  live-docs file looks uncreated.

### Retention (D4)

- The retained set is the current manifest, the pinned ones, and two predecessors
  (I14). A manifest leaves the set when its file is deleted, before the data files only
  it referenced.
- Pins are durable: standalone, the manifest records them; under groups, the journal
  passes the pinned set at open and it overrides the manifest's.
- Orphans (files no retained manifest names, in strata's name patterns) are deleted
  only after a successful open, under the owner lock.
- Snapshots are per-core handles published by message (ARCHITECTURE, Read path); a
  segment's file is mapped from the moment it joins a snapshot, so unlinking it is safe
  and retention never consults snapshot counts. Open mappings decide only when space
  returns, which storage admission counts (T5).
- Mappings have one owner (Goal 5, R7): the owner core counts, per segment, the cores
  whose snapshots hold it. A core that drops its last snapshot holding a segment sends
  the owner a release message; at zero the owner unmaps it. No refcount is shared
  across cores and no thread collects.
- Retention diffs in-memory file sets: the index holds every file it owns and each
  retained manifest's files (an older manifest is read once), so a commit scans no
  directory; open scans once, for orphans. A round in which a kept manifest cannot be
  read, for any reason, removes nothing, and `retention_blocked()` names it; a later
  round reads it again. A manifest damaged for good blocks retention until its pin is
  released, so the host reports it and repairs or releases the checkpoint.
- Retention and reclamation run after the commit point: a failed unlink, truncate, or
  directory sync there leaves the file queued (or the removed manifests owed their
  sync) and fails neither the commit nor a merge's admission.
- Obsolete data files are queued and freed in steps: truncated from the end
  `reclaim_step` bytes at a time (16 MiB), then unlinked, about `reclaim_slice` bytes
  (64 MiB) per commit, the rest by `reclaim()`, and the whole queue when storage
  admission is short. A file is truncated only after its last mapping is released.
  Freeing costs 7–130 ms per GiB across the four platforms (`docs/perf/reclamation.md`).
- Retention and orphan deletion run in bounded slices on the owner core, and the gate
  reports the reclamation row (CPU per GB, longest slice).

### Install (D5)

`install(checkpoint)` copies the checkpoint's files under fresh local names from the
target's counters, verifies each whole, writes their manifest through the commit
protocol at a generation above every existing one, and leaves the old files as orphans.
It discards the buffer, pending live-docs, and the version map, and adopts the
checkpoint's `lastdocid`, schema version, applied position, and index UUID (which
belongs to the shard). A crash at any point opens the old index or the installed one.
Storage admission reserves the copy and the commit's metadata file and manifest. Should
the disk still refuse after admission, two outcomes follow: an ENOSPC while copying
leaves the target's old content, since nothing in memory changes before the copy is
written; an ENOSPC in the commit leaves the installed content in memory, uncommitted, and
the target's next commit makes it durable.

## Writer

```
apply(transaction, position)
   │
   ▼ stage 1 (owner core, sequential): overlay verdicts, docids, keys, versions
   │
   ▼ stage 2: inversion into K buffers (D18), contiguous docid runs per buffer
   │ budget, refresh, or commit, at a transaction boundary
   ▼
 flush: sort by docid, drop buffer-deleted documents → seg-<id>.sx
```

### Operations (D7)

A transaction is a list of operations applied atomically:

- add with an engine-assigned docid (`lastdocid + 1`);
- replace and delete by key;
- replace and delete by docid: a replace at an absent docid creates it there, and
  `lastdocid` becomes the larger of the two;
- set and delete metadata entries; set the schema version.

A key and an explicit docid that name different live documents reject the operation.
The invariant: at most one live copy per docid and per key. Ids that depend on
`lastdocid` (Xapiand's numeric auto ids) are resolved by the leader before proposing, so
they reach apply as explicit docids.

### Keys and versions (D8, D16)

- The key is the host's own unique term: the index records the host's key prefix
  (Xapiand's `Q`) in the manifest, and a keyed document carries exactly one term with
  it, which stage 1 checks. strata indexes nothing extra.
- Lookups consider live copies only: the version map, then segments newest first, each
  checking its key range, `pk_filter`, then the term dictionary, whose single-posting
  form inlines the ordinal (D15 keeps df 1 per segment). A key whose copies are all
  deleted is absent: re-adding it takes a new docid and version 1, as Xapiand does.
- Expected version 0 requires absence; any other value must equal the current version;
  a mismatch rejects with the current version; no expectation is unconditional.
- The version map is a cache of writer state, cleared at flush (I5); clearing it never
  changes a verdict. It also locates unflushed documents for get-by-id.

### Transactions (D9, D10)

- Stage 1 evaluates a whole transaction on an overlay (key → docid, version, deleted;
  docid → live) and passes it to stage 2 only when every check passes. A rejected
  transaction leaves no trace in the buffers, the version map, pending live-docs, or
  `lastdocid`, and consumes no docids.
- Validation that can reject runs in prepare or stage 1 (a term repeated within a
  document, term length, position and length limits, stored-record size), so stage 2
  never fails for a deterministic reason. A host cancel is a transaction the host never
  submits.
- Refresh, budget flushes, commits, and checkpoints cut at transaction boundaries. A
  transaction larger than the buffer may flush part of itself; that segment is
  published only with the transaction's end.
- A transaction differs from a group-commit log entry: a version conflict rejects its
  transaction, not the entry. The applied position is the pair (log index,
  transactions applied from that entry), so a commit between two transactions of one
  entry resumes at the next transaction after recovery (R3). Apply skips every
  transaction at or below `applied_position()`.
- Apply's only verdicts are deterministic: version and schema-version mismatches, and
  document validation. ENOSPC, EIO, and memory exhaustion never reject an accepted
  transaction: the core stops applying until the condition clears, or fences (D2).

### Deletes and flush (D11, D15)

- Deleting a flushed document sets its bit in its segment's pending live-docs
  generation, written at commit (Minor 4). Deleting a buffered document drops it: a
  flush writes only live documents, sorted by docid, so every segment holds at most one
  copy of each docid and key and is born without deletes.
- A buffer frozen for a background flush, and a merge, each record the live-docs state
  they read; deletes after that point are applied to the new segment's first pending
  generation by docid when it is installed, on the owner core between transactions.
- Frozen buffers and merge inputs are freed by their owner when the flush or merge
  settles (R7).

## Statistics and ranking (D13, D14)

- Scoring statistics come from one population, as in Lucene: N and total length
  include deleted documents until a merge drops them, like the document frequencies,
  so df ≤ N and weights stay non-negative. Live counts and live total length are what
  the host reports (Xapiand's `doccount`, `av_length`, and `_doc_del`).
- Multi-segment search weights each segment's scorers with the snapshot's statistics
  and collects through one collector that orders hits by (score descending, docid
  ascending). Scorers skip only blocks and windows whose bound is strictly below the
  k-th score; within a docid-ordered segment, ordinal order is docid order, so equal
  bounds may skip past the point where every docid ahead exceeds the k-th hit's. In a
  permuted segment, equal bounds never skip (R2).
- Counting shortcuts that read document frequencies apply only to segments without
  deletes (C3); live bits AND into each window's match bits otherwise (I3).

## Merges (D11)

- A tiered policy with the pinned Lucene 10.4.0 `TieredMergePolicy` defaults, read from
  its source (I1): segments per tier, merge width, floor, maximum merged size, and the
  deleted-percentage bound past which a maximum-size segment is rewritten alone.
- A segment whose documents are all deleted leaves the manifest at the next commit,
  without a merge.
- With `auto_merge`, the policy runs after every flush: `refresh()`'s and `commit()`'s.
- A merge reads its sources in docid order, drops deleted documents, and writes one
  segment with the same docids, in docid order or reordered (P1); deletes that arrive during the merge carry over at
  install. Stored frames copy raw when none of their documents is deleted and they stay
  contiguous (I7).
- Merges reserve an estimate of their output before starting and grow it to the
  output's real size before writing it, which can exceed the estimate since postings can
  grow when merged (T5); they run in the background,
  rate-limited, and publish through the next refresh and commit.
- As built, a host-owned `merge_scheduler` per node (`index_options::scheduler`) runs
  every index's merges on its worker threads under one bytes-per-second token bucket;
  without one, merges run inline. A merge shares its sources and copies their live docs;
  the owner core installs its output at the next `refresh()`, `commit()`, or
  `wait_merges()`, carrying deletes made since the copy through its ordinal map, and
  discards it when a source left meanwhile (including by `install`). Merge-debt
  backpressure is separate (RocksDB survey K4(b)).

## Memory budget (D17, D20)

- One budget covers the node. Every category is charged as it is allocated: buffers,
  frozen buffers until their flush settles, the flush's sort and spill buffers, the
  version map, pending live-docs, merge buffers, and filter construction.
- Flushing starts below the limit by the in-flight flush size; at the limit, stage 1
  suspends until a flush settles. The version map forces a flush past 25% of the
  index's share of the budget (R9).
- The budget has `ledger`'s shape (a capacity, move-only permits, explicit settlement,
  a pressure signal) so the swap is mechanical. `journal::Admission` is single-executor
  while the budget spans owner cores, and Detent's `StorageLedger` is not a library, so
  neither fits as is. Strata does not extract `ledger`; PLAN changes accordingly, and
  step 5's timing and API are coordinated with the Detent work before
  `memory_budget` is written. The switch takes `ledger`'s capacity and permit core
  only, never the retirement runtime with its cleanup thread (Goal 5, R7).
- As built, the class is `byte_budget` (`include/strata/budget.hh`): one instance is the
  node's indexing memory, another its storage. Storage admission (T5) holds each index's
  file bytes, reserves a transaction's estimated bytes at admission (rejecting it with
  `verdict::no_space` when they do not fit), and a merge's output before it writes it.
- The flush's memory is charged on the indexing budget: each run's `flush_charge` wraps one
  permit on the run's thread, and the writer reports its term table, arena chunks, posting
  buffers, per-document columns, scratch, and output at their allocations (never per
  document), drawn 256 KiB at a time and past the capacity if need be. The permit settles
  when the run's segment is published. Segment bytes are unchanged; a budgeted index may
  flush sooner. Merge buffers are not charged yet.

## Writer parallelism (D18)

- Target: the N-thread row at least equals the faster of Lucene's and Tantivy's
  multi-threaded indexing on the 5M corpus, measured in the gate session, and
  single-thread indexing reaches ≥ 1.0× the faster engine (T4).
- The decision rests on the Phase 2 writer's stage-2-only rate measured on a quiet
  host. Below twice the target, inversion runs in K buffers: stage 1 stays sequential on
  the owner core and dispatches transactions to K inversion buffers on writer threads,
  each taking contiguous docid runs and flushing its own segments; a refresh cuts every
  buffer at one transaction boundary (I10).
- Merges leave the writer's thread for the node's merge scheduler (D11, as built), so
  the writer threads spend no time on them; deletes stay on the owner core.

## Performance plan (R5)

Phase 3's gate holds every ratchet's Phase 3 row (PLAN, "Ratchets"). The gaps at the
Phase 2 gate, the work that closes each, and how it is shown:

| Row | At Phase 2 | Work | Shown by |
|---|---|---|---|
| Query classes | 1.0–1.45× floor-subtracted; 34 of 40 rows above 1.0× by the interval | PLAN's committed Track P items (P2, P3, P6, P7, P8, P9, P5, nested WAND), each in `docs/perf/` | The gap table re-measured with Lucene with reordering before the first item, then per item |
| Per-segment fixed cost | One segment | Term lookup and scorer setup per segment (P6); key terms in the dictionary | Query rows on the multi-segment index before merging |
| Single-thread indexing | 0.84× Tantivy at 1M | A profile of the Phase 2 writer, then D18's measurement and decision | The indexing row with Phase 3's costs (key lookups, versions, the docid sort, merges, commits) |
| Serving memory | 1.33× Tantivy's resident index pages | A smaller verification region (P15), chosen in format 12 | The cgroup memory row |
| Term dictionary | 2.53× | P12, with key terms counted | The disk row's dictionary counterpart |
| Postings | 1.017× `.doc` | Reordering at merge (P1) or an encoding change | The disk row against Lucene with reordering |

Each Phase 3 decision with a cost states its four-axis effect, measured when it lands:

| Decision | Query | Indexing | Memory | Disk |
|---|---|---|---|---|
| Docid sort at flush | none | the sort's time per flush | the sort buffer, charged to the budget | none |
| `pk_filter` | none on search | building per flush | pinned per open segment (about 9 bits per key) | about 9 bits per key |
| `versions` | none | per document | none until read | bit-packed per document |
| Docids section | docid → ordinal on get-by-id | per document | sparse index pinned | about one byte per document monotone; more permuted |
| Two directory barriers per commit | none | commit latency | none | none |
| Key terms | dictionary lookups slightly longer | per document | none | in the dictionary (P12 target) |

## Scale (R6)

At the per-node rows of DISTRIBUTION.md's scale contract (at least 1,000 indexes per
node, most idle):

- **Commit barriers:** two directory fsyncs plus one per new file per commit. At 1,000
  indexes, commits are batched by group commit across indexes on the node (Track C2's
  budget), and the gate states fsyncs per second per node at a stated commit rate.
- **Memory budget:** the node budget divided by 1,000 can be smaller than a useful
  buffer; idle indexes hold no buffer, and an active index draws a buffer of at least a
  stated minimum, so the active count is bounded by the budget. The gate measures flush
  and merge load at 1,000 open indexes with a stated active fraction.
- **Files and mappings:** segments per index times indexes stays within
  `vm.max_map_count` (65,530): the merge policy caps segments per index, and idle
  indexes may unmap their segments, reopening on first use; the gate states the counts
  at 1,000 indexes.
- **Merges and reclamation:** scheduled across indexes from one per-node budget, never
  unbounded per index.
- **Version map:** bounded per index by its flush share; zero for idle indexes.
- **`install`:** transfer cost per node is the checkpoint's bytes, rate-limited like
  merges.

## Deterministic core

```
apply(transaction, position)    pure in state and transaction
applied_position()              the last applied position
visible_position()              the last position a published snapshot reflects
checkpoint()                    commits and pins the manifest; returns its reference
install(checkpoint)             see Install
```

### Digest (D12)

The logical-content digest is the sum modulo 2^64 of xxHash64 (seed 0) over records,
each a kind byte followed by little-endian fields:

| Kind | Fields |
|---|---|
| `P` posting | docid u64, term length u32, term bytes, wdf u32, position count u32, positions u32 each |
| `D` document | docid u64, version u64, length u32, stored length u32, stored bytes |
| `L` | `lastdocid` u64 |
| `S` | schema version u64 |
| `M` metadata | key length u32, key bytes, value length u32, value bytes |

One `P` record per live posting, one `D` per live document, one `L` and one `S`, and
one `M` per metadata entry. One term-major pass over each segment's postings computes it, skipping dead
ordinals; it asserts at most one live copy per docid and per key and every docid at or
below `lastdocid`. Tests compare digests and the sequence of verdicts (accepted;
rejected with the current version; assigned docids), and on small runs the digest after
every transaction. Revision stays out of it.

## faultfs (D19)

`faultfs` implements `kronuz::journal::IO` in memory, and the flush's output and spill
files write through its `File`. Its model is new and stronger than the journal's test
model (`cluster/test/journal_test.cc`, which keeps whole files and an all-or-nothing
namespace):

- unsynced data lost, kept, or torn per 512-byte sector; growth persisting as zeroed
  sectors;
- any subset of unsynced creates, renames, and unlinks surviving; a rename atomic;
- a failed fsync dropping unsynced data, with a later fsync succeeding;
- failures before or after their effect, typed EIO or ENOSPC; ENOSPC from a capacity;
  short writes;
- reads from the model's bytes (the reader takes a `string_view`).

It starts in strata; moving it into the split repositories, with the journal's tests
adopting it, is coordinated with the Detent work.

## Gate

- **T1 Power loss**: a transaction is acknowledged when a `commit()` covering it
  returns. An inconsistent open is one whose digest matches no commit point from the
  last acknowledged commit to the last attempted one, or a fail-closed open after power
  loss alone. The workload covers adds, replaces, and deletes by key and by docid,
  rejected transactions, metadata, refreshes, budget flushes, merges with concurrent
  deletes, commits, `checkpoint`, `install`, and `recover --to`; crash points include
  recovery and a second crash during it; seeds replay deterministically; at least
  10,000 crash points, zero losses, zero inconsistent opens. Injected corruption fails
  closed, tested apart.
- **T2 Digest**: two replays, and schedules with a flush after every transaction and
  none, merges losing sources' documents while they run, a refresh between a delete and
  a re-add, a merge dropping a deleted copy before its key or docid returns, and replay
  after recovery and after `install`: equal digests and verdict sequences.
- **T3 Write amplification**: bytes written over final index bytes, counted the same
  way for both engines, commits included, with equal buffers and merge settings, on the
  corpus append-only and with 50% replaces.
- **T10 Churn** (TESTING, "Churn" and "Steady state"): long add, replace, and delete
  cycles against a model, with bursts, mass deletes, and documents from bytes to
  megabytes, equal to the model at every cycle; at a constant live set, disk, segment
  count, deleted fraction, query time, and writer memory stay within stated bounds of a
  fresh index of the same documents; and the comparative soak against Lucene, Tantivy,
  and Xapian, where strata's disk, memory, and query time after churn meet the ratchets.
- **T4 Indexing**: single- and multi-thread at ≥ 1.0× the faster of Lucene and
  Tantivy by the interval's lower bound, on the 5M corpus with keys in every engine;
  the replace-heavy row; the gate report also states the key filters' cost per insert
  as the segment count grows (I2).
- **T5 Disk full**: storage admission reserves a commit's bytes; merges reserve their
  output; the budget counts file bytes, so on a real filesystem its capacity leaves one
  block per file of headroom for allocation rounding; a removed file's bytes count as
  free only once a directory sync makes the removal durable, when filesystems release
  the blocks, and a drain that removed files syncs the directory; truncated bytes count
  as free once the file's sync makes the truncation durable; a merge's or install's
  reservation is held through any drain while it writes; standalone admission rejects new transactions while space is short; ENOSPC at
  write time recovers in place, at fsync time fences and recovers by reopening; a test
  fills faultfs's capacity during flushes, merges, and every file class, frees space,
  and checks continued operation and the digest.
- **T6**: one CPU-hour of fuzzing per new or changed decoder (manifest, live-docs,
  metadata, `pk_filter`, versions, docids, the format-12 sections), a fixture per new
  format, TSan over flush and merge threads and snapshot publication. Coverage at
  TESTING.md's targets over Phase 3's code and Phase 2's (C16); every corruption check
  of the new decoders reached by a resealed unit test or the replayed corpus (C13); the
  test-parity inventory's Phase 2 and Phase 3 rows (Lucene's index-writer tests under
  deletes, updates, merges, concurrency, crashes, and corruption; Tantivy's merges,
  deletes, and document store) (R8).
- **T7 Parity**: the parity queries unchanged on an index of many flushes and merges
  without deletes; with deletes, the drift against Xapian vectors from the same update
  stream, exact again after a merge drops every deleted document.
- **T8 Ratchets** (must-win, R4): every class of every command at ≤ 1.0× of the
  reference by the interval's upper bound, on the multi-segment index before merging
  and after it; memory (indexing within the node budget plus 10% and ≤ 1.0×, serving
  ≤ 1.0×, allocations not growing with matches); disk (the whole index and every
  section ≤ 1.0× their counterparts, keys configured alike). `verify` in C14's form
  (ordinals, both scoring schemes) and `parity` on the multi-segment index before and
  after merging, and on one with deletes.
- **T9**: Phase 2 conditions C1 (D15, before merge code), C2 (D14, before hits merge
  across segments), C3 (with deletes), C5, C7, C9 (format 12), and C13–C17 at their
  stated points. D14's test: documents duplicated and replaced across flushes and
  merges, pruned top-k equal to a brute-force sort by (score, docid), under both
  scoring schemes, at k = 1, 10, and 100.
