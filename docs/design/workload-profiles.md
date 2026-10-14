# Workload profiles: strata as a primary store

Status: mostly parked. Items 1–3 of "What has to change now" are scheduled because
Xapiand needs them regardless:

- Its `index: none` fields produce documents with no terms.
- Phase 3R's gate checks linearizable get-by-id.

The profiles themselves, index sorting, field-update generations, counters, and the
graph use case are parked as possible scope creep. They are kept here so the format
does not close the door on them.

## The verdict

1. **One engine with per-field structure selection: yes.** A search engine already holds
   most of a document store: stored records, a primary-key map, versions, columnar doc
   values, and sorted-set intersection (which is what a graph query is). The work is
   making each structure optional and the key path independent of text indexing.
2. **Swappable backends (RocksDB, PostgreSQL, or LMDB under strata): no.** Two
   durability models, two recovery paths, and two test matrices for one product. It
   breaks the rule of one implementation per mechanism, and the engine would no longer
   control its own I/O.
3. **What strata should not absorb:** hot counters at high rates, and multi-shard
   transactions. Both are handled below with bounded mechanisms or left out
   deliberately.

Using a search engine as the system of record earned its bad name from Elasticsearch:

- Acknowledged writes lost under partitions.
- Refresh lag on reads.
- No conditional writes until sequence numbers arrived.
- Mapping explosions.

strata already removes the first three. It acknowledges at quorum durability (Track C),
which fixes the lost writes. It checks expected versions in stage 1, which gives
conditional writes. Real-time get (below) removes the refresh lag. Schemas are the
host's (Xapiand's), with dynamic-field limits.

## Xapiand already has the knob

Xapiand's schema sets `index` per field: `none`, `field_terms`, `field_values`,
`field_all`, `global_terms`, and their combinations (`src/database/schema.cc`). A field
can be stored only, indexed for search, kept as a value for sorting and ranges, or any
mix. strata keeps that model and extends it to every section.

## Profiles

A profile is a named default for which structures an index builds. Fields can still
override it. All profiles share one segment format, one write path, and one commit.

| Profile | Built | Skipped | Use |
|---|---|---|---|
| `search` (default) | Everything the schema asks for | — | Full-text and faceted search |
| `document` | Key dictionary, stored records, doc values for declared fields | Term blocks, postings, positions, and lengths for unindexed fields | Profiles, posts, settings: get, put, and conditional update by key |
| `graph` | Key dictionary, keyword postings of edges, time-sorted docids | Positions, lengths, stored records beyond the edge payload | Followers, friends, memberships: adjacency lists, intersections, k most recent |
| `log` | Stored records, doc values, terms for declared fields; time-sorted, append-only | Primary-key filter and version map (no updates) | Events, activity, audit; freezes as it ages (P20) |

Like PostgreSQL without a `tsvector`, the `document` profile pays nothing for text it
never searches. The `graph` profile is where a search engine is better than a key-value
store. An adjacency list is a postings list, mutual friends is an intersection, and
"friends who posted about X" is a conjunction with a text term, all with skip lists and
block-max pruning already built.

## What has to change now

These are the decisions that are cheap before Phase 4 and expensive after.

1. **The key dictionary is its own section.** The primary-key lookup today is the
   primary-key filter plus a term-block lookup, so it depends on the term dictionary.
   The key gets its own sorted section: an FST-indexed key-block section, the same
   `fst` and block code. It holds key → docid and version, and the filter stays in
   front. A segment with no term dictionary is then valid and cheap, and keys support
   ordered range scans (`document` profile).
2. **Every section is optional per segment.** The table of contents already names its
   sections. Readers treat a missing section as "no such structure", never as
   corruption, and the gate tests a segment of every profile.
3. **Real-time get.** A get by key reads the version map and the writer's buffer before
   the segments. It sees an acknowledged write before refresh, as Elasticsearch's
   real-time get does. Buffered stored records are already in an arena, so it costs one
   lookup.
4. **Field-update generations.** Live docs are per-generation side files. The same
   mechanism carries updated doc values: a numeric or keyword column update writes a
   side file for that generation, without reindexing the document (Lucene's updatable
   doc values). Merge folds the generations. This is one mechanism, generations, used
   for both deletes and updates.
5. **Merge operators for counters.** An increment is a delta record in the
   field-update generation, summed at read and folded at merge. It serves likes, view
   counts, and follower counts at moderate rates. A counter hotter than the budget
   (thousands of increments per second on one key) belongs in an aggregating cache in
   front, and the profile says so rather than pretending.
6. **Index sorting by a field.** Segments may be sorted by a declared doc value
   (descending time for `graph` and `log`), so "the k newest" is the first k postings
   with no scoring. Merge already writes reordered docids (the docid map, Track P1),
   so this is a new sort key, not a new mechanism.
7. **Atomic batches per shard, and locality to use them.** A batch is already atomic
   within a shard. With placement routing (`placement-strategies.md`), all of a user's
   documents under `/users/<id>` route to one shard, and a post with its timeline
   entries commits atomically. Cross-shard transactions are out of scope. A request
   that needs them spans shards with idempotent, versioned writes.

## What it costs

- **Format.** One new section (keys), optional-section rules, and field-update side
  files. Phase 4 changes the format anyway, for doc values by slot and points, so
  format 13 takes all of them in one version. The test that format 12 is rejected,
  or migrated, comes with it.
- **Write path.** Stage 1 already has the version map. Real-time get reads it, and
  field updates are new operation types in the same total order.
- **Testing.**
  - The equivalence oracle gains a key-value model: gets, conditional puts, range scans
    by key, and counters, checked against a map.
  - The fuzzers gain segments of every profile.

## How it is measured

Against the systems people actually use for these roles:

- **`document`:** gets and conditional puts per second, p99 latency, and bytes per
  document, against RocksDB and PostgreSQL (`jsonb` with a primary key, no `tsvector`).
- **`graph`:** adjacency fetch, intersection of two adjacency lists, and k most recent,
  against PostgreSQL with B-tree indexes and against RocksDB with prefix scans.
- **`log`:** ingest rate and bytes per event, against Elasticsearch's data streams and
  Quickwit.
- Every result is reported per platform (Linux, FreeBSD, macOS; x86_64 and arm64), as
  for search.

## Where it fits

- **Items 1–3 and 6 (format and read path):** Phase 4, before its format freezes.
- **Items 4–5 (field-update generations, counters):** Track P, after Phase 3's
  lifecycle, which owns generations.
- **Item 7 (atomic batches and locality):** with Track D3/D4.
- **The profiles:** Xapiand's `_settings`, mapped in the D4 namespace work.
