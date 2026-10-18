# Format 13

Revision 1, amended for the re-check's findings ("Changes after the re-check"). It folds revision 0 (`1bc0a38`, amended by
the branch's F7 to F9 commits) together with its preliminary review
(`docs/reviews/format-13-design.md` at `4266374`: APPROVE WITH CONDITIONS, F13-R1 to F13-R17,
conditions C1 to C6), the owner's decisions recorded in PLAN (`PLAN.md:16-20, 34-41, 47-48` at
main `bcf89c8`), the system review's SYS-1, SYS-4, SYS-6, SYS-7, SYS-9, and SYS-16, and what two
live documents address to format 13: the automata review (`docs/reviews/automata-design.md` on
`phase5/automata` at `9b9e151`: AUT-R3, AUT-R4, AUT-R13, and its conditions C4 and C5) and the
query-gap design (`docs/design/query-gap.md` on `design/query-gap` at `8a6503f`, revision 0: M16
to M23). It sits on the layers design at revision 3 (`layers.md` at `a874ab0`;
`phase4/layers-refactor` at `707066b`, steps 0 to 7b), not on main's `segment`. Each of those
branches' worktrees was compared with `origin`: none holds an unpushed commit.

Format 13 is the one segment format version that carries every pending format change (PLAN,
"Carried rows"): the key dictionary as its own section (`phase-4.md` D12), the query gap's skip
entries (`docs/perf/query-gap-merged.md`), the postings change P1 leaves owed, Phase 4's doc
values, term lists, index sort, and optional sections (`phase-4.md` D2), reserved kinds for
points and for Track P's P8, P14, P18, and P20, and the format-13 items of the layers design.
One design, one review, one set of fixtures and fuzz bases. Decisions are F1 to F23; gate rows
T1 to T11; owner questions Q5 and Q6 (Q1 to Q4 are the review's, answered).

It consolidates; it does not reopen. Where `phase-4.md` or `layers.md` decided a point and its
review closed it, this document cites the decision and fixes its bytes. The open choices are
the key codec (F8) and the postings encodings (F10 to F13), which name candidates and the
measurement that chooses each. No size or speed below is a measurement of format 13: numbers
are format 12's (`docs/gates/phase-3-ratchets.md`, `docs/perf/`), or estimates, marked as such.

Code is cited at `phase4/layers-refactor` `707066b`, the base of step 7c, except `format.hh`
and `keys.hh`, which layers does not change and which are cited at this branch's last code
commit, `e779835`. Clean room, as `phase-4.md`: Xapian's behavior enters from its documentation
and `strata-oracle` vectors; Lucene and Tantivy from their documentation and javadoc.

## Changes from revision 0

| Item | Change | Where |
|---|---|---|
| SYS-1 | Format 13's decisions are bytes and codecs (`format.hh`, `keys.hh`, new codec headers); their open, build, merge, and query paths are layers' core, sink, and plugins. The branch's F2 to F9 commits are redone as layers' step 7c on this branch after its rebase onto `707066b` | "The rebase onto layers" |
| F13-R1 | A 16-byte prefix (magic, version, a CRC-32C of both), frozen in every version, read first by every open; at the index, the manifest's version range and structure mask classify; until F20, a manifest-named segment that throws `unsupported_segment` is corruption; layers' step-0 format-12 files kept byte for byte as refusal fixtures; `media_test` sweeps the prefix | F2, F6, F20, T1 |
| F13-R2 | Keys routed by prefix in the planner as a key leaf, never by a miss inside a segment; stage 1 and the `inverted` builder refuse a key-prefixed term; multi-term patterns over the key prefix expand over `keys`; `keys` holds the key's bytes exactly | F8 |
| F13-R3 | One per-kind table in `format.hh` (id, owner, requiring bit, class, rank) replaces `section_rule` and the reader's switch; plugins own bit masks (`inverted` owns `norms`; the core owns `index_sort`); a bit this binary cannot read is `unsupported_segment`, or by the manifest `unsupported_index` or corruption; a bit outside the table is corruption; T1's free-and-absent cases | F3, F4, T1 |
| F13-R4 | The tail holds only pinned sections, in S1's order; `lengths` leaves it; `inverted`'s statistics go to the head of `term_index` with no inner CRC; the layout is canonical; T7 asserts one tail run | F3, F5, F7, T7 |
| (designer) | `lengths` follows `terms` instead of closing the streamed sections, so the sink holds only pinned sections | F5; changes `layers.md` L2 |
| F13-R5 | The digest changes once, in step 7c's digest commit; the keys commit before it keeps an interim key record so `digest.txt` holds until then | F21, step 7c |
| F13-R6 | Key codecs crossed with the filter, eight candidates, one reader path, hits and misses at 1 and 50 segments; a decision rule; the projection restated with the filter (estimates); K2's ordinals through `find_run`; `key_index`'s count; the counterpart row | F8, F22 |
| F13-R7 | The carried rows are freeze criteria, on SYS-6 evidence; F10's expected share stated from the profile | F1, F10 |
| F13-R8 | One selection over F10 to F13 against the postings constraint; advance offsets relative to level 1's group base (always within 16 bits), level 1 keeping format 12's wide path; flags in a field of their own; F12's lasts only in flagged terms; validation per block decoded; losing candidates leave the format | F10 to F13, "Choosing F10 to F13 together" |
| F13-R9 | The writer refuses a nonzero version without a key in every segment; `check()` verifies it; at width 64 the minimum is 0 | F9 |
| F13-R10 | The writer refuses a key-prefixed term instead of dropping it; the seam's condition restated | F8 |
| F13-R11 | Closed on layers' side (step 7a, `d911396`); `merge_options::key_prefix` is deleted in step 7c's keys commit | Step 7c |
| F13-R12, Q2 | Every Track P item with an on-disk effect placed; P8, P14, P18, and P20 reserved (kinds 18 to 22, bits 8 to 12), with points, under one adding rule | F1, F3, F4, F23 |
| F13-R13 | `binary` columns defined; the column's own version byte dropped; bytes per dimension's mechanism stated (Q6); term-list frames bounded, keys left out of them; the sort count in LEB128; points by F23 | F15 to F18 |
| F13-R14 | Libraries' on-disk forms freeze with the format, with the rule for encoders whose output varies | F22 |
| F13-R15 | T1, T2, T3, T5, T7, and T11 restated; `poststats` projects candidate bytes instead of its seed counts; `keybench` for T5's baseline | Gate rows, F22 |
| F13-R16 | Done criteria K1 to K12 in the ledger | `docs/status/format13.md` |
| F13-R17 | Text corrected ("Why now", F8, F5, F20) | — |
| C6, Q3 | The rule for adding points, and every other reserved structure, within format 13 | F16, F23 |
| Q1 | The dictionary row is measured against keyed Lucene `-bp` | F8, T3 |
| Owner, automata C5 | The `keys` reader scans from a lower bound (`PLAN.md:47-48`), so an expansion with an empty literal seeks; layers' prefix-scan concept follows; AUT-R4's matcher test skips `keys` for a pattern that cannot match under the key prefix | F8; changes `layers.md` L3 |
| AUT-R13, automata C4 | The expansion class's tail-decode profile joins the freeze criteria and weighs E4 | F1, "Choosing F10 to F13 together" |
| Query gap | M16's layouts join F10's candidates; M17, M18, and M23 join the joint selection; a group's impact entries stay contiguous and a block's bound one read away; its E1 supplies F1's attribution | F10, F11, "Choosing F10 to F13 together" |
| (designer) | The manifest records a structure mask in place of plugin identifiers | F20; changes `layers.md` L16 |

## Changes after the re-check

The re-check (`docs/reviews/format-13-design.md`, "Re-check", at `bae7fc1`: APPROVE WITH
CONDITIONS) raised F13-R18 to F13-R26. Each is answered here in its own commit.

| Item | Change | Where |
|---|---|---|
| F13-R18 | Keyless segments arrive with the version rule in 7c-5; through 7c-4 every index keeps `keys` and writes `versions`, as format 12 does | F9, step 7c |
| F13-R19 | K3 is a `termdict` option (a block-body codec the caller supplies, with K4's parameters and K2's run columns), not a second dictionary beside it | F8, first steps |
| F13-R20 | `termdict` records its parameters and codec only when they differ from today's, in an options form a leading value above 255 announces; a dictionary with today's settings keeps today's bytes | F8, F22, first steps |
| F13-R21 | If M17 or M18 is kept, `inverted`'s statistics record the presets it assumes, and readers use a first or argmax pair only under a recorded preset | F7, F11, "Choosing F10 to F13 together" |
| F13-R22 | Points take 1 to 16 bytes per dimension (the owner's Q6): a `bytes` slot with points holds values of exactly its width, compared as unsigned bytes, so IPv6 addresses and uuids fit with no new column kind; Q6 answered | F15, F16, owed changes, owner questions |

## What this builds on, and what it changes

| Design | What revision 1 uses |
|---|---|
| `layers.md` revision 3 | L2's three orders and S1's tail; L3 (key semantics in the core, encodings in the plugin, routing by the planner, the key reader's concept, whose `scan` now takes a lower bound); L4 (`P::structure`, `P::structures`, the per-kind table at step 7); L5 (builders and the sink); L6 (merges, `merge_options`, the order source); L7 (handles, presence by requiring bit, unreadable bits); L8 (the digest split, once); L9 (statistics per plugin); L10 (`document::key`); L11 (`holds<P>()` per segment, step 7b); L12 (the document profile); L14 step 7c; L16; S1, S2 |
| `phase-4.md` | D2, D3, D3a, D4, D5, D11, D12, D14, D15, D17, and R19's reserved flag |
| `docs/reviews/phase-4-design.md` | R1, R4, R6, R7, R19, R20, N2, N3, M3 |
| `workload-profiles.md` | Items 1 to 4 (keys section, optional sections, real-time get, a values generation) |
| `reordering.md` | D5's tie rule (F13); the 14.5 MB ordinal projection (Risks) |
| `docs/reviews/automata-design.md` | AUT-R4 (keys in expansion; a lower-bound scan), AUT-R3 (termdict's additive `cursor::seek`), AUT-R13 and its C4 (the tail profile before the freeze), C5 (ruled by the owner) |
| `query-gap.md` revision 0 | M16 to M23 as format-13 candidates, decided by its E3 to E7 before the freeze (its D5); E1's attribution |
| `rocksdb-survey.md` | K3 (bounded, ordered key scans that skip segments by key range) and K5 (key-ordered merges), both on `keys`' scan |
| PLAN | Carried rows, "Every gate", Track P, "What waits", the owner's decisions |

Changes owed to other documents, each made with the commit that implements it or by that
document's owner, as the lead decides:

| Where | Change | Why |
|---|---|---|
| `layers.md` L2, format-13 column (`:200`) | `lengths` after `terms`, with `inverted`'s other streamed sections, not last among them | F5 |
| `layers.md` L3 (`:236-254`) | `key_reader::scan(from, f)` from a lower bound, with no prefix argument (the owner's ruling, `PLAN.md:47-48`) | F8 |
| `layers.md` L16 (`:862-893`) and L7 (`:568-573`) | The manifest records a structure mask, not plugin identifiers; "the manifest lists the bit's plugin" becomes "the manifest's mask holds the bit" | F20 |
| `phase-4.md` D2 (`:54-56`) | The freeze waits for no D15 result | F16, F23 |
| `frozen-index.md:46-49` | A frozen segment is format 13 with P20's structures under F23: a reader without them refuses it, it does not skip them | F23 |
| `BENCHMARKS.md:166-175` | `terms`, `term_index`, `keys`, `key_index` against `.tim`, `.tip`, `.tmd`; `docids`, `versions` against the id field's doc values | F8 |
| `limits.md:36` | Bytes per point dimension: 1 to 16 per slot (Lucene's 16), by F15a's declaration byte; a `bytes` slot's points compare its w-byte values as unsigned bytes | F15 (F13-R22) |
| `phase-4.md` D3a (`:143-144`) | "`bytes` ... never compared as values" excepts a slot's points, which compare its fixed-width values as unsigned big-endian bytes | F15 (F13-R22) |
| `limits.md:40` | Sort components: a LEB128 count, so "no count limit" holds in the format | F18 |
| `reordering.md:306-307` | The 14.5 MB projection is K2's to K4's ordinals | F8 |

## Why now

Format 12 is what Phase 3 measured (`phase-3-ratchets.md:198-211`, strata reordered against
keyed Lucene `-bp`): the whole index 0.999×, postings 1.014×, the term dictionary 1.20×. The two
misses are carried here. The query rows are carried too: 1.33× to 1.34× after merging at
`2e774db`, before reordering (`phase-3-ratchets.md:119-125`); reordering took 16% to 21% off
strata's global times in the quick tier (`phase-3-ratchets.md:324-343`), and the Phase 3 gate's
five-trial run re-records them. The one query profiled per block (`query-gap-merged.md:24-28`,
`+time +for +kids`, `COUNT`) spends about 23% of its time in skip-entry work (`decode_group`
14%, `block_index_for` 6%, `skip_sized_impacts` 3%), at about 300 ns per touched block against
roughly 90 ns for Lucene; removing all of that work would leave about 230 ns. F10 removes part
of that share, not the gap. Phase 4's sections need a format regardless, and every format change
after format 13 costs every plugin a reader for N − 1 (F2).

## F1. Scope and freeze

**Scope.** F2 to F23, and nothing else. Track P's items, each placed (F13-R12):

| Item | On disk | In format 13 |
|---|---|---|
| P3 variable block-max partitions | Impact partitions | F12's per-term flag; no bytes at P0 |
| P4 Elias-Fano, runs | A per-block ordinal codec | F11's codec codes, kept only if chosen together (F10 to F13) |
| P5 next-word entries | — | No hook if its entries are terms under a host prefix, as Lucene's common-grams filter emits bigrams; a structure of its own would need format 14 or a reservation the owner adds |
| P8 phrase prefilters | Per-block pair summaries | Reserved kind 18, bit 8 (F23) |
| P13 positions | A per-block positions codec | F14, only if it wins before the freeze |
| P14 stored dictionaries | A trained zstd dictionary per segment | Reserved kind 19, bit 9 (F23) |
| P18 holes | Marks of punched `stored` regions, if they are segment bytes | Reserved kind 20, bit 10 (F23) |
| P20 frozen indexes | Quantized impacts; a static key hash | Reserved kinds 21 and 22, bits 11 and 12 (F23) |
| P21 threshold seeds | Per-term dominance pairs | F13, chosen with F10 to F12 |
| P1, P12, P15 | Format 12's | Unchanged |
| P2, P6, P7, P9 to P11, P16, P17, P19 | None (P17 changes merge output bytes, not the format: L6) | — |

**The freeze.** Format 13 freezes once, when all of these hold on the freeze candidate, a
branch build:

1. This design, revised, clears a full design review with its code (PLAN, "What waits").
2. The candidates of F8 and F10 to F13 are chosen by their rules, each kept and losing
   candidate's table in `docs/perf/` (C3), and every losing candidate's decoder and hook is
   removed.
3. The carried disk rows, after a reordered full merge of the 5M keyed index (section sizes
   are exact, so a branch build's are the gate's): postings ≤ 1.0× keyed `-bp`'s `.doc`
   (833.7 MB, `phase-3-ratchets.md:204`); the dictionary row (`terms`, `term_index`, `keys`,
   `key_index`) ≤ 1.0× keyed `-bp`'s `.tim`, `.tip`, `.tmd` (75.8 MB, `:206`; the owner's
   reference, `PLAN.md:38-39`); the whole index ≤ 1.0× keyed `-bp`'s 2,542.5 MB (`:203`); every
   other section no larger than format 12's figure (`:201-208`).
4. The carried query rows, after merging: per class and command, the projection (the class's
   branch A/B ratio of format 13 over main's format 12, times format 12's recorded ratio at the
   Phase 3 gate's commit, over the full gate query set, not the quick sample) ≤ 1.0 by its upper
   bound, the product of the two intervals' upper bounds. A class that misses needs a profile
   that attributes the remaining time to code outside the format, each part assigned to a named
   Track P item with its projected effect and closed before Phase 4's gate; the query-gap
   design's E1 is that attribution (`query-gap.md`, D3). No part of a miss is attributed to the
   format at the freeze.
5. T4's guard (format 13 over format 12 ≤ 1.03 by the upper bound, every class) and T5's key
   lookups.
6. The expansion class's tail profile (AUT-R13, automata C4): a branch profile of prefix,
   wildcard, and fuzzy queries on the 5M segment, `or` and count modes, states the share of
   their time in tail decoding, and E4's decision weighs that class when it dominates. Phase 1's
   review left the question to "the format version in Phase 5"
   (`docs/reviews/phase-1.md:239-244`), and format 13 is that version.
7. F22's fixtures and fuzz bases pinned at the freeze commit, one CPU-hour per decoder (T2), and
   all eight CI jobs green at that commit (C2).

The evidence for 2 to 6 is the owner's SYS-6 kind (`PLAN.md:19-20`): branch A/Bs under a hold
the lead grants and the section sizes of a branch build, recorded in `docs/perf/` as candidate
tables, never as gate rows. The gate rows run on main at landing and confirm them; since a miss
there costs format 14, every projection states its interval. D15 is no longer a freeze criterion
(F16, C6).

Until the freeze, the branch writes format 13 with candidate encodings, fixtures are re-pinned
with each change, and no index outside tests holds format 13.

## F2. Version and compatibility

- The header's version is 13 (`format.hh:68`); a reader accepts versions in
  [`min_read_version`, `version`], both 13 today.
- **The prefix, frozen in every version.** Bytes 0 to 15 of every segment from format 13 on are
  the magic `STRSEG01`, the version u32, and a CRC-32C of bytes 0 to 11 (F6). Every open reads
  them first, through one `format::read_version(view)`:
  1. the magic wrong: `corrupt_segment`;
  2. the CRC good: a version in range opens; one outside throws `unsupported_segment`;
  3. the CRC bad, the version below 13, and bytes 12 to 15 zero: `unsupported_segment`, since
     format 12 and earlier wrote a zero flags word there (main's `format.hh:138-144`);
  4. the CRC bad otherwise: `corrupt_segment`.
  A flipped version byte is damage under rule 4, at every level, and a real format-12 file is
  refused under rule 3. Format 13's prefix CRC is `0x4ff2df7b` (computed from the bytes), not 0,
  so a flip from 13 to 12 cannot pass for rule 3.
- `read_version` runs before `decode_footer` in `core_segment`'s constructor
  (`reader.hh:101-103`), `load_segment` (`index.hh:2077-2091`), and `written_segment`
  (`index.hh:835-836`), so another version's footer never needs to parse (F13-R1).
- **At the index** (F20, L16 as F20 revises it): the manifest's version range and structure mask
  refuse an index this binary cannot read with `unsupported_index` before any segment opens. A
  segment whose version lies outside the manifest's range, or whose structure bits are not in
  its mask, is `corrupt_segment`, retried and healed like any damage. Until F20 lands (step 7c's
  manifest commit), `load_segment` turns a manifest-named segment's `unsupported_segment` into
  `corrupt_segment`, since a manifest names only segments its own binary wrote.
- **Format 12 is refused.** No index exists outside tests and benchmark runs, and every Phase 3
  gate index is rebuilt (R20). The format-12 fixtures at `707066b` (main's `small.sx`,
  `permuted.sx`, `manifest.bin`, `live.bin`, and `meta.bin`, and layers' step-0 segments
  `stored-frames.sx`, `keyless.sx`, `wide-skips.sx`, `merge-two.sx`, `merge-deletes.sx`, and
  `empty.sx`) move unchanged to `test/fixtures/format12/` as real format-12 files: each segment
  is refused by `segment` with `unsupported_segment`, and an index over them is refused as
  corruption before F20 and with `unsupported_index` after it, when the version-1 manifest is
  itself refused.
- `media_test`'s sweep (`media_test.cc:100-101`, a stride of size / 48 from offset 0) gains bytes
  0 to 15 of every segment: a transient flip there is retried and healed; a sticky one fails as
  damage.
- **The promise.** From format 13 on, a reader carries N − 1 (R20; L7, S2): a format-14 binary
  opens format-13 segments with their own decoder, and the writer writes only N. Section versions
  follow the segment's version (S2); plugins have none. The prefix is what lets each binary tell
  a newer segment from a damaged one without its manifest.

## F3. Section kinds: one table

One table in `format.hh` holds, per kind, its id, name, owner (the core or a plugin's `id`), the
structure bit that requires it (none: mandatory), its verification class, and its file rank. The
core's open, the sink, the structure check, and each plugin's `rules` read it: `P::rules` becomes
the rows whose owner is `P::id` (L4). It replaces `section_rule` (`section_io.hh:222-226`), the
core's and plugins' rule arrays (`reader.hh:146-150, 244-251, 454-457, 544-546`), `rule_of`
(`format.hh:135-162` at `e779835`), and revision 0's pinned switch, so no second copy of a kind's
class or presence exists.

| Rank | Kind | Id | Owner | Requiring bit | Class | Holds |
|---:|---|---:|---|---|---|---|
| 0 | `header` | 0 | core | mandatory | pinned | F6 |
| 1 | `postings` | 3 | `inverted` | `inverted` | regions | F10, F11 |
| 2 | `positions` | 4 | `inverted` | `inverted` | regions | Positions and counts |
| 3 | `terms` | 1 | `inverted` | `inverted` | regions | Dictionary blocks |
| 4 | `lengths` | 5 | `inverted` | `inverted` | first use | Exact lengths only (F7) |
| 5 | `keys` | 12 | `keys` | `keys` | regions | Key blocks (F8) |
| 6 | `stored` | 6 | `stored` | `stored` | regions | Frames and their index (F19) |
| 7 | `values` | 14 | `values` | `values` | regions | Slot directory and columns (F15) |
| 8 | `term_lists` | 16 | `term_lists` | `term_lists` | regions | Each document's terms (F17) |
| 9 | `docids` | 7 | core | mandatory | pinned | Unchanged from format 12 |
| 10 | `stats` | 8 | core | mandatory | pinned | The document count (F7) |
| 11 | `index_sort` | 17 | core | `index_sort` | pinned | The sort description (F18) |
| 12 | `norms` | 9 | `inverted` | `norms` | pinned | One `encode_length` byte per document |
| 13 | `term_index` | 2 | `inverted` | `inverted` | pinned | Statistics, then the dictionary's index (F7) |
| 14 | `versions` | 10 | `keys` | `keys` | pinned | F9 |
| 15 | `key_index` | 13 | `keys` | `keys` | pinned | Count, range, filter, the key blocks' index (F8) |
| — | retired | 11 | — | — | — | Format 12's `pk_filter`; corruption wherever it appears |
| reserved | `points` | 15 | `points` | `points` | by its first release | F16 |
| reserved | `pair_summaries` | 18 | `inverted` | `pair_summaries` | by its first release | P8 |
| reserved | `stored_dictionary` | 19 | `stored` | `stored_dictionary` | by its first release | P14 |
| reserved | `stored_holes` | 20 | `stored` | `stored_holes` | by its first release | P18 |
| reserved | `quantized_impacts` | 21 | `inverted` | `quantized_impacts` | by its first release | P20 |
| reserved | `key_hash` | 22 | `keys` | `key_hash` | by its first release | P20 |

- **Classes.** Pinned sections are checked whole at open; regions sections per region on first
  read (`reader.hh:116-124`); `lengths` whole on its first use.
- **Ranks follow S1** (L2): the header; then each plugin's streamed sections in layout order;
  then the tail: the core's pinned sections, then each plugin's in layout order. A reserved
  kind's class and rank are fixed by the release that first writes it (F23), placed by the same
  rule: a pinned kind after its owner's pinned sections, any other after its owner's streamed
  ones. Since no format-13 segment holds a reserved kind before that release, placing it then
  moves no existing section, and every earlier segment stays canonical.
- Kinds are stable numbers: a later format adds kinds and never renumbers. Kind 11 stays retired,
  so a format-12 segment's `pk_filter` cannot be read as anything else.
- The section count bound stays 64 (`format.hh:289`), above the 23 ids.

## F4. Structure flags

The footer's structures word (F6) records which structures the segment was built with, one bit
per structure. A plugin owns a mask of bits (`P::structures`, L4): its own bit, which `holds<P>()`
reads (L11), and its optional sub-structures.

| Bit | Value | Structure | Owner | Requires | State |
|---:|---:|---|---|---|---|
| 0 | 1 | `inverted` | `inverted` | — | defined |
| 1 | 2 | `norms` | `inverted` | `inverted` | defined |
| 2 | 4 | `keys` | `keys` | — | defined |
| 3 | 8 | `stored` | `stored` | — | defined |
| 4 | 16 | `values` | `values` | — | defined |
| 5 | 32 | `points` | `points` | — | reserved (F16) |
| 6 | 64 | `term_lists` | `term_lists` | — | defined |
| 7 | 128 | `index_sort` | core | `values` | defined |
| 8 | 256 | `pair_summaries` | `inverted` | `inverted` | reserved (P8) |
| 9 | 512 | `stored_dictionary` | `stored` | `stored` | reserved (P14) |
| 10 | 1024 | `stored_holes` | `stored` | `stored` | reserved (P18) |
| 11 | 2048 | `quantized_impacts` | `inverted` | `inverted` | reserved (P20) |
| 12 | 4096 | `key_hash` | `keys` | `keys` | reserved (P20) |
| 13 | 8192 | `stored_spans` | `stored` | `stored` | proposed (Q5) |
| 14 to 31 | | none | | | zero |

`index_sort` requires `values`, since D14's order is read from slot values at merge (L6's order
source). Bits 13 to 31 stay zero unless the owner answers Q5.

**What a set bit means at open** (F13-R3; C6 rule 4):

- A bit whose structure this binary reads: its sections open by the presence rules below.
- A defined or reserved bit whose structure this binary lacks (a reserved bit before its release,
  or `inverted` in a binary built with `document_layout`) is never read past and never called
  corruption: a lone segment throws `unsupported_segment`. In an index, F20's mask decides: a
  binary refuses an index whose mask holds the bit with `unsupported_index` before any segment
  opens, and a segment that sets a bit its manifest's mask lacks is corruption, since the manifest
  vouches for what its segments hold.
- A bit outside the table: corruption. No format-13 writer sets one, and the footer's CRC rules
  out a media flip.
- A sub-structure's bit without its owner's own bit, or `index_sort` without `values`:
  corruption.

**Presence** (L7): a section is present exactly when the bit that requires it is set; mandatory
sections always are. `positions` belongs to `inverted` and is written empty when no term has
positions; `versions` is `keys`' (R1). A plugin whose own bit is clear has none of its sections,
and its reader opens empty and answers "no such structure": a segment without `inverted` has no
terms (`term` returns nothing, `length` is 0); without `keys`, `version(ord)` is 0 and `may_hold`
false; without `stored`, every record is empty. A segment with only `keys` and `stored` is the
`document` profile (L12). At open, a clear bit with any of its sections present, a set bit with
one absent, a mandatory section absent, a kind outside F3's table, or the retired kind is
corruption. This closes format 12's acceptance of any kind (main's `format.hh:222`).

**Who sets bits.** The writer sets them from the index's configured structures, never from the
documents, at flush and at merge alike: a configured `stored` structure whose run has no records
still writes its empty section and sets its bit, and a merge's output takes the index's
configuration at the merge, as a flush does. Revision 0's "a merge's output is keyed when any
input is" (`merge.hh:157` at `e779835`) becomes this rule; a key prefix is fixed at an index's
creation and checked at open (`index.hh:229, 1999-2000`), so the two agree for `keys`.

`holds<P>()` reads a per-segment mask the core computes at open (layers step 7b, `a86610e`,
`reader.hh:769-791`); step 7c sets it from the footer's bits. T1 gains the free-and-absent
cases no test covers today: `stored` absent with its bit clear (opens, empty records); `norms`
absent with its bit clear; `inverted` absent with its bit clear, which layers' reader still
refuses (it requires `lengths` and `norms`, `reader.hh:262-271`); and a `document`-profile
segment.

## F5. Table of contents and file order

- The table keeps format 12's entry layout (main's `format.hh:156-168`): kind u16, region shift
  u16, offset u64, length u64, CRC u32, then the region CRCs. Each kind at most once.
- **One encoding per segment** (F13-R4): the header's entry at offset 0 with length 36 (F6);
  every later entry at the previous entry's end rounded up to 64 bytes (`section_alignment`,
  `format.hh:87`), in F3's rank order; the table at the last section's end rounded up to 64. This
  is arithmetic on the decoded table, with no extra read, so gaps, overlaps, and unaligned
  offsets all fail as corruption. Padding is written as zeros and is under no checksum; no
  reader reads it.
- **File order** (S1): the header; the streamed and first-use sections in layout order
  (`postings`, `positions`, `terms`, `lengths`; `keys`; `stored`; `values`; `term_lists`); then
  the tail: `docids`, `stats`, `index_sort`; `norms`, `term_index`; `versions`, `key_index`;
  then the table of contents and the footer. An open reads the header page and one contiguous
  run ending at the footer (T7).
- **`lengths` follows `terms`.** F13-R4 and L2 (revision 3) put `lengths` last among the
  streamed sections. With the statistics moved out (F7), an open reads nothing of `lengths`, so
  its place costs the open nothing either way; placing it last would make the sink hold its
  encoded bytes (10.1 MB at 5M, `phase-3-ratchets.md:191`) across `keys`' and `stored`'s
  sections, charged to the flush (D17), for no reader gain. Here each plugin's streamed sections
  stay contiguous and in layout order, and L2's format-13 column changes to match.
- **The sink places by rank** (L2, L5): `section_sink` (`section_io.hh:246-302`) looks up each
  section's class in F3's table. A streamed section is written when its plugin produces it and
  must arrive in rank order (a logic error otherwise). A pinned section is held, its bytes
  charged as `append_built` charges them now (`writer.hh:99-104`), until the core's finish,
  which writes the held sections in rank order, then the table and the footer
  (`finish_segment`, `writer.hh:114-133`, gains the structures word from the builders' `built`).
  Plugins never see offsets (L5).

## F6. Header and footer

```
header (36 bytes)  [magic "STRSEG01"][version u32 = 13][CRC-32C of bytes 0-11 u32]  (F2's prefix)
                   [flags u32 = 0][segment id 16 bytes]
footer (56 bytes)  [toc offset u64][toc length u64][toc CRC u32][doc count u32]
                   [min docid u64][max docid u64][structures u32][reserved u32 = 0]
                   [CRC-32C of the preceding 48 bytes u32][magic "SEND"]
```

- The first 16 bytes are frozen in every version (F2); bytes 16 on are version 13's. A CRC over
  the frozen bytes only, not the whole header, leaves the rest of the header free to change in a
  later version. The header still sits in its 64-byte slot, so it costs no disk byte.
- The header's flags word and the footer's reserved word must be zero; nonzero is corruption.
- The structures word is F4's. The footer grows from 48 bytes (main's `format.hh:63`) to 56; its
  layout is version 13's alone, since every reader reads the prefix first.
- The segment id keeps format 12's meaning (I13).

## F7. Statistics

L9 (revision 3): each structure's statistics live in a pinned section of its own.

- `stats` (the core's): [document count u64], 8 bytes against format 12's 28 (`reader.hh:132`).
- `inverted`'s statistics, [total length u64][minimum length u32][maximum length u32][maximum
  positions u32], 20 bytes, lead `term_index`, before the dictionary's index. `term_index` is
  pinned, present whenever `inverted` is, checked whole at open with its section CRC, and copied
  out with the index (`reader.hh:272-274`). There is no inner CRC: the section's covers them, and
  the fuzzer's resealing reaches the total length and the positions bound that
  `positions_reader` relies on (F13-R4). Revision 0's 24-byte `lengths` header and its CRC go.
- **The presets M17 and M18 assume** (F13-R21), only if either is kept: the statistics end with
  [preset count u8][per preset: scheme u8 (0 `bm25`, 1 `bm25_xapian`, `search.hh:48`), k1, b, and
  `min_normlen` as IEEE-754 binary64, little-endian], 26 bytes for one preset. M17's order uses
  the first; M18's argmax pairs follow the list's order. The writer records the index's presets at
  the flush or merge, never its inputs', as it sets structure bits (F4). A reader uses a list's
  first pair, or a block's argmax pair, only when the query's scheme and parameters equal a
  recorded preset bit for bit; under any other it evaluates every pair, as today. An index
  option can change after a segment is written, and without this record the segment could not
  say which preset its order assumed. A count of 0 or a scheme code above 1 is corruption. If
  neither is kept, the statistics stay 20 bytes.
- `lengths` holds only [width u8][bit-packed lengths], verified whole on its first use
  (`reader.hh:296-308`). The default scheme reads norms only and never touches it; an open reads
  none of it.
- A segment without `inverted` has no statistics: its total length is 0, which the manifest's
  totals accept (L9).

## F8. The key dictionary (D12; R6)

**The key leaves the term dictionary.** The key is the core's field (`document::key`, L3, L10),
held in `keys` exactly as operations name it (`o.key`; for Xapiand the `Q` prefix included). The
core never adds or strips a prefix, and `document::key` holds the same bytes (F13-R2).

- **The key prefix is the `keys` structure's option**, recorded in the manifest (F20). Every key
  starts with it, as stage 1 already requires of a key operation's key (`index.hh:1576`).
- **Terms and keys are disjoint by construction.** Stage 1 rejects a document whose `inverted`
  part holds a term with the key prefix (`inverted_plugin::validate` reads the prefix from its
  `validate_context`), and the `inverted` builder refuses one with `std::invalid_argument`
  (F13-R2, F13-R10: refuse, never drop); `check()` verifies that the dictionary holds none.
  Revision 0's stage-1 rule for a boolean key term (`index.hh:1533` at `e779835`) belonged to the
  key as a term and goes with it. In a layout without `inverted`, the prefix may be empty.
- **Routing is the planner's** (L3): the term table (`searcher.hh:116-177`) and `snapshot_df`
  (`searcher.hh:104-110`) resolve a term with the key prefix through `keys_plugin::reader` as a
  key leaf: an iterator over at most one ordinal (`keys::reader::find`, deleted copies included,
  which the live filter removes as for any term), wdf 0, no positions, and an upper bound of 0
  under both schemes. It never asks `inverted`. Its df counts the segments whose `keys` hold it,
  deleted copies included, as format 12's postings did, so `verify` and `parity` see the counts
  and matches format 12's key term gave them. No reader falls back to another, and
  `keys_plugin::depends_on` is empty.
- **Multi-term expansion** (L3; SYS-9; AUT-R4): a pattern whose literal prefix starts with the key
  prefix expands over `keys` only; a pattern whose literal prefix is a proper prefix of the key
  prefix, the empty one included, expands over both readers and merges in byte order, unless the
  matcher, run over the key prefix's bytes, reports a dead prefix within them, which skips `keys`
  in a few steps (AUT-R4's refinement); any other expands over `inverted` only. Over `keys`, the
  expansion runs the same matcher and seek targets as over the dictionary (automata D6), through
  the lower-bound scan below, so `keys` needs no intersect of its own and any K candidate serves.
  Expansion limits count both sources, and each expanded key is a key leaf. Xapiand's index
  listing, a wildcard over its id prefix (Xapiand's `src/server/search_views.cc:2292-2294` at
  `7e8cccb7d`), finds its keys again. Format 13 provides the key leaf and the scan; automata's
  `expand.hh` runs the rule and its keys-equivalence row (AUT-R4, fixes 3 and 5).

**The reader's interface** (L3 as revised; the owner's ruling, `PLAN.md:47-48`):

```cpp
bool may_hold(std::string_view key) const;  // the range, then the filter if kept
std::optional<ordinal_t> find(std::string_view key, live_bits live) const;  // its ordinal, if live
std::uint64_t version(ordinal_t ord) const;  // 0 in a keyless segment
// f(key, ord) for every key >= from, in order, deleted copies included, until f returns false.
template <class F> void scan(std::string_view from, F&& f) const;
```

- **`scan` takes a lower bound, not a prefix.** A prefix walk is a scan from the prefix that stops
  at the first key past it; an expansion re-seeks by returning false and scanning again from the
  matcher's next target, so a pattern with an empty literal skips whole key ranges instead of
  walking every key (AUT-R4: under `obama~2`, a key whose two bytes after `Q` are none of `o`,
  `b`, `a`, `m` is dead at its third byte). `keys.hh:186-195` already has this form; layers'
  concept (`layers.md:236-249`, a prefix with format 12's `deps`) changes to it, and format 13's
  `deps` is empty.
- **A seek's cost.** Over K1, K2, and K4, a scan starts through `termdict::reader::scan(from)`,
  which builds every key of `from`'s block below it (`termdict.hh:479-493`); automata's additive
  `termdict::reader::cursor::seek(from)` (AUT-R3, O2) makes it one restart run. K3 takes the
  same path and decompresses the block first (F13-R19).
- **The key leaf** calls `find` with no live bits (`live_bits{}`, every ordinal live), so a deleted
  copy reaches the scorer's live filter as any term's posting does; `lookup_key` passes the
  segment's live bits and takes the first live copy, newest segment first (L3).
- **Bounded scans** (the RocksDB survey's K3, `rocksdb-survey.md:208`): the core skips a segment
  whose `key_index` range (`keys.hh:131-133`) misses the scan's bounds and stops each segment's
  scan at the upper bound in its callback, so every candidate keeps the range, with or without
  the filter. The same scan serves delete by key range and K5's key-ordered merges (L6's order
  source).
- **The seam's condition** (F13-R10, for Phase 7): the seam inventory confirms that Xapiand's only
  term with the key prefix is its id term, which the seam sends as `document::key`. At Xapiand
  `7e8cccb7d` (its MIT files), the id field is boolean (`src/database/schema.cc:9157-9159`) with
  no position by default (`schema.cc:2136, 5305-5310`), and accuracy prefixes come from
  `get_prefix(acc)` (`schema.cc:1603-1611, 4847`), which with hashed prefixes is a serialised
  field number (`src/database/utils.cc:71-78`) whose first byte can be any byte: the inventory
  checks that none of their terms leads with the key prefix, or the index takes a longer prefix.

**Layout** (F13-R6, the key count included):

```
keys       the candidate's key blocks (below); every key maps to its ordinal
key_index  [key count LEB][with keys: min key (LEB length, bytes), max key (likewise),
           filter length LEB, fusefilter filter8 over xxHash64 seed 0 (I12)][the blocks' index]
```

- One copy per key per segment: the writer rejects a repeated key (`keys.hh:64`), and a cursor
  checks that keys strictly increase and match the count (`keys.hh:170`).
- Versions come from `versions` only (R6); a key entry holds no version.
- Lookups: `may_hold` checks the range, then the filter when the candidate keeps one; `find`
  floors the blocks' index and decodes one block, verifying its regions first.
- Range scans merge per-segment cursors in key order, skip deleted ordinals, and read one
  snapshot (R6). The merge collects keys through the reader's `scan` from the empty key, with
  empty deps (`merge.hh:257-272` scans the key prefix with `inverted`'s deps today).

**Codec candidates**, each with the filter (F) and with the range alone (R): eight in all.

| Id | Key text | Ordinals | Library change |
|---|---|---|---|
| K1 | `termdict` blocks as built: restart 32, 32 to 64 entries, alphabet-coded front coding | LEB128 in each entry's value (`keys.hh:74-78`) | none |
| K2 | As K1 | Each restart run's ordinals bit-packed at `bit_width(doc_count − 1)` in the run's first value, the other values empty | `termdict`'s builder cuts blocks only at run boundaries (an option; readers unchanged) |
| K3 | `termdict` blocks with compressed bodies: up to 128 entries within 64 KiB of raw block bytes, each body one zstd frame; a body over 64 KiB, one oversize key's, stored raw | As K2 | `termdict`: K4's parameters, and a block-body codec the caller supplies (an option; today's bytes when off) |
| K4 | As K2 with restart 64 and 48 to 96 entries per block | As K2 | `termdict`: restart interval and block bounds as parameters, recorded in its index only when they differ from today's (F13-R20) |

- **K2's ordinal needs no new `termdict` call.** `find_run` already calls its visitor for every
  entry from the restart through the key (`termdict.hh:463-477`), the mechanism term infos use
  for their run cursor (`format.hh:482-485`); the first value is the run's column, and the count
  of entries visited gives the key's index in it. A cursor keeps the last restart's column. The
  builder option keeps a block cut from ending a run early, which would leave unread ordinals in
  that run's column.
- **K3 is a `termdict` option, not a second dictionary** (F13-R19). It keeps `termdict`'s FST
  over first keys, its blocks, front coding, and restart runs, so `find_run`, `scan`, and
  automata's `cursor::seek` serve it, with one transform between a block's length and its
  contents: [raw length LEB, 0 when stored raw][the frame, or the raw bytes]. `termdict` takes
  the codec as a pair of functions, as it takes its verifier (`termdict.hh:452`), so it links no
  compressor; strata passes `compressors`' zstd. The builder cuts before an entry that would take
  a block's raw body past 64 KiB, so a larger body holds one entry and is stored raw. A lookup
  decompresses one block into a buffer its caller owns, through an added overload; the existing
  calls refuse a dictionary with a codec. Terms and keys then share one block codec, one seek
  path, and one fuzz target, `termdict`'s own (`fuzz/fuzz_reader.cc`), and P12's dictionary can
  be measured with the same option.
- **`termdict`'s index keeps today's bytes at today's settings** (F13-R20). Its index opens with
  the alphabet's size as a LEB128, at most 255 (0 for all 256 bytes), and today's reader refuses
  a larger value (`termdict.hh:198-215`). A value v above 255 announces the options form: v mod
  256 is the alphabet's size code, and v / 256 is a nonzero flags word, where bit 0 means
  [restart interval LEB][minimum entries LEB][maximum entries LEB][target block bytes LEB]
  follow, and bit 1 means the blocks carry F13-R19's body transform. The builder writes the
  options form only when a parameter differs from today's (32, 32, 64, 1,024) or a codec is set,
  so K1's and K2's dictionaries, `terms` included, are byte for byte today's, and a reader
  without the options refuses the form instead of misreading it. A block's restart count stays
  bounded by its own bytes, as its lengths are, so the parameters need no cap.
- **K3 allocates within C9's bound.** A compressed block holds at most 64 KiB of raw keys
  (`stored_block_size`), so decompressing one allocates no more than a stored frame does
  (`format.hh:76-86`), and a key over that is stored raw and read in place: no key limit below
  term bytes' 2^32 − 1 is needed.
- **Every candidate is a choice the writer makes alone**; the losers leave the format before the
  freeze (F1).

**How the candidates are measured** (F13-R6): `keysim` (F22) over the keys of a branch build's
reordered 5M merge:

1. Bytes: each candidate's `keys` and `key_index` with the same fields (blocks, index, count,
   range, and the filter when kept).
2. Time: through the same reader path, mapped and region-verified, `may_hold` then `find`, at 1
   segment and through `lookup_key` over a 50-segment keyed index of the same corpus; on hits
   (the index's keys, shuffled) and on misses (each hit key with one byte replaced by another
   byte of its alphabet, so the range check passes and the filter or a block decides). The median
   of batches of 1,024, as `keysim` times today.

**Rule:** among the candidates whose lookups meet T5 and whose dictionary row meets T3, the
smallest; within 1% of its size, the faster on misses at 50 segments, the common case of an
upsert. If none meets both, the candidate report says so and the owner decides before the freeze.

**Where the bytes go today, as estimates.** No candidate is measured. Against keyed `-bp`'s
75.8 MB (`phase-3-ratchets.md:206`), with the unkeyed dictionary at 39.9 MB in docid order
(`:245`), before the key blocks' index:

| Part | Bytes at 5M | Source |
|---|---:|---|
| Key text, K1 and K2's front coding | about 24 MB | Estimate: the 52 MB the keys add today (`:232`), less 18.0 MB of ordinals, 5.0 MB of wdf, and about 5 MB of df and flags (`:238-240`) |
| Key text, K3's compressed blocks | about 10.6 MB | zlib level 6 as the stand-in (`:243-245`) |
| Ordinals, K1 (LEB128) | 18.0 MB | 3.58 bytes each (`:238-239`) |
| Ordinals, K2 to K4 | 14.5 MB | 5,032,102 keys at 23 bits; `reordering.md:306-307` |
| Filter | 5.7 MB | `phase-3-ratchets.md:190` |

On those estimates, K1F is about 87.6 MB (1.16×), K1R 81.9 MB (1.08×), K2F 84.1 MB (1.11×), K2R
78.4 MB (1.03×), K3F 70.7 MB (0.93×, the review's figure), and K3R 65.0 MB (0.86×); K4 has no
estimate. Only K3 passes with the filter, which is why the filter is a candidate and `keysim`
decides.

**Disk accounting** (F13-R6, Q1): the dictionary row counts `terms`, `term_index`, `keys`, and
`key_index` against keyed Lucene `-bp`'s `.tim`, `.tip`, and `.tmd` (the owner's reference,
`PLAN.md:38-39`), which hold Lucene's id terms; `docids` and `versions` keep the id field's doc
values, which the keyed builds carry as versions (`BENCHMARKS.md:130-131`). BENCHMARKS'
counterpart table changes with this (`BENCHMARKS.md:166-175`); keys count once (`phase-4.md`
D12), and Phase 4's "`keys` against the id field's terms" (`phase-4.md:498-499`) is this row.

## F9. Versions

Unchanged from format 12: [minimum u64][width u8][(version − minimum) at width bits]
(`keys_builder::encode_versions`, `writer.hh:682-700`), validated at open (`reader.hh:501-508`).
Present exactly when `keys` is. A keyless segment has no `versions`, and `version(ord)` is 0 (D2,
M3).

- **A version belongs to a key.** A document without a key has version 0 in every index, and its
  verdict reports 0: otherwise a docid-addressed replace would read `current + 1` from the buffer
  and 0 after a flush, and replicas flushing at different times would disagree. Conditional
  operations on a keyless document reject in stage 1 (M3).
- **The writer enforces it** (F13-R9): the `keys` builder refuses a nonzero version without a key
  in every segment (`std::invalid_argument`), and `check()` verifies it. Today it refuses only in
  keyless segments (`writer.hh:571` at `e779835`), and layers' keyless fixture writes `1 + d % 3`
  for keyless documents (`fixtures_test.cc:79, 101`); that fixture is re-pinned in the digest
  commit (F21).
- **Keyless segments and this rule land together** (F13-R18): in step 7c's digest commit
  (7c-5). A keyless segment without the rule would read 0 after a flush for a document the
  buffer holds at version 1, or at `current + 1` after a docid replace, so verdicts and the
  digest would depend on flush timing. Until that commit every index is configured with `keys`
  and writes `versions` for every document, as format 12 does.
- **Width 64.** The writer writes minimum 0 when the width would be 64, so every file it writes
  passes the reader's range check (`reader.hh:506`, `lo ≤ 2^64 − 2^width`). Today a writer can
  write a 64-bit span with a nonzero minimum, which the reader refuses (the layers ledger's
  pre-existing finding).
- **The structure is the index's** (F4): a keyed segment with no keyed document writes empty
  `keys` and `key_index` sections and a `versions` section of width 0.

## F10. Skip entries for the count and advance paths

Format 12's level 0 is one LEB128 stream per group of 32 blocks: per block the last ordinal's
delta, the byte length with the bitset flag, the positional deltas, and the block's competitive
pairs (`writer.hh:772-855` at `e779835`, byte for byte `inverted_builder` at `707066b`).
Advancing to a block decodes the group's whole stream, impacts included (`decode_group`,
`reader.hh:846-890` at `e779835`). The count and advance paths never read impacts.

**The change.** Per group, level 0 splits into:

1. **Advance entries**, fixed width, one per block: the block's last ordinal and the offset of
   its bytes from the group's blocks offset in level 1. A block's byte length is the next entry's
   offset minus its own; the group's last block ends at the next group's base, or the term's
   tail.
2. **Flags**, a field of their own (F13-R8): the block's ordinal codec (FOR or bitset, and E2's
   and E3's codes if kept) and E1's bit if kept. Its width follows the kept set: one bit per block
   when only FOR and bitset remain, a byte per block otherwise.
3. **Impact entries**, read only by scoring paths: each block's pairs in `encode_sized_impacts`'
   form (`format.hh:413-427`), located by a fixed-width offset per group in level 1.
4. **The positional stream**: the first position index and the position and count block offsets
   as deltas restarting per group, read only by phrase paths.

**Level 1 keeps the wide path.** One fixed entry per group holds its last ordinal and its blocks,
advance, impact, and positional offsets, at u32, or u64 for a term that needs them, as format 12
decides per term (`writer.hh:818-829` at `e779835`). Advance offsets are relative to their
group's base, so they never need more than 16 bits: an ordinal block is at most 513 bytes (FOR at
width 32, and a bitset or E2 or E3 block is chosen only when smaller), a wdf block at most 1,155
(`bitpack::max_encoded_bytes`, `bitpack.hh:400` at the pinned `abf8972`), so 32 blocks span at
most 53,376 bytes. Revision 0's A1, a u32 offset with the bitset flag in bit 0, capped a term at
2 GiB of blocks (F13-R8); this form has no cap, and F22's fixture with 64-bit skip offsets
exercises level 1's wide path.

**Width candidates** (bytes projected exactly by `poststats` from format 12's lasts and sizes,
F22):

| Id | Advance entry | Bytes |
|---|---|---|
| A1 | last u32, offset u16 | 6 per block, plus flags |
| A2 | last minus the group's base as u16 when the group's ordinal span fits, else u32 (a width bit per group in level 1); offset u16 | 4 or 6 per block, plus flags |
| A3 | lasts, offsets, and flags bit-packed per group at the group's widths, recorded in level 1 | ⌈(wl + wo + wf) × 32 / 8⌉ per group |
| A4 | As A2, with the group's lasts and offsets in two arrays, so the lasts are searched in place with SIMD (the query-gap design's M16) | As A2 |

A1, A2, and A4 read an entry with one load; A3 needs a lane extract, as `length()` does
(`reader.hh:296-308`). Each is also measured with one entry per 256 postings (two blocks), M16's
second granularity, which halves the entries and makes an advance decode up to two blocks. The
query-gap design's E6 microbenchmarks them over E1's access traces (`query-gap.md`, E6).

**Two asks of the query path** (`query-gap.md`, "What the query path needs from format 13"): a
group's impact entries stay contiguous, so a range bound walks consecutive blocks' pairs, as the
per-group stream above does; and a block's bound is one read away, which M17 gives by a frontier's
first pair and M18 by an argmax pair stored beside the advance entry, if either is kept.

**Validation, per block decoded** (F13-R8). Locating a block reads fixed-width entries without
decoding or checking them; a binary search reads no neighbor, so revision 0's "per entry read"
could not hold. When a block is decoded, the reader checks that its offset and length (its entry
and the next) lie inside the term's blocks area, that its codec code is one the format keeps, and
that its ordinals strictly increase within (the previous entry's last, its own last], the last
equal to its entry's; otherwise `corrupt_segment`. Format 12's `check_increasing`
(`reader.hh:1033-1040` at `e779835`) is that check and stays, unless M23 (F11) stores deltas so
that ordinals increase by construction and the check becomes their sum against the entry's last.
A corrupt entry whose checksums were recomputed may steer an advance to a wrong block; decoding
it then throws, or yields increasing ordinals below the document count consistent with its
entry, so no consumer sees ordinals out of order or range, though a match may be missed. That is
`fuzz_segment`'s rule ("fence or answer") for inputs with resealed checksums, and its harness
checks that every iterator's ordinals increase and stay below the count. Media damage fails a
region CRC first.

**Expected effect, from the profile** (F13-R7): in the one query profiled, skip-entry work is
about 23% of its time (`query-gap-merged.md:24-28`), and `check_increasing` (4%) stays. F10 can
take at most that share off; the record's "the remaining gap is per-block skip-entry decoding and
validation" overstates it. The rest of that query's time is postings decoding and intersection
(postings fill 22%, conjunction fill 10%, advance 6%, `or_bitset` and `load_bitset` 5% each),
which F11's codecs and Track P's query items address, and which F1's freeze criterion 4
attributes.

## F11. Postings encoding (P1's owed row; P4)

Strata reordered is 845.6 MB against keyed `-bp`'s 833.7 MB `.doc` (1.014×,
`phase-3-ratchets.md:204`): 11.9 MB over. The section holds blocks, tails, skip data, and
per-block impacts. `strata_engine poststats` (F22) reports, over a segment, the bytes in level 1,
level 0's last, length, positional, and impact fields, ordinal blocks (FOR and bitset
separately), wdf blocks, and tails, with counts of blocks per encoding and of blocks whose wdfs
are all 1, and projects each candidate below from the same blocks.

**Candidates.** E1 to E5 are per-block or per-term choices the writer makes by size alone, so a
merge's output is a function of its input (F21); M17, M18, and M23 are rules for every block,
kept or dropped whole:

| Id | Change | Hook |
|---|---|---|
| E1 | Omit a block's wdf bytes when every wdf is 1; today such a block is `bitpack`'s 5-byte constant block (`bitpack.hh:403-408`) | One flag bit per block (F10) |
| E2 | Partitioned Elias-Fano as a block ordinal codec (P4) | A codec code (F10) |
| E3 | Runs of consecutive ordinals (P4) | A codec code |
| E4 | A tail of 16 to 127 postings as one FOR block of its length; `bitpack` packs only 128 values today, so this needs a partial-block form there (additive) | Implied by df mod 128 |
| E5 | A block's pairs reduced to those that differ from its group's frontier | One bit per group |
| M17 | Frontiers ordered by score under the index's preset at the segment's average length, so a reader bounds a list by its first pair (the query-gap design) | A writer's ordering rule; the preset recorded with `inverted`'s statistics (F7, F13-R21) |
| M18 | One argmax pair per block for each preset, whole frontiers kept per group and per term (the query-gap design) | Replaces the block's frontier in the impact entries; the presets recorded as for M17 |
| M23 | Ordinal deltas stored as gap − 1, so a block's ordinals increase by construction and the reader checks only their sum against the entry (the query-gap design) | The FOR block's meaning; no flag |

A FOR block whose deltas are all equal is already a 5-byte constant block (`bitpack.hh:403-408`),
so a block that is one run of consecutive ordinals costs 5 bytes in format 12; E3 gains only on
blocks of several runs. E5 and M18 both shrink the block-level pairs, so at most one stays. M17,
M18, and M23 are the query-gap design's, decided by its E5 and E6 (`query-gap.md`, "Before the
freeze, and after"), whose tables go to this format's candidate report. Every candidate is chosen
with F10, F12, and F13 below.

## F12. Variable-size block-max partitions (P3)

F10's impact entries are separate from the 128-posting compression blocks, so their granularity
need not follow them (Mallia et al., 2017: variable-sized block-max intervals over
document-ordered postings).

- **No bytes at P0** (F13-R8): a term whose partitions are its blocks, the default, stores no
  partition lasts; the advance entries' lasts serve. A per-term flag in the term's skip header
  marks a partitioned term, whose impact entries then carry their own last ordinals.
- **Candidates:** P0, one partition per block (format 12's bounds); P1, Mallia et al.'s greedy
  partitioning against a target average size, cut where the bound's error over the partition
  would grow past a threshold, both presets.

## F13. Threshold seeds (P21)

A top-k query starts with threshold 0 and prunes nothing until k candidates are scored. P21
stores, per term, what a seed needs so pruning starts at the first posting, for k = 10, 100, and
1,000, and stays exact (PISA's seed is exact only at k = 10: `phase-3-ratchets.md:309-322`).

**What is stored is not a score.** Scores depend on the scheme and on statistics that change
with every commit. Instead, for each k ≤ df, the term stores a few dominance pairs (w, l): pairs
such that at least k of the segment's postings of the term have wdf ≥ w and exact length ≤ l.
For any score non-decreasing in wdf and non-increasing in length (both BM25 forms), every such
posting scores at least f(w, l), so the term's k-th best score in the segment is at least the
largest f(w, l) over its pairs. The query computes f under its own scheme and statistics.

- **Lengths round up.** `encode_length` rounds down (`format.hh:342-352`), which bounds scores
  from above; a seed needs a bound from below, so l is stored as the smallest length code whose
  decoded value is at least the true length, or as an exact length.
- **Where seeds apply:** a disjunction's threshold starts at the largest seed over its terms and
  segments, since a document's sum of non-negative contributions is at least one term's, and the
  k-th best across segments is at least any one segment's. Seeds do not apply to conjunctions or
  phrases, to segments with deletes (a seed may count a deleted posting), or where the threshold
  must admit ties: pruning drops only scores strictly below the seed, so the docid tie rule stays
  exact (`reordering.md` D5).
- **Where they live:** in the postings header of terms with skip blocks (df ≥ 128), at most 3
  pairs per k; shorter lists have none. Their bytes count in the postings row.

## Choosing F10 to F13 together (F13-R8)

F10's entries, F11's flags and codecs, F12's lasts, and F13's pairs all spend the same carried
budget, the postings section's 11.9 MB, so they are chosen as one combination, not one by one.

1. **Projection, before code.** `poststats` over the postings of a reordered 5M merge projects
   each candidate's bytes exactly from format 12's blocks, since each is a function of data the
   section already holds: A1 to A4 from the lasts and sizes, at 128 and 256 postings per entry;
   E1 from the all-ones blocks (5 bytes each); E2 and E3 from each block's decoded ordinals, as
   the minimum of every codec per block; M23 from the same ordinals at gap − 1; E4 from the
   tails; E5, M18, and F12's P1 from the pairs, with frontier-length histograms per level (the
   query-gap design's E1 asks for them); F13 from each term's postings and exact lengths.
   Interacting items (the codec minimum with M23; E5, M18, and P1) are projected jointly. A
   candidate that no combination can carry under the constraint is dropped before any code, and
   so is one whose ceiling the query-gap design's E3 to E6 project below 3% on every row it
   targets (`query-gap.md`, D5).
2. **The constraint:** postings ≤ 833.7 MB, and the whole index ≤ 2,542.5 MB with every other
   section as built. The whole index has 3.2 MB of headroom at format 12's figures (2,539.3 MB,
   `phase-3-ratchets.md:203`), so the two rows are read together: a byte one section grows is
   paid for by another.
3. **The objective:** ns per touched block on `COUNT` intersections and unions, and the pruned
   top-k rows (`TOP_10`, `TOP_100`, `TOP_1000` on union and intersection_union), from branch
   A/Bs of each combination against main's format 12 under a hold the lead grants (SYS-6, C3).
4. **Order:** F10's layout first (one of A1 to A4 at one granularity: the base the others hook
   into), then E1, then E2, E3, and M23, then E4, then the pairs (E5 or M18, and M17), then
   F12's P1, and F13, each measured as an increment over the combination before it, on top of
   the query-gap design's format-free bound fixes (its D6). Each kept item's table shows its
   bytes and time against the kept set without it; an item is kept when it lowers the objective
   beyond the A/B's interval, or when the constraint needs its bytes and the T4 guard holds with
   it. E4 is also weighed by F1's tail profile of the expansion class (AUT-R13).
5. **If no combination meets the constraint and F1's freeze criterion 4**, the candidate report
   says so and the owner decides before the freeze: format 14 with its N − 1 cost stated, or a
   trade between axes with both sides' numbers (PLAN, "Ratchets").
6. **Losing candidates leave the format before the freeze**, decoders and hooks with them. The
   only per-block choice that stays is one the writer makes by size alone: the codec code.

Each candidate's measurement:

| Candidate | Bytes, projected by `poststats` | Time, from the A/Bs | Kept when |
|---|---|---|---|
| A1 to A4, at 128 or 256 | Their entries and flags against format 12's level-0 last and length fields | `COUNT` and pruned top-k ns per touched block; the query-gap design's E6 microbenchmark | The fastest whose bytes leave the constraint reachable |
| E1 | −5 bytes per all-ones block | Top-k rows (no wdf block to decode) | It saves bytes and the guard holds |
| E2, E3 | Σ per block of the smallest codec, against FOR and bitset alone | `COUNT` and top-k intersection rows; E6's probes | The constraint needs them and the guard holds |
| M23 | FOR at gap − 1 against today's deltas | Every decode (`check_increasing` was 4% of the profiled query); E6 | It saves time or bytes and the guard holds |
| E4 | FOR of the tail against its LEB128 bytes, tails of 16 to 127 | Term rows, and the expansion class (F1's tail profile) | It saves bytes and the guard holds |
| E5 | −the pairs equal to the group frontier's, +1 bit per group | Pruned top-k rows | It saves bytes and the guard holds |
| M17 | The preset record, 26 bytes per segment (F7) | Term, union, and intersection `TOP_k`; the query-gap design's E5 in format-12 bytes | Its rows gain beyond the interval |
| M18 | −each block's frontier past one pair per preset, +the preset record (F7) | As M17 | It saves bytes and the guard holds |
| F12 P1 | +one last per partition in flagged terms | Pruned top-k rows | Its rows gain beyond the interval |
| F13 | +up to 9 pairs per term with skip blocks | Union top-k with seeds against without (T6) | T6 passes and its rows gain |

## F14. Positions (P13)

Positions pass at 0.980× keyed `-bp` (`phase-3-ratchets.md:205`) and are not carried. Format 13
changes them only if P13's candidates win their row before the freeze: a per-block choice among
PFOR (today's), FOR, and Elias-Fano behind a codec field in the positional stream's entries, and
deltas within a document coded against the document's length. Without a win, positions keep
format 12's encoding and no codec field.

## F15. Values (D4; R4, R10)

The `values` section is the slot directory followed by one column per slot, in `docvalues.hh`'s
column codec (`docvalues.hh:23-40`):

```
values  [slot count LEB][per slot: declaration (F15a), column offset u64, column length u64]
        [columns, each 8-byte aligned]
```

- **F15a, a declaration** (D3): slot id u32, logical type u8 (`i64`, `u64`, `f64`, `date`,
  `bytes`, `binary`), flags u8 (multi-valued, doc values, points), and for points index dimensions
  u8 (at most 8), data dimensions u8 (at most 16), and bytes per dimension u8 (1 to 16; N2,
  F13-R22).
  Slots strictly increase. The directory copies the declarations the segment was written under,
  so `install`, merges, and frozen indexes decode it without the slot table.
- **Bytes per dimension, 1 to 16** (F13-R22; the owner's Q6, `PLAN.md:52-54`), Lucene's
  maximum (`limits.md:36`). A numeric slot's points take 8, D3a's order-preserving map of its
  64-bit value. A `bytes` slot declared with points at w bytes per dimension and d data
  dimensions holds values of exactly d × w bytes, split into its dimensions in order; stage 1
  refuses a value of any other length, and the tree compares each dimension as unsigned big-endian bytes, as
  `memcmp` orders them. IPv6 addresses and uuids (16 bytes) fit, the slot's doc values stay the
  `bytes` kind's dictionary ordinals, and no column kind is added. D3a's "never compared as
  values" (`phase-4.md:143-144`) then excepts a slot's points. A declaration with w of 0 or above
  16, or w other than 8 on a numeric slot, is corruption in the directory and refused by
  `declare_slot`. Points' first release (F23) defines the tree's byte dimensions; the
  declaration freezes now with `values`.
- **`binary` columns** (F13-R13): kind 4 in `docvalues.hh`'s codec, whose kinds are `u64`,
  `i64`, `f64`, and `bytes` (`docvalues.hh:63`): the `bytes` kind's sorted per-segment dictionary,
  always multi-valued, each document's values as dictionary ordinals in the host's order with
  duplicates kept (D4: "`binary` slots keep order and duplicates"), so StringList sort keys and
  N7's collapse keys read them in stored order. Statistics and skip groups are `bytes`': minimum
  and maximum over ordinals, which order as the values' bytes do.
- **One version scheme** (S2): the column's own version byte (`docvalues.hh:109, 194, 269`) goes;
  the codec is a function of the segment's format version, which `open_context::version`
  passes. Columns begin with their kind byte.
- **The exact accumulator's width** (the Phase 4 review's finding 4, `phase-4-design.md:794-795`)
  is recorded in the column header for `f64` sums: format 13 writes 0, persisting no `f64`
  accumulator; integer sums keep their 128 bits.
- Presence uses `docset`'s frozen form (R4); skip groups are R10's.

## F16. Points (D5; C6)

Points are reserved, not frozen: kind 15 (F3) and bit 5 (F4), added within format 13 by F23's
rule, which replaces revision 0's "both freeze with D15's result" and its freeze item 3 (C6).

- The first release that writes points fixes their envelope and tree body for format 13. The
  tree body is `bkd`'s serialized form, which does not exist yet (`bkd` builds and queries in
  memory), and D15's result decides the dimension counts and the shape path before points' first
  write, not before the freeze.
- Points are their own plugin, appended to the layout (L2). An index lists them in its mask when
  it first declares a point slot (D3's `declare_slot`), so an index without point slots stays
  readable by a binary without the plugin (C6, rule 3).
- F15a already carries each slot's points flag, dimensions, and bytes per dimension (up to 16,
  F15), so the slot table needs no change when points arrive. Revision 0's envelope sketch is withdrawn.

## F17. Term lists (D11; R7)

Opt-in per index (bit 6). Per document, its `inverted` terms in order with wdf, and positions
when configured: [term count LEB][per term: shared prefix with the previous term LEB, suffix
length LEB, suffix bytes, wdf LEB][with positions: per term, position count LEB and deltas],
packed into zstd frames with `stored`'s frame index layout (F19).

- **Frames are bounded** (F13-R13; R19, C9): a frame holds at most `stored_block_size` of lists
  or one larger list, and stage 1 rejects a document whose list would exceed
  `max_stored_record`, so no frame exceeds `max_stored_frame` (`format.hh:76-86`), which bounds
  what reading one allocates.
- **A list holds no key.** The key is the core's field, held once in `keys` (F8). Xapiand's info
  endpoint and its `_terms` aggregation show the id term (`FEASIBILITY.md:111`); the seam inserts
  `document::key` at its byte-order place when it presents a list (Phase 7).
- Reported in the disk table, excluded from the ratios (`BENCHMARKS.md:175`).

## F18. Index sort (D14)

Defined now, written when D14 is built. `index_sort` is the core's section (L4, F3): [component
count LEB][per component: slot id u32, direction u8, missing u8 (first or last), selector u8 (min,
max)]. Its bit says the segment's ordinals follow that order, and it requires `values` (F4).

- The count is LEB128 (F13-R13), so `limits.md`'s "no count limit" for sort components
  (`limits.md:40`) holds in the format; the section's length bounds it.
- A format-13 reader validates it (every slot in the values directory, every byte in range) and
  serves queries as for any segment; using the order for early termination is D14's code.

## F19. Stored frames

Format 12's layout (`stored_builder`, `writer.hh:720-764`) with one reservation (R19): bit 31 of
a frame index entry's uncompressed size means "the frame's last record continues in the next
frame". Format 13 writers never set it, and readers reject it: it already fails the frame bound
(`reader.hh:580`, `max_stored_frame` well below 2^31). Revision 0 left its first use to a later
format version; Q5 asks whether it gets a structure bit instead, so chunked records can arrive by
F23's rule.

P14's dictionary compression arrives by F23 (kind 19, bit 9): a segment that sets bit 9
decompresses its frames with the dictionary in `stored_dictionary`; any other decodes its frames
as frozen here.

## F20. Manifest version 2

The manifest (`files.hh:30, 119, 171-215`) changes once, with format 13:

- **The file family's version** (`files::version`, `files.hh:30`, shared by the manifest, the
  live-docs files, and the metadata files) becomes 2. A reader checks the file's CRC first and
  its version second (`files.hh:119`), so a flipped version is `corrupt_index`; a well-formed file
  of another version is now `unsupported_index`, not corruption. The frame (the magic, the LEB128
  version after it, the trailing CRC-32C) is frozen in every later version, as the segment's
  prefix is.
- **The slot table's generation file**, `slots-<gen>.kv`: its generation, size, and CRC (D3, N1).
- **The structure mask** (replacing L16's plugin identifiers): the bits the index is configured
  with, together with every bit any of its segments sets. Open and `install` refuse a mask with a
  bit this binary cannot read with `unsupported_index`, naming the structure from F4's table; a
  segment that sets a bit outside the mask is corruption; a structure this binary has and the
  mask lacks is fine, its sections absent and its bit clear (L16). A plugin counts as listed
  when one of its bits is in the mask, so L16's rule holds per plugin and also per sub-structure
  (`norms`, P8's, P14's, P18's, P20's), which a list of plugin names cannot express. A bit enters
  the mask when the index is configured with its structure and leaves it once no segment sets it
  and the configuration has dropped it, so an unfrozen index becomes readable again by binaries
  without P20's structures.
- **The segment versions** (L16, S2): the lowest and highest format version among its segments;
  open refuses a range outside [`min_read_version`, `version`] with `unsupported_index`, and a
  segment outside the range is corruption. No per-segment version is recorded (S2).
- **Per segment entry**, a values generation, 0 for none (`workload-profiles.md` item 4); no
  code writes nonzero, and a reader rejects nonzero until field updates exist.
- **The key prefix** moves from the manifest's index fields (`files.hh:177`) to the `keys`
  structure's options, still recorded in the manifest.
- **Checks** (L16's, step 7c's manifest commit): an index whose mask holds a bit the binary
  cannot read, opened and installed; a highest version above the binary's; a segment outside the
  mask or the range (corruption); a version-1 manifest (`unsupported_index`).

## F21. The digest at format 13

L8, unchanged: the `D` record carries only the core's fields (docid, version, key as length u32
and bytes); `inverted` emits a length record for every document, length 0 included, and its `P`
records; `stored` a record of its bytes, none for an empty one; `values` its `V` records (D17);
the core adds the slot-table record. Structures added by F23 add records only if they hold content
no other structure digests (none of the reserved ones does).

**The digest changes value once** (F13-R5): in step 7c's digest commit, which carries the split,
the key in `D`, `inverted`'s length record for every document, F9's version 0 for keyless
documents and its writer check, and drops the interim key record that the keys commit keeps (the
`P` record format 12's key term produced, so `digest.txt` holds through the earlier commits).
That commit re-pins layers' `digest.txt` and the fixtures that write a keyless document with a
version, and runs T2's schedules. A merge's output bytes are a function of its inputs, its order,
and the writer's presets alone, so replicas that merge alike write alike (T8).

## F22. Fixtures, fuzz bases, libraries, and tools

- **Fixtures**, pinned by `fixtures_test` (`test/fixtures_test.cc`) and re-pinned with every
  format change on the branch: one per profile (`search`: every defined structure; `document`:
  `keys` and `stored`; `log`: `inverted` without `keys`; keyless and empty segments), one per slot
  type (`i64`, `u64`, `f64`, `date`, `bytes`, `binary`), one permuted (reordered) segment, one
  with 64-bit skip offsets, and one per reserved bit set, refused as unsupported. With them, the
  format-12 refusal fixtures of F2.
- **Fuzz bases** (T2): `fuzz_segment` gains a section selector, an input byte naming the kind
  its mutations stay inside, so each decoder gets an hour of its own; a base per profile and per
  kind of F3; a mutation that drops, duplicates, reorders, or renumbers table entries and flips
  structure bits; and `refresh_checksums` reseals every checksum the reader checks (sections,
  regions, the table, the footer, and the header prefix). Every decoder F3 adds (the header
  prefix, the kept K codec and `key_index`, the advance and impact entries, each kept block codec,
  `values`' directory and its `binary` column, `term_lists`, `index_sort`) gets a checked-in corpus
  and one CPU-hour at the freeze commit.
- **Libraries whose bytes freeze** (F13-R14): `termdict`'s blocks and index (today's form at
  today's settings, F8's options form otherwise; F13-R20), `fst`'s automaton,
  `bitpack`'s FOR, PFOR, and constant blocks, `fusefilter`'s `filter8`, `docset`'s frozen form,
  `varint`'s LEB128, zstd frames through `compressors`, and `hashes`' CRC-32C and xxHash64 at seed
  0, each pinned by commit (`CMakeLists.txt:10-28`). Each library's README states the form strata
  freezes at its pinned commit. A pin bump keeps every format-13 fixture byte-identical, or it is
  format 14 when it changes what a reader accepts. The one exception is an encoder whose output
  varies within an unchanged form, as zstd's compressed bytes may at a new release: the bump
  re-pins the fixtures without a format change, since every format-13 reader decodes both, and
  every replica moves to it together, since two binaries would write different bytes for one
  merge (the digest stays equal; T8's byte identity holds per binary). Detent links `varint` and
  `compressors` (its `LIBRARY_BOUNDARIES.md:48, 130`, as the review read it), whose forms are
  standard; the others are strata's alone. `fixtures_test` runs on every push, so a bump that
  changes a byte fails CI.
- **Tools:**
  - `strata_engine sections` learns F3's names (`format::name_of`).
  - `poststats` (F11's breakdown, `strata_engine.cc:245-348` at `e779835`) projects every
    candidate's bytes ("Choosing F10 to F13 together", step 1) and prints frontier-length
    histograms per level (block, group, term), which the query-gap design's E1 and M18 need. Its
    seedable counts at df ≥ 10 and 100 equal its count of terms with skip blocks (`:263-269`), so
    they go; it reports F13's seed bytes instead.
  - `keysim` (F8's candidates, `:350-446`) sizes all eight candidates with the same fields and
    times each through the same reader path, on hits and misses, at 1 and 50 segments (F8). Today
    it counts the filter and range in K1 only, times K1 through `find_key` and K3 from heap strings
    without a filter or verification, times hits only, and times neither K2 nor K4 (F13-R6).
  - `keybench` (new): `lookup_key` over an index, hits and misses, the median of batches. It is
    written against main's API and touches no format code, so the lead can land it on main and
    record format 12's T5 row first (F13-R15).
  - They measure; no branch claim rests on them. Their numbers go to `docs/perf/` as candidate
    tables, under a hold the lead grants (C3).

## F23. Adding a reserved structure within format 13

The owner's rule for points, applied to every reserved structure (C6; `PLAN.md:34-41`):

1. Each reserved kind and bit (F3, F4) stays reserved for its item and is never reused, whatever
   the item's design decides; an item that turns out not to need its kind leaves it unused.
2. The first release that writes a reserved structure fixes, for format 13, its sections' layout,
   verification class, and file rank (F3's placement rule). Changing any of them afterwards is
   format 14 (S2). That release's segments carry version 13.
3. An added structure never changes how a frozen section decodes in a segment that does not set
   its bit. It may change how its owner's sections decode in segments that set it (P14's
   dictionary-compressed frames, P18's punched regions), and its first release states exactly
   how.
4. A binary writes the structure only in an index configured with it. The configuration is a
   replicated operation (D3's `declare_slot` for points; an index option for the others), so
   every replica sets the bit from the same log position, and the manifest's mask gains the bit
   at that commit (F20).
5. A binary without the structure's reader refuses such an index with `unsupported_index` and
   such a segment with `unsupported_segment`, never as corruption and never by skipping the
   section (F4).
6. Under mixed binaries (Track D8), the configuration that adds a structure is accepted only once
   every replica's binary reads it; until then it rejects deterministically. D8 enforces it.
7. A structure derived from content that other structures digest adds no digest record (F21).
8. Its release brings its fixture, an hour of fuzzing for its decoder (T2), its T1 cases (its bit
   set without its reader is unsupported), and its gate rows.

| Item | Kind | Bit | Owner | Configured by | When its bit is set | Digest |
|---|---:|---:|---|---|---|---|
| Points (D5, F16) | 15 | 5 | `points` | `declare_slot` with points | Point queries read its trees | none: built from values (D17) |
| P8 phrase prefilters | 18 | 8 | `inverted` | an `inverted` option | Phrase scorers may consult its summaries before decoding positions | none: derived from positions |
| P14 stored dictionaries | 19 | 9 | `stored` | a `stored` option | `stored`'s frames decode with its dictionary | none: records hold the uncompressed bytes |
| P18 holes | 20 | 10 | `stored` | P18's reclamation | The `stored` regions it marks are not verified and hold no live record | none |
| P20 quantized impacts | 21 | 11 | `inverted` | freeze (`frozen-index.md`) | Scorers may use its quantized scores | none: derived |
| P20 key hash | 22 | 12 | `keys` | freeze | `find` may use the hash | none: derived |

- P8's and P20's `inverted` sections locate a term's data by its postings offset, unique among a
  segment's terms with postings bytes, so they need no field in `postings` or `terms`.
- P18's design decides whether its marks are segment bytes. If they live in the per-generation
  live-docs file instead (`workload-profiles.md` item 4's mechanism), kind 20 and bit 10 stay
  reserved and unused. If they are segment bytes, punching rewrites the segment's tail at a new
  size and footer CRC, which the manifest's segment entry records (`files.hh:51`), and P18's
  design reconciles the two.
- A frozen segment that becomes writable again keeps P20's structures until its next merge, which
  drops them when the configuration does (`frozen-index.md:50-55`); the mask then drops their
  bits (F20).

## Gate rows

Branch rows (T1, T2, T7, T8, T9) are functional and run in CI on the branch. Measured rows (T3 to
T6, T10, T11) run on main commits on the gate VM (PLAN, "What waits"). The freeze's candidate
selections (F1) run before that, on branch builds under a hold the lead grants, recorded in
`docs/perf/` as evidence, never as gate rows (SYS-6).

| Row | Target | Procedure |
|---|---|---|
| T1 Format rules | Every rule of F2 to F6, F20, and F23 has a test that builds a valid segment or index, breaks the rule, and expects the stated outcome. The prefix: magic, each byte of 8 to 15 flipped, a real format-12 file, version 14 with a good CRC. The footer and table: checksums, bounds, count, unknown and retired kinds, a kind twice, rank order, a gap, misalignment, overlap, the table's place. Structure bits: each defined bit flipped both ways, a sub-structure without its owner, `index_sort` without `values`, each reserved bit set (unsupported), bits 13 to 31. The free-and-absent cases of F4. At the index: a segment outside the manifest's range or mask, a mask bit the binary cannot read (open and install), a version-1 manifest, and, before F20, a manifest-named unsupported segment (corruption, retried and healed). The format-12 refusal fixtures | `test/format13_test.cc`, `fixtures_test`, `index_test`, `media_test`; ctest on all eight CI jobs |
| T2 Decoders | Every decoder F3 adds survives `fuzz_segment` for one CPU-hour each at the freeze commit, under ASan and UBSan, with no finding open and a hang timeout; corpora replay in every CI build; every corruption check of T1 is also reached by a resealed fuzz input | `fuzz/fuzz_segment.cc` with `STRATA_FUZZ=ON`, `-fsanitize=fuzzer,address,undefined`, `-max_total_time=3600` per section selector |
| T3 Disk | After a reordered full merge of the 5M keyed index: postings ≤ 1.0× keyed `-bp`'s `.doc`; the dictionary row (F8's accounting) ≤ 1.0× keyed `-bp`'s `.tim`, `.tip`, `.tmd`; the whole index ≤ 1.0× keyed `-bp`'s; every other section no larger than format 12's figure (`phase-3-ratchets.md:201-208`); new sections reported | `strata_engine merge` with reordering and `strata_engine sections`, against `results/keyed-bp-n2.sizes` (`bench/night_p1.sh`, step 3) |
| T4 Queries | The guard: per class and command, format 13 over format 12 ≤ 1.03 by the interval's upper bound, both merged and reordered; blocks decoded and ns per touched block reported for `COUNT` intersections and unions. The carried rows: F1's criterion 4 before the freeze, PLAN's ≤ 1.0× at landing | `bench/ab.sh` in-process, each binary serving its own build, five trials, both query sets; `bench/phase2/gate.py` for the carried rows |
| T5 Keys | `lookup_key` at 1 and 50 segments, hits and misses, ≤ format 12's row by the interval's upper bound, that row recorded on main with its raw file and commit (C5's check); Lucene's id lookup reported beside it; indexing rows (1 and 8 threads, keyed) ≥ 1.0× the faster engine and ≤ 1.03 of format 12's time | `strata_engine keybench` (F22), recorded on main for format 12 before format 13 replaces it; `bench/phase2/lucene_seek`; `bench/index_trials.sh` |
| T6 Seeds | `verify` clean at k = 10, 100, 1,000, both schemes, on one segment and 16, with and without deletes; union `TOP_k` with seeds over without ≤ 1.0 by the upper bound; seed bytes in the postings row and in the joint selection | `strata_engine verify`; `bench/ab.sh` with seeds toggled by a preset |
| T7 Open | A fresh open touches the header page and one run of pages ending at the footer, and no byte of `lengths` | `test/format13_test.cc` with a counting `IO` and the verified-region pages it reads |
| T8 Determinism | The fixtures' hashes are equal on all eight CI jobs; a reordering merge's output hash is pinned at `reorder_threads` 1 and 8 | CI |
| T9 Lifecycle | Phase 3's T1 crash points, T2's digest schedules, and T5's disk-full rows pass over format 13, keyed, keyless, and with the `document` profile | `lifecycle_test`, `index_test`, `media_test` |
| T10 Memory | Serving memory on the merged 5M index ≤ format 12's, both measured in one session; pinned bytes per open segment stated (the tail, the slot directory) at 1,000 indexes per node (D18) | `bench/serve_mem.py`; a test that sums pinned bytes |
| T11 Cold start | A report row: open time per GB and first query after a dropped page cache, format 13 against format 12, in the bare-metal session once a host is named (`BENCHMARKS.md:29-32, 114`; SYS-4). T7 is S1's functional check until then | BENCHMARKS, "open time per GB" |

## Questions for the owner

Answered: Q1 to Q4 through the review (`format-13-design.md`, "Owner questions"), Q6 by the
owner (`PLAN.md:51-54`):

- **Q1**, the dictionary row's reference: keyed Lucene `-bp` (`PLAN.md:38-39`). F8, T3.
- **Q2**, Track P's format items: P8, P14, P18, and P20 reserved, with the points rule
  (`PLAN.md:39-41`). F1, F23.
- **Q3**, freezing with points unwritten: yes, by a stated rule (`PLAN.md:34-36`). F16, F23.
- **Q4**, candidate runs on branch builds: evidence for a choice, never a gate row
  (`PLAN.md:19-20`). F1, gate rows.
- **Q6**, bytes per point dimension: up to 16, Lucene's maximum, with a mechanism for 128-bit
  values such as IPv6, not a 64-bit cap (`PLAN.md:52-54`; F13-R22). F15, F16.

Open:

- **Q5. R19's continuation flag** (F19). Bit 31 of a frame's size is reserved, but a format-13
  reader rejects it, so chunked records would need a format version.
  - (a) Revision 0: a later format version enables it.
  - (b) Reserve bit 13, `stored_spans` (F4), so a release can enable continuation within format
    13 by F23's rule: frames then mean "continues" by bit 31 only in segments that set bit 13.
  - Recommended: (b). It costs nothing now; R19's deadline is Phase 7's gate (Elasticsearch's
    100 MB bodies), and a format 14 for it alone costs every plugin an N − 1 decoder.

Neither blocks step 7c or any first step.

## The rebase onto layers

The base is `phase4/layers-refactor` at `707066b`: steps 0 to 6, 7a, and 7b, CI green on all
eight jobs at `1d60098` (run 39070316303, per its ledger; `707066b` adds only its ledger), or main
once layers lands there, as the lead names. `c8c03f6`, the base SYS-1 named, became `8645134` in
layers' own rebase onto main. Format-13 code returns to `reader.hh`, `writer.hh`, `index.hh`, and
`merge.hh` only on the rebased branch, as step 7c, after this revision's re-check (the review's
required changes).

**Mechanics.** The old commits do not replay: their reader, writer, index, and merge hunks are
against main's `segment` (30 conflict hunks in the review's merge probe). The developer starts a
branch at the base, cherry-picks the documentation commits, takes `format.hh` and `keys.hh` from
`e779835` as their starting content, writes 7c-1 to 7c-7 below on layers' structure, and uses the
old commits' tests (`format13_test.cc` and the F8 and F9 cases in `segment_test` and
`index_test`) as the starting cases, adapted to `basic_segment` (`core()`, `get<P>()`,
`holds<P>()`). The rebased branch replaces `phase4/format13` by a force push the lead approves;
the old head stays reachable under a name the lead chooses. Each commit builds and passes on all
eight jobs before the next (L14), and re-pins its fixtures in the same commit.

**The branch's commits, one line each:**

| Commit | Subject | At the rebase |
|---|---|---|
| `1bc0a38` | Design revision 0 and the ledger | Survives as is (documentation) |
| `d1da0c9` | F2 to F6 | `format.hh` survives and is revised (F2's prefix, F3's table, F5's canonical layout, F4's classification); the `reader.hh` and `writer.hh` parts are replaced by layers' `core_segment` and `section_sink` (7c-1); `fuzz_segment.cc`, `strata_engine.cc`, `CMakeLists.txt`, and `format13_test.cc` change (re-applied on layers' versions); fixtures and pinned hashes are regenerated |
| `96083a4` | F7 | Replaced by 7c-2 (L9: statistics at the head of `term_index`, no inner CRC) |
| `360dc7f` | F8: `keys` and `key_index` | `keys.hh` survives as is; the `reader.hh`, `writer.hh`, `check.hh`, and `fuzz_segment.cc` parts are replaced by the `keys` plugin's reader, builder, and check on `keys.hh` (7c-3) |
| `7c2c9bd` | F8: keys leave the dictionary | The segment's fallback (`reader.hh:356, 364-372` at `e779835`) is dropped (F13-R2); `merge.hh`'s parameter removal is replaced by layers' 7a, with `merge_options::key_prefix` deleted in 7c-3; `lookup_key`, the merge's key collection, and the stage-1 rule are redone in 7c-3; its key record in the digest survives only as 7c-3's interim record |
| `e2ac3fe` | F9 | The presence parts are replaced by 7b's mask set from the footer and readers that open empty (7c-4); keyless segments and the version-0 rule (`index.hh:1650` at `e779835`), one commit there, stay one commit in 7c-5, with F13-R9 (F13-R18); "keyed when any input is" becomes F4's configuration rule |
| `e779835` | F22's tools | Changes: `poststats` and `keysim` are re-applied on the plugin readers after 7c-3, with F13-R6's and F13-R15's fixes |
| `4266374` | The preliminary review | Survives as is (documentation) |
| This revision | Design revision 1 and the ledger | Survives as is (documentation) |

**Step 7c, one line each, in order** (L14 step 7c; the review's order):

1. **7c-1, F2 to F6:** `read_version` and the prefix; `unsupported_segment`; F3's and F4's tables
   in `format.hh` replacing `section_rule`; `core_segment`'s open by the table (presence both
   ways, unreadable bits, canonical layout); the sink placing by rank and holding pinned
   sections; the footer's structures from `built`; `load_segment` and `written_segment` reading
   the prefix first and turning a manifest-named unsupported segment into corruption; the
   format-12 fixtures moved to `test/fixtures/format12/`. `pk_filter` stays a `keys` section here,
   as at `d1da0c9`; `digest.txt` is unchanged.
2. **7c-2, F7:** `stats` holds the count; `inverted`'s statistics lead `term_index`; `lengths`
   holds lengths only; `built::legacy_stats` goes.
3. **7c-3, F8:** the `keys` plugin on `keys.hh` (K1 until F8's choice), with empty deps and
   F8's interface (`scan` from a lower bound, `PLAN.md:47-48`, which automata's expansion runs
   over `keys`); the term table's and `snapshot_df`'s key leaf; stage 1, the `inverted` builder,
   and `check()` refusing key-prefixed terms;
   `document::key` and `key_of` from it; the merge collecting keys through `scan` from the empty
   key; `merge_options::key_prefix` deleted; `pk_filter` retired. An interim `P` record per live
   key keeps `digest.txt` unchanged.
4. **7c-4, F9's presence:** readers that open empty; 7b's mask from the footer; the
   free-and-absent cases of `stored`, `norms`, and `inverted`; `document_layout` segments and
   tests (L12). Every index stays configured with `keys` here, so every segment sets bit 2 and
   writes `versions`, a keyless document's version included, as format 12 does, and a keyless
   document's version reads the same before and after a flush (F13-R18).
5. **7c-5, F9's keyless segments and F21:** the `keys` bit from the configuration, keyless
   segments, `keys`' free-and-absent case, and the `log` profile, with the version rule they
   need (version 0 for keyless documents, the writer's version check, F13-R9); the digest split,
   once: `D` with the key, `inverted`'s length record for every document, the interim record
   dropped; `digest.txt` and the keyless fixtures re-pinned; T2's schedules, keyless included.
6. **7c-6, F20:** manifest version 2 (the slots file, the mask, the version range, the values
   generation, the key prefix as `keys`' option); the open and `install` checks; L16's tests.
7. **7c-7:** format 12's layout code deleted (`docids_section`, `search_layout_12`, the legacy
   paths), as L14 lists.

After step 7c, on the same branch: F10 to F13 by "Choosing F10 to F13 together" (after this
revision's re-check of F13-R7 and F13-R8), F15 and F17 as the `values` and `term_lists` plugins,
F18 in the core, F19's check, F14 if P13 wins, and F22's fuzz targets with each decoder.

## First steps

Nothing here builds while the host holds strictly. Steps 1 to 3 respect the review's constraint
until the rebase (C4): no edit to `reader.hh`, `writer.hh`, `index.hh`, `merge.hh`, `search.hh`,
`searcher.hh`, or `fuzz/fuzz_segment.cc`.

1. **Codec units with unit tests**, free functions in `keys.hh` and new headers: K2 (the run
   column through `find_run`), K3's codec pair (zstd through `compressors`, for `termdict`'s
   option in step 3); F10's advance, flags, and impact entries for A1 to A4, with the per-block
   validation; F11's E1 to E5; F13's seed pairs against brute force; `keys.hh`'s `scan(from)` against a brute-force sorted filter, from every
   bound and with early stops; `docvalues.hh`'s `binary` kind with its version byte dropped;
   F17's and F18's codecs; `read_version` as an added function, not yet called. `format.hh`'s
   open rules change only in 7c-1, since today's writer would fail them. M17, M18, and M23 are
   the query-gap workstream's experiments (its E5 and E6), not this branch's.
2. **Tools:** `keybench` against main's API, for the lead to land on main and record format 12's
   T5 row; `poststats`' projections, its frontier histograms, and `keysim`'s fair comparison on
   this branch. `poststats` reads only the postings and lengths format 12 already has, so it can
   also land on main, which lets the lead run the projection on the existing reordered gate index
   without a new build, and gives the query-gap design's E1 its main-side mode.
3. **`termdict`, in its own repository:** a builder option to cut blocks only at run boundaries
   (K2), the restart interval and block bounds as parameters (K4), and the block-body codec (K3,
   F13-R19), both recorded in the index's options form only when set (F13-R20), with today's values the defaults and its own CI and fuzz
   corpus, beside automata's additive `cursor::seek` (O2),
   which F8's lower-bound scans use. `termdict` is strata's alone.
4. **After layers lands and the re-check passes:** the rebase and 7c-1 to 7c-7.
5. **After 7c-3:** the projection of "Choosing F10 to F13 together", step 1, and F8's `keysim`
   run, on a branch build's reordered 5M merge, under a hold the lead grants.

## Risks

- **F10 trades bytes for decoding.** Fixed-width entries may grow the carried postings row while
  they shrink the query row; the joint selection reads T3 and T4 together.
- **The key codec trades lookup time for bytes.** By the estimates only K3 passes the dictionary
  row with the filter, and K3 decompresses a block per hit; T5 bounds it.
- **The whole index has 3.2 MB of headroom** at format 12's figures, so every section that grows
  must be paid for elsewhere.
- **F13's seeds may rarely help:** k = 1,000 needs 1,000 postings to dominate a pair, which long,
  flat lists give a weak pair. T6 keeps them only if they win.
- **The key routing changes the digest, `verify`, and Xapiand's `Q`-term queries at once.** T9,
  the E2E suite, and K10 cover it.
- **The rebase redoes F2 to F9 on new code.** The old commits' tests are the oracle for the new
  code's behavior, and every 7c commit but the digest commit keeps `digest.txt`.
- **F23's rule 3 lets a later structure change how its owner's sections decode** in segments that
  set its bit. Old binaries refuse those segments (rule 5), and rule 6 needs Track D8's version
  check before any cluster mixes binaries.
