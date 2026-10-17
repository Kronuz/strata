# Format 13

Revision 0, for the preliminary review. Format 13 is the one segment format version that
carries every pending format change (PLAN, "Carried rows"): the key dictionary as its own
section (`phase-4.md` D12), the query gap's skip entries (`docs/perf/query-gap-merged.md`),
the postings change P1 leaves owed, Phase 4's doc values, points, term lists, and optional
sections (`phase-4.md` D2), the reservations that review asked for (R1), and the
format-13 items of the layers design (`layers.md` S1, S2, L7–L9, L16). One design, one
review, one set of fixtures and fuzz bases. Decisions are F1–F22; gate rows T1–T11.

It consolidates; it does not reopen. Where `phase-4.md` or `layers.md` already decided a
point and its review closed it, this document cites the decision and fixes its bytes. The
open choices are the postings and dictionary encodings (F10–F13, F8's codec), which name
candidates and how each is measured. No size or speed below is a measurement of format 13:
the numbers quoted are format 12's, recorded in `docs/gates/phase-3-ratchets.md` and
`docs/perf/`, or projections already recorded there, marked as such.

Clean room, as `phase-4.md`: Xapian's behavior enters from its documentation and
`strata-oracle` vectors; Lucene and Tantivy from their documentation and javadoc. Nothing
here comes from their source.

## Why now

Format 12 is what Phase 3 measured (`docs/gates/phase-3-ratchets.md`, "Disk after a full
merge", strata reordered against keyed Lucene `-bp`): the whole index 0.999×, postings
1.014×, the term dictionary 1.20×. The two misses are carried to this format. The query
rows are carried too (1.33–1.34× after merging); the profile of the worst count class puts
about 300 ns on each touched block against roughly 90 ns for Lucene, spent in level-0
decoding and validation (`query-gap-merged.md`). Phase 4's sections need a format
regardless, and every format change after format 13 costs a reader for N − 1 (F2).

## F1. Scope and freeze

Format 13 holds F2–F22 and nothing else. It freezes once, when all of these hold:

1. This design's review clears (PLAN: a format lands only after its design review).
2. F8's key codec, F10's level-0 layout, and F11's postings encoding are chosen by their
   measurements (T3, T4, T5), each candidate's numbers recorded in `docs/perf/`.
3. D15's geo measurement is recorded (`phase-4.md`, R1), since it fixes F16's dimension
   counts and whether shape documents need interval trees.
4. The fixtures and fuzz bases of F22 are pinned at the frozen commit.

Until then the branch writes format 13 with candidate encodings, and its fixtures are
re-pinned with each change; no index outside tests holds format 13.

## F2. Version and compatibility

- The header's version field is 13 (`format.hh:61`). A reader accepts versions in
  [`min_read_version`, `version`], both 13 today.
- A version outside that range throws `unsupported_segment`, a new exception that is not
  `corrupt_segment`: a segment from another binary is not damage, so `heal` and the
  read retry (`index.hh:462-493, 2066-2085`) never fence an index for it. Today a version
  mismatch is `corrupt_segment("header magic or version")` (`reader.hh:72`).
- Format 12 is rejected that way. No index exists outside tests and benchmark runs, and
  every Phase 3 gate index is rebuilt (R20).
- **The promise.** From format 13 on, a reader carries N − 1 (R20; `layers.md` L7, S2): a
  format-14 binary opens format-13 segments with their own decoder, and the writer writes
  only N. Section versions follow the segment's version (S2); plugins have none.
- The manifest's own version changes with format 13 (F20), and the manifest records the
  lowest and highest segment version it names (L16), so an index whose segments a binary
  cannot read is refused at the manifest, before any segment opens.

## F3. Section kinds

| Kind | Id | Holds | Verified | Presence |
|---|---:|---|---|---|
| `header` | 0 | Magic, version, flags, segment id (F6) | pinned | mandatory |
| `terms` | 1 | Dictionary blocks | regions | `inverted` |
| `term_index` | 2 | The dictionary's FST | pinned (copied out) | `inverted` |
| `postings` | 3 | Postings and skip data (F10, F11) | regions | `inverted` |
| `positions` | 4 | Positions and counts | regions | `inverted` |
| `lengths` | 5 | Statistics header (F7), then exact lengths | first use | `inverted` |
| `stored` | 6 | Stored frames and their index (F19) | regions | `stored` |
| `docids` | 7 | Unchanged from format 12 (`docids.hh:22-33`) | pinned | mandatory |
| `stats` | 8 | Core statistics (F7) | pinned | mandatory |
| `norms` | 9 | One `encode_length` byte per document | pinned | `norms` |
| `versions` | 10 | Unchanged from format 12 (`writer.hh:924-944`) | pinned | `keys` |
| — | 11 | Format 12's `pk_filter`; never written in format 13 | — | corruption |
| `keys` | 12 | Key blocks: key → ordinal (F8) | regions | `keys` |
| `key_index` | 13 | Key range, key filter, the key blocks' FST (F8) | pinned | `keys` |
| `values` | 14 | Slot directory and doc-values columns (F15) | regions | `values` |
| `points` | 15 | One tree per point slot (F16) | regions | `points` |
| `term_lists` | 16 | Each document's terms (F17) | regions | `term_lists` |
| `index_sort` | 17 | The sort description (F18) | pinned | `index_sort` |

- "pinned" sections are checked whole at open; "regions" per region on first read, as
  today (`reader.hh:116-132`); `lengths` keeps its lazy whole check (`reader.hh:300-313`).
- Kinds are stable numbers: a later format adds kinds and never renumbers. Kind 11 stays
  retired, so a format-12 segment's `pk_filter` cannot be read as anything else.
- `keys` and `key_index` split one structure the way `terms` and `term_index` do, so the
  part read on every lookup is pinned and the blocks are verified as read.

## F4. Presence and structure flags

The footer's structure flags (F6) record which structures the segment was built with, one
bit per row: `inverted` 1, `norms` 2, `keys` 4, `stored` 8, `values` 16, `points` 32,
`term_lists` 64, `index_sort` 128. Bits 8–31 are zero; a set unknown bit is corruption.

- **Mandatory:** `header`, `docids`, `stats`, in every segment.
- **Required together:** a set bit needs every section of its row; `positions` belongs to
  `inverted` and is written empty when no term has positions. `norms` requires `inverted`.
  `versions` is `keys`' (R1: whenever `keys` is present).
- **Free:** `stored`, `values`, `points`, `term_lists`, `index_sort`, each its own bit.
- **At open:** a clear bit with any of its sections present, a set bit with one of its
  sections absent, a mandatory section absent, or a kind not in F3's table is corruption.
  This closes `decode_toc`'s acceptance of any 16-bit kind (`format.hh:222`) and the
  reader's `default: break` (`reader.hh:104`).
- **Absent structures answer "no such structure":** a segment without `inverted` has no
  terms (`term` returns nothing; `length` is 0, `phase-4.md` D2); without `keys`,
  `version(ord)` is 0 and `may_hold_key` is false; without `stored`, every record is
  empty. A segment with only `keys` and `stored` is the `document` profile (L12).

What a writer sets is a function of the index's configured structures, never of the
documents: a configured `stored` structure whose run has no records still writes its
(empty) section and sets its bit, so two replicas with the same configuration always
agree on flags.

## F5. The table of contents and file order

- The table keeps format 12's entry layout (`format.hh:156-168`): kind u16, region shift
  u16, offset u64, length u64, CRC u32, the region CRCs. Each kind at most once; sections
  never overlap; entries sorted by offset (new: a writer emits them in file order, and the
  reader rejects any other order, so one segment has one encoding).
- The section count bound stays 64 (`format.hh:217`), above F3's 17 kinds.
- **File order** (`layers.md` S1): the header; then the streamed sections (`postings`,
  `positions`, `terms`, `keys`, `stored`, `values`, `points`, `term_lists`) in that order;
  then the pinned tail: `docids`, `stats`, `lengths`, `norms`, `versions`, `term_index`,
  `key_index`, `index_sort`; then the table of contents and the footer. An open reads the
  header page and one contiguous tail run. Each section starts on a 64-byte boundary, as
  today (`format.hh:79`).

## F6. Header and footer

```
header (32 bytes)  [magic "STRSEG01"][version u32 = 13][flags u32 = 0][segment id 16 bytes]
footer (56 bytes)  [toc offset u64][toc length u64][toc CRC u32][doc count u32]
                   [min docid u64][max docid u64][structures u32][reserved u32 = 0]
                   [CRC-32C of the preceding 48 bytes u32][magic "SEND"]
```

- The header's flags word and the footer's reserved word must be zero; a nonzero value is
  corruption. They are the room a later version extends without moving a field.
- The structures word is F4's. The footer grows from 48 bytes (`format.hh:63`) to 56.
- The header id keeps format 12's meaning (I13).

## F7. Statistics

`layers.md` L9: each structure's statistics live in its own section.

- `stats` (core): [document count u64]. 8 bytes, against format 12's 28
  (`reader.hh:106-110`).
- `lengths` gains a header before its width byte: [total length u64][minimum length
  u32][maximum length u32][maximum positions u32], the 20 bytes that left `stats`, then a
  CRC-32C of those 20 bytes. An open reads the header, so it checks that CRC; the section
  stays verified whole on first use (`reader.hh:300-313`), and the default scheme, which
  reads norms only, never reads the rest.
- A segment without `inverted` has no lengths: its total length is 0, which the
  manifest's totals already accept (L9).

## F8. The key dictionary (D12; R6)

**The key leaves the term dictionary.** A document's key is a core field (`layers.md` L3:
`document::key`), and the `inverted` structure never indexes it. Queries on the key
prefix (Xapiand's `Q` terms) route to `keys`; the planner, `verify`, and the digest change
with them (R6). The key prefix leaves the manifest's index options and becomes the
`keys` structure's option (L4's table).

- **The key term is boolean.** Until the core's `document::key` lands with the layers
  refactor, the index still takes the key from the document's one term with the key
  prefix. That term must have wdf 0 and no positions, or stage 1 rejects the operation as
  invalid, so dropping it from the dictionary loses nothing (`index.hh`, `key_of`).
- **Routing.** A term lookup that misses the dictionary asks `keys`
  (`segment::key_term`): a key reads as a boolean term with one posting, df 1 and cf 0, so
  every query path, `verify`, and the statistics see what format 12's key term gave them.
  No term in the dictionary can equal a key, since a document's terms hold one key-prefixed
  term at most and the writer removes it.
- **The digest** keeps its `P` record for each live key (wdf 0, no positions), read from
  `keys`, so its value does not change until F21's split.

**Layout.**

```
keys       termdict blocks over the keys, each value the key's ordinal (LEB128, candidate K1)
key_index  [min key: LEB length, bytes][max key: likewise][fuse filter8 over xxHash64,
           seed 0 (I12)][the blocks' FST]
```

- One live copy per key per segment, as today: a key names one ordinal. The writer
  rejects a repeated key (`std::invalid_argument`), and the reader checks at scan time
  that keys strictly increase.
- Versions come from `versions` only (R6); a key entry holds no version.
- Lookups: `may_hold_key` checks the range and the filter (pinned), then `find` floors
  the FST and decodes one restart run, verifying its region.
- Range scans merge per-segment cursors in key order, skip deleted ordinals, and read one
  snapshot (R6); the merge collects keys with the same cursor instead of scanning the
  term dictionary for the prefix (`merge.hh:67-84`).

**Codec candidates** (chosen by T3's dictionary row and T5's lookup row):

| Id | Key text | Ordinal |
|---|---|---|
| K1 | termdict's alphabet-coded front coding (`docs/perf/p12-term-dictionary.md`), restart 32, blocks 32–64 | LEB128 per key |
| K2 | As K1 | Bit-packed at `bit_width(count − 1)` in a per-block column after the block's entries |
| K3 | Front-coded blocks of 128 keys compressed with zstd (the pinned release) | As K2 |
| K4 | As K1 with restart 64, blocks 48–96 | As K2 |

The dictionary row's recorded analysis (`phase-3-ratchets.md`, "Where the key terms'
bytes go") is the starting point, not a result: in docid order, delta-coded ordinals were
projected at 9.8 MB and the key text at 10.6 MB with zlib standing in for the codec; under
reordering the ordinals lose their locality, projected at about 14.5 MB
(`reordering.md`, Risks). K2 is for that case: if a permuted segment's ordinals in key
order carry no locality, `bit_width(count − 1)` bits each is within a bit of what any code
needs, with no per-key length to decode. Each candidate's size and lookup cost is measured by `strata_engine keysim` (F22) on
the 5M merged, reordered index's keys before one is chosen.

**Disk accounting.** The dictionary row counts `terms`, `term_index`, `keys`, and
`key_index` against keyed Lucene's `.tim`, `.tip`, and `.tmd`, which hold its id field's
terms. Moving the keys to their own section is not a saving by relabeling: the keys are
counted once, in that row (`phase-4.md` D12, "the disk row counts keys once").

## F9. Versions

Unchanged from format 12: [minimum u64][width u8][(version − minimum) at width bits]
(`writer.hh:924-944`), validated at open (`reader.hh:415-423`). Present exactly when
`keys` is. A keyless segment has no `versions`, and `version(ord)` is 0 (D2, M3).

- **A version belongs to a key.** A document without a key has version 0 in every index,
  and its verdict reports 0, not a count of its replacements: otherwise a docid-addressed
  replace would read its current version from a keyed segment before a flush and as 0
  from a keyless one after it, and replicas flushing at different times would disagree.
  Conditional operations on a keyless document already reject in stage 1 (M3).
- **The structure is the index's.** The writer's `keyed` option, set from the index's key
  prefix, decides the `keys` bit; a merge's output is keyed when any input is. A keyed
  segment with no keyed document writes empty `keys` and `key_index` sections.

## F10. Skip entries for the count and advance paths

Format 12's level 0 is one LEB128 stream per group of 32 blocks: per block the last
ordinal's delta, the byte length and bitset flag, the positional deltas, and the block's
competitive pairs (`writer.hh:778-805`; `reader.hh:830-874`). Advancing to a block decodes
the whole group's stream, impacts included, and validates every entry. The count and
advance paths never read impacts.

**The change.** Level 0 splits into two arrays per term:

1. **Advance entries**, fixed width, one per block, read by every path: the block's last
   ordinal and the offset of its bytes from the term's blocks area, with the bitset flag.
   A block's byte length is the next entry's offset minus its own. Locating the block
   holding a target is a search over fixed-width lasts, with no decoding; validation is
   per entry read (lasts increase, offsets increase and stay inside the area).
2. **Impact entries**, read only by scoring paths: each block's competitive pairs in
   `encode_sized_impacts`' form (`format.hh:329-343`), located by a fixed-width offset per
   group, so a scorer decodes one group's pairs only when a bound needs them.

The positional fields (first position index, position and count block offsets) move to a
third per-group stream read only when positions are (phrase paths), keeping their deltas.
Level 1 keeps its role (one fixed entry per group) but drops the pairs offset, which the
impact entries' own group offsets replace.

**Width candidates** (chosen by T4's per-block cost row and T3's postings row):

| Id | Advance entry | Bytes per block |
|---|---|---:|
| A1 | last u32, offset u32 (bit 0 the bitset flag) | 8 |
| A2 | last − group base u16 where the group's span fits, else u32; offset from group start u16 where it fits, else u32; widths per group | 4 or 6 or 8 |
| A3 | Per group, both fields bit-packed at the group's widths (`bitpack` FOR of 32 values) | ⌈(wl + wo) × 32 / 8⌉ per group |

A1 and A2 read an entry with one load; A3 needs a lane extract, as `length()` does
(`reader.hh:302-313`). Their bytes against format 12's LEB128 stream are unmeasured;
`poststats` (F11) reports format 12's level-0 bytes per field, and the postings row (T3)
states each candidate's difference, since this change trades bytes for decoding work and
the postings row is itself carried.

## F11. Postings encoding (P1's owed row; P4)

Strata reordered is 845.6 MB against keyed `-bp`'s 833.7 MB `.doc` (1.014×). The section
holds blocks, tails, skip data, and per-block impacts. Format 13 first measures where the
bytes are, then changes only what the measurement points at.

**The breakdown.** `strata_engine poststats` (F22) reports, over a segment, the bytes in
level 1, level 0's last, length, positional, and impact fields, ordinal blocks (FOR and
bitset separately), wdf blocks, and tails, with counts of blocks per encoding and of
blocks whose wdfs are all 1.

