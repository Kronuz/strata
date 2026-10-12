# Phase 3 design: index lifecycle

This document specifies what Phase 3 builds on the Phase 2 segment: an index of many
segments with a single writer, primary keys, versions, updates and deletes,
near-real-time snapshots, durable commits through manifests, tiered merges, metadata,
atomic batches, a node-wide indexing memory budget, the deterministic core interface
(`apply`, `checkpoint`, `install`), a logical-content digest, and `faultfs`. Phase 3R
puts consensus groups on top of this core; nothing here talks to a network.

## Conventions

- An **index** is one shard: one directory, one writer, one owner core.
- **Docids** are the shard's stable ids: positive, monotonic, never reused. A replace
  keeps its docid. A segment maps its ordinals to docids (the Phase 2 `docids`
  section) and docids to ordinals through a sorted array.
- Ties between equal scores break by **docid** across segments (review condition C2):
  segments are visited in docid order of their first document, and a merged top-k
  compares docids, never ordinals.
- Every file the index writes is written once, fsynced, and published by rename and a
  directory fsync (`F_FULLFSYNC` on macOS). Nothing is ever modified in place.

## Directory

```
index/
  seg-<id>.sx            immutable segments (Phase 2 format)
  seg-<id>.<gen>.live    live-docs generations
  meta-<n>.kv            metadata generations
  manifest-<N>           commit points, N increasing
  lock                   held by the owning process
```

`<id>` is a 64-bit segment id assigned from the manifest's counter. A segment's
segment id is written into its header's segment-id field (Phase 2 reserves 16 bytes).

## Manifest

`manifest-N` is a single checksummed file:

| Field | Width |
|---|---|
| Magic `STRMAN01`, format version | 8 + 4 |
| Index UUID | 16 |
| N (manifest generation) | u64 |
| Revision | u64 |
| `lastdocid` | u64 |
| Next segment id | u64 |
| Applied position (log index, position in batch; zero standalone) | u64 + u32 |
| Metadata generation and its CRC-32C | u64 + u32 |
| Segment count | u32 |
| Per segment: id, file size, file CRC-32C (footer CRC), document count, live count, live-docs generation and CRC-32C | 8 + 8 + 4 + 4 + 4 + 8 + 4 |
| CRC-32C of everything above | u32 |

- **Commit** writes new segment files and live-docs generations, fsyncs each, writes
  `manifest-(N+1)` to a temporary name, fsyncs it, renames it, and fsyncs the
  directory. The rename is the commit point.
- **Recovery** opens the highest `manifest-N` whose CRC verifies and whose every
  referenced file exists with the recorded size and footer CRC. If the highest
  manifest fails, recovery fails closed: it reports the failure and does not fall
  back to an older generation (ARCHITECTURE, "Manifest and commit"). An explicit
  `recover --to N` operator path opens an older one.
- **Retention**: a file is deleted only when no open snapshot, no manifest at or after
  the oldest retained one, and no pinned checkpoint references it. Files not
  referenced by any retained manifest at open (a crash between writing a segment and
  committing it) are deleted at open.

## Writer

```
 apply(batch) ──▶ stage 1 (sequential): docids, versions, primary keys, deletes
                        │
                        ▼
                  stage 2: inversion into the in-memory segment (Phase 2 writer)
                        │ budget, refresh, or commit
                        ▼
                  flush → seg-<id>.sx (+ live-docs for older segments) → snapshot
```

- **Stage 1** assigns docids and resolves each operation against the live version
  map, then the segments (below). It is deterministic: its result depends only on the
  index state and the batch.
- **Stage 2** appends to the in-memory segment through `segment_writer` (Phase 2).
- **Deletes** inside the buffer mark the buffered ordinal dead in a buffer-local
  bitset; deletes of committed documents set bits in a pending live-docs generation
  of their segment.
- **Flush** writes the buffer as a segment through the streaming flush (Phase 2), with
  the buffer's dead ordinals as the new segment's first live-docs generation.

### Primary keys and versions

- A document's primary key is a term in a reserved prefix (`\0pk:` followed by the
  key bytes), indexed with wdf 0 and no positions, so it lives in the term
  dictionary with document frequency 1 per live copy.
- Each segment carries a **primary-key filter**: a binary fuse filter (`fusefilter`,
  a new Kronuz library: about 9 bits per key, 0.4% false positives, three memory
  accesses) in a new `pk_filter` section. A lookup checks segments newest first,
  consulting the filter before the term dictionary.
- The **live version map** holds, for every key touched since the last refresh, its
  docid, version, and whether it is deleted. Lookups check it before the segments.
  Refresh clears entries whose state is now visible in a segment.
- An operation's expected version (Xapiand's `_version`) is checked against the
  current version from the map or the segments; a mismatch rejects the operation
  with the current version, deterministically.

### Updates

An update is delete-by-key plus add with the same docid. The added document goes into
the buffer at a new ordinal; the old copy's live bit clears. Because a replaced
document lives at a late ordinal, ordinal order is not docid order; every docid-order
walk and tie-break goes through the docid maps.

