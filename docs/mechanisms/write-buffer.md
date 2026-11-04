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
28, 32 or 36  inline:  [key len varint][stored len varint][key][stored]   padded to 8 bytes
32 or 40      whole:   a `document` object (key, stored and every other part), 8-byte aligned
```

- **One shape for every layout.** A record keeps the key and the stored bytes after its header: no
  per-value allocation, no string headers, one block of memory. A tombstone, and a record of a
  layout that inverts documents as they arrive, hold the key alone. The lengths are LEB128
  varints right after the fields (D3): one byte each below 128, two below 16,384, which covers
  every inline record, since a record fits a chunk. A 16 B key and a 100 B value take
  28 + 2 + 116 = 146 B, 152 B in the lane.
- **Every record is at most a chunk.** `record::emplace` is the only way in: it computes the
  inline stride and appends the inline form only when the caller allows it and the stride is at
  most `wbuf::min_chunk` (1 KiB); otherwise it appends a whole record (a `static_assert` keeps it within a
  chunk at the widest header), built by the caller's callback. Tombstones and inverting records go through the
  same call, so a key past about 990 B makes a whole key-only record, as before this change. The
  lane refuses a record past a chunk in every build (`append_n` throws `logic_error`), so a missed
  bound fails loudly instead of writing past the chunk.
- **Inline is opt-in per plugin.** A layout buffers documents inline only when every plugin with a
  part either has exactly `stored_plugin::part` and the stored plugin's builder, or declares
  `inline_part(part)` (the test plugin with field uses; the bench's `carry` plugin). A plugin that
  declares `inline_part` must derive from `stored_plugin` and keep `stored_builder`, or the layout
  does not compile (`inline_kind`). Any other layout keeps every document whole, so every
  plugin's `add` runs at the flush; `writer::add_stored_view` itself does not compile for such a
  layout, and the flush calls it only under `if constexpr (inline_layout())`.
- `record::key()`, `stored()` and `materialize()` read either form. `materialize()` builds a
  `document` for the digest, which asks each plugin for its digest records.
- A plugin's `buffered` and `resolve` hooks still run on a copy of the document before the record
  is built, so the record holds what the hooks left; only layouts with such a hook pay the copy,
  and a whole record takes the copy by move.
- `stride()` is computed from the record's own flags and lengths; the lane reads it to walk a chunk
  and to destroy records on `truncate()` (a rejected transaction) or at the end.

## The flush, the room check and the estimate

- A flush pins the lane and borrows from it. An inline record goes to the segment writer through
  `add_stored_view(fields, stored)`; a whole record through `add(fields, document, borrowed)`. The
  stored builder keeps views and checks that each lies in the lease's chunks.
- `lane_fits` bounds the transaction's bytes: two records per operation, each at the widest any of
  them can take: the whole stride, or an operation's inline stride with the widest header when
  that fits a chunk, a chunk when the widest header passes one but the narrowest does not, and the
  whole stride when even the narrowest passes (such a record is always whole). A transaction the
  lane cannot take freezes the buffer first, so an apply never fails halfway on a full chunk
  directory.
- **The memory estimate charges the lane's chunks as they are taken, and what whole records hold
  beyond them**, for every layout: `buffered_bytes()` is the lane's `bytes()` plus, in a document
  layout, `b.bytes` (each whole record's `doc_bytes` less its stride; `doc_bytes` is
  `sizeof(document) + 96 + part_bytes`), or in an inverting layout the inverter's allocations.
  An inline record costs its place in the lane and nothing more, so `buffer_share` counts the
  inline saving: a buffer holds more records before it flushes. The subtraction cannot wrap: a
  `static_assert` holds the widest whole stride (40 + `sizeof(document)`) within the smallest
  `doc_bytes` (`sizeof(document) + 96`), and an `assert` checks it at the push.
- The lane's chunks double from 1 KiB to 1 MiB, so the open chunk is charged whole the moment it is
  taken: a buffer can flush with up to half its charge an empty chunk while the lane is under
  2 MiB, and up to 1 MiB of it above. A buffer that holds only rejected records counts as holding
  something while it has a chunk (`holds_rejected`), since its `b.bytes` can be 0.
- `file_sink` reserves its 1 MiB gather once; an append never passes it (a write that would is
  drained first or passed through), so it never regrows.

## Where it is tested

`test/write_buffer_reads_test.cc` (`record_layout`, `record_strides`: raw bytes of both forms and
every flag shape, cut and re-append; `lane_charged`: the charge is the lane's bytes for inline
records, plus `doc_bytes` less the stride for a whole one), `test/write_buffer_test.cc` (`inline_records`: sizes through
the chunk edge, rejected batches, flush; `chunk_limit_sized_transactions`; `long_key_records`:
tombstones of keys from 500 B to 70,000 B as a fresh lane's first record and in a two-chunk lane),
`test/fields_test.cc` (whole records with field uses; `own_builder_parts`: a plugin with its own
builder keeps its documents whole and its `add` runs), `bench/kv/proto/bufmem.cc` (bytes per record per layout).

## Design vs code

1. The design (D3) sizes a record at "24 + 1 + 2 + 16 + 100 = 143 B, aligned to 144 B", but its own
   layout puts the lengths at 28 (3 B of padding after the flags byte): the code is 28 + 2 + 116 =
   146 B, 152 B in the lane. The varint lengths and their offsets (28, 32 with one field) match.
2. The code adds an ordinal field (flag 4) after the docid prev field, for inverting layouts; the
   design names only the docid prev field. With both fields the lengths sit at 36.
3. The design says nothing of parts beyond the key and stored bytes; the code holds such a
   document whole unless its plugin opts in (above).
4. Values past about 990 B stay whole because a record must fit the 1 KiB first chunk: a second
   shape at a common size. Open for the architect (ledger).
5. D3 says chunks are "charged to the node budget as they are taken"; the code does so for every
   layout. A chunk is charged whole at once, so at small shares (under 2 MiB of lane) the records
   a flush holds can drop to about half the share: `defer_io_test`'s 6000 B share and
   `background_merge_test`'s pressure flush count moved with it.