**Candidates**, each a per-block or per-term choice the writer makes by size alone (so a
merge's output is a function of its input, F21):

| Id | Change | Format hook |
|---|---|---|
| E1 | Omit a block's wdf bytes when every wdf in it is 1 | A flag bit in the advance entry |
| E2 | Partitioned Elias-Fano as a third ordinal block encoding (P4) | A 2-bit codec field: FOR, bitset, Elias-Fano, reserved |
| E3 | Roaring-style run containers for runs of consecutive ordinals (P4) | The same codec field |
| E4 | Tails of 16 or more postings as one FOR block of their length | Implied by df mod 128 |
| E5 | Per-block impacts reduced to the pairs that differ from the group's frontier | A per-group bit |

E2 and E3 cost decode time per block; each is kept only if T4's count and intersection
rows do not regress past T4's bound. If the breakdown shows the gap lives elsewhere, the
candidate list changes in revision 1, before any is built.

## F12. Variable-size block-max partitions (P3)

F10's impact entries are separate from the 128-posting compression blocks, so their
granularity need not follow them (Mallia et al., 2017: variable-sized block-max
intervals over document-ordered postings). The impact entries become a list of
partitions, each its last ordinal and its competitive pairs; with fixed partitions it is
one per block, as format 12.

- **Hook in format 13:** each impact entry carries its own last ordinal, so a writer may
  partition freely, and readers bound by partition rather than by block index.
- **Candidates:** P0, one partition per block (format 12's bounds, the default); P1,
  Mallia et al.'s greedy partitioning against a target average size, cut where the
  bound's error over the partition would grow past a threshold, both presets.
- **Measured** by T4's pruned top-k rows and the postings row; P1 is kept only if it wins.

## F13. Threshold seeds (P21)

A top-k query starts with threshold 0 and prunes nothing until k candidates are scored.
P21 stores, per term, what a seed needs so pruning starts at the first posting, for k =
10, 100, and 1,000, and stays exact (PISA's seed is exact only at k = 10:
`phase-3-ratchets.md`, "What PISA's harness runs").

**What is stored is not a score.** Scores depend on the scheme and on statistics that
change with every commit, so a stored score goes stale. Instead, for each k ≤ df, the term
stores a few dominance pairs (w, l): pairs such that at least k of the segment's postings
of the term have wdf ≥ w and exact length ≤ l. For any score that increases with wdf and
decreases with length (both BM25 forms at k1 > 0, b > 0), every such posting scores at
least f(w, l), so the k-th best score of the term in the segment is at least
max over pairs of f(w, l). The query computes f under its own scheme and statistics.

- **Lengths round up.** `encode_length` rounds down (`format.hh:256-262`), which bounds
  scores from above; a seed needs a bound from below, so l is stored as the smallest
  length code whose decoded value is at least the true length, or as an exact length.
- **Where seeds apply:** a disjunction's threshold starts at the largest seed over its
  terms and segments, since a document's sum of non-negative contributions is at least
  one term's, and the k-th best across segments is at least any one segment's k-th best.
  Seeds do not apply to conjunctions or phrases, to segments with deletes (a seed may count
  a deleted posting), or when the threshold must admit ties: pruning drops only scores
  strictly below the seed, so the docid tie rule stays exact (`reordering.md` D5).
- **Where they live:** in the postings header of terms with skip blocks (df ≥ 128), at
  most 3 pairs per k; shorter lists have no seeds. This keeps them out of the carried
  dictionary row; their bytes are reported in the postings row.
- **Measured** by T6: exactness under `verify`, and the pruned top-k rows against no
  seeds.

## F14. Positions (P13)

Positions pass at 0.980× keyed `-bp` and are not carried. Format 13 changes them only if
P13's candidates win their row before the freeze: per-block choice among PFOR (today's),
FOR, and Elias-Fano, behind a 2-bit codec field in the positional stream's entries; and
deltas within a document coded against the document's length. Without a win, positions
keep format 12's encoding and no codec field.

## F15. Values (D4; R4, R10)

The `values` section is the slot directory followed by one column per slot, each in
`docvalues.hh`'s column codec as built (`docvalues.hh:22-37`):

```
values  [slot count LEB][per slot: declaration (F15a), column offset u64, column length u64]
        [columns, each 8-byte aligned]
```

- **F15a, a declaration** (D3): slot id u32, logical type u8 (`i64`, `u64`, `f64`,
  `date`, `bytes`, `binary`), flags u8 (multi-valued, doc values, points), and for points
  index dimensions u8, data dimensions u8, bytes per dimension u8 (N2: at most 8, 16, and
  8). Slots strictly increase. The directory copies the declarations the segment was
  written under, so `install`, merges, and frozen indexes decode it without the slot table.
- The column codec's own version byte is checked equal to 1 for format 13.
- **The exact accumulator's width** (condition 4) is recorded in the column header for
  `f64` sums: format 13 writes 0, persisting no `f64` accumulator; integer sums keep their
  128 bits. A later version may persist accumulators by writing a nonzero width.
- Presence uses `docset`'s frozen form (R4); skip groups are R10's.

## F16. Points (D5; open on D15)

```
points  [tree count LEB][per tree: slot id u32, index dimensions u8, data dimensions u8,
        bytes per dimension u8, leaf size u32, point count u64, offset u64, length u64]
        [trees, each 8-byte aligned, in Kronuz/bkd's serialized form]
```

The envelope is fixed here; the tree body is `bkd`'s, whose on-disk form does not exist
yet (`bkd` builds and queries in memory today). Both freeze with D15's result (F1, item 3).
Until then the branch writes no `points` section, and the structure bit stays clear.

## F17. Term lists (D11; R7)

Opt-in per index (the `term_lists` bit). Per document, its terms in order with wdf, and
positions when configured, packed into zstd frames of at most `stored_block_size` bytes
with the same frame index as `stored` (`writer.hh:877-906`): a document's list is
[term count LEB][per term: shared prefix with the previous term LEB, suffix length LEB,
suffix bytes, wdf LEB][with positions: per term, position count LEB and deltas]. Reported
in the disk table, excluded from the ratios (BENCHMARKS, "term lists").

## F18. Index sort (D14)

Reserved and defined now, written when D14 is built. `index_sort` holds [component count
u8][per component: slot id u32, direction u8, missing u8 (first or last), selector u8 (min,
max)], and its bit says the segment's ordinals follow that order. A format-13 reader
validates the section and serves queries as for any other segment; using the order for
early termination is D14's code.

## F19. Stored frames

Format 12's layout (`writer.hh:877-906`) with one reservation (R19): bit 31 of a frame
index entry's uncompressed size means "the frame's last record continues in the next
frame". Format 13 writers never set it and readers reject it set; the version that first
writes it carries this decoder with the bit accepted. The largest frame stays bounded by
`max_stored_frame` (`format.hh:78`), well below 2^31.

## F20. Manifest version 2

The manifest (`files.hh:30-31, 171-215`) changes with format 13, once:

- The slot table's generation file, `slots-<gen>.kv`, its generation, size, and CRC (D3,
  N1).