### Batches

A batch is atomic: its operations become visible together. Stage 1 checks every
operation's expected version against the state before the batch and the batch's own
earlier operations; a failure cancels the whole batch, which undoes its buffered
additions (truncate the buffer back to the batch's first ordinal: the in-memory
segment supports `truncate(ordinal)`) and its pending live-docs bits.

## Statistics

- Collection statistics (document count N, total length, per-term document
  frequency) are summed over a snapshot's segments.
- **Deleted documents** leave N and total length immediately (each segment's live
  count and live total length are kept with its live-docs generation) but stay in
  term document frequencies until a merge drops them, as in Lucene and Xapian. The
  parity gate measures the drift on update-heavy indexes.
- **Counting shortcuts** that read document frequencies (Phase 2: a term's df,
  inclusion-exclusion, required-plus-optional, term minus term) apply only to
  segments without deletes (review condition C3); a segment with deletes counts by
  iterating live documents.

## Snapshots and refresh

- A **snapshot** is an immutable, reference-counted list of open segments with their
  live-docs generations and the collection statistics. Queries take a snapshot and
  keep it for their whole run, across suspensions.
- **Refresh** flushes the buffer (when not empty), applies pending live-docs
  generations, and publishes a new snapshot without fsync. The default interval is
  1 s; `refresh=true` forces one.
- **Multi-segment search** builds per-segment scorers with global weights (idf from
  summed df, the snapshot's average length) and merges per-segment top-k through one
  collector; deleted documents are skipped by a live-docs check in each segment's
  iteration (two-phase where the scorer allows).

## Merges

- A **tiered policy** in the manner of Lucene's: segments sorted by size, merges of up
  to 10 similar-size segments, a floor of 2 MB below which segments merge freely, a
  maximum merged size of 5 GB, and deletes counted against a segment's size so
  segments with many deletes merge sooner.
- A merge reads its source segments in docid order through their docid maps, drops
  deleted documents, and writes one segment with the same docids; stored-record
  frames copy without recompression when none of their documents is deleted.
- Merges run in the background, rate-limited by a byte budget, and publish through
  the next refresh and commit; a merge never blocks the writer.
- Write amplification is reported against Lucene's tiered policy at equal segment
  budgets.

## Metadata

A key-value store (Xapiand's `_metadata`) whose current generation is a file
`meta-<n>.kv`: sorted keys and values in the `termdict` format with a CRC-32C. A
commit that changes metadata writes a new generation and references it from the
manifest, so metadata commits atomically with documents.

## Memory budget

One indexing budget covers the node. Each index's writer reports its buffer's bytes
(arena, term table, slices, stored records) after every batch; when the node total
exceeds the budget, the largest buffer flushes first. Phase 3 uses a small
`strata::memory_budget` (a capacity, per-owner accounts, and a pressure signal). The
`ledger` primitive planned in Detent's library split replaces it once that split is
coordinated with the Detent work; until then strata does not extract it.

## Deterministic core

```
apply(batch, position)    applies a batch at a log position; pure in state and batch
applied_position()        the last applied position
visible_position()        the last position a published snapshot reflects
checkpoint()              commits and pins the manifest; returns its reference
install(checkpoint)       replaces the index with a checkpoint's files and manifest
```

- **Apply** takes prepared documents (analysis already done by the leader), expected
  versions, and the schema version; it rejects a schema-version mismatch.
- **Determinism**: two indexes that apply the same batches in the same order hold the
  same logical content, whatever their flush and merge timing.
- **Logical-content digest**: a 64-bit hash over, in docid order, each live
  document's docid, version, stored record, and its (term, wdf, positions) list,
  plus the metadata entries in key order. It is independent of segmentation,
  ordinals, and merge history. Tests compare it across replays and across indexes
  built with different flush and merge schedules.

## faultfs

All index file I/O goes through an `io` interface (create, write, fsync, rename,
directory fsync, read, delete, list). `faultfs` implements it in memory with a power
model extracted from the journal's test model (`cluster/journal`'s `journal_test.cc`):

- Data written but not fsynced may be lost, kept, or torn at 512-byte granularity
  when power fails.
- A rename is durable only after its directory's fsync.
- Any operation can fail with `EIO` or `ENOSPC` on schedule.

The extraction from the journal's tests touches `cluster`, so it is coordinated with
the Detent work first; until then `faultfs` starts inside strata with the same model
and moves when the split lands.

## Gate

As PLAN.md: power-loss injection at every barrier of a randomized workload across at
least 10,000 crash points with zero acknowledged-write loss and zero inconsistent
opens; the digest equal across two replays and across different flush and merge
schedules; write amplification against Lucene's tiered policy; multi-thread indexing
must-win rows; disk full fails closed and recovers. Plus the Phase 2 review's
conditions C2 (docid tie-breaks across segments) and C3 (count shortcuts only without
deletes).
