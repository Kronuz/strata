# Phase 3 design: index lifecycle

This document specifies what Phase 3 builds on the Phase 2 segment: an index of many
segments with one writer, keys, versions, updates and deletes, near-real-time
snapshots, durable commits through manifests, tiered merges, metadata, atomic
transactions, a node-wide indexing memory budget, the deterministic core interface
(`apply`, `checkpoint`, `install`), a logical-content digest, and `faultfs`. Phase 3R
puts consensus groups on top of this core; nothing here talks to a network. The
architect's review (`docs/reviews/phase-3-design.md`) settles D1–D20; each section
names the rulings it implements.

## Conventions

- An **index** is one shard: one directory, one writer, one owner core.
- **Docids** are the shard's ids: positive, at most one live copy each. The engine
  assigns `lastdocid + 1`; the host may also address a docid explicitly (D7).
- **Ordinal order is docid order in every segment** (D15): flushes sort by docid and
  merges write in docid order.
- Files are written once and never modified. Integers in new files are LEB128
  (`varint::leb128`) unless a table says otherwise; fixed-width fields are
  little-endian; every file ends in a CRC-32C of its preceding bytes.
- All index I/O goes through `kronuz::journal::IO` (`cluster/completion/io.h`, used
  unchanged at a pinned commit, D19).

## Directory

