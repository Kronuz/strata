# Format 13

Revision 1, amended for the re-check's findings ("Changes after the re-check"). It folds revision 0 (`1bc0a38`, amended by
the branch's F7 to F9 commits) together with its preliminary review
(`docs/reviews/format-13-design.md` at `4266374`: APPROVE WITH CONDITIONS, F13-R1 to F13-R17,
conditions C1 to C6), the owner's decisions recorded in PLAN (`PLAN.md:16-20, 34-41, 47-48` at
main `bcf89c8`), the system review's SYS-1, SYS-4, SYS-6, SYS-7, SYS-9, and SYS-16, and what two
live documents address to format 13: the automata review (`docs/reviews/automata-design.md` on
`phase5/automata` at `9b9e151`: AUT-R3, AUT-R4, AUT-R13, and its conditions C4 and C5) and the
query-gap design (`docs/design/query-gap.md` on `design/query-gap`, revision 0 at `8a6503f`: M16
to M23; revision 2 at `3cba768`, "What format 13 must record", folded by F13-R31). It sits on the layers design at revision 3 (`layers.md` at `a874ab0`;
`phase4/layers-refactor` at `707066b`, steps 0 to 7b), not on main's `segment`. Each of those
branches' worktrees was compared with `origin`: none holds an unpushed commit.

Format 13 is the one segment format version that carries every pending format change (PLAN,
"Carried rows"): the key dictionary as its own section (`phase-4.md` D12), the query gap's skip
entries (`docs/perf/query-gap-merged.md`), the postings change P1 leaves owed, Phase 4's doc
values, term lists, index sort, and optional sections (`phase-4.md` D2), reserved kinds for
points and for Track P's P8, P14, P18, and P20, and the format-13 items of the layers design.
One design, one review, one set of fixtures and fuzz bases. Decisions are F1 to F23; gate rows
T1 to T11; owner questions Q1 to Q6, all answered.

It consolidates; it does not reopen. Where `phase-4.md` or `layers.md` decided a point and its
review closed it, this document cites the decision and fixes its bytes. The open choices are
the key codec (F8) and the postings encodings (F10 to F13), which name candidates and the
measurement that chooses each. No size or speed below is a measurement of format 13: numbers
are format 12's (`docs/gates/phase-3-ratchets.md`, `docs/perf/`), or estimates, marked as such.

Code is cited at `phase4/layers-refactor` `d508158`, the base the lead named and step 7c's
(re-read there from revision 1's `707066b`: F13-R34, F13-R38), except `format.hh` and `keys.hh`,
cited at this branch's pre-rebase code commit `e779835`, and citations marked otherwise. Each
cites the base's code that step 7c replaced, not the branch's. Clean room, as `phase-4.md`: Xapian's behavior enters from its documentation
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
| F13-R11 | Closed on layers' side (step 7a, `d911396`); in step 7c `merge_options::key_prefix` stays, meaning only the inverted builder's backstop, keys found by the keys reader's scan (F13-R38) | Step 7c |
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
| F13-R23 | Bit 13 is reserved for `stored_spans` (the owner's Q5): bit 31 of a frame's size means "continues" only where it is set; F23's table gains its row; Q5 answered | F4, F19, F23, T1, owner questions |
| F13-R24 | A zstd bump needs no coordinated restart: nothing compares segment bytes across binaries, so replicas on two zstd releases coexist through a one-node-at-a-time upgrade | F21, F22, T8 |
| F13-R25 | The layers re-check's format-13 items: the key prefix stays the index's, passed by the core; stage 1 refuses an add whose key lacks it; a key leaf over buffered keys; the length record keyed on the index's structures; T7 by protected pages; the readable bits and one presence bit per reader; the versions writer's minimum rule at every width | F4, F8, F9, F20, F21, T7, step 7c, owed changes |
| F13-R26 | Text: the base's tip named; 7c-3's interim record covers buffered keys, and its scope names every keyed test and tool; Q5 and Q6 answered; `limits.md:36` leaves the owed changes | Rebase, step 7c, owed changes |
| research/indexing 5.3 | Proposed, then ruled by the second re-check (F13-R32) | "Copy merges" |

## Changes after the second re-check

The second re-check (`format-13-design.md`, "Second re-check", at `81289c3`: APPROVE WITH
CONDITIONS) raised F13-R27 to F13-R34 and ruled on the copy-merge proposal; the lead's rulings
follow it in the ledger. Each is answered here in its own commit.

| Item | Change | Where |
|---|---|---|
| F13-R30 | Sections align to 8 bytes; bit 15, `compact`, reserved for the core; a set bit outside F4's table is one this binary lacks, not corruption; T5's and T10's rows follow PLAN's many-index workload | F4, F5, F6, F23, T1, T5, T10 |
| F13-R33 | F8's routing, disjointness, and add-path rules apply only when the index's key prefix is nonempty; the prefix is empty exactly in a keyless index or a layout without `inverted`; 7c-3 tests term queries and wildcards over a keyless index | F8, step 7c |
| F13-R28 | The manifest records the configured bits apart from the union with the segments' bits; the length record keys on the configured bits, which replicas set from the log; the union serves only the readability checks | F20, F21 |
| F13-R34 | The rebase's base is layers `d508158` (R12 to R19 folded, main merged); `b2f7600` is dropped at the rebase | The rebase onto layers |
| F13-R35 | The footer's bits are classified before the table is decoded, so a later release's segment, its own kinds placed and its table changed, is unsupported, not corrupt; presence applies only in a segment whose bits this binary reads | F4, T1 |
| F13-R36 | Norms not built are derived from the exact lengths on first use, so a fresh open of every profile reads no byte of `lengths`; T7 covers the search, norms-less, and document profiles | F7, T7 |
| F13-R37 | `check()` refuses an ordinal named by two keys, a range other than the first and last key, and a key the filter misses; the fuzzer decodes `keys`' blocks | F8, F22 |
| F13-R38 | Text: `merge_options::key_prefix` kept as the backstop's; code cited at `d508158`; the index's configured bits from its layout | F8, step 7c, owed changes |
| F13-R39 | A keyless index refuses a document's key as `invalid` (the lead's ruling) | F8 |
| F13-R31 | Query-gap revision 2's format-13 text: M18 out of F10, F11, and the selection (QG-R4's five readmission conditions); M17's record, order, bound, and checks; the order preset an index option; M23's check a 64-bit sum; the design cited at revision 2 | F7, F10, F11, F20, F22, "Choosing F10 to F13 together" |
| F13-R32 | The copy-merge proposal gives way to the second re-check's rulings: bit 14, `postings_runs`, reserved for items 1 to 3 (item 1 as a run table); items 4 and 5 are merge rules; F1's P17 row moves to bit 14 | F1, F4, F23, "Copy merges" |
| F13-R27 | `termdict`'s options form is validated (no zero interval or minimum, minimum at most maximum, flags 0 and 1 only); the reader sizes its restart storage from each block's restart count; K3's raw length is checked before decompressing and matched after; a flag the freeze does not write comes later only under a structure bit | F8, first steps |
| F13-R22 (row) | The owed-changes table regains `limits.md:35-36`, quoting strata's columns, which still read "recorded per tree" | Owed changes (C10) |
| F13-R29 | PISA's references (Q7, the lead's yes): postings against the smaller of keyed `-bp`'s and PISA's; query projections against each row's fastest engine from the one calibration, PISA's exact rows included; the freeze criteria and rows on PLAN's samples and one confirmation | F1, the joint selection, T3 to T6, T10, owner questions, owed changes |

## What this builds on, and what it changes

