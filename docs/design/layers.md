# Layers design: the lifecycle core and section plugins

Revision 4: revision 3, approved with conditions by its re-check
(`docs/reviews/layers-design.md`, "Re-check (revision 3)"), with that re-check's findings
folded in as revision 4's table lists; revision 4 awaits the architect's check. Revision 1
(R1–R11) was approved by revision check 1. Revision 2 (S1, S2, and revision check 1's
finding 1) had no check of its own; revision 3's re-check covered it, together with
format 13's preliminary review's findings for layers (F13-R2 to F13-R5, F13-R9, F13-R11)
and the system review's SYS-6 and SYS-16, which revision 3 folded in. Phase 4's D1 (`phase-4.md`, "Two layers") makes
strata two layers; the Phase 4 review's revision check 1 (N4) and revision check 2 (M2)
set the conditions. This document specifies the split concretely enough to implement:
who owns each piece of today's code, the plugin interface, how documents carry plugin
data, how an index type composes its plugins at compile time, the order of the
refactor, its checks, and the alternatives. Decisions are numbered L1–L17.

The refactor changes no bytes. Format 12 stays byte-identical through every step; the
format changes that the split makes possible (the `keys` section, the pinned tail,
statistics per plugin, the split document record, the plugin set in the manifest)
land with format 13 and are marked as such.

## Revision 1

| Item | Change | Where |
|---|---|---|
| R1 | `validate` and `key_of` take a `validate_context` (options, key prefix, slot table at the operation's position); `bytes` and `disk_estimate` stay static and depend only on the part | L4, L10 |
| R2 | Each reader receives one verification handle per owned section at construction; a verify is today's arithmetic, with no lookup by kind | L7 |
| R3 | No reader holds a sibling reader; format 12's `keys` reader receives the `inverted` reader at call time; `basic_segment` stays movable, checked by `static_assert` and an ASan test | L3, L14 |
| R4 | The measurement plan: multi-segment indexes before and after merging, allocations exactly equal, serving memory, step 5's timed forced merge with a byte-identity hash and T3, the ≤ 1.03 upper-bound rule with a ten-trial rerun, Linux and FreeBSD (arm64 before Phase 3R), mandatory A/Bs at steps 2, 3, 5, and 6 | L15 |
| R5 | The flush memory charge leaves this refactor: Phase 3's D17 lands it on today's code before the ratchet rows are recorded | L5, L14 |
| R6 | From format 13, `D` holds only core fields; each plugin emits its own records, and nothing for a document it has nothing for | L8 |
| R7 | The manifest records each plugin's identifier and section versions, checked at open and at `install`; `unsupported_segment` stays as a segment-level defense | L16 |
| R8 | The standard for the P17 and I7 copy paths: per-section decoded equality with the rebuild path, `check()`, `verify` and `parity`, determinism, the fixture re-pinned in the same commit with T3 | L6 |
| R9 | File order is decoupled from plugin order; format 13 puts the core's sections at fixed positions after the header; `docids_section` exists only in format 12's layout | L2 |
| R10 | The doc-values skip index is ruled in the review's R10 and lands on `phase4/docvalues` with D4's text; this design does not change it | L6 (the `values` row) |
| R11 | The fuzz-replay verdict defined; step 0's extra fixtures; distinct empty bases per layout entry; the `sizeof(document)` pin holds through format 12 only | L7, L10, L14 |

The five open questions of the first draft are answered by R6–R9 and R5.

## Revision 2

Revision check 1 approved revision 1 and ruled its two open questions; both are
format-13 text and touch none of steps 0–6.

| Item | Change | Where |
|---|---|---|
| S1 | Pinned sections sit at the end of the file: the tail is `docids`, `stats`, then each plugin's pinned sections in layout order, then the table of contents and footer; streamed sections lie between the header and the tail; the header alone opens the file. Fixed, not measured: no pinned-first candidate is built. Replaces R9's "after the header" | L2, L14 |
| S2 | Section versions follow the segment's format version; `P::section_version` is deleted; the manifest records the plugin identifiers and the lowest and highest segment format version, with no per-segment record | L4, L16 |
| Finding 1 | Under format 13's split digest, `inverted` emits its per-document length record for every document, length 0 included (the owner's choice) | L8 |

Revision 1's open questions are closed by S1 and S2; none remain.

## Revision 3

Format 13's preliminary review (`format-13-design.md` on `phase4/format13`, `4266374`)
rules the four conflicts the system review found between format 13 and this design
(`system-2026-10-17.md`, SYS-1). Its findings that change this design, and SYS-16's:

| Item | Change | Where |
|---|---|---|
| F13-R2 | Format 13's key routing is the planner's: a term with the key prefix is a key leaf read from the `keys` reader, never a fallback inside a segment; `keys_plugin::depends_on` is empty; `inverted`'s part holds no key-prefixed term; multi-term expansion over the key prefix reads `keys`' ordered prefix scan; `keys` holds the key's bytes exactly as operations name them | L3, L14 step 7c |
| F13-R3 | `P::structure` becomes `P::structures`, a mask (`inverted` owns `inverted` and `norms`), with the plugin's own bit as `P::structure`; `index_sort` is the core's; one per-kind table in `format.hh` (id, owner, requiring bit, verification class, file rank) replaces `section_rule` at step 7c; presence is "the bit that requires the section"; a set bit without a reader is `unsupported_segment`, `unsupported_index`, or corruption by the manifest; `holds<P>()` reads the segment | L4, L7, L11 |
| F13-R4 | Format 13's tail: the core's pinned sections (`docids`, `stats`, `index_sort`), then each plugin's pinned sections in layout order; `lengths` leaves the tail; `inverted`'s statistics go to a pinned section of its own (format 13's F7 as revised) | L2, L9 |
| F13-R5 | The digest changes once, in one step-7c commit that re-pins `digest.txt` and runs T2's schedules | L8, L14 step 7c |
| F13-R9 | The digest's one commit also re-pins the fixtures that write a keyless document with a version; the refusal of such a document is F13-R9's fix in 7c's keys builder | L8 |
| F13-R11 | `merge_segments` takes a named `merge_options`; no positional tail | L6, L14 step 7a |
| SYS-16 | `holds<P>()` is per segment, not a static property of the layout | L11, L14 step 7b |
| SYS-6 | Branch A/Bs under a hold the lead grants are evidence for keeping a step, never gate rows (PLAN, "What waits") | L15 |

## Revision 4

The re-check of revision 3 (`layers-design.md`, "Re-check (revision 3)") approved it with
conditions. Its findings, as folded in:

| Item | Change | Where |
|---|---|---|
| R12 | `contains<P>()`, a `static constexpr` membership test, beside the runtime `holds<P>()`; generic code guards `get<P>()` with `if constexpr` on the first and reads the file with the second | L11, L14 step 7b |
| R13 | Stage 1 enforces the key routing's disjointness (a key-prefixed term in `inverted`'s part, and a `d.key` without the prefix, are `invalid`), the builder's throw a backstop; the key prefix stays the index's, passed by the core to `validate_context`, `flush_context`, `check()`, and the planner; the buffer path's predicates gain the key leaf | L1, L3, L4, L5, L10, L11, L14 step 7c |
| R14 | The manifest's plugin set is the index's (the plugins whose structures it may hold, joined when the index first needs one, and under mixed binaries only once every replica has it), not the writing binary's; L8's length-record exception is stated on that set, so L12's digest equality holds | L8, L12, L16 |
| R15 | S1's open check faults on the pages between the header page and the tail (`mprotect` over an anonymous copy) and asserts no verified region after the open, since pinned reads never count as verified regions | L2, L14 step 7c |
| R16 | The bits a binary reads are the core's (with `index_sort` once its reader exists) and each layout entry's `P::structures`; the core claims `index_sort`'s kind; `basic_segment` computes the mask before the readers (done on this branch) and passes each reader its bits in `open_context`; 7c's checks add the unreadable bit's three cases and the clear-bit case | L7, L11, L14 step 7c |
| R17 | Equal verdicts allow one type change, from a leaked type to `corrupt_segment`; step 2's 29 changed types and its duplicate-kind rejection are recorded, the latter for the full review | L7 |
| R18 | The format-12 `section_plugin` concept and the layout's checks are built; `plugin_base` waits for its first user; the stub `document_layout` test is built; the digest's key invariant reads `keys`' scan and the index's merge runs over its own layout; format 12's `D` record and `scan`'s prefix precondition are stated as they are; a refused empty flush fails `fixtures_test` | L2, L3, L4, L8, L12 |
| R19 | Text: `key_reader`'s `prefix`; "step 7" is 7c; F13-R9's row; the byte-identity rule's exceptions (7a, 7c); `merge_options`' designated-only review rule; the mask computed by `basic_segment`; the stored merge decodes each frame once; an empty `order` with one permuted source; format 13's file order by rank; 7c's interim `P` record; the revision history | L2, L3, L6, L11, L14, header |

