> Status: CURRENT (2026-11-04) how `include/strata/core/manifest_log.hh` works, on branch
> `phase/manifest-log-steps12` (step 2 of `docs/design/manifest-log.md` revision 1, D3; the re-check's
> C7). Nothing uses it yet: step 3 moves commit and recovery onto it. Maintainer: the manifest-log
> workstream (`docs/status/manifest-log-steps12.md`).

# The manifest log's records

An index's commit state is one append-only log: a **snapshot**, then one **edit** per commit carrying
only what changed. This header holds the records' codec and replay; journal's `framing.h` carries
and checks their bytes (D2). No record has a magic or a CRC of its own.

## State

`log_state` is what a snapshot holds and an edit changes: the UUID, the generation, fourteen scalars
(revision, last docid, next segment id, applied position as index and count, schema version,
configured structures, mask, min and max segment versions, document count, total length, live count,
live length), pins (ascending), segments by id, metadata (keys ascending), the field table, and the
index's attributes. A `log_segment` is `segment_ref`'s fields with its deleted set as ascending
ordinals in place of a live-docs file.

`log_sizes` holds what each section's entries take in a snapshot. `apply()` keeps it as edits change
the state; `recount()` sets it from scratch for a state built by hand. `snapshot_size()` adds the
fixed parts and the counts to it without walking a section; `snapshot_bytes()` frames that size as
the first record of a log (`framing::record_end`, 512-byte blocks, no manifests): D7's S, exact after
every edit (C7).

## Bytes

Every integer is LEB128 in its shortest form; the decoder refuses a longer one, so a record decodes
from exactly one encoding.

```
snapshot: [1][generation][files version][UUID 16][14 scalars][pins: n, each]
          [segments: n, entries][metadata: n, (key, value) sized][fields: n, declarations][attributes]
edit:     [2][generation][scalar mask][the carried scalars][section mask][sections present, in bit order]
entry:    [id][identity 16][size][footer CRC][documents][live][live length]
          [deleted: 0 none | 1 bitset, sized | 2 ascending deltas, sized][values generation][attributes]
```

- An edit's scalar mask has bit i for the i-th scalar; it carries those that differ from the state it
  follows, never the UUID. Its section mask: 0 pins (added, removed), 1 segments (removed ids, added
  entries), 2 deletes (per segment: id, ordinals as ascending deltas, live count and live length
  after), 3 metadata (set pairs, removed keys), 4 fields (the table size extended, then the
  declarations), 5 segment attributes (per segment, the whole set), 6 index attributes (the set). A
  set bit with an empty section, or a bit past the last, is damage.
- A deleted set takes the smaller of a bitset (`ceil(documents / 8)` bytes) and its deltas, the
  bitset on a tie, and none exactly when nothing is deleted; another choice is damage.
- Declarations are the field table's (`fields_detail::put_decl`); attributes are the manifest's
  (`put_attributes`): an unknown even tag is dropped on reading, an unknown odd one refuses.

## Decoding

`decode_snapshot` and `decode_edit` follow `decode_manifest_for`: readability first (the files
version, then the mask and segment versions against `readable_by`, as `unsupported_index`), every
count bounded by the bytes left before anything is reserved for it, then the values. The deleted
set's memory is bounded by its bytes: a bitset's size is checked against the document count before
it is read, and deltas take a byte each, so a huge document count costs nothing. A snapshot's checks
are the manifest's: segments ascending and below the next id, identities distinct, totals equal to
the segments' sums, versions present exactly with segments, pins ascending and at most the
generation, keys ascending, no name twice and one key field.

## Replay

`apply(state, edit)` runs D3's checks, then changes the state, so a `corrupt_index` leaves it as it
was: generations contiguous; a removed segment or pin or key present; an added segment absent, below
the next id (after the edit's scalars), its identity new; a deleted ordinal below its segment's
document count and not yet deleted, with the segment's live count after equal to its count less the
new ones and its live length not above the old; the table size extended the table's, no name twice,
one key field; pins at most the generation. The document, live and live-length totals are running
sums: the state's, less what removed segments and new deletes take, plus what added segments bring,
compared with the scalars after the edit whether the edit carries them or not.

`diff(d, c)` builds the whole edit d → c from two states: tests and fuzz seeds use it. It refuses
(`std::invalid_argument`) a pair no edit joins: another UUID, a generation other than d's next, a
segment changed other than by deletes and attributes, a cleared delete, a field table not extended.

## Proof

`test/manifest_log_test.cc`: the layout byte by byte; fixtures under `test/fixtures/manifest_log`
(a snapshot and an edit of each kind); a round trip over 40 seeds × 60 random successive pairs
(replaying the encoded `diff(d, c)` onto d gives c, snapshots encode equal, the tracked size is the
snapshot's, framed size included); each replay check rejected with the state unchanged; every prefix
and bit the decoder refuses. `fuzz/fuzz_manifest_log.cc` replays fuzzed logs and requires every state
replay reaches to decode back unchanged from its snapshot.