- The writing binary's structure identifiers (`inverted`, `keys`, `stored`, `values`, ...)
  and the lowest and highest segment format version among its segments (L16, S2).
- Per segment entry, a values generation, 0 for none (`workload-profiles.md` item 4); no
  code writes nonzero yet, and a reader rejects nonzero until field updates exist.
- The key prefix moves from the manifest's index fields to the `keys` structure's options
  (F8), still recorded in the manifest.

Version 1 manifests are rejected like format 12 segments (F2).

## F21. The digest at format 13

`layers.md` L8, unchanged: the `D` record carries only the core's fields (docid, version,
key as length u32 and bytes); each structure emits its own records (`inverted` a length
record for every document and its `P` records, `stored` a record of its bytes, `values`
its `V` records, D17); the core adds the slot-table record. The digest changes value once,
at format 13. A merge's output bytes are a function of its inputs, its order, and the
writer's presets alone, so replicas that merge alike write alike (T8).

## F22. Fixtures, fuzz bases, and tools

- **Fixtures,** pinned by tests and re-pinned with every format change on the branch:
  one per profile (`search`: every structure but `points`; `document`: `keys` and
  `stored`; `log`: `inverted` without `keys`; keyless and empty segments), one per slot
  type, one permuted (reordered) segment, and one with 64-bit skip offsets. `small.sx`
  and `permuted.sx` become format 13.