## Terms

- **Core:** the lifecycle layer. It owns docids, ordinals, live docs, the version map,
  key resolution, the write buffer, flush and merge orchestration, manifests, commits,
  budgets, `apply`, `checkpoint`, `install`, and the digest's composition.
- **Plugin:** a type that owns one or more section kinds: their encoding, building at
  flush, merging, verification at open, digest records, and a reader for queries.
- **Part:** the slice of a host document that one plugin consumes (terms, a stored
  record, slot values).
- **Layout:** the compile-time list of an index type's plugins (and, in format 12
  only, the core's `docids_section` marker).
- **Output order:** the ordinal order of the segment being written: docid order at
  flush, docid order or a permutation (P1) at a merge.

## Where the seams are today (L1)

Every piece of `include/strata/` goes to exactly one owner. "Shared" means format
primitives both layers use, with no lifecycle or search semantics.

### `format.hh`

| Lines | What | Owner |
|---|---|---|
| 46–51 | `ordinal_t`, `corrupt_segment` | Shared |
| 55–63, 75 | Magic, version, header and footer sizes, region shifts, section alignment | Shared |
| 64–74 | Stored block size, record and frame limits | `stored` plugin |
| 71–73 | `max_term_bytes` | `inverted` plugin |
| 76–80 | Block size, docids kinds | Shared (`block`); core (docids kinds) |
| 82–95 | The `section` enumeration | Shared: one numbering space; each plugin claims its kinds |
| 97–124 | `put`, `get`, `put_leb`, `get_leb` | Shared |
| 128–251 | Section entries, header, table of contents, footer | Shared |
| 253–267 | `encode_length`, `decode_length` | `inverted` plugin |
| 269–508 | Term info, impacts, competitive pairs | `inverted` plugin |

### `writer.hh`

| Lines | What | Owner |
|---|---|---|
| 68–72 | `posting_input` | `inverted` |
| 75–223 | `output`, `spill`, file and string outputs, `counting_output`, `section_stream` | Shared (moves to `section_io.hh`) |
| 227–236 | `term_postings` | `inverted` |
| 250–290 | `add`, `add_tokens`, `add_document` | Façade (L10): splits into the core's per-document fields and each plugin's builder |
| 295–346 | `flush`: sorts the term table, feeds `flush_from` | `inverted` builder |
| 357–363 | Header section | Core |
| 364–427 | Postings, positions spill, term dictionary | `inverted` builder |
| 429–434 | `stats`: document count, then total, minimum, and maximum length and most positions | Core writes the count; the rest is `inverted`'s (L9) |
| 435–453 | Section appends in file order | Core (`section_sink`), order per L2 |
| 456–465 | Table of contents and footer | Core |
| 479–501 | `begin_document` | Split: docid checks core; version and key `keys`; stored `stored`; length `inverted` |
| 505–776 | Posting buffers, the term hash table, postings and positions encoders | `inverted` |
| 781–803 | `encode_stored` | `stored` |
| 806–821 | `pack_u32` | Shared (a `bitpack` helper; `lengths` is its only user today) |
| 826–844 | `encode_docids` | Core |
| 847–865 | `encode_versions` | `keys` |
| 869–876 | `encode_pk_filter` | `keys` |

### `reader.hh`

| Lines | What | Owner |
|---|---|---|
| 64–72 | Footer, table of contents, header check | Core |
| 73–105 | Per-kind pinned checksum and section capture | Core loop over each plugin's section rules (L7) |
| 106–110 | `stats`, document count against the footer | Core; total length and most positions `inverted` |
| 111–115, 133 | Lengths and norms size checks | `inverted` |
| 116–132 | Lazily verified regions (`residency::verified_regions`) | One shared object per segment, built by the core from the plugins' rules (L7) |
| 134, 144–172, 345–373 | Docids: `validate_docids`, `docid`, `ordinal_of` | Core |
| 135, 175–184, 377–385 | Versions | `keys` |
| 136, 188–192, 388–402 | Key filter | `keys` |
| 139–140, 285–338 | Term dictionary, `term`, `scan_terms`, `scan_dictionary` | `inverted` |
| 195, 263–281 | `norm`, `total_length`, `average_length`, `length` | `inverted` |
| 199–204, 336–337 | Postings and positions verification and bytes | `inverted`, through its section handles (L7) |
| 209–249 | `stored` | `stored` |
| 254–261 | `verify_all` | Core loop over plugins |
| 418–422 | The lazily verified `lengths` state | `inverted` |
| 437–1186 | `postings_iterator`, `positions_reader` | `inverted` |

### `index.hh`

| Lines | What | Owner |
|---|---|---|
| 37–46 | `term_entry`, `document` | Composed document (L10) |
| 48–72 | `operation`, `transaction`, `verdict` | Core, generic over the document type |
| 155–182 | `index_options` | Core; `key_prefix` stays the index's in every format, recorded in the manifest and passed by the core to whatever needs it (L3, R13) |
| 184–688 | `index`: create, open, apply, refresh, merge, commit, reclaim, heal, checkpoint, install | Core |
| 513–583 | `digest` | Core composes; records from plugins (L8) |
| 587–592 | `check` | Core loop over plugins |
| 683–687, 903–926 | `find_key`, `locate_docid`, `lookup_key` | Core semantics; `lookup_key`'s segment probe (917–923) calls the `keys` reader (L3) |
| 711–716 | `doc_bytes` | Core sum over plugin parts (L10) |
| 891–899 | `estimate_bytes` | Core sum over plugin parts |
| 1026–1037 | `key_of` | `keys` plugin, with a `validate_context` (L3, L10) |
| 1040–1050 | `valid` | Core fold over plugin validators, with a `validate_context` |
| 1052–1167 | `evaluate`, undo | Core |
| 1172–1229 | `flush` | Core; `invert` (1185–1201) drives the builders (L5) |
| 1233–1327 | `commit_files`, `live_length` | Core; lengths through the length provider (L9) |

### Other files

