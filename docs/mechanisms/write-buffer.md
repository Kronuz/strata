# The write buffer's records

> Status: CURRENT (2026-11-04) as built on `perf/wbuf-inline`; the design is
> `docs/design/write-buffer.md` (D3). This note covers the record's shape only; the version map,
> position index, runs and freeze are in the design until their notes are written.

## What it does

A write buffer holds the documents written since the last flush. Each accepted write appends one
**record** to the buffer's lane (chunks of 1 KiB doubling to 1 MiB, addressed by 32-bit references).
Reads find a record through the version map (by key) or the docid map, and the flush reads the
records in order of key or docid.

## The record

```
 0  docid u64 | version u64 | position u32 | prev ref u32     stage 1 fills these
24  flags u8 (tombstone, docid prev, ordinal, whole) | 3 B padding
28  docid prev u32 and ordinal u32, when their flags are set
32  (or 40)  inline:  [u32 key len][u32 stored len][key][stored]   padded to 8 bytes
            whole:    a `document` object (key, stored and every other part)
```

- **One shape for every layout.** A record keeps the key and the stored bytes after its header: no
  per-value allocation, no string headers, one block of memory. A tombstone, and a record of a
  layout that inverts documents as they arrive, hold the key alone.
- **Whole records are the fallback**, chosen per record by `push_record`: the inline form would pass
  a chunk (1 KiB; a value past about 900 B), or the document has a part the segment writer reads
  beyond the stored bytes. That is any part not derived from `stored_plugin::part`, or a derived part
  whose plugin's `inline_part(part)` says no (the test plugin with field uses). A layout with such a
  plugin therefore keeps whole documents; a plugin that wants its parts inline needs a serializer,
  which no plugin has.
- `record::key()`, `stored()` and `materialize()` read either form. `materialize()` builds a
  `document` for the digest, which asks each plugin for its digest records.
- A plugin's `buffered` and `resolve` hooks still run on a copy of the document before the record
  is built, so the record holds what the hooks left; only layouts with such a hook pay the copy.
- `stride()` is computed from the record's own flags and lengths; the lane reads it to walk a chunk
  and to destroy records on `truncate()` (a rejected transaction) or at the end.

## The flush and the room check

- A flush pins the lane and borrows from it. An inline record goes to the segment writer through
  `add_stored_view(fields, stored)`; a whole record through `add(fields, document, borrowed)`. The
  stored builder keeps views and checks that each lies in the lease's chunks.
- `lane_fits` bounds the transaction's bytes: two records per operation, each at the widest of the
  largest header, its inline stride (capped at a chunk) and a whole record. A transaction the lane
  cannot take freezes the buffer first, so an apply never fails halfway on a full chunk directory.
- The memory estimate that drives the flush threshold (`doc_bytes`, `estimate_for`) is unchanged
  from the whole-document record, so flush points match the layouts before this change.

## Where it is tested

`test/write_buffer_reads_test.cc` (`record_layout`, `record_strides`: raw bytes of both forms and
every flag shape, cut and re-append), `test/write_buffer_test.cc` (`inline_records`: sizes through
the chunk edge, rejected batches, flush; `chunk_limit_sized_transactions`), `test/fields_test.cc`
(whole records with field uses), `bench/kv/proto/bufmem.cc` (bytes per record per layout).

## Design vs code

1. The design (D3) sizes a record at "24 + 1 + 2 + 16 + 100 = 143 B" with a 24-byte header; the code
   has a 32-byte header and an 8-byte length pair (156 B for 16 B keys and 100 B values, 160 B
   aligned in the lane).
2. The design says nothing of parts beyond the key and stored bytes; the code holds such a document
   whole (above).
3. The memory estimate still charges `sizeof(document) + 96 + part_bytes` per document, more than an
   inline record occupies. Charging the real stride would change flush points.