- **Fuzz bases:** `fuzz_segment` gains a base per profile and every section kind of F3,
  plus a mutation that drops, duplicates, or renumbers table-of-contents entries and
  flips structure bits (F4), with checksums refreshed so the changes reach the open rules.
  New decoders (`keys`, `key_index`, the advance and impact entries, `values`' directory,
  `term_lists`, `index_sort`) each get a corpus and one CPU-hour at the freeze commit.
- **Tools:** `strata_engine sections` learns F3's names; `strata_engine poststats` (F11's
  breakdown) and `strata_engine keysim` (F8's candidates over a segment's keys: bytes, and
  lookup time per key as the median of batches of 1,024) are added. They measure; they
  are not run on the branch for any claim.

## Gate rows

Branch rows (T1, T2, T7, T8, T9) are functional and run in CI on the branch. Measured
rows (T3–T6, T10, T11) run on `main` commits on the gate VM only (PLAN, "What waits"),
except the candidate selections of F1 item 2, which the lead schedules on the gate VM
from branch builds before the freeze and records in `docs/perf/` as candidate
comparisons, not gate results.

| Row | Target | Procedure |
|---|---|---|
| T1 Format rules | Every rule of F2–F6 has a test that builds a valid segment, breaks the rule, and expects the stated exception: each section removed (free: "no such structure"; mandatory, required, or flagged: `corrupt_segment`); each structure bit flipped; an unknown and a retired kind; entries out of file order; nonzero reserved words; versions 12 and 14 (`unsupported_segment`) | `test/segment_test.cc`, `test/format13_test.cc`, ctest on every CI platform |
| T2 Decoders | Every decoder F3 adds survives `fuzz_segment` for one CPU-hour at the freeze commit with ASan and UBSan, no finding open; the corpus replays in every CI build; every corruption check of T1 also reached by a resealed fuzz-base test | `fuzz/fuzz_segment.cc` built with `STRATA_FUZZ=ON`, `-fsanitize=fuzzer,address,undefined`, `-max_total_time=3600` per base |
| T3 Disk | After a reordered full merge of the 5M index: postings ≤ 1.0× keyed `-bp`'s `.doc`; the dictionary row (F8's accounting) ≤ 1.0× its `.tim`, `.tip`, `.tmd`; the whole index ≤ 1.0× keyed `-bp`'s; every other section no larger than format 12's 2026-10-17 figure; new sections reported | `strata_engine merge` with reordering, `strata_engine sections`, against `results/keyed-bp-n2.sizes` (`bench/night_p1.sh`, step 3) |
| T4 Queries | Per class and command, format 13 over format 12 ≤ 1.03 by the interval's upper bound, both indexes merged and reordered; the carried "after merging" rows reported against PLAN's ≤ 1.0×; blocks decoded and ns per touched block reported for `COUNT` intersections and unions | `bench/ab.sh` in-process A/B, each binary serving its own build, five trials, both query sets; `bench/phase2/gate.py` for the carried rows |
| T5 Keys | Key lookup at 1 and 50 segments ≤ Phase 3's T4 key-filter row (R6's check); indexing rows (1 and 8 threads, keyed) ≥ 1.0× the faster engine and ≤ 1.03 of format 12's time | `bench/index_trials.sh`; the key-lookup bench of Phase 3's T4 |
| T6 Seeds | `verify` clean at k = 10, 100, 1,000, both schemes, on one segment and 16, with and without deletes; union `TOP_k` rows with seeds over without ≤ 1.0 by the upper bound; seed bytes in the postings row | `strata_engine verify`; `bench/ab.sh` with seeds toggled by a preset |
| T7 Open | A fresh open touches only the header page and the tail run (F5), counted by the verified-region and page reads a test records | `test/format13_test.cc` with a counting `IO` |
| T8 Determinism | The fixtures' hashes are equal on Linux x86_64 and arm64, macOS, and FreeBSD; a reordering merge's output hash is pinned at `reorder_threads` 1 and 8 | CI on every platform |
| T9 Lifecycle | Phase 3's T1 crash points, T2's digest schedules, and T5's disk-full rows pass over format 13, with keys, values, and the `document` profile | `lifecycle_test`, `index_test`, `media_test` |
| T10 Memory | Serving memory on the merged 5M index ≤ format 12's; pinned bytes per open segment stated (the tail, `key_index`'s filter and FST, the slot directory) at 1,000 indexes per node (D18) | `bench/serve_mem.py`; a test that sums pinned bytes |
| T11 Cold start | First query after a dropped page cache and open time per GB, format 13 against format 12 (S1's claim shown, not assumed) | BENCHMARKS, "open time per GB" |

## Implementation order

The decisions least likely to change go first, each a small commit citing its F-number,
with tests and the fixtures re-pinned in the same commit:

1. F2 version handling and `unsupported_segment`; F3's kind table; F4's structure flags
   and F6's footer; F5's open rules (unknown kinds, file order).
2. F7's statistics split.
3. F8's `keys` and `key_index` with candidate K1, the writer's key path and the reader's
   lookup and cursor; then the index's key routing and the merge's key collection.
4. F9 under F4's presence rules (keyless segments).
5. The tools of F22, then F10, F11, F13 behind presets, each with its candidates.
6. F15, F17, F18, F19; F20 with the manifest; F21 with the layers branch.
7. F16 after D15.

**The layers branch.** `layers.md` L14 step 7 puts format 13 on the plugin interface.
This branch builds format 13 on `main`'s `segment` and `segment_writer`, keeping each
decision's encoding in its own function (as `docids.hh` is one unit for both, P1-R17) so
the plugins call the same code when the refactor resumes. Which branch rebases on which
is the lead's call.

## Risks

- F10 trades bytes for decoding: fixed-width entries may grow the carried postings row
  while they shrink the query row. T3 and T4 are read together, and a candidate that
  passes one by failing the other is not chosen.
- F8's codec trades lookup time for bytes (K3 decompresses a block per lookup); T5
  bounds it.
- F13's seeds may rarely help: k = 1,000 needs 1,000 postings to dominate a pair, which
  long, flat lists give a weak pair. T6 keeps them only if they win.
- Moving the key out of the term dictionary changes the digest, `verify`, and Xapiand's
  `Q`-term queries at once; T9 and the E2E suite cover the routing.