| Design | What revision 1 uses |
|---|---|
| `layers.md` revision 3 | L2's three orders and S1's tail; L3 (key semantics in the core, encodings in the plugin, routing by the planner, the key reader's concept, whose `scan` now takes a lower bound); L4 (`P::structure`, `P::structures`, the per-kind table at step 7); L5 (builders and the sink); L6 (merges, `merge_options`, the order source); L7 (handles, presence by requiring bit, unreadable bits); L8 (the digest split, once); L9 (statistics per plugin); L10 (`document::key`); L11 (`holds<P>()` per segment, step 7b); L12 (the document profile); L14 step 7c; L16; S1, S2 |
| `phase-4.md` | D2, D3, D3a, D4, D5, D11, D12, D14, D15, D17, and R19's reserved flag |
| `docs/reviews/phase-4-design.md` | R1, R4, R6, R7, R19, R20, N2, N3, M3 |
| `workload-profiles.md` | Items 1 to 4 (keys section, optional sections, real-time get, a values generation) |
| `reordering.md` | D5's tie rule (F13); the 14.5 MB ordinal projection (Risks) |
| `docs/reviews/automata-design.md` | AUT-R4 (keys in expansion; a lower-bound scan), AUT-R3 (termdict's additive `cursor::seek`), AUT-R13 and its C4 (the tail profile before the freeze), C5 (ruled by the owner) |
| `query-gap.md` revision 2 (`3cba768`) | M16, M17, M19 to M23 as format-13 candidates, decided by its E3 to E7 before the freeze (its D5); M18 out (QG-R4); "What format 13 must record"; E1's attribution |
| `rocksdb-survey.md` | K3 (bounded, ordered key scans that skip segments by key range) and K5 (key-ordered merges), both on `keys`' scan |
| PLAN | Carried rows, "Every gate", Track P, "What waits", the owner's decisions |

Changes owed to other documents, each made with the commit that implements it or by that
document's owner, as the lead decides:

| Where | Change | Why |
|---|---|---|
| `layers.md` L2, format-13 column (`:200`) | `lengths` after `terms`, with `inverted`'s other streamed sections, not last among them | F5 |
| `layers.md` L3 (`:236-254`) | `key_reader::scan(from, f)` from a lower bound, with no prefix argument (the owner's ruling, `PLAN.md:47-48`) | F8 |
| `layers.md` L16 (`:862-893`) and L7 (`:568-573`) | The manifest records a structure mask, not plugin identifiers; "the manifest lists the bit's plugin" becomes "the manifest's mask holds the bit" | F20 |
| `layers.md` L1 (`:147`), L3 (`:277-280`), and the contexts (`:308, 379-384`) | The key prefix stays the index's: the core passes it to `validate_context`, `flush_context`, the merge's context, `check()`, and the planner; no plugin reads another's options | F8 (F13-R25 1; layers R13) |
| `layers.md` L10 (`:701-705`) | The buffer path's predicates gain the key leaf over a buffered document's key | F8 (F13-R25 3; layers R13) |
| `layers.md` L8 (`:604-606`) | `inverted`'s length record is keyed on the index's configured structures, not the layout | F21 (F13-R25 4; layers R14) |
| `layers.md` L7 (`:568-573`) and L14 7c (`:807`) | The readable bits, `index_sort` among the core's; presence computed before the readers and passed in `open_context`; T7 by protected pages | F4, T7 (F13-R25 5 and 6; layers R15, R16) |
| `phase-4.md` D2 (`:54-56`) | The freeze waits for no D15 result | F16, F23 |
| `frozen-index.md:46-49` | A frozen segment is format 13 with P20's structures under F23: a reader without them refuses it, it does not skip them | F23 |
| `BENCHMARKS.md:166-175` | `terms`, `term_index`, `keys`, `key_index` against `.tim`, `.tip`, `.tmd`; `docids`, `versions` against the id field's doc values | F8 |
| `BENCHMARKS.md:58` and its counterpart table | PISA is the reference for postings bytes and query classes (Q7), no longer "not a ratchet target"; the counterpart table places PISA's files beside `.doc` | F1, F13-R29 |
| `layers.md:903` (L14 7c) | "`merge_options::key_prefix` deleted" becomes kept as the inverted builder's backstop, keys reaching the merge through the keys reader's scan (deviation 1, accepted) | Step 7c (F13-R38) |
| `limits.md:35-36` | Row 35, strata's columns ("Up to 8 and 16, recorded per tree"; "One byte each in the tree header"): recorded per slot declaration (F15a) as well as per tree. Row 36, strata's columns ("Recorded per tree"; "A format field, chosen by D15's measurement"): bytes per dimension 1 to 16, Lucene's maximum; mechanism F15a's byte and the fixed-width `bytes` rule (a `bytes` slot's points hold exactly d × w bytes, compared as unsigned bytes). Lucene's column ("Up to 16") stays | F15 (F13-R22, C10) |
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
| P17 rebase copies | The copy merge's runs within postings | Reserved bit 14, `postings_runs`, with the copy merge (F23; F13-R32): one mechanism |
| P2, P6, P7, P9 to P11, P16, P19 | None | — |

**The freeze.** Format 13 freezes once, when all of these hold on the freeze candidate, a
branch build:

1. This design, revised, clears a full design review with its code (PLAN, "What waits").
2. The candidates of F8 and F10 to F13 are chosen by their rules, each kept and losing
   candidate's table in `docs/perf/` (C3), and every losing candidate's decoder and hook is
   removed. H22's codec verdict is among them (F13-R44, the lead's ruling): after the freeze a
   block codec could come only under an F23 bit, two decoders of `postings` for format 13's life.
3. The carried disk rows, after a reordered full merge of the keyed index at PLAN's
   confirmation size (`PLAN.md:109-122`; section sizes are exact, so a branch build's are the
   gate's): postings ≤ 1.0× the smaller of keyed `-bp`'s `.doc` (833.7 MB at 5M,
   `phase-3-ratchets.md:204`) and PISA's counterpart, its compressed documents and frequencies
   with its block-max data as the gate builds them, at the same corpus size, its raw file and
   commit in `docs/perf/` (F13-R29, Q7; C9); the dictionary row (`terms`, `term_index`, `keys`,
   `key_index`) ≤ 1.0× keyed `-bp`'s `.tim`, `.tip`, `.tmd` (75.8 MB, `:206`; the owner's
   reference, `PLAN.md:38-39`); the whole index ≤ 1.0× keyed `-bp`'s 2,542.5 MB (`:203`); every
   other section no larger than format 12's figure (`:201-208`).
4. The carried query rows, after merging: per class and command, the projection (the class's
   branch A/B ratio of format 13 over main's format 12, times format 12's ratio against the
   row's fastest of Lucene, Lucene `-bp`, and Tantivy from the one calibration, the query-gap
   design's E0 and its D2; F13-R29) ≤ 1.0 by its upper bound, the product of the two intervals'
   upper bounds. PISA is reported beside every row and binds from Phase 6's 0.7× (F13-R45, the
   lead's ruling, superseding Q7's reading): every format-dependent part of PISA's lead that E1
   assigns (F12's partitions, P20's quantized impacts, H22's codec) has its place in format 13,
   a kept candidate or a reserved structure. A class that misses needs a profile
   that attributes the remaining time to code outside the format, each part assigned to a named
   Track P item with its projected effect and closed before Phase 4's gate; the query-gap
   design's E1 is that attribution (`query-gap.md`, D3), and it covers PISA's lead. Part of that
   lead is format: block-max data over 40-posting blocks and quantized scores
   (`phase-3-ratchets.md:309-313`), which are F12's partitions and P20's reserved structure, so
   "no part of a miss is attributed to the format at the freeze" is checked against both.
5. T4's guard (format 13 over format 12 ≤ 1.03 by the upper bound, every class) and T5's key
   lookups.
6. The expansion class's tail profile (AUT-R13, automata C4): a branch profile of prefix,
   wildcard, and fuzzy queries on the 5M segment, `or` and count modes, states the share of
   their time in tail decoding, and E4's decision weighs that class when it dominates. Phase 1's
   review left the question to "the format version in Phase 5"
   (`docs/reviews/phase-1.md:239-244`), and format 13 is that version.
7. F22's fixtures and fuzz bases pinned at the freeze commit, one CPU-hour per decoder (T2), and
   all eight CI jobs green at that commit (C2).
8. Every row of "Freeze readiness" has its place (F13-R47; the owner's ruling).

Items 3 to 6, T3 to T6, and T10 run on PLAN's targeted runs (`PLAN.md:109-122`; F13-R29): work
iterates on samples, one calibration names each row's fastest engine, every reference calibrated
at those sizes, and one confirmation sized for minutes is the row; the 5M figures cited here stay
Phase 3's record, not a size these rows run at.

The evidence for 2 to 6 is the owner's SYS-6 kind (`PLAN.md:19-20`): branch A/Bs under a hold
the lead grants and the section sizes of a branch build, recorded in `docs/perf/` as candidate
tables, never as gate rows. The gate rows run on main at landing and confirm them; since a miss
there costs format 14, every projection states its interval. D15 is no longer a freeze criterion
(F16, C6).

Until the freeze, the branch writes format 13 with candidate encodings, fixtures are re-pinned
with each change, and no index outside tests holds format 13.

## F2. Version and compatibility

- The header's version is 13 (`format.hh:68`); a reader accepts versions in
  [`min_read_version`, `version`], both 13 at the freeze.
- **Until the freeze, a pre-freeze version** (CR11, the lead's ruling). Format 13's bytes keep
  changing until F1's freeze (F10 to F13, F8's codec, the `values`, `term_lists`, and
  `index_sort` plugins), so segments write `format::pre_freeze` (`0x80000001`) as both `version`
  and `min_read_version`, and the manifest records it as its segments' range. A reader accepts
  only that exact number: an index from an earlier pre-freeze build, 7c's segments at 13
  included, is refused under rule 2 as `unsupported_segment`, and by its manifest's range as
  `unsupported_index`, instead of misread. Each commit that changes format 13's bytes bumps it;
  the freeze sets both to 13, which re-pins the fixtures and the prefix CRC below. Its prefix
  CRC is `0x4062b616`; format13_test refuses versions 13 and `pre_freeze - 1`.
- **Format 13's checkpoint** (the owner's quick freeze, 2026-10-20; PLAN, "Quick freezes before
  release"; tagged `format-13`). `format::pre_freeze` is 13 (it was `0x80000008`) and
  `files::pre_freeze` is 2 (it was `0x80000005`, after version 1 at format 12), so segments write
  13 as `version` and `min_read_version`, the manifest records 13 as its range, and the files
  family writes 2. A reader accepts only those: a build's earlier pre-freeze numbers, 12, and 14
  are `unsupported_segment` or `unsupported_index`. The prefix CRC is `0x4ff2df7b`; the fixtures
  differ from the build before only in the prefix (segments) and the shorter version varint
  (files, four bytes), and `digest.txt` and `seeds.txt` are unchanged. A checkpoint promises no
  later read of 13: the next byte change sets `format::pre_freeze` to `0x80000009` (format 14's
  series) and the next layout change sets `files::pre_freeze` to `0x80000006`, each bumped per
  change, and the fixtures are re-pinned with each.
- **The bump is checked** (C17 item 1). `test/fixtures/pre_freeze.txt` pins `format::pre_freeze`
  and `files::pre_freeze` beside a hash of each family's fixtures (the segments' `.sx`, the
  files' `.bin`); `fixtures_test` fails when a family's bytes change and its version does not,
  re-pin or not, and when a bump is left unrecorded.
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
  so a flip from 13 to 12 cannot pass for rule 3 (likewise the pre-freeze number's).
- `read_version` runs before `decode_footer` in `core_segment`'s constructor
  (`reader.hh:103-105`), `load_segment` (`index.hh:2081-2095`), and `written_segment`
  (`index.hh:839-840`), so another version's footer never needs to parse (F13-R1).
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
core's and plugins' rule arrays (`reader.hh:148-152, 246-253, 456-459, 547-549`), `rule_of`
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
  read (`reader.hh:118-126`); `lengths` whole on its first use.
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

The footer's structures words (F6) record which structures the segment was built with, one bit
per structure, 64 bits: bits 0 to 31 in the first u32, 32 to 63 in the second (F13-R40), read as
one little-endian u64 and classified alike, so a later release has 48 unlisted bits before
format 14. A plugin owns a mask of bits (`P::structures`, L4): its own bit, which `holds<P>()`
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
| 13 | 8192 | `stored_spans` | `stored` | `stored` | reserved (R19) |
| 14 | 16384 | `postings_runs` | `inverted` | `inverted` | reserved (copy merges, P17; F13-R32) |
| 15 | 32768 | `compact` | core | — | reserved (small segments) |
| 16 | 65536 | `version_clocks` | `keys` | `keys` | reserved (durability D12: an HLC per version) |
| 17 to 63 | | none | | | lacked by every format-13 binary today; 32 to 63 in the footer's second word (F13-R40) |

`index_sort` requires `values`, since D14's order is read from slot values at merge (L6's order
source). Bit 13 is reserved for chunked stored records (the owner's Q5, `PLAN.md:51-52`; F19);
bit 14, `postings_runs`, for copy merges and P17 (F13-R32); bit 15, `compact`, is the core's, reserved for the small-segment profile (`small-indexes.md`,
direction 4; F13-R30); bit 16, `version_clocks`, for the durability design's hybrid logical
clock per version (its D12 and D16; F23). Every other bit is one no format-13 binary reads
today, and takes the second rule below.

**What a set bit means at open** (F13-R3; C6 rule 4). The bits a binary reads (F13-R25 6;
layers R16) are the core's, `index_sort` included since F18's reader validates it, and each
layout entry's `P::structures`; the core's own kinds are `header`, `docids`, `stats`, and
`index_sort`. The rules below classify a set bit by them.

- A bit whose structure this binary reads: its sections open by the presence rules below.
- A bit whose structure this binary lacks (a reserved bit before its release, `inverted` in a
  binary built with `document_layout`, or a bit no table of this binary lists, F13-R30) is never
  read past and never called corruption: a lone segment throws `unsupported_segment`. In an index, F20's mask decides: a
  binary refuses an index whose mask holds the bit with `unsupported_index` before any segment
  opens, and a segment that sets a bit its manifest's mask lacks is corruption, since the manifest
  vouches for what its segments hold.
- A bit no table lists is a later release's structure (F23), not damage: the footer's CRC still
  fails a flipped bit as `corrupt_segment`, and a resealed input is refused either way. So a
  plugin's later item, such as a one-block dictionary without its FST, needs no reservation at
  the freeze.
- A sub-structure's bit without its owner's own bit, or `index_sort` without `values`:
  corruption.

**Presence** (L7), in a segment whose bits this binary reads (the bits are classified first, from
the footer, before the table is decoded: F13-R35): a section is present exactly when the bit that
requires it is set; mandatory
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
creation and checked at open (`index.hh:229, 2003-2004`), so the two agree for `keys`.

`holds<P>()` reads a per-segment mask the core computes at open (layers step 7b, `a86610e`,
`reader.hh:813-835`). From step 7c, `basic_segment` computes it from the footer's bits before any
reader opens and passes each reader its bit in `open_context`, so `holds<P>()` and the reader's
emptiness are one bit, never two derivations of it (F13-R25 6). T1 adds the three
classifications of a set bit the binary lacks: a lone segment (`unsupported_segment`), that
segment in an index whose mask holds the bit (`unsupported_index`) and in one whose mask lacks it
(`corrupt_segment`); and a plugin's bit clear, where `holds<P>()` is false and the reader answers
empty. T1 also gains the free-and-absent cases no test covers today: `stored` absent with its
bit clear (opens, empty records); `norms` absent with its bit clear; `inverted` absent with its
bit clear, which layers' reader still refuses (it requires `lengths` and `norms`,
`reader.hh:264-273`); and a `document`-profile segment.

## F5. Table of contents and file order

- The table keeps format 12's entry layout (main's `format.hh:156-168`): kind u16, region shift
  u16, offset u64, length u64, CRC u32, then the region CRCs. Each kind at most once.
- **One encoding per segment** (F13-R4): the header's entry at offset 0 with length 36 (F6);
  every later entry at the previous entry's end rounded up to 8 bytes (`section_alignment`,
  `format.hh:87`, from 64; F13-R30), in F3's rank order; the table at the last section's end
  rounded up to 8. Nothing needs more: fixed-width reads are `memcpy`s (`format.hh:174-179`,
  `docset.hh:1039`), `docset`'s SIMD loads are unaligned, and no aligned load exists in strata's
  or its pinned libraries' code. At 8 bytes a segment's padding is at most 84 bytes, about 390
  bytes less than at 64, 21% of a one-document file. This
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
                   [min docid u64][max docid u64][structures bits 0-31 u32][bits 32-63 u32]
                   [CRC-32C of the preceding 48 bytes u32][magic "SEND"]
```

- The first 16 bytes are frozen in every version (F2); bytes 16 on are version 13's. A CRC over
  the frozen bytes only, not the whole header, leaves the rest of the header free to change in a
  later version.
- The header's flags word must be zero; nonzero is corruption. The footer's second structures
  word, reserved as zero before F13-R40, holds bits 32 to 63: a set one is a structure this
  binary lacks, `unsupported_segment` alone and `unsupported_index` by the mask, never damage
  (F4). Every format-13 segment so far writes it zero, so no byte changes.
- The structures word is F4's. The footer grows from 48 bytes (main's `format.hh:63`) to 56; its
  layout is version 13's alone, since every reader reads the prefix first.
- **The segment id is the segment's physical identity** (F13-R41; durability D8 and D16; I13,
  closing Phase 3's condition 8). It is unique across every producer of the index's segments
  (replicas, terms, installs), written once by its producer and never rewritten by a copy
  (install, segment shipping, repair, object storage). Bytes 0 to 7 are the producing leader's
  term, which the host sets (`set_producer_term`; 0 standalone), so ids from different terms
  never collide, which the durability design asks for segment mode; bytes 8 to 15 are a 64-bit
  base drawn at open plus a per-open count, through splitmix64's bijective finalizer, so they
  never repeat within an open and collide across replicas flushing in one term (document mode)
  only by chance, about n² / 2⁶⁵ for n segments. Lucene's and Tantivy's segment ids are random
  128-bit values, Lucene's a random base incremented per id. `index_options::identity_seed`
  fixes the base for tests that compare two indexes' files. Format 12's meaning (the local
  segment number and the index UUID's first 8 bytes) collided across replicas, which share the
  UUID and number segments alike (`probe_identity.out`). The local number stays the file's name
  and the manifest's `id`. Segment bytes change only in this field's value; fixtures pass their
  id in, so none is re-pinned.

## F7. Statistics

L9 (revision 3): each structure's statistics live in a pinned section of its own.

- `stats` (the core's): [document count u64], 8 bytes against format 12's 28 (`reader.hh:134`).
- `inverted`'s statistics, [total length u64][minimum length u32][maximum length u32][maximum
  positions u32], 20 bytes, lead `term_index`, before the dictionary's index. `term_index` is
  pinned, present whenever `inverted` is, checked whole at open with its section CRC, and copied
  out with the index (`reader.hh:274-276`). There is no inner CRC: the section's covers them, and
  the fuzzer's resealing reaches the total length and the positions bound that
  `positions_reader` relies on (F13-R4). Revision 0's 24-byte `lengths` header and its CRC go.
- **The preset M17 assumes** (F13-R21, F13-R31), only if M17 is kept: the statistics end with
  [preset count u8][per preset: scheme u8 (0 `bm25`, 1 `bm25_xapian`, `search.hh:48`), k1, b, and
  `min_normlen` as IEEE-754 binary64, little-endian], 26 bytes for one preset, M17 ordering by the
  first. The writer records the index's order preset (F20) at the flush or merge, never its
  inputs', as it sets structure bits (F4). A reader uses a list's first pair only when the
  query's scheme and parameters equal a recorded preset bit for bit; under any other it evaluates
  every pair, as today. An index option can change after a segment is written, and without this
  record the segment could not say which preset its order assumed. A count of 0, a scheme code
  above 1, or a preset outside QG-R1's preconditions (0 ≤ b ≤ 1, k1 ≥ 0, `min_normlen` ≥ 0, each
  finite) is corruption. If M17 is not kept, the statistics stay 20 bytes.
- `lengths` holds only [width u8][bit-packed lengths], verified whole on its first use
  (`reader.hh:298-310`). The default scheme reads norms only and never touches it; an open reads
  none of it.
- A segment without `inverted` has no statistics: its total length is 0, which the manifest's
  totals accept (L9).

## F8. The key dictionary (D12; R6)

**The key leaves the term dictionary.** The key is the core's field (`document::key`, L3, L10),
held in `keys` exactly as operations name it (`o.key`; for Xapiand the `Q` prefix included). The
core never adds or strips a prefix, and `document::key` holds the same bytes (F13-R2).

- **The key prefix is the index's** (F13-R25 1; layers R13), in the manifest's index fields
  (`files.hh:177`, F20), not a plugin's option. The core passes it to whoever needs it:
  `validate_context` for stage 1, the flush's and the merge's contexts for the `inverted`
  builder's backstop, `check()`, and the planner, which reaches both readers through
  `basic_segment`'s `get<P>()` (L11). No plugin reads another's options, so `inverted` never
  depends on `keys`.
- **An empty prefix turns the rules below off** (F13-R33). The prefix is empty exactly in a
  keyless index (from 7c-3 to 7c-4 every index carries `keys`, F13-R18, so a keyless one carries
  it with an empty prefix). An index configured with `keys` takes a nonempty prefix in every
  layout, a `document_layout` one included, as creation and open check (CR5; F13-R47 item 4): a
  layout without `inverted` uses the prefix only to name keys. Routing, disjointness, and the
  add-path rule apply only when the prefix is nonempty: with an empty one, every term is
  `inverted`'s and no term is refused for its first bytes.
- **A keyless index refuses a key** (F13-R39, the lead's ruling): an index not configured with
  `keys` refuses, as `invalid` in stage 1, an add or replace whose `document::key` is set, so a
  host that sends keys to a keyless index fails loudly instead of losing them; one condition,
  deterministic on every replica. Today that index is exactly the one with an empty key prefix
  (F20's configured bits hold `keys` only with a prefix).
- **Every key starts with it** (F13-R25 2). Stage 1 refuses a key operation whose key lacks it
  (`index.hh:1580`), and, in an index with a key prefix, an add whose `document::key`, when
  present, lacks it, as `invalid`: the add path's form of the same check. Otherwise such a key
  would be invisible to every term query and wildcard, the index listing included.
- **Terms and keys are disjoint by construction.** Stage 1 rejects a document whose `inverted`
  part holds a term with the key prefix (`inverted_plugin::validate` reads the prefix from its
  `validate_context`), and the `inverted` builder refuses one with `std::invalid_argument`
  (F13-R2, F13-R10: refuse, never drop); `check()` verifies that the dictionary holds none.
  Revision 0's stage-1 rule for a boolean key term (`index.hh:1533` at `e779835`) belonged to the
  key as a term and goes with it.
- **Routing is the planner's** (L3): the term table (`searcher.hh:116-177`) and `snapshot_df`
  (`searcher.hh:104-110`) resolve a term with the key prefix through `keys_plugin::reader` as a
  key leaf: an iterator over at most one ordinal (`keys::reader::find`, deleted copies included,
  which the live filter removes as for any term), wdf 0, no positions, and an upper bound of 0
  under both schemes. It never asks `inverted`. Its df counts the segments whose `keys` hold it,
  deleted copies included, as format 12's postings did, so `verify` and `parity` see the counts
  and matches format 12's key term gave them. The buffer path routes alike (F13-R25 3; layers
  R13): L10's predicates over buffered documents gain the key leaf over a buffered document's
  `document::key`, so delete by query (D16) on an id term or an id wildcard reaches buffered
  documents, whose terms no longer hold the key. No reader falls back to another, and
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
  empty deps (`merge.hh:258-273` scans the key prefix with `inverted`'s deps today).

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
  a block's raw body past 64 KiB, so a larger body holds one entry and is stored raw. A raw
  length above 64 KiB in a compressed body is corruption before anything is decompressed, and the
  frame must decompress to exactly that length (F13-R27), so the bound holds against the file's
  bytes, not only the writer's. A lookup
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
  without the options refuses the form instead of misreading it.
- **The options form is validated** (F13-R27). Its decoder refuses a restart interval or a minimum
  of 0 (a runtime interval divides), a minimum above the maximum, and any flag bit but 0 and 1.
  Today's block decoder checks counts against the constants and holds restart offsets in a fixed
  array of ⌈64 / 32⌉ = 2 (`termdict.hh:229, 342, 347, 401, 437`), which K3's 128 entries and K4's
  96 do not fit, so the reader sizes its restart storage from each block's restart count, which
  the block's bytes bound (each offset takes at least one byte), and steps runs by the recorded
  interval; no cap is needed. A flag the freeze does not write (P12's `terms` with K3's codec, for
  one) arrives later only under an F23 structure bit, since a reader without the option would
  call the form corruption, which F23 rule 5 forbids for a later binary's structure.
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
(`keys_builder::encode_versions`, `writer.hh:682-700`), validated at open (`reader.hh:504-511`).
Present exactly when `keys` is. A keyless segment has no `versions`, and `version(ord)` is 0 (D2,
M3).

- **A version belongs to a key.** A document without a key has version 0 in every index, and its
  verdict reports 0: otherwise a docid-addressed replace would read `current + 1` from the buffer
  and 0 after a flush, and replicas flushing at different times would disagree. Conditional
  operations on a keyless document reject in stage 1 (M3).
- **The writer enforces it** (F13-R9): the `keys` builder refuses a nonzero version without a key
  in every segment (`std::invalid_argument`), and `check()` verifies it. Today it refuses only in
  keyless segments (`writer.hh:571` at `e779835`), and layers' keyless fixture writes `1 + d % 3`
  for keyless documents (`fixtures_test.cc:79, 113`); that fixture is re-pinned in the digest
  commit (F21).
- **Keyless segments and this rule land together** (F13-R18): in step 7c's digest commit
  (7c-5). A keyless segment without the rule would read 0 after a flush for a document the
  buffer holds at version 1, or at `current + 1` after a docid replace, so verdicts and the
  digest would depend on flush timing. Until that commit every index is configured with `keys`
  and writes `versions` for every document, as format 12 does.
- **The minimum's rule, at every width** (F13-R25 7). The reader requires `lo ≤ 2^64 − 2^width`
  (`reader.hh:509`), so that `lo` plus any packed value stays below 2^64, and the writer's width
  is `bit_width(hi − lo)` (`writer.hh:686`). A valid section breaks the check at any width above
  1 when its versions sit near 2^64 − 1, not only at 64 (the layers re-check,
  `layers-design.md:806-811`). The writer writes minimum 0 whenever `lo + 2^width − 1` would
  exceed 2^64 − 1. The width is then 64: at width w ≤ 63 the condition means `lo > 2^64 − 2^w ≥
  2^63`, so `hi ≥ 2^63`, and at width 64 the span is already at least 2^63. Every file the writer
  writes passes the check, and the check stays as it is.
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
(`reader.hh:298-310`). Each is also measured with one entry per 256 postings (two blocks), M16's
second granularity, which halves the entries and makes an advance decode up to two blocks. The
query-gap design's E6 microbenchmarks them over E1's access traces (`query-gap.md`, E6).

**Two asks of the query path** (`query-gap.md`, "What the query path needs from format 13"): a
group's impact entries stay contiguous, so a range bound walks consecutive blocks' pairs, as the
per-group stream above does; and a block's bound is one read away, which M17 gives by a frontier's
first pair if M17 is kept. M18's argmax pair beside the advance entry is out (F13-R31; QG-R4).

**Validation, per block decoded** (F13-R8). Locating a block reads fixed-width entries without
decoding or checking them; a binary search reads no neighbor, so revision 0's "per entry read"
could not hold. When a block is decoded, the reader checks that its offset and length (its entry
and the next) lie inside the term's blocks area, that its codec code is one the format keeps, and
that its ordinals strictly increase within (the previous entry's last, its own last], the last
equal to its entry's; otherwise `corrupt_segment`. Format 12's `check_increasing`
(`reader.hh:1033-1040` at `e779835`) is that check and stays, unless M23 (F11) stores deltas so
that ordinals increase by construction. Its check is then a 64-bit sum (F13-R31; QG-R5, QG-R25):
the block's decoded values S and their count n satisfy S + n = last − base, with the base −1
before a term's first block, the entry's last above its base and below the document count, so no
partial sum can wrap past 2^32; the 25-bit lane comparison is its equivalent only for a block
whose header declares at most 25 bits (a FOR width, an exception block's width plus its high
bits, or a constant's value), the width read from the header, never from the values.
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
merge's output is a function of its input (F21); M17 and M23 are rules for every block, kept or
dropped whole. M18 (one argmax pair per block for each preset) is out of format 13 (F13-R31):
it enters this selection only with all five of QG-R4's conditions met: E1's block-frontier
histogram; its bytes against the alternatives that keep whole frontiers (argmax-first with the
rest delta-coded, and E5); a `SCHEME=bm25_xapian` A/B of the group-only fallback per row; P17's
cost; and Phase 2's D3 rule (stored pairs dominate the dropped ones) amended by name.

| Id | Change | Hook |
|---|---|---|
| E1 | Omit a block's wdf bytes when every wdf is 1; today such a block is `bitpack`'s 5-byte constant block (`bitpack.hh:403-408`) | One flag bit per block (F10) |
| E2 | Partitioned Elias-Fano as a block ordinal codec (P4) | A codec code (F10) |
| E3 | Runs of consecutive ordinals (P4) | A codec code |
| E4 | A tail of 16 to 127 postings as one FOR block of its length; `bitpack` packs only 128 values today, so this needs a partial-block form there (additive) | Implied by df mod 128 |
| E5 | A block's pairs reduced to those that differ from its group's frontier | One bit per group |
| M17 | Each stored frontier argmax-first at the segment's own clamped average length under the recorded preset, the rest in `competitive()`'s order, ties by wdf; a reader bounds a list by w · t₁ / (t₁ + c · K(ℓ₁, A)) · (1 + 2⁻⁴⁰), c = min(1, A / A'), only under the recorded scheme and parameters, on stored frontiers only (QG-R1); `check()` verifies domination and argmax-first; P17's copy path re-selects each copied frontier's first pair at the merged segment's average (QG-R2) | A writer's ordering rule; the preset recorded with `inverted`'s statistics (F7), the order preset an index option (F20) |
| H22 | SIMD-BP128's decode (PISA's codec) for the FOR blocks, against `bitpack`'s FOR and exception decode over E1's block mix (E6's microbenchmark; F13-R44, the lead's ruling). `bitpack`'s block is already SIMD-BP128's vertical layout, 128 integers in four 32-bit lanes in 16 × b bytes, so the price may be the decoder alone; ported into `bitpack` under PISA's notice if kept, the losing decoder removed before the freeze | The FOR block's decoder; a codec code only if the layout differs |
| M23 | Ordinal deltas stored as gap − 1, so a block's ordinals increase by construction; the reader's check is F10's 64-bit sum (QG-R5); `bitpack`'s delta decode gains its +1 mode | The FOR block's meaning; no flag |

A FOR block whose deltas are all equal is already a 5-byte constant block (`bitpack.hh:403-408`),
so a block that is one run of consecutive ordinals costs 5 bytes in format 12; E3 gains only on
blocks of several runs. M17 and M23 are the query-gap design's, decided by its E5 and E6
(`query-gap.md` revision 2, "What format 13 must record"), whose tables go to this format's candidate report. Every candidate is chosen
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
   tails; E5 and F12's P1 from the pairs, with frontier-length histograms per level (the
   query-gap design's E1 asks for them); F13 from each term's postings and exact lengths.
   Interacting items (the codec minimum with M23; E5 and P1) are projected jointly. A
   candidate that no combination can carry under the constraint is dropped before any code, and
   so is one whose ceiling the query-gap design's E3 to E6 project below 3% on every row it
   targets (`query-gap.md`, D5).
2. **The constraint:** postings ≤ the smaller of keyed `-bp`'s `.doc` (833.7 MB at 5M) and PISA's
   counterpart (F1 item 3; F13-R29), and the whole index ≤ keyed `-bp`'s (2,542.5 MB at 5M) with
   every other section as built, each at the confirmation size. The whole index has 3.2 MB of headroom at format 12's figures (2,539.3 MB,
   `phase-3-ratchets.md:203`), so the two rows are read together: a byte one section grows is
   paid for by another.
3. **The objective:** ns per touched block on `COUNT` intersections and unions, and the pruned
   top-k rows (`TOP_10`, `TOP_100`, `TOP_1000` on union and intersection_union), from branch
   A/Bs of each combination against main's format 12 under a hold the lead grants (SYS-6, C3).
4. **Order:** F10's layout first (one of A1 to A4 at one granularity: the base the others hook
   into), then E1, then E2, E3, and M23, then E4, then the pairs (E5 and M17), then
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

Format 12's layout (`stored_builder`, `writer.hh:720-764`) with one reservation (R19; F13-R23):
in a segment that sets bit 13 (`stored_spans`, F4), bit 31 of a frame index entry's uncompressed
size means "the frame's last record continues in the next frame". Elsewhere it is corruption, as
now: it already fails the frame bound (`reader.hh:583`, `max_stored_frame` well below 2^31). No
format-13 writer sets bit 13 before its release, so chunked records arrive by F23's rule, not by
a format version (the owner's Q5, `PLAN.md:51-52`).

P14's dictionary compression arrives by F23 (kind 19, bit 9): a segment that sets bit 9
decompresses its frames with the dictionary in `stored_dictionary`; any other decodes its frames
as frozen here.

## F20. Manifest version 2

The manifest (`files.hh:30, 119, 171-215`) changes once, with format 13:

- **The file family's version** (`files::version`, `files.hh:30`, shared by the manifest, the
  live-docs files, and the metadata files) is 2 at format 13's checkpoint. Between checkpoints it is a
  pre-freeze number (`files::pre_freeze`, whose value the code holds), bumped with each layout
  change of any of the three files, so another pre-freeze build's files are `unsupported_index`,
  never misparsed (C17 item 2), as segments' `format::pre_freeze` is (F2). A reader checks the file's CRC first and
  its version second (`files.hh:119`), so a flipped version is `corrupt_index`; a well-formed file
  of another version is now `unsupported_index`, not corruption. The frame (the magic, the LEB128
  version after it, the trailing CRC-32C) is frozen in every later version, as the segment's
  prefix is.
- **The slot table's generation file**, `slots-<gen>.kv`: its generation, size, and CRC (D3, N1).
- **The configured bits** (F13-R28): the structures the index is configured with, set only by
  F23 rule 4's replicated operation (or at creation), so every replica holds the same bits at
  the same log position. They decide what a flush or merge writes (F4) and what the digest
  records (F21).
  Both they and the mask below are 64 bits, F4's whole range, each a LEB128 (F13-R40), so a
  bit past 31 is classified, never read as damage.
- **The structure mask** (replacing L16's plugin identifiers): the configured bits together
  with every bit any of its segments sets. It serves only the readability checks, at open and
  at `install`; which segments a replica holds differs between replicas, so nothing replicas
  compare reads it (F13-R28). Open and `install` refuse a mask with a
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
- **Per segment entry**, its identity (F6, F13-R41), 16 raw bytes after the local number: open
  refuses an entry whose identity is not the file header's as `corrupt_index` before reading the
  segment, as it does a size or footer CRC that differs (I13), and a manifest naming one
  identity twice is damage. `install` copies it with the bytes. D8's "verified whole by id,
  size, and CRC" then binds the prefix too, which no CRC above it covers. The files family's
  pre-freeze version bumps to `0x80000002` (C17).
- **Per segment entry**, a values generation, 0 for none (`workload-profiles.md` item 4); no
  code writes nonzero, and a reader rejects nonzero until field updates exist.
- **Per segment entry, last, an attribute list** (F13-R43): [count LEB128][per attribute: tag
  LEB128, length LEB128, bytes], tags strictly increasing, one byte when empty. It holds
  per-segment state a binary reads without opening the segment, so a field added later is a
  tag, not files version 3, which every format-13 binary would refuse. A tag's low bit says what
  a binary without it does: 1 (required) refuses the index as `unsupported_index`, as a mask bit
  does (F13-R42); 0 (a cache, such as bounds recomputable from the segment) is dropped at decode,
  so that binary's next commit leaves it out rather than keep a value it cannot maintain (PNG's
  critical and ancillary chunks). F20 keeps the tag table; no tag is defined yet. The first,
  defined with the retention design, is the [min, max] of the index's retention slot, written
  at flush, merge, and install, so time-windowed retention decides per segment across closed
  indexes; HLC bounds and a memory tier's "not yet on disk" mark take tags later, not versions.
  An absent cache tag means unknown, never empty (F13-R50), since a binary without it drops it
  and its commit writes the entry without it: a reader recomputes the value from the segment or
  acts as if it were unbounded (retention keeps the segment), and a writer that knows the tag
  restores it at its next commit. A tag whose absence would be unsafe is required (odd), as the
  memory tier's mark must be. Every later release follows this rule.
  The files family's pre-freeze version bumps to `0x80000003` (C17).
- **Last, an index-level attribute list** (F13-R51), of the per-segment list's form and tag rule,
  after the segments, one byte when empty. It holds this replica's index state, which is neither
  replicated nor digested, so a later field of that kind is a tag, not files version 3; `install`
  keeps the target's own list, and a checkpoint with a required tag this binary lacks is
  `unsupported_index`. No tag is defined yet; durability-modes Part B's candidates are the disk
  tier's position and a shared-memory restart's valid mark (DM-design:829-834). Which state goes
  where: per segment, a segment tag; replicated, a metadata key under the reserved prefix (below);
  per replica, an index tag. The files family's pre-freeze version bumps to `0x80000004` (C17).
- **The owner lock is the index directory's** (small-indexes D2: SI-R17, SI-R18, SI-R27; its
  review's C4). Every format-13 index locks its directory from creation: `create()` and `open()`
  take an exclusive, non-blocking `flock` on the directory handle (completion's
  `acquire_directory_owner()`), which writes nothing. `owner.lock` retires: no format-13 index
  creates or reads it, and the manifest gains no field. Busy is `EWOULDBLOCK`; a filesystem not
  known local, or a lock it does not support, follows the index's policy (below); any other
  lock error refuses as it is, nothing written and no fallback to another lock. Binaries from before D2 on one root
  are out of scope (development builds; strata has shipped no release). The files family's
  pre-freeze version bumps to `0x80000005` (C17), and **the bump is load-bearing**: the
  directory lock and `owner.lock` are different locks, so without it a D2 binary would open a
  pre-D2 index under the directory lock while a pre-D2 binary held `owner.lock` on it, two
  writers; with it the D2 binary refuses that index as `unsupported_index` before it writes
  (small-indexes T5b).
  **The owner's ruling (2026-10-19) replaces SI-R18's refusal by type.** Completion locks
  the directory on any filesystem and exposes its classification
  (`PosixIO::directory_filesystem()`: Linux by an `f_type` allow-list, FUSE non-local; macOS
  and the BSDs by `MNT_LOCAL`). `index_options::nonlocal` decides the rest:
  - `warn` (the default): `create()` and `open()` proceed on a filesystem not known local, the
    lock taken (it then excludes local writers only), and the index reports it through
    `filesystem_warning()` (the filesystem's name, `local`, `locked`), state the host reads as
    it reads `retention_blocked()` and `needs_heal()`. A lock the filesystem does not support
    (`ENOTSUP`, `EOPNOTSUPP`) proceeds unlocked, reported with `locked` false: two writers are
    not excluded.
  - `refuse` (opt-in): a filesystem not known local refuses with `errc::not_supported` naming
    its type, before the lock, and an unsupported lock refuses, nothing written in either case.
    FUSE's type is never refused (the lead's call, since sshfs is FUSE); it is reported as
    under `warn`. An unsupported lock refuses on every filesystem, FUSE included: running
    unlocked is what `refuse` exists to stop (the lead's call).
  Busy still refuses (a detected second writer), and so does any other lock error, as it is.
  The classification is the IO's own, `IO::directory_filesystem()` (completion's additive
  virtual, DL2): a wrapper forwards it, and an IO that does not classify reports itself
  `unclassified`, not local, so `refuse` refuses it and `warn` warns. A failed classification
  (`fstatfs` errors) under `warn` proceeds, locked, with a warning naming the failure as an
  unknown filesystem; under `refuse` it refuses with the failure's own error (DL4, the lead's
  ruling). **Residual (DL3, accepted by the owner):** a FUSE mount that macOS or the BSDs flag
  `MNT_LOCAL` (macFUSE's `local` option, as sshfs mounts it) is classified local and gets no
  warning, where Linux always warns on FUSE; the kernel's flag is the evidence, and `refuse`
  never refuses FUSE's type anyway. `F_FULLFSYNC` on the directory handle (the macOS barrier)
  has been tried on APFS only; on a non-local mount, where `warn` admits the lock may not
  exclude, it has not been run.
  **The directory's own trust (the lead's ruling, 2026-10-19: warn, do not babysit).** Completion's
  `PosixIO` refuses a root that is a symlink, is owned by another user, or is writable by group
  or others; a host admits them with `PosixIOOptions` (`follow_root_symlink`,
  `allow_shared_directory`, both false by default, so Detent's behaviour is unchanged) and the
  IO reports what it found as `IO::directory_trust()` (`symlinked_root`, `foreign_owner`,
  `shared_writable`; the base default is unknown, like `directory_filesystem()`). Strata's tools
  pass both options. A symlinked root (a data volume) proceeds silently: the descriptor is held,
  so nothing re-resolves it, and entries inside the directory stay `O_NOFOLLOW`.
  `index_options::directory_trust` decides the rest, before the lock and so before anything is
  written:
  - `warn` (the default): a foreign owner or a group/other-writable directory proceeds and the
    index reports it through `directory_trust_warning()` (`foreign_owner`, `shared_writable`),
    read as `filesystem_warning()` is. It is independent of the filesystem warning.
  - `refuse` (opt-in): the same directories refuse with `errc::permission_denied`, nothing
    written, as the PosixIO defaults did.
  An IO that does not report its trust (a wrapper that does not forward `directory_trust()`) is
  unknown, so it is not trusted: `refuse` refuses it and `warn` reports it with `known` false.
  A wrapper forwards it, as it forwards `directory_filesystem()`.
- **Index options** (F23 rule 4's configurations, the order preset, retention's slot) live in the
  metadata file under the reserved key prefix `strata.` (`files::reserved_meta_prefix`), which is
  replicated and digested already (F21's `M` records), so a new option needs no manifest field;
  the configured bits and the key prefix stay manifest fields, since open reads them before the
  metadata file. Stage 1 refuses a host's `set_meta` or `delete_meta` under the prefix as
  `invalid`, before the undo step, already before the freeze (F13-R49), so no host key takes an
  option's name; the engine writes each option through its own replicated operation. Decoding
  stays permissive: a binary carries a `strata.` key it does not know as an opaque entry, since an
  option that changes what a reader must do arrives with a mask bit (F23 rule 4).
- **The key prefix** stays in the manifest's index fields (`files.hh:177`), the index's, not a
  plugin's option (F8; F13-R25 1).
- **The order preset**, if M17 is kept (F13-R31; QG-R1): an index option, so a metadata key
  under the reserved prefix (above, F13-R48), set by a replicated operation, so replicas order
  frontiers alike; F7's record copies it into each segment written under it, and readers take it
  from there, never from the index.
- **Readability first** (F13-R42). `decode_manifest_for` reads the version (`unsupported_index` when
  another), then the mask and the segment versions, which it classifies against what the binary
  reads (`files::readable_by`; `unsupported_index`) before it checks any other field: the slots
  reference, the values generations, and every field after them. So a later release's field under
  its mask bit (F23 rule 4) is unsupported, never damage, and a replica never takes Track C's
  repair (D10) for an index it merely cannot read. The live-docs and metadata files are read
  only after the manifest, so the mask classifies them too. T1's index cases include a values
  generation and a slots reference under a lacked mask bit (unsupported, at open and at
  `install`) and a values generation alone (damage).
- **Checks** (L16's, step 7c's manifest commit): an index whose mask holds a bit the binary
  cannot read, opened and installed; a highest version above the binary's; a segment outside the
  mask or the range (corruption); a version-1 manifest (`unsupported_index`).

## F21. The digest at format 13

L8, unchanged: the `D` record carries only the core's fields (docid, version, key as length u32
and bytes); `inverted` emits a length record for every document of an index configured with
`inverted`, length 0 included, and its `P` records; `stored` a record of its bytes, none for an
empty one; `values` its `V` records (D17); the core adds the slot-table record. Structures added
by F23 add records only if they hold content no other structure digests (none of the reserved
ones does but `version_clocks`). Bit 16's HLC per version is content: in an index configured with
it, the core's `D` record gains the document's HLC as a u64 after its version, and only there, so
every other index's digest is unchanged (durability D12 item 4).

The length record is keyed on the index's configured bits (F20), not on the binary's layout
(F13-R25 4; layers R14) and not on F20's mask (F13-R28): the configured bits come from the log,
so replicas with equal content digest equally even while one still holds a segment that sets a
bit the configuration has dropped and another has merged it away. Thus a `search_layout` index
configured without `inverted` emits none, so it digests equal to a `document_layout` index
holding the same keys and records (L12), and two binaries digest one index equally.

**The digest changes value once** (F13-R5): in step 7c's digest commit, which carries the split,
the key in `D`, `inverted`'s length record for every document of an index configured with it,
F9's version 0 for keyless documents and its writer check, and drops the interim key record that
the keys commit keeps (the `P` record format 12's key term produced, so `digest.txt` holds
through the earlier commits). That commit re-pins layers' `digest.txt` and the fixtures that
write a keyless document with a version, and runs T2's schedules. A merge's output bytes are a
function of its inputs, its order, and the writer's presets alone, so replicas on one binary
that merge alike write alike (T8); across binaries only the digest is compared (F22).

## F22. Fixtures, fuzz bases, libraries, and tools

- **Fixtures**, pinned by `fixtures_test` (`test/fixtures_test.cc`) and re-pinned with every
  format change on the branch: one per profile (`search`: every defined structure; `document`:
  `keys` and `stored`; `log`: `inverted` without `keys`; keyless and empty segments), one per slot
  type (`i64`, `u64`, `f64`, `date`, `bytes`, `binary`), one permuted (reordered) segment, one
  with 64-bit skip offsets, one with an M17-ordered segment if M17 is kept, and one per reserved
  bit set, refused as unsupported. With them, the
  format-12 refusal fixtures of F2.
- **Fuzz bases** (T2): `fuzz_segment` gains a section selector, an input byte naming the kind
  its mutations stay inside, so each decoder gets an hour of its own; a base per profile and per
  kind of F3; a mutation that drops, duplicates, reorders, or renumbers table entries and flips
  structure bits; and `refresh_checksums` reseals every checksum the reader checks (sections,
  regions, the table, the footer, and the header prefix). Every decoder F3 adds (the header
  prefix, the kept K codec and `key_index`, the advance and impact entries, each kept block codec
  (with M23 kept, a block whose gaps wrap by 2^32 and one whose header declares 26 bits),
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
  re-pins the fixtures without a format change, since every format-13 reader decodes both. It
  needs no coordinated restart (F13-R24): nothing compares segment bytes across binaries.
  Replicas compare the logical digest (L8; "physical segment files differ between replicas",
  `REPLICATION.md:64-65, 84-86`), and a binary checks only CRCs over the bytes it reads, whoever
  wrote them: an installed checkpoint's files are checked against the source's own CRCs
  (`install`, `index.hh:635-644` at `d508158`). So replicas on two zstd releases coexist
  through SYS-3's one-node-at-a-time upgrade, and T8's byte identity holds per binary. Detent links `varint` and
  `compressors` (its `LIBRARY_BOUNDARIES.md:48, 130`, as the review read it), whose forms are
  standard; the others are strata's alone. `fixtures_test` runs on every push, so a bump that
  changes a byte fails CI.
- **Tools:**
  - `strata_engine sections` learns F3's names (`format::name_of`).
  - `poststats` (F11's breakdown, `strata_engine.cc:245-348` at `e779835`) projects every
    candidate's bytes ("Choosing F10 to F13 together", step 1) and prints frontier-length
    histograms per level (block, group, term), which the query-gap design's E1 and QG-R4 need. Its
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
   verification class, file rank (F3's placement rule), and alignment: F5 aligns every kind to 8
   bytes today, and a kind that needs more (vectors) states its own, which the canonical layout
   then applies to it (F13-R47 item 2). Changing any of them afterwards is
   format 14 (S2). That release's segments carry version 13.
3. An added structure never changes how a frozen section decodes in a segment that does not set
   its bit. It may change how its owner's sections decode in segments that set it (P14's
   dictionary-compressed frames, P18's punched regions), and its first release states exactly
   how. For the core's `compact` (bit 15) the owner's sections include the table of contents:
   its entry form may change in segments that set the bit, while the prefix (F2) and the footer
   (F6) never do, so a reader reads the bit before the table. Likewise a later core structure may
   change how every section is stored in segments that set it, as encryption at rest would
   (`foundations-vision.md:63`), since `compact` already replaces every section's CRCs.
4. A binary writes the structure only in an index configured with it. The configuration is a
   replicated operation (D3's `declare_slot` for points; an index option for the others), so
   every replica sets the bit from the same log position, and the manifest's mask gains the bit
   at that commit (F20). This holds for the index's files too (F13-R42): every field or encoding a
   later release writes in the manifest, a live-docs file, or a metadata file arrives with a mask
   bit, field updates' values generations and P18's marks included, whichever file holds them, so
   a binary without it refuses the index by the mask before it reads the field.
5. A binary without the structure's reader refuses such an index with `unsupported_index` and
   such a segment with `unsupported_segment`, never as corruption and never by skipping the
   section (F4).
6. Under mixed binaries (Track D8), the configuration that adds a structure is accepted only once
   every replica's binary reads it; until then it rejects deterministically. D8 enforces it.
7. A structure derived from content that other structures digest adds no digest record (F21).
8. Its release brings its fixture, an hour of fuzzing for its decoder (T2), its T1 cases (its bit
   set without its reader is unsupported), and its gate rows.
9. **Frozen fields** (F13-R47 item 3): a value of a frozen field that the freeze does not write
   arrives only under a structure bit, so a format-13 binary refuses it as unsupported, never as
   corrupt; in a segment without that bit the value stays corruption. F13-R27 states it for
   `termdict`'s flags; it holds equally for F15's `f64` accumulator width (0 at the freeze), F7's
   scheme codes above 1, F10's codec codes, and every reserved byte the freeze writes as 0.

| Item | Kind | Bit | Owner | Configured by | When its bit is set | Digest |
|---|---:|---:|---|---|---|---|
| Points (D5, F16) | 15 | 5 | `points` | `declare_slot` with points | Point queries read its trees | none: built from values (D17) |
| P8 phrase prefilters | 18 | 8 | `inverted` | an `inverted` option | Phrase scorers may consult its summaries before decoding positions | none: derived from positions |
| P14 stored dictionaries | 19 | 9 | `stored` | a `stored` option | `stored`'s frames decode with its dictionary | none: records hold the uncompressed bytes |
| P18 holes | 20 | 10 | `stored` | P18's reclamation | The `stored` regions it marks are not verified and hold no live record | none |
| P20 quantized impacts | 21 | 11 | `inverted` | freeze (`frozen-index.md`) | Scorers may use its quantized scores | none: derived |
| P20 key hash | 22 | 12 | `keys` | freeze | `find` may use the hash | none: derived |
| R19 stored spans (F19) | none | 13 | `stored` | a `stored` option, or the document-size design | Bit 31 of a frame's size means its last record continues | none: `stored`'s record holds the whole record |
| Copy merges and P17 (F13-R32) | none | 14 | `inverted` | an index option enabling copy merges | `postings` and `positions` hold runs: a per-segment table of run starts gives a block inside one run its base, max(previous last, its run's start − 1); short blocks anywhere, a third codec code in F10's flags; position tails mid-stream. Its release settles the four items under "Copy merges" (F13-R46) | none: derived from postings |
| Version clocks (durability D12) | none | 16 | `keys` | an index option, the `available` preset | `keys`' segments hold a hybrid logical clock per document version (F9's `versions` alongside, same width rules); a merge keeps each document's clock; last-writer-wins merges on healing compare it, ties by origin incarnation | the `D` record's HLC, only where configured (F21) |
| Small segments (`small-indexes.md`, direction 4) | none | 15 | core | an index option; `perf/small-indexes`' M0 to M2 measure it first | The table may imply offsets by the canonical rule, give lengths as LEB128, and replace the section and region CRCs with one whole-file CRC | none: a layout, not content |

- P8's and P20's `inverted` sections locate a term's data by its postings offset, unique among a
  segment's terms with postings bytes, so they need no field in `postings` or `terms`.
- P18's design decides whether its marks are segment bytes. If they live in the per-generation
  live-docs file instead (`workload-profiles.md` item 4's mechanism), kind 20 and bit 10 stay
  reserved and unused. If they are segment bytes, punching rewrites the segment's tail at a new
  size and footer CRC, which the manifest's segment entry records (`files.hh:51`), and P18's
  design reconciles the two.
- Bit 16 reserves the place the durability design's D12 item 3 asks for. A 64-bit HLC could also
  be the version itself, since F9's `versions` hold any u64 (F13-R25 item 7), but versions count
  per key and the HLC orders across replicas, so the clock is its own column. Unconfigured it
  costs no byte, as D12 states. Last-writer-wins also needs a delete's HLC until the conflict
  window closes, which live-docs bits do not hold. A segment never changes after it is written,
  and a delete of a document in an older segment lives in that segment's live-docs file, so the
  HLC's place is not a field of this sub-structure (F13-R52): it is tombstones as keyed
  documents, or a live-docs encoding carrying each delete's HLC under mask bit 16, which rule 4
  and F13-R42 already admit (refused by the mask before the file is read). Both places exist,
  so the freeze does not wait on the durability design's choice (DM-R16); its release fixes the
  kind and layout (rule 2).
- A frozen segment that becomes writable again keeps P20's structures until its next merge, which
  drops them when the configuration does (`frozen-index.md:50-55`); the mask then drops their
  bits (F20).

## Freeze readiness (F13-R47 item 1)

The owner's ruling (`docs/status/format13.md`, "Before the freeze"): each need of the stack
either has a reserved place, a structure bit and its kinds that a later format-13 release adds
(F23), or its field in the frozen bytes before the freeze, so everything is addable without
format 14. Room: 48 unlisted structure bits (F13-R40) and the manifest's attribute tags (F13-R43).

| Need | Place | Owner | Digest | Before the freeze |
|---|---|---|---|---|
| Memory-resident segments (`foundations-vision.md:46-48`) | None needed: offsets are file-relative, fixed-width reads are `memcpy`s, any view opens (`written_segment` serves heap bytes); verifying and holding a RAM-built segment is reader policy (P15); a tier mark is a manifest attribute | core | none | nothing |
| Time bounds per segment (retention by window) | A `values` column's [min, max] inside the segment (F15); outside it, a manifest attribute (F13-R43) | `values`; F20's tag table | none: derived | F13-R43, done |
| Per-document TTL | A `date` slot (F15) an index option names; a drop is a replicated delete | `values` | its `V` records | nothing |
| HLC per version (durability D12) | Bit 16, `version_clocks` (F4, F23) | `keys` | the `D` record's HLC, only where configured (F21) | the reservation, done |
| Physical identity for segment shipping (durability D8, D16) | The header's id: the producing term, then 8 bytes no producer repeats; recorded in the manifest, checked at open (F6, F20, I13) | core | none | F13-R41, done |
| Columnar doc values and per-segment sketches | Values: F15, built before the freeze. Sketches: an F23 structure under an unlisted bit, seeded hashes so T8 holds | `values`; a sketch plugin | values' `V`; sketches none (derived, rule 7) | nothing |
| Vectors | An F23 plugin under an unlisted bit, kinds above 22, its alignment fixed by its first release (rule 2) | a vector plugin | content: its own records (F21) | rule 2's alignment, done |
| Bit 14's copy merges | Reserved bit 14; the four items under "Copy merges" (F13-R46) | `inverted` | none: derived | the text, done |
| The query work's bounds and codec | F10 to F13 chosen with format 13 (F1 item 2); P8's and P20's bounds reserved (bits 8, 11); H22's codec verdict (F1 item 2, F13-R44) | `inverted` | none | F13-R44's verdict, owed |
| Index options | The metadata file under a reserved prefix (F20) | core | `M` records | nothing |
| Encryption at rest (`foundations-vision.md:63`) | A core structure under an unlisted bit that changes how every section is stored (rule 3) | core | none | nothing |

Two rules bind every later release: the bits and the manifest are classified before anything a
later release may write (F13-R35, F13-R42), and a frozen field's unwritten value comes only under
a bit (rule 9).

## Copy merges (research/indexing; F13-R32)

The indexing research note's copy-merge items (`docs/research/indexing.md` §5.3 on
`research/indexing` at `5f956bc`) were proposed here at revision 1's amendment and ruled by the
second re-check (`format-13-design.md`, "The proposed copy-merge items"). None enters format 13's
frozen grammar:

1. **A stored list base**, as a run table instead: a per-segment table of run starts, the base of
   a block whose ordinals lie in one run max(previous last, its run's start − 1). In a segment of
   one run that is today's rule; it costs no byte per term; F10's per-block check and M23's 64-bit
   sum read that base. With items 2 and 3, under bit 14. The rule holds for copied blocks, which
   never span a run, and not for a block that spans a run start s, with ordinals s − 3 to s + 5,
   whose base by the run of its last would be s − 1, past its first ordinal: a term re-encoded
   inside a bit-14 segment whose blocks cross runs (`perf/encode-once`'s
   `docs/perf/encode-once.md`, "For format 13's bit 14", item 1; F13-R46).
2. **Short blocks anywhere**, under bit 14: the tail code is a third codec code in F10's flags only
   in segments that set the bit (F23 rule 3); F10's 16-bit offsets hold (a short block of 127
   postings takes at most 1,270 bytes). The copy is a per-term choice by a deterministic rule that
   bounds short blocks.
3. **Positions**, under bit 14: a source's position tail sits mid-stream; level 0's first-position
   fields locate each run's positions.
4. **Concatenation order**: no format change. The copy merge's design states its limits: under
   `index_sort` a copy only where the concatenation keeps the order, and a permuted output's cost
   to the docid-ordered tie skip.
5. **Re-encoding only sources with deletes**: no format change. P17 is this copy path with
   deletes, one mechanism, so it takes bit 14 too (F1).

**What bit 14's release settles** (F13-R46, from encode-once's prototype at `72d1928`). None needs
a field in the frozen grammar: each lies inside `postings`, `positions`, or a section bit 14's
release adds (F23 rule 3), and a format-13 binary refuses a bit-14 segment before it reads any of
them (F13-R35).

1. **Re-encoded terms**: cut their blocks at run starts, or carry a per-term or per-block marker
   that names the base, since item 1's rule fails for a block across runs.
2. **A short block's posting count**: F10's per-block flags carry only the codec; the prototype
   prefixes a LEB128 count inside the block.
3. **The position grid**: a position tail mid-stream moves the 128-position grid, so a block's
   first position sits at (its first position − its run's first position) mod 128 in the block
   level 0 points at; level 0 gives that offset, but a reader that jumps through level 1 does not
   know its run's first position, so the release adds a field or a rule.
4. **The run table's section**: F23's row gives bit 14 no kind; the table must live in one, whose
   kind, rank, and verification class the release fixes (rule 2).

C12 records encode-once's flush result, which settles where bit 14 pays: at a flush of 100,000
documents (18 runs) 41.5% of postings land in short blocks and the segment grows 10.3% (postings
+24.0%), for 4% to 7% of one-thread indexing CPU, so bit 14 is for copy merges, whose sources are
whole segments and whose short blocks number at most one per term per source.

Items 4 and 5 join L6 as the copy merge's rules when its design is written. Bit 14's release lands
only with C12's evidence: merge time against a re-encoding merge brought toward its bound, at
sources of 1,000 to 100,000 documents; bytes at each; T4's guard over a copy-merged index; and P17
as the same path. Format 13's joint selection chooses its encodings on a re-encoded index, where no
run exists, so none of this enters it.

## Gate rows

Branch rows (T1, T2, T7, T8, T9) are functional and run in CI on the branch. Measured rows (T3 to
T6, T10, T11) run on main commits on the gate VM (PLAN, "What waits"). The freeze's candidate
selections (F1) run before that, on branch builds under a hold the lead grants, recorded in
`docs/perf/` as evidence, never as gate rows (SYS-6).

| Row | Target | Procedure |
|---|---|---|
| T1 Format rules | Every rule of F2 to F6, F20, and F23 has a test that builds a valid segment or index, breaks the rule, and expects the stated outcome. The prefix: magic, each byte of 8 to 15 flipped, a real format-12 file, version 14 with a good CRC. The footer and table: checksums, bounds, count, unknown and retired kinds, a kind twice, rank order, a gap, misalignment, overlap, the table's place. Structure bits: each defined bit flipped both ways, a sub-structure without its owner, `index_sort` without `values`, each reserved bit set, bits 13 to 15 included, and bits 16 to 31 (all unsupported, by the mask in an index); bit 14 without `inverted` (damage). The free-and-absent cases of F4. At the index: a segment outside the manifest's range or mask, a mask bit the binary cannot read (open and install), a version-1 manifest, and, before F20, a manifest-named unsupported segment (corruption, retried and healed). The format-12 refusal fixtures | `test/format13_test.cc`, `fixtures_test`, `index_test`, `media_test`; ctest on all eight CI jobs |
| T2 Decoders | Every decoder F3 adds survives `fuzz_segment` for one CPU-hour each at the freeze commit, under ASan and UBSan, with no finding open and a hang timeout; corpora replay in every CI build; every corruption check of T1 is also reached by a resealed fuzz input | `fuzz/fuzz_segment.cc` with `STRATA_FUZZ=ON`, `-fsanitize=fuzzer,address,undefined`, `-max_total_time=3600` per section selector |
| T3 Disk | After a reordered full merge of the keyed index at PLAN's confirmation size (F13-R29): postings ≤ 1.0× the smaller of keyed `-bp`'s `.doc` and PISA's counterpart (F1 item 3); the dictionary row (F8's accounting) ≤ 1.0× keyed `-bp`'s `.tim`, `.tip`, `.tmd`; the whole index ≤ 1.0× keyed `-bp`'s; every other section no larger than format 12's figure (`phase-3-ratchets.md:201-208`); new sections reported | `strata_engine merge` with reordering and `strata_engine sections`, against `results/keyed-bp-n2.sizes` (`bench/night_p1.sh`, step 3) |
| T4 Queries | The guard: per class and command, format 13 over format 12 ≤ 1.03 by the interval's upper bound, both merged and reordered; blocks decoded and ns per touched block reported for `COUNT` intersections and unions. The carried rows: F1's criterion 4 before the freeze, PLAN's ≤ 1.0× at landing | `bench/ab.sh` in-process, each binary serving its own build, on PLAN's sample and its one confirmation (F13-R29); `bench/phase2/gate.py` for the carried rows, against each row's fastest engine from the one calibration, PISA's exact rows included |
| T5 Keys | `lookup_key` at 1 and 50 segments, hits and misses, ≤ format 12's row by the interval's upper bound, that row recorded on main with its raw file and commit (C5's check); Lucene's id lookup reported beside it; indexing (F13-R30, PLAN's many-index workload): per index on one thread as throughput and CPU per document, the single-index row on the shared pool with no thread setting, and `perf/small-indexes`' M2 aggregate row, each ≤ 1.03 of format 12's time, with the ≥ rows against the baselines as PLAN states them | `strata_engine keybench` (F22), recorded on main for format 12 before format 13 replaces it; `bench/phase2/lucene_seek`; `bench/index_trials.sh` |
| T6 Seeds | `verify` clean at k = 10, 100, 1,000, both schemes, on one segment and 16, with and without deletes; union `TOP_k` with seeds over without ≤ 1.0 by the upper bound; seed bytes in the postings row and in the joint selection | `strata_engine verify`; `bench/ab.sh` with seeds toggled by a preset |
| T7 Open | A fresh open touches the header page and one run of pages ending at the footer, and no byte of `lengths`, for every profile: search, without norms (F13-R36), and document | `test/format13_test.cc` (F13-R25 5; layers R15): the segment copied into an anonymous mapping, every whole page strictly between the header's page and the page holding the tail's first byte made inaccessible (`mprotect`, `PROT_NONE`), then opened, so any read outside the header page and the tail faults; `verified_regions() == 0` after the open. The fixture is large enough that at least one page is protected at the platform's page size (16 KiB on macOS arm64), which the test asserts, and it pins the order. Pinned sections are checksummed from the file's bytes, never counted as verified regions, so a counting `IO` alone cannot see them |
| T8 Determinism | The fixtures' hashes are equal on all eight CI jobs; a reordering merge's output hash is pinned at `reorder_threads` 1 and 8. Byte identity is per binary, its pins included; across binaries the digest is equal (F22) | CI |
| T9 Lifecycle | Phase 3's T1 crash points, T2's digest schedules, and T5's disk-full rows pass over format 13, keyed, keyless, and with the `document` profile | `lifecycle_test`, `index_test`, `media_test` |
| T10 Memory | Serving memory on the merged index at PLAN's confirmation size ≤ format 12's, both measured in one session; resident and pinned bytes per open segment (the tail, the slot directory) at 1, 10, 100, and 1,000 documents, and per open index and open time at M0's 10,000 indexes, against format 12 (F13-R30; PLAN's tens of thousands open per instance) | `bench/serve_mem.py`; a test that sums pinned bytes |
| T11 Cold start | A report row: open time per GB and first query after a dropped page cache, format 13 against format 12, in the bare-metal session once a host is named (`BENCHMARKS.md:29-32, 114`; SYS-4). T7 is S1's functional check until then | BENCHMARKS, "open time per GB" |

## Questions for the owner

All answered: Q1 to Q4 through the review (`format-13-design.md`, "Owner questions"), Q5 and Q6
by the owner (`PLAN.md:51-54`), Q7 by the lead (the ledger's rulings after the second re-check).

- **Q1**, the dictionary row's reference: keyed Lucene `-bp` (`PLAN.md:38-39`). F8, T3.
- **Q2**, Track P's format items: P8, P14, P18, and P20 reserved, with the points rule
  (`PLAN.md:39-41`). F1, F23.
- **Q3**, freezing with points unwritten: yes, by a stated rule (`PLAN.md:34-36`). F16, F23.
- **Q4**, candidate runs on branch builds: evidence for a choice, never a gate row
  (`PLAN.md:19-20`). F1, gate rows.
- **Q5**, R19's continuation flag: bit 13 reserved for `stored_spans` under F23's rule
  (`PLAN.md:51-52`; F13-R23). F4, F19, F23.
- **Q7**, PISA and the carried rows (F13-R29): PISA's references apply to the rows that close in
  format 13: postings bytes now, and PISA's exact rows for query classes (the lead, after the
  second re-check). F1, the joint selection, T3, T4.
- **Q6**, bytes per point dimension: up to 16, Lucene's maximum, with a mechanism for 128-bit
  values such as IPv6, not a 64-bit cap (`PLAN.md:52-54`; F13-R22). F15, F16.

## The rebase onto layers

Revision 1 planned the base as `phase4/layers-refactor` at `707066b`: steps 0 to 6, 7a, and 7b,
CI green on all eight jobs at `1d60098` (run 39070316303, per its ledger; `707066b` adds only
its ledger), or main once layers lands there, as the lead names. Layers has since folded R12 to
R19 (`ea04731`, with code in `reader.hh`, `index.hh`, and `merge.hh`, among them R16's presence
before the readers) and merged main (`d508158`, CI green on all eight jobs, run 39109639725),
the base the lead named; the branch was rebased there (F13-R34). Citations below at `707066b`
predate it, and step 7c was written against `d508158`'s code. `c8c03f6`, the base SYS-1 named,
became `8645134` in layers' own rebase onto main. Format-13 code returns to `reader.hh`,
`writer.hh`, `index.hh`, and `merge.hh` only on the rebased branch, as step 7c, after this
revision's re-check (the review's required changes).

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
| `7c2c9bd` | F8: keys leave the dictionary | The segment's fallback (`reader.hh:356, 364-372` at `e779835`) is dropped (F13-R2); `merge.hh`'s parameter removal is replaced by layers' 7a, with `merge_options::key_prefix` kept as the backstop's in 7c-3 (F13-R38); `lookup_key`, the merge's key collection, and the stage-1 rule are redone in 7c-3; its key record in the digest survives only as 7c-3's interim record |
| `e2ac3fe` | F9 | The presence parts are replaced by 7b's mask set from the footer and readers that open empty (7c-4); keyless segments and the version-0 rule (`index.hh:1650` at `e779835`), one commit there, stay one commit in 7c-5, with F13-R9 (F13-R18); "keyed when any input is" becomes F4's configuration rule |
| `e779835` | F22's tools | Changes: `poststats` and `keysim` are re-applied on the plugin readers after 7c-3, with F13-R6's and F13-R15's fixes |
| `4266374` | The preliminary review | Survives as is (documentation) |
| `41730a3` | Design revision 1 and the ledger | Survives as is (documentation) |
| `bae7fc1`, `ac255f8` | The re-check, and K13 and K14 | Survive as is (documentation) |
| `1db48ab` on | F13-R18 to F13-R26 in the design, one commit each, and the ledger | Survive as is (documentation) |
| `b2f7600` | Main's `std::jthread` fix (`adb329e`), cherry-picked | Dropped: `d508158` holds `adb329e` (F13-R34) |

**Step 7c, one line each, in order** (L14 step 7c; the review's order):

1. **7c-1, F2 to F6:** `read_version` and the prefix; `unsupported_segment`; F3's and F4's tables
   in `format.hh` replacing `section_rule`; `core_segment`'s open by the table (presence both
   ways, the readable bits and unreadable ones, canonical layout); the mask computed before the
   readers and passed in `open_context` (F13-R25 6); the sink placing by rank and holding pinned
   sections; the footer's structures from `built`; `load_segment` and `written_segment` reading
   the prefix first and turning a manifest-named unsupported segment into corruption; the
   format-12 fixtures moved to `test/fixtures/format12/`. `pk_filter` stays a `keys` section here,
   as at `d1da0c9`; `digest.txt` is unchanged.
2. **7c-2, F7:** `stats` holds the count; `inverted`'s statistics lead `term_index`; `lengths`
   holds lengths only; `built::legacy_stats` goes.
3. **7c-3, F8:** the `keys` plugin on `keys.hh` (K1 until F8's choice), with empty deps and F8's
   interface (`scan` from a lower bound, `PLAN.md:47-48`, which automata's expansion runs over
   `keys`); the term table's and `snapshot_df`'s key leaf, and the buffer path's (F13-R25 3);
   the key prefix the index's, passed by the core to `validate_context`, the flush's and merge's
   contexts, `check()`, and the planner (F13-R25 1); stage 1, the `inverted` builder, and
   `check()` refusing key-prefixed terms; stage 1 refusing an add whose key lacks the prefix
   (F13-R25 2), each only under a nonempty prefix, with term queries and wildcards over a
   keyless index among its tests (F13-R33); `document::key` and `key_of` from it; the merge
   collecting keys through `scan` from the empty key; `merge_options::key_prefix` kept, the
   one channel from the index into a merge, now carrying only the backstop's prefix (F13-R25 1,
   F13-R38); `pk_filter`
   retired. An interim `P` record per live key, for buffered documents' keys as well as
   segments', since a buffered document's terms no longer hold its key, keeps `digest.txt`
   unchanged (F13-R26). The key moves from `terms` to `document::key` in every keyed test and
   tool at the base: `background_merge_test`, `churn_test`, `docids_test`, `fixtures_test`,
   `index_test`, `lifecycle_test`, `media_test`, `merge_oracle.hh` and `merge_oracle_test`,
   `merge_test`, `reclaim_test`, `reorder_test`, `segment_test`, `fuzz/fuzz_segment.cc`, and
   `tools/strata_engine.cc`; `files_test` and `fuzz/fuzz_files.cc` keep the manifest's prefix
   field, which stays (F13-R25 1).
4. **7c-4, F9's presence:** readers that open empty on the bit `open_context` passes (F13-R25
   6); 7b's mask from the footer; the free-and-absent cases of `stored`, `norms`, and
   `inverted`; `document_layout` segments and tests (L12). Every index stays configured with
   `keys` here, so every segment sets bit 2 and writes `versions`, a keyless document's version
   included, as format 12 does, and a keyless document's version reads the same before and after
   a flush (F13-R18).
5. **7c-5, F9's keyless segments and F21:** the `keys` bit from the configuration, keyless
   segments, `keys`' free-and-absent case, and the `log` profile, with the version rule they
   need (version 0 for keyless documents, the writer's version check, F13-R9, and its minimum
   rule, F13-R25 7); the digest split, once: `D` with the key, `inverted`'s length record for
   every document of an index configured with it (F13-R25 4), the interim record dropped; `digest.txt` and the keyless fixtures re-pinned; T2's schedules, keyless included.
6. **7c-6, F20:** manifest version 2 (the slots file, the mask, the version range, the values
   generation; the key prefix stays in the index fields); the open and `install` checks; L16's
   tests.
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
   open rules change only in 7c-1, since today's writer would fail them. M17 and M23 are the
   query-gap workstream's experiments (its E5 and E6), not this branch's.
2. **Tools:** `keybench` against main's API, for the lead to land on main and record format 12's
   T5 row; `poststats`' projections, its frontier histograms, and `keysim`'s fair comparison on
   this branch. `poststats` reads only the postings and lengths format 12 already has, so it can
   also land on main, which lets the lead run the projection on the existing reordered gate index
   without a new build, and gives the query-gap design's E1 its main-side mode.
3. **`termdict`, in its own repository:** a builder option to cut blocks only at run boundaries
   (K2), the restart interval and block bounds as parameters (K4), and the block-body codec (K3,
   F13-R19), both recorded in the index's options form only when set (F13-R20) and validated as
   F8 states (F13-R27), with today's values the defaults and its own CI and fuzz
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