```
index/
  seg-<id>.sx            immutable segments (format 10: Phase 2 plus pk_filter, versions)
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
- applied position (log index; zero standalone) (I11);
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

### Segment format 10 (D6, D16)

Phase 2's format plus two sections:

- `pk_filter`: a `fusefilter::filter8` (pinned at `b4f4ce1`) over the 64-bit hashes of
  the segment's key terms; the hash is xxHash64 of the term bytes with seed 0 (I12).
- `versions`: one version per ordinal, bit-packed from the segment's minimum.

The `docids` section becomes monotone (D15) and stores bit-packed deltas with a sparse
index every 128 ordinals (I6); docid → ordinal is a binary search over the index, then a
block decode. Format 9 segments are rejected with a version error; a fixture pins
format 10. The 16-byte header id holds the 64-bit segment id and the index UUID's first
8 bytes, checked against the manifest entry at open (I13).

### Metadata (I8)

`meta-<gen>.kv`: a plain sorted file of (key length, key, value length, value) in
LEB128 lengths, then a CRC. Values of any size (Xapiand's schema) fit.

## Commit and recovery

### Commit (D1, D2)

1. Write each new data file (segments, live-docs, metadata) under its final name
   through `IO::create_exclusive` and `File::sync` it.
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

### Recovery (D3)

- Recovery selects the highest final-named `manifest-N`; `.tmp` files are never
  candidates. If its CRC fails, or a file it names is missing or differs in size or
  CRC, the open fails closed and changes nothing on disk. Under the commit protocol,
  power loss alone never produces that state.
- Opening an older manifest (`recover --to N`, or group recovery at a checkpoint's
  manifest) first deletes every higher manifest and syncs the directory, before its
  first write.

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

### Install (D5)

`install(checkpoint)` copies the checkpoint's files under fresh local names from the
target's counters, verifies each whole, writes their manifest through the commit
protocol at a generation above every existing one, and leaves the old files as orphans.
It discards the buffer, pending live-docs, and the version map, and adopts the
checkpoint's `lastdocid`, schema version, applied position, and index UUID (which
belongs to the shard). A crash at any point opens the old index or the installed one.

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
  transaction, not the entry. The applied position names a transaction boundary, and
  apply ignores positions at or below `applied_position()`.
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

## Statistics and ranking (D13, D14)

- Scoring statistics come from one population, as in Lucene: N and total length
  include deleted documents until a merge drops them, like the document frequencies,
  so df ≤ N and weights stay non-negative. Live counts and live total length are what
  the host reports (Xapiand's `doccount`, `av_length`, and `_doc_del`).
- Multi-segment search weights each segment's scorers with the snapshot's statistics
  and collects through one collector that orders hits by (score descending, docid
  ascending). Scorers skip only blocks and windows whose bound is strictly below the
  k-th score; within a segment, ordinal order is docid order, so equal bounds may skip
  past the point where every docid ahead exceeds the k-th hit's.
- Counting shortcuts that read document frequencies apply only to segments without
  deletes (C3); live bits AND into each window's match bits otherwise (I3).

## Merges (D11)

- A tiered policy with the pinned Lucene 10.4.0 `TieredMergePolicy` defaults, read from
  its source (I1): segments per tier, merge width, floor, maximum merged size, and the
  deleted-percentage bound past which a maximum-size segment is rewritten alone.
- A merge reads its sources in docid order, drops deleted documents, and writes one
  segment with the same docids; deletes that arrive during the merge carry over at
  install. Stored frames copy raw when none of their documents is deleted and they stay
  contiguous (I7).
- Merges reserve their output's bytes before starting (T5), run in the background,
  rate-limited, and publish through the next refresh and commit.

## Memory budget (D17, D20)

- One budget covers the node. Every category is charged as it is allocated: buffers,
  frozen buffers until their flush settles, the flush's sort and spill buffers, the
  version map, pending live-docs, merge buffers, and filter construction.
- Flushing starts below the limit by the in-flight flush size; at the limit, stage 1
  suspends until a flush settles. The version map forces a flush past a stated share.
- The budget has `ledger`'s shape (a capacity, move-only permits, explicit settlement,
  a pressure signal) so the swap is mechanical. `journal::Admission` is single-executor
  while the budget spans owner cores, and Detent's `StorageLedger` is not a library, so
  neither fits as is. Strata does not extract `ledger`; PLAN changes accordingly, and
  step 5's timing and API are coordinated with the Detent work before
  `memory_budget` is written.

## Writer parallelism (D18)

- Target: the N-thread row at least equals Lucene and Tantivy's multi-threaded indexing
  on the 5M corpus (about 14,000 documents/s on the gate VM's 8 vCPUs), and
  single-thread indexing reaches ≥ 1.0× Tantivy (T4).
- The decision rests on the Phase 2 writer's stage-2-only rate measured on a quiet
  host. Below twice the target, inversion runs in K buffers: stage 1 stays sequential on
  the owner core and dispatches transactions to K inversion buffers on writer threads,
  each taking contiguous docid runs and flushing its own segments; a refresh cuts every
  buffer at one transaction boundary (I10).

## Deterministic core

```
apply(transaction, position)    pure in state and transaction
applied_position()              the last applied position
visible_position()              the last position a published snapshot reflects
checkpoint()                    commits and pins the manifest; returns its reference
install(checkpoint)             see Install
```

### Digest (D12)

The logical-content digest is the sum modulo 2^64 of xxHash64 over each live
(docid, term, wdf, positions) and each live (docid, version, length, stored record),
plus `lastdocid`, the schema version, and the metadata entries, with a specified byte
layout. One term-major pass over each segment's postings computes it, skipping dead
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
- **T4 Indexing**: single-thread ≥ 1.0× Tantivy; the N-thread target (D18); the
  replace-heavy and memory rows gate Phase 3.
- **T5 Disk full**: storage admission reserves a commit's bytes; merges reserve their
  output; standalone admission rejects new transactions while space is short; ENOSPC at
  write time recovers in place, at fsync time fences and recovers by reopening; a test
  fills faultfs's capacity during flushes, merges, and every file class, frees space,
  and checks continued operation and the digest.
- **T6**: one CPU-hour of fuzzing per new decoder (manifest, live-docs, metadata,
  `pk_filter`, versions, docids), a fixture per new format, TSan over flush and merge
  threads and snapshot publication.
- **T7 Parity**: the parity queries unchanged on an index of many flushes and merges
  without deletes; with deletes, the drift against Xapian vectors from the same update
  stream, exact again after a merge drops every deleted document.
- **T8** (report): the five query commands on the multi-segment index and on one with
  deletes, against Phase 2's rows.
- **T9**: Phase 2 conditions C1 (D15, before merge code), C2 (D14, before hits merge
  across segments), C3 (with deletes), C11 (key terms count toward its target, D16).