| File | Owner |
|---|---|
| `merge.hh` 21–31 (`merge_source`, `merge_plan`), 55–79 (docid order, the plan) | Core |
| `merge.hh` 41–54 (key collection) | `keys` merge |
| `merge.hh` 77 (per-document data) | Each plugin's merge |
| `merge.hh` 80–145 (term union, alphabet) | `inverted` merge |
| `files.hh` (manifest, live docs, metadata) | Core; format 13's manifest gains the plugin set (L16) |
| `merge_policy.hh`, `budget.hh`, `faultfs.hh` | Core |
| `searcher.hh` 19–29 (`segment_snapshot`), 108–137 (`search`, `search_count`) | Core view; generic over the segment type (L11) |
| `searcher.hh` 84–100 (`snapshot_statistics`, `snapshot_df`) | `inverted` |
| `search.hh`, `scorer.hh`, `exact.hh` | `inverted`'s query side; scorers take its reader (L11) |
| `check.hh` | Core loop; each plugin's `check` |
| `ordered.hh` | Shared (used by aggregations, not by either layer's lifecycle) |

## The layout and the three orders (L2)

An index type is `basic_index<Layout>`:

```cpp
template <class... Entries> struct layout;

// Format 12 only: where the core's docids section sits between plugins. Deleted with
// format 12.
struct docids_section {};

using search_layout_12 = layout<inverted_plugin, docids_section, keys_plugin, stored_plugin>;
using search_layout    = layout<inverted_plugin, keys_plugin, stored_plugin>;  // format 13
using document_layout  = layout<keys_plugin, stored_plugin>;                   // format 13 (L12)

using index    = basic_index<search_layout>;     // search_layout_12 until format 13
using segment  = basic_segment<search_layout>;
using document = basic_document<search_layout>;
```

Three orders exist, and from format 13 they are independent:

| Order | Fixes | Format 12 | Format 13 |
|---|---|---|---|
| File order | Byte identity; what an open touches | `search_layout_12`'s order, which reproduces today's file exactly: header; `postings`, `positions`, `terms`, `term_index`, `lengths`, `norms`; `docids`; `versions`, `pk_filter`; `stored`; `stats` (`writer.hh:357–453`) | The header; then streamed and first-use sections (`postings`, `positions`, `terms`, `keys`, `stored`, Phase 4's columns and trees) by file rank, `lengths` last among them; then the pinned tail: the core's `docids`, `stats`, and `index_sort`, then each plugin's pinned sections by file rank (`inverted`'s `norms` and `term_index`, then `keys`' `versions` and `key_index`); then the table of contents and footer (S1, F13-R4) |
| Plugin order | Builder, reader, and merge call order; dependencies open first | Layout order | Layout order |
| Digest order | Field order inside a record | Layout order inside `D` (today's record) | None: `D` holds only core fields, and a sum of records has no order (L8) |

In format 13 the file order is the per-kind table's file rank (L4), which the sink reads;
a layout's order decides only the plugin order, so no check ties the two (R19).

Pinned sections (verified whole at open: `docids`, `stats`, `versions`, the key filter,
`term_index`, `norms`; `reader.hh:76–83`) sit at the end of the file in format 13,
next to the table of contents and footer that an open reads anyway, so an open reads
the header page and one contiguous tail run (S1). The placement costs no extra pass at
flush or merge: streamed sections go straight to the file, and `docids` and `stats`
are built from per-document fields known at `add`, so nothing waits on them. Placing
pinned sections first would spill and copy `postings`, the largest section, at every
flush and merge (`writer.hh:421–427` shows the copy `positions` already pays), so that
layout is not built. The cold-start row (BENCHMARKS, "open time per GB", first query
with a dropped page cache) and the indexing rows are reported for format 13 against
format 12, so the choice is shown, not assumed. Format 13's fixture pins the order, and
L14 7c's open test proves what an open touches by faulting on every other page (R15).

The layout is checked at compile time (`basic_segment`'s `static_assert`s): every entry
but `docids_section` satisfies `section_plugin` (L4); no two of the core and the plugins
claim a section kind; every plugin's declared dependencies (L3) appear earlier in the
layout; `docids_section` appears at most once (once in format 12's layout, never in
format 13's). Phase 4's `values`, `points`, and `term_lists` plugins
append to `search_layout`; appending changes no existing index's digest (L8) and no
existing segment's bytes.

A plugin derives from `plugin_base<Derived>` (CRTP), which supplies defaults (a merge
that rebuilds through the builder, no digest records, no statistics). `plugin_base`
arrives with the first plugin that takes one of its defaults: each format-12 plugin
writes its own merge, and none has records or statistics yet, so a base with no user
would be dead code (R18). The concept check does not wait for it: `basic_segment`
asserts it per layout entry.

## Keys: semantics in the core, encodings in the plugin (L3)

N4's split, as code:

- **The core owns** the version map (`index.hh:1593–1594`), newest-first resolution
  (`lookup_key`, 913–926), version checks and conditional operations (1113–1126), the
  docid–key invariants, and the key's place in the buffered entry (746–751).
- **The `keys` plugin owns** the `versions` and `pk_filter` sections today and the
  `keys` section in format 13, behind this reader interface:

```cpp
template <class R>
concept key_reader = requires(const R& r, std::string_view key, std::string_view prefix, ordinal_t ord,
                              live_bits live, const typename R::deps& d) {
	{ r.may_hold(key) } -> std::same_as<bool>;                          // filter and range
	{ r.find(key, live, d) } -> std::same_as<std::optional<ordinal_t>>; // the live ordinal holding key
	{ r.version(ord) } -> std::same_as<std::uint64_t>;                  // 0 for a keyless segment
	{ r.scan(prefix, d, [](std::string_view, ordinal_t) { return true; }) }; // keys starting with `prefix`, in order
};
```

`scan` takes a prefix, not a lower bound: format 12's keys are the terms under the key
prefix, and format 13's multi-term expansion over the key prefix runs the ordered scan
from the pattern's literal prefix (F13-R2).

`lookup_key` becomes: the version map, then for each segment newest first,
`keys.may_hold(key)` and `keys.find(key, live, seg.deps<keys_plugin>())`, then
`keys.version(ord)`. The digest's one-live-copy-per-key check and the merge's key
collection (`merge.hh:41–54`) use `scan`. Format 12's `scan` cannot tell a key from
another term, so its prefix must start with the key prefix (both callers pass it); format
13's `keys` section holds only keys, and the precondition goes with format 12 (R18).

**Format 12's dependency, passed at call time.** In format 12 the key is a term:
`key_of` scans the document's terms for the prefix (`index.hh:1026–1037`), and `find`
probes the term dictionary and walks the key term's postings (918–923). The format-12
`keys` plugin declares `using depends_on = type_list<inverted_plugin>`, and its
`deps` type is `struct { const inverted_plugin::reader& inverted; }`.
`basic_segment::deps<P>()` builds it from the segment's reader tuple at each call; no
reader stores a pointer to a sibling, so moving a segment never leaves one dangling
(R3). Format 13 removes the dependency: the key is a core field of the document
(`document::key`), the `keys` section holds key → ordinal, `deps` is empty, and the
`inverted` plugin no longer indexes the key. Format 13 rejects format 12 (`phase-4.md`,
"Compatibility"), so the format-13 `keys` plugin never reads a format-12 segment.

**Format 13's key routing is the planner's (F13-R2).** No reader falls back to another
when a lookup misses, which would be a dependency from `inverted` on `keys`, the reverse
of the one format 13 deletes. Instead:

1. In a keyed index every term with the key prefix is a key, since stage 1 allows no
   other. The planner resolves such a term through `keys_plugin::reader` as a key leaf:
   an iterator over at most one ordinal, wdf 0, no positions, never asking `inverted`.
   Its df counts the segments whose `keys` hold it, deleted copies included, as format
   12's postings did.
2. Stage 1 refuses what would break rule 1, as `invalid` (R13): `inverted`'s `validate`
   rejects a key-prefixed term in its part, with the prefix from `validate_context`; and
   in a layout with `inverted` and a key prefix, a document whose `d.key`, when present,
   does not start with the prefix is rejected too, the add path's form of the check
   `replace_key` and `delete_key` make today (`index.hh:1576`). The `inverted` builder
   still refuses a key-prefixed term (`std::invalid_argument`), with the prefix from
   `flush_context`, as a backstop no index call reaches, as `writer.hh:165` is for a
   repeated term. So the dictionary and `keys` are disjoint by construction, and
   `check()` verifies it with the prefix the core passes.
3. A multi-term pattern whose literal prefix starts with the key prefix expands over
   `keys` only, through `scan`; one whose literal prefix is a proper prefix of the key
   prefix (the empty one included) expands over both readers and merges in byte order;
   any other expands over `inverted` only. Expansion limits count both sources.
4. `keys` holds the key's bytes exactly as operations name them (for Xapiand, the `Q`
   prefix included); the core never adds or strips a prefix, and `document::key` holds
   the same bytes.

**The key prefix is the index's (R13).** It is an index option recorded in the manifest,
as today, and never a plugin's: the core passes it to `validate_context`, to
`flush_context` for rule 2's backstop, to `check()`, and to the planner, so no plugin
reads another's options and `inverted` takes no dependency on `keys` in the direction
rule 1 removes. The planner sits over `basic_segment` (L11), which reaches both readers
through `get<P>()`. Automata's `expand.hh` (SYS-9) takes rule 3's first two cases
through `keys`' `scan`; its reviewer checks it against these rules.

**Movability.** `basic_segment` stays movable, as `segment` is today. Its readers hold
only their section bytes (views into the file, which the owner keeps alive, as today)
and pointers to the core's heap-allocated integrity object (L7), whose address survives
a move, as `dict_`'s context does today (`reader.hh:140, 427`). A
`static_assert(std::is_nothrow_move_constructible_v<segment>)` pins it, and an ASan test
moves a format-12 segment and then looks up a key (step 2). `strata_engine` returns
segments by value from `open()` (`tools/strata_engine.cc:368` and others), which stays
correct without relying on copy elision.

A layout without `keys_plugin` is keyless: the core compiles out the version map with
`if constexpr`, and an operation that names a key or an expected version rejects as
invalid in stage 1.

## The plugin interface (L4)

```cpp
// What stage 1 can consult; built per operation by the core.
struct validate_context {
	const index_options& options;
	std::string_view key_prefix;   // the index's, from the manifest (index.hh:1028); every format (R13)
	const slot_table& slots;       // as of this operation: earlier declare_slot operations
	                               // of the same transaction included (D3)
};

template <class P>
concept section_plugin = requires {
	typename P::part;     // what a host document carries for this plugin (L10)
	typename P::builder;  // builds the plugin's sections at flush (L5)
	typename P::reader;   // an open segment's view of its sections (L7)
	{ P::id } -> std::convertible_to<std::string_view>;      // stable name, recorded in the manifest (L16)
	{ P::rules } -> std::convertible_to<std::span<const section_rule>>;  // sections it owns
	{ P::structure } -> std::convertible_to<std::uint32_t>;   // its own structure flag bit (format 13)
	{ P::structures } -> std::convertible_to<std::uint32_t>;  // every bit it owns, P::structure included
} && requires(const validate_context& vc, const typename P::part& part, digest_record& rec) {
	{ P::validate(vc, part) } -> std::same_as<bool>;         // stage-1 checks (index.hh:1040–1050)
	{ P::bytes(part) } -> std::same_as<std::uint64_t>;       // buffer memory (711–716)
	{ P::disk_estimate(part) } -> std::same_as<std::uint64_t>;  // storage admission (891–899)
} && requires(const merge_context& m, std::span<const typename P::reader* const> in, section_sink& sink) {
	{ P::merge(m, in, sink) } -> std::same_as<built>;        // L6
};

struct section_rule {
	format::section kind;
	enum verify : std::uint8_t { pinned, regions, first_use } check;  // L7
	enum need : std::uint8_t { with_plugin, optional } presence;      // L7
};

struct built {
	std::uint32_t structures = 0;  // flag bits the plugin built (0: none of its sections)
};
```

**Structure bits are a mask per plugin (F13-R3).** A plugin owns one bit of its own,
`P::structure`, which `holds<P>()` reads (L11), and may own optional sub-structures:
`inverted` owns `inverted | norms`, since D2 makes norms optional and an optional
section without a bit cannot be told from a lost one. `keys`, `stored`, `values`,
`points`, and `term_lists` own one bit each. `index_sort` is the core's: D14's order is
the merge's order source, which the core calls (L6). A sub-structure's bit set without
its plugin's own bit is corruption.

**One per-kind table, in `format.hh`, from step 7c (F13-R3).** Per section kind: its id,
its owner (the core or a plugin's `id`), the bit that requires it (0: mandatory), its
verification class (`pinned`, `regions`, `first_use`), and its file rank. The core's
open, the sink's order, the structure check, and each plugin's `rules` read it; `P::rules`
becomes the table's rows whose owner is `P::id`, so no second copy of a kind's class or
presence exists. Until step 7c, `section_rule` and `built` stay as they are in
`section_io.hh`, and this branch edits neither `format.hh` nor `keys.hh`.

`bytes` and `disk_estimate` are pure functions of the part: no runtime state, so their
sums equal today's values (`index.hh:711–716, 891–899`) and flush triggers and storage
admission do not move. `validate` and `key_of` take the context, because they read the
key prefix today and the slot table in Phase 4 (R1).

Format 12's `section_plugin` (`reader.hh`) checks what format-12 plugins have: `id`,
`rules`, a reader constructed from the `open_context` with `verify_all`, `merge`, and,
for a plugin with a part, `validate`, `bytes`, and `disk_estimate`. Format 12's builders
are the writer's classes (`inverted_builder`, `keys_builder`, `stored_builder`), not
`P::builder`, and no plugin has `structure` or `structures`; both join the concept at 7c,
with the per-kind table (R18).

The core calls plugins only through these names. Every call resolves at compile time;
the core holds plugin state in `std::tuple<typename P::builder...>`,
`std::tuple<typename P::reader...>`, and the document's base classes, and visits them
with fold expressions.

## Flush (L5)

The core sorts the buffer's live documents by docid (`index.hh:1173–1178`), splits
them into runs (1180–1187), and per run:

```cpp
struct doc_fields {
	std::uint64_t docid;
	std::uint64_t version;  // 0 for a keyless document
	std::string_view key;   // empty for a keyless document
};

struct flush_context {
	std::uint32_t version;          // the format being written
	std::uint8_t region_shift;      // segment_writer::region_shift today
	std::uint32_t doc_count;        // documents in this run
	const slot_directory* slots;    // the declarations the segment is written under; null before format 13
	std::string_view key_prefix;    // the index's; format 13's `inverted` builder refuses a term with it (L3)
};

template <class B, class Part>
concept section_builder = requires(B& b, ordinal_t ord, const doc_fields& f, const Part& p, section_sink& sink) {
	{ b.add(ord, f, p) } -> std::same_as<void>;   // in output order, ordinals from 0
	{ b.bytes() } -> std::same_as<std::uint64_t>; // memory it holds now
	{ std::move(b).finish(sink) } -> std::same_as<built>;
};
```

`P::builder` is constructed from the `flush_context`. The core calls `add` for every
plugin, in layout order, for each document in output order, then `finish` in layout
order. The `inverted` builder is today's term table and posting buffers
(`writer.hh:469–576`); its `finish` is `flush` plus `flush_from`'s postings, positions,
dictionary, lengths, and norms (295–448). The `stored` builder is `stored_` and
`stored_ends_` with `encode_stored`. The `keys` builder is `versions_`, `key_hashes_`,
and the key range with `encode_versions` and `encode_pk_filter`.

**The sink.** `section_sink` is the core's: it wraps `counting_output`, aligns each
section, runs `section_stream`, and appends the table-of-contents entry. A section
produced alongside another (positions beside postings) goes through
`sink.deferred(kind)`, a stream into a `spill`, and `sink.place(deferred)` aligns and
copies it later, as `writer.hh:365–427` does today. The sink, not the plugin, decides
file order (L2). A plugin never writes the header, the table of contents, or the
footer, and never sees file offsets.

**Memory.** The writer's term table and posting buffers are uncharged today. Charging
them is Phase 3's D17 work ("the flush's sort and spill buffers", `phase-3.md:301–303`)
and lands in Phase 3, on today's `segment_writer`, before Phase 3's ratchet rows are
recorded (R5). The refactor carries that charge through unchanged: wherever D17 reads
the writer's memory, step 3 reads the sum of the builders' `bytes()`, which equals it.
The refactor adds no charge and stays byte-identical with no exception. The merge holds
32 bytes a document, as main's did: the sort's docid and source, then `order` and `docids`
(LC2; `merge_charge` holds 23,164,195 bytes where main holds 23,344,211).

## Merges (L6)

M2's interface. The core computes what it computes today (`merge.hh:55–79`): live
documents of every input in docid order, the old-to-new ordinal map, the output count,
and the duplicate-docid check. It passes:

```cpp
struct merge_input {
	const core_segment* core;              // docids and document count
	const std::vector<std::uint64_t>* deleted;  // bit per ordinal; null: none deleted
	std::span<const ordinal_t> new_ordinal;     // [old ordinal] = new ordinal, or no_more_docs
	bool live(ordinal_t) const;
};
struct source_ref {
	std::uint32_t input;
	ordinal_t ord;
};
struct merge_context : flush_context {
	std::span<const merge_input> inputs;
	std::span<const source_ref> order;     // the permutation: [new ordinal] = its source
	std::span<const std::uint64_t> docids; // [new ordinal] = its docid
	std::span<const key_map> keys;         // format 12: [input] each live document's key
	bool docid_order;                      // false when the output is reordered (P1)
};

// P::merge(const merge_context&, std::span<const typename P::reader* const> inputs,
//          section_sink&) -> built
```

**The core's entry point takes named options (F13-R11).** A positional tail after the
sink lets a call that drops one argument rebind the next silently (`{}` for the key
prefix would bind the region shift to 0, which only the reader's region-size check
catches). So:

```cpp
struct merge_options {
	std::uint8_t region_shift = format::default_region_shift;
	const task::cancel_token* cancel = nullptr;   // checked per document, term, and frame
	flush_charge* memory = nullptr;               // the merge's working memory, when budgeted
	std::span<const merge_ref> order;             // [new ordinal] = its source; empty: docid order,
	                                              // except one permuted source keeps its order
	scratch* records = nullptr;                   // out of docid order: where stored records spill
	std::string_view key_prefix;                  // format 12 only: the key terms' prefix; deleted at format 13
};
merge_plan merge_segments(std::span<const merge_source>, const segment_id&, output& sink,
                          spill& positions, const merge_options& = {});
```

Callers name each field with designated initializers, in declaration order
(`{.order = order, .key_prefix = "Q"}`), so a field removed later fails to compile where
it is used instead of rebinding. The protection holds for designated callers only:
`merge_options` is an aggregate, so positional initialization (`{12, &token}`) still
compiles. "Designated initializers only" is therefore a review rule for every
`merge_options` a change adds (R19). Format 13 deletes
`key_prefix` with the key's move into the document (L3); nothing in the merge or in the
`keys` plugin's scan depends on it beyond format 12's key collection.

**Where `order` comes from** (reordering review, P1-R4 (d)). The core cannot compute a
non-docid order itself: bisection reads the `inverted` plugin's postings, an index sort
reads `values`, and key order reads `keys`. A layout names an optional order source, a
function the core calls before any plugin's merge, given the inputs' readers and the live
documents in docid order, returning `order` (or nothing, for docid order). P1's bisection,
Phase 4's D14 index sort, and the RocksDB survey's K5 key order are its three users; one
mechanism, three sort keys. Main's `merge_segments` takes the same `order` today
(`reordering.md` D3), so step 5's oracle includes the permuted path.

`inputs[i]` is the plugin's own reader for input `i`, typed, so a plugin reads its
sections directly. A plugin with dependencies (format 12's `keys`) also receives each
input's `deps`. What each plugin does:

| Plugin | Merge |
|---|---|
| `inverted` | Today's term-major union (`merge.hh:80–145`): the alphabet from the inputs' dictionaries, each term's live postings remapped through `new_ordinal`. Later, P17: a postings block whose ordinals all stay live and shift by one constant is copied with only its base rewritten. |
| `stored` | Rebuild: for each new ordinal, `add` the record read from its source. Each source frame is decompressed once: in docid order the merge reads a frame at a time per source, and out of docid order each source's live records spill to `records` first (P1-R4 (b), `merge.hh:202–209`); step 5's merge row shows the cost. Later, I7: a frame whose documents are all live and stay contiguous in `order` is copied raw, its index entry rewritten. |
| `keys` | Rebuild per new ordinal: its docid, its input's version, and its key; format 12's core collects keys with `scan`. |
| `values`, `points` (Phase 4) | Rebuild columns and trees from live documents in `order` (D17); 1-D trees merge by stream. The `values` section's skip index is the review's R10, on `phase4/docvalues`. |

`plugin_base` supplies the default merge: construct the builder, and for each new
ordinal call `add(ord, fields, in[src.input]->part(src.ord))`, with `fields` formed from
`docids[ord]` and the source (LC2: the core materializes no per-document fields). A plugin whose
reader cannot rebuild a part (the `inverted` reader is term-major) overrides `merge`.

**Byte identity.** The rebuild paths reproduce today's merge output byte for byte;
step 5 checks it against the old `merge_segments`, kept as a test oracle until that
step lands.

**The copy paths** (P17, I7) change merge output bytes without changing the format, and
the digest does not cover what a bad copy damages (skip data, block and whole-list
competitive pairs, norms, the stored frame index, region CRCs). Each lands after the
refactor as its own change, and its commit shows, for random merges with and without
deletes, in docid order and reordered (R8):

1. Every section of the copied output decodes to the same content as the rebuild
   path's output for the same merge: postings with skip data and impacts, positions,
   norms and lengths, the stored frame index and records.
2. `check()` passes on the copied output.
3. `verify` and `parity` return identical results on both outputs.
4. The output is deterministic: the same inputs give the same bytes.
5. T3's write-amplification row, before and after, with the merge fixture re-pinned in
   the same commit.

## Open and verification (L7)

```cpp
// One owned section, resolved once at reader construction.
struct section_handle {
	std::string_view bytes;
	std::uint64_t base;                       // the section's file offset
	const residency::verified_regions* regions;  // the segment's shared object (heap, stable)
	bool verify(std::uint64_t offset, std::uint64_t length) const {
		return regions->verify({base + offset, length});
	}
};

struct open_context {
	std::uint32_t version;       // the segment's format version
	std::uint64_t doc_count;
	const core_segment& core;    // docids, for plugins that check against them
	std::uint32_t structures;    // format 13: the footer's flags within P::structures, per reader (R16)
	// The handle for an owned kind, or an empty handle when the section is absent.
	section_handle section(format::section kind) const;
};
// P::reader(const open_context&): throws corrupt_segment
// r.verify_all() const; r.check(check_report&, live_bits) const
```

**Handles, not lookups (R2).** A reader calls `open_context::section(kind)` once per
owned section in its constructor and keeps the handles as members. `inverted`'s
`need()` (`reader.hh:988, 1161`) becomes `postings_.verify(offset, n)`, which is
today's `verify_postings` arithmetic exactly (`reader.hh:199–204`): one add and the
shared object's verify, with no lookup by kind on the per-block path. The
`verified_regions` object lives in the core's heap-allocated integrity state, as today
(`reader.hh:412–427`), so handles survive moving the segment. The lazily verified
`lengths` state (418–422, the atomic) moves into `inverted`'s reader, in a heap object
of its own for the same reason.

**The core's open.** It decodes the footer and table of contents and checks the
header (`reader.hh:64–72`). For every entry it finds the claiming rule: the core's own
kinds (header, docids, stats, and from format 13 `index_sort`, R16) or a plugin's. An entry no rule claims is corruption in a
format version that knows the kind, and `unsupported_segment` when the layout lacks the
plugin that owns it; L16's manifest check normally rejects such an index first, so this
is the segment-level defense. By rule, it checks `pinned` sections' CRCs whole (today
the list at 76–82), registers `regions` sections with one shared `verified_regions`
(116–132, unchanged: one object per segment, sorted spans), and leaves `first_use`
sections to the plugin. Then `basic_segment` computes the presence mask from the
footer's flags, and only then constructs each reader in layout order, passing each its
own bits in `open_context::structures`, so `holds<P>()` and the reader's emptiness are one
derivation of one fact (R16).

**Counts.** The `inverted` reader keeps a copy of the document count for `statistics`
(`search.hh:54`) and its ordinal checks; the core checks it equal to its own at open,
as `reader.hh:110` checks the footer's today.

**Presence.** In format 12 every plugin of the layout is required (today's open fails
without `lengths`, 111). From format 13 the footer's structure flags record what the
segment holds, and a section is present exactly when the bit that requires it is set
(F13-R3; the per-kind table of L4 names the bit, 0 for the core's mandatory sections). A
plugin whose own bit is clear has none of its sections, and its reader opens empty and
answers "no such structure" (`term` returns nothing, `stored` returns empty). This is
`phase-4.md`'s "required together" and "free" rules, stated per bit.

**The bits a binary reads (R16)** are the core's (with `index_sort` once its reader
exists) and each layout entry's `P::structures`. Any other set bit, `index_sort`'s
included until its reader lands, takes the rule below.

**A set bit this binary cannot read** is never read past and never called corruption by
the segment alone (F13-R3): a lone segment throws `unsupported_segment`; an index whose
manifest lists the bit's plugin is refused with `unsupported_index` (L16); a set bit
whose plugin the manifest does not list is corruption, since the manifest vouches for
what its segments hold. This is how `points` arrives after the freeze (owner's ruling on
SYS-4), and it covers `values`, `term_lists`, and `index_sort` until their readers land.

**Format N − 1.** One plugin, two layouts, selected by `open_context::version` inside
the reader's constructor, as N4 asks. The core never translates sections. The writer
writes only the current version. A plugin that keeps an N − 1 layout keeps its decoder
and the fixture for it; when N + 1 lands, N − 1's decoder is deleted.

**Equal verdicts.** Where a check below says a fuzz corpus replay "finds the same
verdicts", it means the same accept-or-reject outcome per input, and the same exception
type except where the old code leaked a type other than `corrupt_segment`
(`std::invalid_argument`, `termdict::corrupt`): that input may change only to
`corrupt_segment`, which `heal` and `media_test` rely on. It is not the same message: the
rule loop may report a different one of two damaged sections first (R11, R17). Step 2
changed 29 of the corpus's 1,089 inputs that way and no outcome. It also rejects a kind
listed twice in the table of contents, which main accepted by reading its first or last
entry; no corpus input has one, and the full review rules on it as a behavior change.

## The digest (L8)

The digest stays one sum of xxHash64 over records (`phase-3.md`, "Digest"). The core
composes it; plugins contribute records.

**Format 12** keeps today's records exactly, pinned by step 0's digest value: the `D`
record is `'D'`, docid, version, `inverted`'s length (u32), then `stored`'s size and
bytes (`index.hh:524–533`), assembled by the core, which names the three readers. Its
fields are not in layout order (`inverted` precedes `keys` in `search_layout_12`), so no
per-plugin hook in layout order can build it, and none is added for a record 7c's F21
commit deletes (R18). `inverted` emits today's `P` records
(534–543, 550–560, 566). The core emits `L`, `S`, and `M` (568–581).

**Format 13** splits `D` (R6):

- `D` carries only the core's fields: docid, version, key (length u32, bytes).
- Each plugin with per-document content emits its own record: `inverted` a length
  record and its `P` records, `stored` a record holding the stored bytes, `values` its
  `V` records (D17).
- A plugin never adds fields to the core's record.
- A plugin with nothing for a document emits nothing: no stored record for an empty
  one. The rule is stated on content, so two implementations of one layout cannot
  differ. The exception is `inverted`'s length record, which every document of an
  index whose plugin set (L16) includes `inverted` emits, length 0 included (revision
  check 1, finding 1, stated on the index's set rather than on the compile-time layout,
  R14), so two binaries digest one index equally.
- The core adds the slot-table record.

Records come from `P::records(reader, live, emit)` over a segment and
`P::records(docid, part, emit)` over a buffered document. Since the digest is a sum,
record order is irrelevant, so two layouts digest the content they share equally, and
appending a plugin to a layout leaves every existing index's digest unchanged, which
rolling upgrades (Track D8) need when replicas on two binaries compare digests. The
cost is one more hash per document per plugin, off the query and indexing paths. The
digest changes value once, at format 13, together with the key's move into `D`: one
step-7c commit carries the split, the key in `D`, `inverted`'s length record for every
document, and format 13's version 0 for keyless documents (F9), drops any interim
record (format 13's `P` record per key), re-pins step 0's `digest.txt` and the fixtures
that write a keyless document with a version, and runs T2's schedules (F13-R5, F13-R9).

**Invariants** stay in the core: one live copy per docid (from `D`) and per key (from
the `keys` reader's `scan` over live ordinals, and the buffer's keys).

Records are generated from readers and buffered parts on each `digest()` call, never
stored with a segment: a record set cached per segment would go stale with every live-
docs generation, and content-derived bytes would enter the format. Records depend only
on docid and content, so T2's independence of segmentation holds by construction.

## Statistics and lengths (L9)

The core needs a document's length in places that are not search: the manifest's
totals (`index.hh:1295–1302`) and live-docs files' live length (1257, 1321–1327). A
plugin that provides lengths declares `static constexpr bool provides_length = true`
and implements `reader::length(ord)`, `reader::total_length()`, and `length(part)`; the
layout allows at most one. The core reads it through `if constexpr`; without one, every
length is 0, which the manifest's format already accepts.

The `stats` section is the core's. In format 12 it is the document count followed by
`inverted`'s 20 bytes (`writer.hh:429–434`): the format-12 core calls the provider's
`legacy_stats()` to append them, and `reader.hh:106–110`'s check splits them back. In
format 13, `stats` holds only the core's fields at its fixed position (L2), and each
plugin's statistics live in a pinned section of its own, so a plugin's sections are
self-contained and an open reads them in the tail run. `inverted`'s 20 bytes go to the
head of `term_index`, which is pinned, present whenever `inverted` is, and resealed by
the fuzzer with every other section checksum; `lengths` then holds only lengths and is
read on first use (F13-R4, format 13's F7 as revised). `legacy_stats` is deleted with
format 12.

## Documents, transactions, and the buffer (L10)

The document is the composition of the parts:

```cpp
template <class E> struct empty_part {};   // one distinct type per layout entry

template <class... Entries>
struct basic_document<layout<Entries...>> : core_part, plugin_part_t<Entries>... {};

struct core_part {};                  // format 13: std::string key;
struct inverted_plugin::part {        // today's document::terms and ::length
	std::vector<term_entry> terms;
	std::uint32_t length = 0;
};
struct stored_plugin::part {
	std::string stored;
};
```

`plugin_part_t<E>` is `E::part` for a plugin with one, and `empty_part<E>` otherwise
(`docids_section`, format 12's `keys`): a distinct empty base per entry, so two empty
entries never become a duplicate base, and empty-base optimization keeps them free.
Inheritance keeps today's source working: `d.terms`, `d.length`, `d.stored` (as tests
and `tools/strata_engine.cc` use them) name the same members, default construction is
unchanged, and the core reaches a part with `static_cast<const P::part&>(doc)` without
naming a member. Phase 4's parts add `values` (slot → typed values) and points, which
are derived from values (D17).

`basic_operation<L>` and `basic_transaction<L>` are today's `operation` and
`transaction` (`index.hh:48–62`) with `document` replaced by `basic_document<L>`;
`declare_slot` (D3) is a core operation. The aliases keep today's names.

**The buffer.** `buffered` (`index.hh:746–751`) keeps `docid`, `version`, `key`,
`dead`, and the whole `basic_document<L>`, as today (`index.hh:1151` copies it in). The
core never inspects a part:

| Today | After |
|---|---|
| `valid(d)` (1040–1050) | `(P::validate(vc, d) && ...)`, in layout order; stage 1 rejects `invalid` on the first false |
| `key_of(d, valid)` (1026–1037) | `keys_plugin::key_of(vc, d)`, reading `vc.key_prefix`; format 13 reads `d.key`, which must start with `vc.key_prefix` when `inverted` is in the layout (L3, R13) |
| `doc_bytes(d)` (711–716) | `sizeof(document) + 96 + (P::bytes(d) + ...)`, the same sum as today |
| `estimate_bytes(t)` (891–899) | `64 + Σ ops (16 + key and meta sizes + (P::disk_estimate(doc) + ...))`, the same sum |
| `invert` (1188–1199) | `add` on each builder for each document |
| Digest over `buffer_` (562–567) | L8's records from the parts |

The sums equal today's values, which keeps flush triggers and storage admission, and
therefore every segment's contents, unchanged. A `static_assert` pins
`sizeof(document)` at today's value through format 12. At format 13 the core part gains
`key`, so `doc_bytes` and every flush trigger change; that change is measured with
format 13's indexing rows, not here.

**Reads of buffered documents.** Real-time get (D13) and delete by query's buffer path
(D16) read parts directly: `stored_plugin::stored(part)`, and a per-plugin leaf
predicate for boolean matching (`inverted_plugin::matches(part, term)`,
`values_plugin::matches(part, range)`), defined with D16. Neither needs the core to know
a part's shape. From format 13 the buffered document's terms hold no key (L3, rule 2),
so the buffer path's predicates add the key leaf over the buffered entry's key, under
L3's routing: a key-prefixed term or pattern matches the key and never the terms
(R13). Without it, delete by query on an id term or an id wildcard would miss every
buffered document.

## Query-side access (L11)

```cpp
template <class L>
class basic_segment {
public:
	const core_segment& core() const noexcept;                          // docid, ordinal_of, doc_count
	template <class P> const typename P::reader& get() const noexcept;  // static_assert: P in L
	template <class P> typename P::deps deps() const noexcept;          // L3; empty from format 13
	template <class P> static constexpr bool contains() noexcept;       // P is in L
	template <class P> bool holds() const noexcept;                     // this segment holds P
};
```

`holds<P>()` is per segment (SYS-16, F13-R3): it reads a presence mask that
`basic_segment` computes at open from the core, one bit per layout entry, since only the
layout knows its entries. The mask is computed before the readers open, and each reader
gets its bits from it (L7, R16). In format 12 every plugin of the layout is present, so
the mask is full; from format 13 an entry is present when the footer's flags hold its
`P::structure`. It is a member, not a `static constexpr`, because format 13's presence
rules (L7) make it a property of the file. For a plugin outside the layout it is false
with no load.

`contains<P>()` is the compile-time question, whether P is in the layout at all (R12). A
runtime `if` instantiates its body, so only `if constexpr` keeps `get<P>()`, which does
not compile outside the layout, out of code generic over layouts; that code asks both:

```cpp
if constexpr (S::template contains<P>()) {
	if (s.template holds<P>()) { /* s.template get<P>() */ }
}
```

`get<P>()` returns a reference into the segment's reader tuple: a compile-time offset,
no branch, no virtual call. Search code takes the plugin's reader, not the segment:
`term_scorer`, `phrase_scorer`, `postings_iterator`, and `positions_reader` take
`const inverted_plugin::reader&` (`search.hh:219–222, 867`; `reader.hh:439, 1035`).
`statistics` and `snapshot_df` take the same reader. The query planner is the
exception: it routes between `inverted` and `keys` (L3), so it takes the segment, reaches
both readers through `get<P>()`, and gets the key prefix from the core (R13). `segment_snapshot`
(`searcher.hh:19–29`) becomes generic over the segment type, and `search` and
`search_count` (108–137) resolve hits' docids through `core()`.

The scorer interface (`search.hh:122`) is virtual today and stays so: per-document
calls cost what they cost now. The refactor adds no dispatch on any per-posting or
per-document path; the only new step per query is `get<P>()` once per segment at
scorer setup, which compiles to an address computation and allocates nothing. It
multiplies with the segment count, which is why L15 measures the multi-segment index
before merging.

## The document profile (L12)

```cpp
using document_layout = layout<keys_plugin, stored_plugin>;
using document_index  = basic_index<document_layout>;
```

`document_index` is a sorted key-value store with versions, snapshots, and atomic
batches: `apply` with `add`, `replace_key`, `delete_key`, and conditional versions;
`find_key`; gets of the stored record by key; `keys_plugin::reader::scan` for ordered
range scans; `checkpoint`, `install`, and the digest. It links no term dictionary,
postings, scorers, or `bitpack` postings code.

It exists from format 13 only. In format 12 the key is a term, so `keys` depends on
`inverted` (L3), and format 12's open requires `lengths`; a format-12 segment without
`inverted` is invalid. Until format 13, a compile-only test (`concepts_test.cc`)
instantiates `basic_document` and `basic_segment` over `document_layout` with a stub
`keys` that depends on nothing, to keep the composition honest; `basic_index` over it
waits for format 13, since format 12's index reads `inverted` directly (the digest's `P`
records, the lengths). With format 13,
its tests are the oracle's key-value model (`workload-profiles.md`, "What it costs")
and the gate's segment of every profile; its measurement against RocksDB is
`workload-profiles.md`'s and not this refactor's.

The runtime profiles (`search`, `document`, `graph`, `log`) are a different mechanism:
they choose which of an instantiation's plugins build sections in a given index, via
structure flags. Xapiand uses `search_layout` with runtime profiles; the compile-time
`document_layout` is for strata used as a library. Under L8's format-13 digest, a
`search_layout` index holding only keys and stored records, whose plugin set (L16)
therefore lists only `keys` and `stored`, digests equal to a `document_layout` index with
the same content.

## Code size and compile time (L13)

- **Instantiations.** A binary instantiates each layout it names once. Xapiand and
  `strata_engine` name one, so the search code generated is today's with the same
  functions inlined; no plugin code is duplicated. Member functions of a class
  template are instantiated only when used.
- **Compile time.** `index.hh` becomes a template, so each translation unit that
  includes it compiles the core it uses. Today every test TU already compiles
  `index`'s inline members, so the difference is template overhead, not new code.
  If it grows the test build measurably, `index.hh` declares
  `extern template class basic_index<search_layout>;` and one `.cc` instantiates it.
- **Measured per step,** without a new tool: `size -A` (Linux) or `size -m` (macOS) of
  `strata_engine` and `index_test`, and the wall time of a clean build of the test
  targets with `-ftime-trace` on `index_test.cc`, before and after. Nothing is
  measured yet; this document makes no size claim.

## Order of the refactor (L14)

Each step builds and passes on every platform in CI before the next starts. Steps 0 to
7b keep format 12 byte-identical, with no exception, and the public API source-compatible
through the aliases, with one exception made on purpose: 7a changes `merge_segments`'
signature, whose every caller is in this repository. Step 7c is format 13. Steps may be written and reviewed on the branch
at any time; none lands on `main` before Phase 3's ratchet rows are recorded at a
stated commit (N4), and that commit includes Phase 3's D17 flush charge (R5). A
measured step lands with its A/B report in `docs/perf/` (L15).

| Step | Change | Checks beyond the full test suite |
|---|---|---|
| 0 | Fixtures pinning today's behavior: a segment with stored records across several frames (today's `small.sx` stores none, `segment_test.cc:306`); a keyless segment (no `pk_filter`, `writer.hh:451`); an empty segment, if `flush` can produce one; a segment written with `wide_skip_offsets`; a two-segment merge's output; a merge of inputs with deletes; the digest value of a fixed transaction script with keys, replaces, deletes, metadata, and buffered documents | They pass on the unchanged code |
| 1 | Move `output`, `spill`, `counting_output`, `section_stream` (`writer.hh:75–223`) to `section_io.hh`; add `section_sink`, `section_rule`, `section_handle` | Moves only; fixtures |
| 2 | Open path: `core_segment` plus `inverted`, `keys`, `stored` readers with section handles (L7) and call-time `deps` (L3); `segment` aliases `basic_segment<search_layout_12>` with forwarding members (`seg.term()`, `seg.stored()`, …), so callers compile unchanged | Fixtures open; `fuzz_segment` corpus replay finds the same verdicts (L7); `heal`, `media_test`; the move `static_assert` and the ASan move-then-lookup test; **A/B** |
| 3 | Flush path: builders and the sink; `segment_writer` becomes a façade over them (the fixture test and `fuzz_segment` use it); the D17 charge reads the builders' `bytes()` | Step 0's fixtures byte-equal; flush runs (D18) byte-equal; `multisegment_test` under TSan; **A/B** |
| 4 | `basic_document`, `basic_operation`, `basic_transaction`, `basic_index<L>`; `validate_context`; `key_of`, `valid`, `doc_bytes`, `estimate_bytes`, and the digest through plugins | Step 0's digest value; `index_test` and T2's schedules with a key prefix set and unset; `churn_test`; manifest, live-docs, and metadata fixtures; `multisegment_test` under TSan |
| 5 | Merge path through L6's interface (M2); the old `merge_segments` kept in a test as the oracle, then deleted | Step 0's merge fixtures byte-equal; random merges, with and without deletes, byte-equal to the oracle; digest unchanged across merges; `multisegment_test` under TSan; **A/B with the merge rows** |
| 6 | Search code on plugin readers (L11); the forwarding members removed | `verify` and `parity` on the multi-segment indexes; **A/B, the full L15 plan** against the recorded Phase 3 commit |
| 7a | `merge_segments(sources, id, sink, spill, const merge_options&)`, every caller naming its fields (F13-R11) | Step 0's merge fixtures byte-equal; `merge_oracle_test` unchanged in what it compares |
| 7b | `holds<P>()` reads a per-segment presence mask the core computes at open; full in format 12 (SYS-16, F13-R3) | Fixtures; a test that `holds<P>()` is true for each plugin of a format-12 segment and false for one outside the layout; `static_assert`s that `contains<P>()` is true for each plugin of the layout and false for one outside it; a function template guarding `get<P>()` with both, instantiated for each (R12) |
| 7c | Format 13 on the plugins, on format 13's branch rebased onto this one, in the review's order (`format-13-design.md`, "The rebase onto layers"): F2–F6 in the core's open and the sink, with `section_rule` merged into `format.hh`'s per-kind table (F13-R3) and the file order of L2; F7's statistics (L9, F13-R4); the `keys` plugin on `keys.hh` with `deps` removed and the planner's key leaf (L3, F13-R2), keeping the interim `P` record per key, which holds the pinned digest until F21's commit, so the digest's value changes once (F13-R5); F9's presence, the mask of 7b set from the footer's flags, readers opening empty (L7); F21's digest, once (L8, F13-R5); F20 with L16's checks (F13-R1); then `docids_section`, `search_layout_12`, and `merge_options::key_prefix` deleted, and `document_layout` tests | Format 13's fixtures and gate, the fixture pinning S1's order; a reader test that a fresh open touches only the header page and the tail (R15): it copies the segment into an anonymous mapping, removes access (`mprotect`, `PROT_NONE`) to every whole page strictly between the header page and the page holding the tail's first byte, and opens it, so any read outside the header page and the tail faults, then asserts `verified_regions() == 0`; counting verified-region pages alone cannot see it, since pinned sections are checksummed from the file's bytes and never become verified regions; the free-and-absent cases (F13-R3); the unreadable set bit's three cases, a lone segment with a bit the binary lacks (`unsupported_segment`) and that segment in an index whose manifest lists the plugin (`unsupported_index`) and in one whose manifest does not (`corrupt_segment`), and a segment with a plugin's bit clear, where `holds<P>()` is false and the reader answers empty (R16); stage 1 rejecting a key-prefixed term in `inverted`'s part and a key without the prefix, and delete by query on an id term and an id wildcard reaching buffered documents (R13); the cold-start and indexing rows against format 12; the manifest tests of L16 |

The first draft's step 7, the flush memory charge, left the refactor (R5).

**Phase 3R.** The refactor lands before Phase 3R's `strata::group` code depends on the
index API. If group code is ready before step 6, it builds against the aliases
(`strata::index`, `strata::segment`, `strata::document`), which stay source-compatible
through every step. No group code names a plugin type before format 13.

## Measurement (L15)

N4's before-and-after, as amended by R4. Each measured step (2, 3, 5, 6) compares its
commit (B) with the previous measured commit (A); step 6 also compares with the
recorded Phase 3 commit, which is the comparison N4 requires. A report per step goes to
`docs/perf/`: the commit pair, every row's interval, and the byte-identity hashes.

**Rows.**

- **Queries:** `bench/ab.sh` over every command (`TOP_10`, `TOP_100`, `TOP_1000`,
  `TOP_100_COUNT`, `COUNT`) and both query sets (`queries`, `terms-stratified`), five
  trials, order alternated, on two indexes: the multi-segment index before merging and
  the same index after a forced merge to one segment (PLAN's Phase 3 gate). Each
  variant serves an index built by its own binary; the run checks by hash that the two
  are byte-identical. Counts must match (`bench/ab.py`).
- **Allocations per query, by class,** from the allocation-counting build: exactly
  equal between A and B, not within noise.
- **Serving memory:** the cgroup row on both indexes, since the reader tuple and the
  split integrity state change what each open segment pins.
- **Indexing:** both rows, single-thread and multi-thread (`STRATA_THREADS`), with
  `bench/index_trials.sh` running the two binaries as two engines in rotated order,
  five trials each, at the same `STRATA_BUDGET_MB`; the output indexes' hashes must be
  equal.
- **Merge** (step 5, then step 6): a timed forced merge of the 5M index to one
  segment, five trials, with the output's hash equal between A and B; T3's
  write-amplification row before and after.

**The decision rule.** Per row, `ab.py`'s median of B/A and its 95% bootstrap interval
(for time; equality for allocations and hashes):

1. A row passes when its interval's upper bound is at most 1.03, BENCHMARKS' 3% effect
   size.
2. A row above that runs again with ten trials.
3. It blocks the step if its upper bound still exceeds 1.03, until it is fixed or the
   architect accepts it with its cause.

**Where rows run (SYS-6).** Every row above is a branch A/B, run only under a hold the
lead grants, and is evidence for keeping or reverting a step, never a gate row (PLAN,
"What waits"). Gate rows are measured on `main` after the step lands. Step 2's `COUNT`
row on the stratified terms (1.156× from one round, `docs/perf/layers-refactor.md`) is
open: it reruns under rule 2's ten trials, under such a hold, before step 3 lands.

**Platforms.** The gate VM (Linux x86_64) and the FreeBSD VM, every measured step.
macOS on the host is a report row. arm64 runs when its replacement machine exists,
before Phase 3R's gate, as the Track P rule asks of every measurement.

## The plugin set in the manifest (L16)

From format 13 the manifest (whose format version changes with format 13 anyway, D3)
records the index's plugin set, the identifiers (`P::id`: `inverted`, `keys`, `stored`,
`values`, …) of the plugins whose structures the index may hold, and the lowest and
highest format version among the manifest's segments. An opaque layout identifier is not
used, since it would forbid appending a plugin.

**The set is the index's, not the binary's (R14; format 13's C6, rules 3 and 5).** A
binary that lists its own plugins would put a newly appended plugin into every index it
writes, and a replica on the binary before it would refuse that index and every
checkpoint with `unsupported_index` whether or not a segment holds the new structure,
breaking Track D8's rolling upgrade and the N and N − 1 rule. So:

1. A plugin joins an index's set when the index first needs it: its profile, for
   `inverted`, `keys`, and `stored`; a slot declaration, for `values` and `points`. It
   never joins only because the binary has it.
2. A binary writes a plugin's structure only into an index whose set lists the plugin.
3. Under mixed binaries a plugin joins only once every replica's binary has it, which
   the cluster's upgrade protocol (Track D8) establishes before it lets the index need
   the plugin.

**One version per segment (S2).** A plugin's section version is a function of the
segment's format version, which the segment's header records and `open_context::version`
passes; plugins have no versions of their own. Every format bump makes each plugin
carry an N − 1 decoder, which is the same code for a plugin whose sections did not
change, matching PLAN's one format version per phase. The manifest holds no
per-segment version: a merge policy that prefers rewriting N − 1 segments reads the
version from the open segment the index already holds (`index.hh:736–744`), so there is
no second source of truth.

- **Open** compares the set and the version range with the binary before touching a
  segment. A plugin the binary lacks, or a format version outside what it reads, fails
  the open with a typed `unsupported_index` naming the plugin or version. A segment
  whose format version lies outside the manifest's range fails the open as corruption. A plugin the binary
  has and the manifest lacks is fine: its sections are absent and its structure flag
  is clear.
- **`install`** and checkpoint transfer check the same set before copying, so a node
  never accepts a checkpoint it cannot read (a Track D8 rolling-upgrade rule).
- **At the segment,** L7's `unsupported_segment` stays as defense, distinct from
  `corrupt_segment`, so `heal` and `media_test` never report a healthy file as
  damaged.
- **Checks** (step 7c): a test opening an index whose manifest names an unknown plugin;
  one installing such a checkpoint; one where the manifest's highest format version is
  above the binary's; one where the manifest's summary disagrees with a segment it
  names, which fails the open as corruption; and one where a binary with a plugin the
  index does not need writes and checkpoints that index, whose set stays without the
  plugin, so a binary lacking the plugin opens and installs it and both digest it equally
  (R14).

## Alternatives (L17)

| Option | Per query | Per document at flush and merge | Typed parts in the buffer | Plugin set | Verdict |
|---|---|---|---|---|---|
| A. Compile-time layout (this design) | Nothing added | Inlined calls | Yes, as base classes | Fixed per binary | Recommended |
| B. Virtual readers, dispatch once per segment | One virtual call per segment at setup, then a downcast | One virtual call per plugin per document | No: parts need type erasure, an allocation or a variant per part per document | Chosen at runtime | Rejected |
| C. `std::function` registry | As B, plus no inlining | As B, plus indirect calls | No | Runtime | Rejected |
| D. One segment class, files reorganized, no interface | Nothing added | Nothing added | n/a | None | Fails D1: the core still knows search |
| E. One file per plugin (Lucene's per-format files) | Nothing added | Nothing added | n/a | n/a | Rejected: more files per commit, sync, and reclaim; the single segment file stays |

B is the strongest alternative. Its query cost is negligible: one indirect call per
segment per query. What decides against it is the write side: a runtime registry has no
type for a document's parts, so the buffer holds them type-erased, which costs an
allocation per part per document on the indexing path that Phase 3 is ratcheting, and
moves stage-1 validation behind indirect calls. B's one advantage, choosing plugins at
runtime, is what structure flags already give within one layout (L12). A also keeps
the format's invariants (section kinds disjoint, dependencies present) checked by the
compiler.

A's costs: the core becomes a template (L13); errors in plugin code surface as template
diagnostics, which the `section_plugin` concept shortens; adding a plugin changes the
layout, so it is a code change, not a configuration.

## Risks

- **File order drift.** A misplaced section changes bytes without failing any reader.
  Step 0's fixtures catch it in format 12; the sink alone decides order (L2).
- **Digest drift.** A field moved between the core and a plugin changes the `D` record.
  Step 0's digest value catches it on every step.
- **A per-block cost in verification.** Handles resolved at construction (L7) keep
  today's arithmetic; step 2's A/B on the term and phrase classes proves it.
- **Dangling references on move.** No reader holds a sibling (L3) and shared state is
  on the heap (L7); the `static_assert` and the ASan test pin it.
- **Hidden coupling in `stats` and lengths.** The manifest and live-docs files carry
  total and live lengths; L9 names the provider so the core's dependence on a search
  quantity is one declared capability, not scattered calls.
- **Format 12's `keys` → `inverted` dependency** (L3) is the one exception to "plugins
  never touch each other". It is typed, passed at call time, and deleted at format 13.
- **Parallel flush.** Builders are per run and own their state; a plugin with static
  mutable state would race. The concept cannot check it; review does, and
  `multisegment_test`, which flushes with four threads (`multisegment_test.cc:120`),
  runs under ThreadSanitizer at steps 3, 4, and 5.
- **Merge copy paths** (P17, I7) are where correctness risk concentrates; L6 states
  their standard.
- **Compile time** (L13), measured per step.
