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
index's attributes. A `log_segment` is `segment_ref`'s fields with its deleted set.

A deleted set (`log_deleted`, the owner's D42) is bitset words while they take no more memory than
the ordinals (at least `doc_count / 64` deleted) and sorted ordinals otherwise, so it holds at most
8 bytes per ordinal and at most its bitset's words. It keeps the bytes its ascending deltas encode
to, so an entry's size never walks the set. A delete edit costs the edit, not the set, so a log's
replay stays a small multiple of its snapshot's read (SR2):

- Words carry a summary: a bit per nonzero word, then a bit per nonzero summary word, up to one
  word (1/63 more memory). A member's neighbour below or above is a few words at each level away,
  and setting a bit marks the levels up to a word already nonzero.
- Ordinals are ascending runs, each more than twice the next (at most 64). An insert appends its
  ordinals as a run and merges the last two while the older is at most twice the newer, so each
  ordinal moves O(log n) times; a lookup is a binary search per run. An allocation failing in a
  merge leaves the set whole, with one run more. Equality compares members, not runs.

`insert` adds ascending new ordinals and, for the delta bytes, looks up each run of new ordinals'
old neighbours. A set changes form once, to words, when its count reaches the threshold; deletes
never clear.

`log_sizes` holds what each section's entries take in a snapshot. `check_edit()` computes the sizes
an edit leads to (in a `log_shape`, with the section counts and scalars) and `apply()` takes them;
`recount()` sets them from scratch for a state built by hand. `snapshot_size()` adds the
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

`decode_snapshot` and `decode_edit` check readability first (the files version, then the mask and
segment versions against `readable_by`, as `unsupported_index`), every count bounded by the bytes left before anything is reserved for it, then the values. The deleted
set's memory is bounded by its bytes (D42): its form follows from its count before anything is
allocated, a bitset's size is checked against the document count before it is read and decodes to
words (its own bytes, rounded to a word) or fewer ordinals, and a deltas body of B bytes holds at most
B ordinals, which decode to at most 8 B bytes as ordinals or words; a huge document count with few
deletes costs nothing. A snapshot's checks
are the manifest's: segment ids from 1, ascending and below the next id, identities distinct, totals equal to
the segments' sums, versions present exactly with segments, pins ascending and at most the
generation, keys ascending, no name twice and one key field.

## Replay

`check_edit(state, edit)` runs D3's checks and returns the shape the edit leads to, changing
nothing; `apply(state, edit)` is `check_edit` and then the change, so a `corrupt_index` leaves the
state as it was, and a writer checks its own edit, and decides D7 on the S after it, before writing
it. The checks: generations contiguous; the next segment id never lowered, so every kept id stays below it and
no id is handed out twice; a removed segment or pin or key present; an added segment absent, its id
nonzero and below the next id (after the edit's scalars), its identity new; a deleted ordinal below its segment's
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
replay reaches to decode back unchanged from its snapshot. Its seeds (`manifest_log_test --seeds`)
are the fixture logs, an edit of each scalar alone (lowered to 0 and raised by one), and random
logs; `sr1-next-segment-lowered` is the security review's input that lowered the next id.
