# Layers design: the lifecycle core and section plugins

Revision 1, for the architect's review. Phase 4's D1 (`phase-4.md`, "Two layers") makes
strata two layers; the review's revision check 1 (N4) and revision check 2 (M2) set the
conditions. This document specifies the split concretely enough to implement: who owns
each piece of today's code, the plugin interface, how documents carry plugin data, how
an index type composes its plugins at compile time, the order of the refactor, its
checks, and the alternatives. Decisions are numbered L1–L16.

The refactor changes no bytes. Format 12 stays byte-identical through every step; the
format changes that the split makes possible (the `keys` section, statistics per
plugin, the key in the document record) land with format 13 and are marked as such.

## Terms

- **Core:** the lifecycle layer. It owns docids, ordinals, live docs, the version map,
  key resolution, the write buffer, flush and merge orchestration, manifests, commits,
  budgets, `apply`, `checkpoint`, `install`, and the digest's composition.
- **Plugin:** a type that owns one or more section kinds: their encoding, building at
  flush, merging, verification at open, digest records, and a reader for queries.
- **Part:** the slice of a host document that one plugin consumes (terms, a stored
  record, slot values).
- **Layout:** the compile-time list of an index type's plugins and the core's
  positioned sections, in file order.
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
| 435–453 | Section appends in file order | Core (`section_sink`), order from the layout (L2) |
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
| 116–132 | Lazily verified regions (`residency::verified_regions`) | Shared object, built by the core from the plugins' rules (L7) |
| 134, 144–172, 345–373 | Docids: `validate_docids`, `docid`, `ordinal_of` | Core |
| 135, 175–184, 377–385 | Versions | `keys` |
| 136, 188–192, 388–402 | Key filter | `keys` |
| 139–140, 285–338 | Term dictionary, `term`, `scan_terms`, `scan_dictionary` | `inverted` |
| 195, 263–281 | `norm`, `total_length`, `average_length`, `length` | `inverted` |
| 199–204, 336–337 | Postings and positions verification and bytes | `inverted` |
| 209–249 | `stored` | `stored` |
| 254–261 | `verify_all` | Core loop over plugins |
| 437–1186 | `postings_iterator`, `positions_reader` | `inverted` |

### `index.hh`

| Lines | What | Owner |
|---|---|---|
| 37–46 | `term_entry`, `document` | Composed document (L10) |
| 48–72 | `operation`, `transaction`, `verdict` | Core, generic over the document type |
| 155–182 | `index_options` | Core (`key_prefix` moves to the `keys` plugin's options in format 13) |
| 184–688 | `index`: create, open, apply, refresh, merge, commit, reclaim, heal, checkpoint, install | Core |
| 513–583 | `digest` | Core composes; records from plugins (L8) |
| 587–592 | `check` | Core loop over plugins |
| 683–687, 903–926 | `find_key`, `locate_docid`, `lookup_key` | Core semantics; `lookup_key`'s segment probe (917–923) calls the `keys` reader (L3) |
| 711–716 | `doc_bytes` | Core sum over plugin parts (L10) |
| 891–899 | `estimate_bytes` | Core sum over plugin parts |
| 1026–1037 | `key_of` | `keys` plugin (format 12: reads the terms part, L3) |
| 1040–1050 | `valid` | Core sum over plugin validators |
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
| `files.hh` (manifest, live docs, metadata) | Core |
| `merge_policy.hh`, `budget.hh`, `faultfs.hh` | Core |
| `searcher.hh` 19–29 (`segment_snapshot`), 108–137 (`search`, `search_count`) | Core view; generic over the segment type (L11) |
| `searcher.hh` 84–100 (`snapshot_statistics`, `snapshot_df`) | `inverted` |
| `search.hh`, `scorer.hh`, `exact.hh` | `inverted`'s query side; scorers take its reader (L11) |
| `check.hh` | Core loop; each plugin's `check` |
| `ordered.hh` | Shared (used by aggregations, not by either layer's lifecycle) |

## The layout (L2)

An index type is `basic_index<Layout>`. A layout lists plugins and the core's
positioned sections in file order:

```cpp
template <class... Entries> struct layout;

// The core's sections that sit between plugins. The header is always first, stats
// always last, so neither is listed; docids is placed where format 12 puts it.
struct docids_section {};

using search_layout   = layout<inverted_plugin, docids_section, keys_plugin, stored_plugin>;
using document_layout = layout<docids_section, keys_plugin, stored_plugin>;  // format 13 (L12)

using index    = basic_index<search_layout>;
using segment  = basic_segment<search_layout>;
using document = basic_document<search_layout>;
```

`search_layout` reproduces format 12's file order exactly: header; `postings`,
`positions`, `terms`, `term_index`, `lengths`, `norms`; `docids`; `versions`,
`pk_filter`; `stored`; `stats` (`writer.hh:357–453`). The layout is checked at compile
time: `docids_section` appears once; no two plugins claim a section kind; every
plugin's declared dependencies (L3) appear in the layout.

The plugin order in the layout is also the digest's field order (L8), so it is frozen
with format 13: later plugins append, never reorder. Phase 4's `values`, `points`, and
`term_lists` plugins append to `search_layout` in that order.

A plugin derives from `plugin_base<Derived>` (CRTP), which supplies defaults (a merge
that rebuilds through the builder, no digest records, no statistics) and checks the
derived type against the `section_plugin` concept (L4) in a `static_assert`.

## Keys: semantics in the core, encodings in the plugin (L3)

N4's split, as code:

- **The core owns** the version map (`index.hh:1593–1594`), newest-first resolution
  (`lookup_key`, 913–926), version checks and conditional operations (1113–1126), the
  docid–key invariants, and the key's place in the buffered entry (746–751).
- **The `keys` plugin owns** the `versions` and `pk_filter` sections today and the
  `keys` section in format 13, behind this reader interface:

```cpp
template <class R>
concept key_reader = requires(const R& r, std::string_view key, ordinal_t ord, live_bits live) {
	{ r.may_hold(key) } -> std::same_as<bool>;                       // filter and range; no false negatives
	{ r.find(key, live) } -> std::same_as<std::optional<ordinal_t>>; // the live ordinal holding key
	{ r.version(ord) } -> std::same_as<std::uint64_t>;               // 0 for a keyless segment
	{ r.scan(key, [](std::string_view, ordinal_t) { return true; }) }; // keys from `key` in order
};
```

`lookup_key` becomes: the version map, then for each segment newest first,
`keys.may_hold(key)` and `keys.find(key, live)`, then `keys.version(ord)`. The digest's
one-live-copy-per-key check and the merge's key collection (`merge.hh:41–54`) use
`scan`.

**Format 12's dependency.** In format 12 the key is a term: `key_of` scans the
document's terms for the prefix (`index.hh:1026–1037`), and `find` probes the term
dictionary and walks the key term's postings (`index.hh:918–923`). The format-12 `keys`
plugin therefore declares `using depends_on = type_list<inverted_plugin>`: its reader
holds a reference to the `inverted` reader, and its `key_of` reads the terms part. The
dependency is compile-time and is the only one between plugins. Format 13 removes it:
the key is a core field of the document (`document::key`), the `keys` section holds key
→ ordinal, and the `inverted` plugin no longer indexes the key. Format 13 rejects format
12 (`phase-4.md`, "Compatibility"), so the `keys` plugin never reads a format-12
segment after that.

A layout without `keys_plugin` is keyless: the core compiles out the version map with
`if constexpr`, and an operation that names a key or an expected version rejects as
invalid in stage 1.

## The plugin interface (L4)

```cpp
template <class P>
concept section_plugin = requires {
	typename P::part;     // what a host document carries for this plugin (L10)
	typename P::builder;  // builds the plugin's sections at flush (L5)
	typename P::reader;   // an open segment's view of its sections (L7)
	{ P::rules } -> std::convertible_to<std::span<const section_rule>>;  // sections it owns
	{ P::structure } -> std::convertible_to<std::uint32_t>;  // its structure flag bit (format 13)
} && requires(const typename P::part& part, digest_record& rec) {
	{ P::validate(part) } -> std::same_as<bool>;          // stage-1 checks (index.hh:1040–1050)
	{ P::bytes(part) } -> std::same_as<std::uint64_t>;    // buffer memory (index.hh:711–716)
	{ P::disk_estimate(part) } -> std::same_as<std::uint64_t>;  // storage admission (891–899)
	{ P::document_fields(part, rec) };                    // its fields of the D record (L8)
} && requires(const merge_context& m, std::span<const typename P::reader* const> in, section_sink& sink) {
	{ P::merge(m, in, sink) } -> std::same_as<built>;     // L6
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
	flush_charge& memory;           // the run's draw on the indexing budget
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
order between its own sections. The `inverted` builder is today's term table and
posting buffers (`writer.hh:469–576`); its `finish` is `flush` plus `flush_from`'s
postings, positions, dictionary, lengths, and norms (295–448). The `stored` builder is
`stored_` and `stored_ends_` with `encode_stored`. The `keys` builder is `versions_`,
`key_hashes_`, and the key range with `encode_versions` and `encode_pk_filter`.

**The sink.** `section_sink` is the core's: it wraps `counting_output`, aligns each
section, runs `section_stream`, and appends the table-of-contents entry. A section
produced alongside another (positions beside postings) goes through
`sink.deferred(kind)`, a stream into a `spill`, and `sink.place(deferred)` aligns and
copies it later, as `writer.hh:365–427` does today. A plugin never writes the header,
the table of contents, or the footer, and never sees file offsets.

**Memory.** `flush_charge` wraps one `byte_budget::permit` per run (permits are not
shared across threads). A builder reports growth at its allocation points (an arena
chunk, a vector's reallocation), not per document: `memory.grow(n)`, which draws past
the capacity if it must, since a flush cannot fail for memory, and raises pressure. The
run's permit settles when its segment is published. Today the writer's memory is not
charged at all; Phase 3's D17 requires it ("the flush's sort and spill buffers"). This
is the one behavior change in the refactor and lands as its own step (step 7).

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
	std::span<const doc_fields> fields;    // [new ordinal] = docid, version, key
	bool docid_order;                      // false when the output is reordered (P1)
};

// P::merge(const merge_context&, std::span<const typename P::reader* const> inputs,
//          section_sink&) -> built
```

`inputs[i]` is the plugin's own reader for input `i`, typed, so a plugin reads its
sections directly. What each plugin does:

| Plugin | Merge |
|---|---|
| `inverted` | Today's term-major union (`merge.hh:80–145`): the alphabet from the inputs' dictionaries, each term's live postings remapped through `new_ordinal`. Later, P17: a postings block whose ordinals all stay live and shift by one constant is copied with only its base rewritten. |
| `stored` | Default (rebuild): for each new ordinal, `add` the record read from its source. Later, I7: a frame whose documents are all live and stay contiguous in `order` is copied raw, its index entry rewritten. |
| `keys` | Rebuild from `fields` (version and key per new ordinal); format 12 collects keys with `scan`. |
| `values`, `points` (Phase 4) | Rebuild columns and trees from live documents in `order` (D17); 1-D trees merge by stream. |

`plugin_base` supplies the default merge: construct the builder, and for each new
ordinal call `add(ord, fields[ord], in[src.input]->part(src.ord))`. A plugin whose
reader cannot rebuild a part (the `inverted` reader is term-major) overrides `merge`.

**Byte identity.** The rebuild paths reproduce today's merge output byte for byte;
step 5 checks it against the old `merge_segments`, kept as a test oracle until that
step lands. The copy paths (P17, I7) change merge output bytes without changing the
format; each lands later as its own measured change with T3's write amplification row,
and re-pins the merge fixture (open question 3).

## Open and verification (L7)

```cpp
struct open_context {
	std::uint32_t version;                 // the segment's format version
	std::uint64_t doc_count;
	const section_table& sections;         // find(kind): the entry, or null when absent
	const region_verifier& regions;        // verify(kind, offset, length), lazily verified sections
	const core_segment& core;              // docids, for plugins that check against them
};
// P::reader(const open_context&): throws corrupt_segment
// r.verify_all() const; r.check(check_report&, live_bits) const
```

**The core's open.** It decodes the footer and table of contents and checks the
header (`reader.hh:64–72`). For every entry it finds the claiming rule: the core's own
kinds (header, docids, stats) or a plugin's. An entry no rule claims is corruption in a
format version that knows the kind, and `unsupported_segment` when the layout lacks the
plugin that owns it (open question 2). By rule, it checks `pinned` sections' CRCs whole
(today the list at 76–82), registers `regions` sections with one shared
`verified_regions` (116–132, unchanged: one object per segment, sorted spans), and
leaves `first_use` sections (lengths, 418–422) to the plugin. Then it constructs each
reader in layout order, so a dependency (L3) is open before its dependent.

**Presence.** In format 12 every plugin of `search_layout` is required (today's open
fails without `lengths`, 111). From format 13 the footer's structure flags record what
the segment holds: a plugin whose bit is set needs all its `with_plugin` sections; one
whose bit is clear has none of them, and its reader opens empty and answers "no such
structure" (`term` returns nothing, `stored` returns empty). This is `phase-4.md`'s
"required together" and "free" rules, stated per plugin; "mandatory" is the core's.

**Format N − 1.** One plugin, two layouts, selected by `open_context::version` inside
the reader's constructor, as N4 asks. The core never translates sections. The writer
writes only the current version. A plugin that keeps an N − 1 layout keeps its decoder
and the fixture for it; when N + 1 lands, N − 1's decoder is deleted.

**Regions.** `region_verifier::verify(kind, offset, length)` takes section-relative
offsets; the core adds the section's file offset. Today's `verify_postings`,
`verify_positions`, and the stored checks (`reader.hh:199–204, 215, 229`) become
calls on it with their kind.

## The digest (L8)

The digest stays one sum of xxHash64 over records (`phase-3.md`, "Digest"). The core
composes it; plugins contribute fields and records:

- **The `D` record** is the core's: `'D'`, docid, version, then each plugin's
  `document_fields` in layout order. For `search_layout` that is `inverted`'s length
  (u32) then `stored`'s size and bytes, which is today's record exactly
  (`index.hh:524–533`). `keys` contributes no field in format 12.
- **Plugin records** come from `P::records(reader, live, emit)` over a segment and
  `P::records(docid, part, emit)` over a buffered document. `inverted` emits today's
  `P` records (534–543, 550–560, 566); Phase 4's `values` emits `V` records (D17).
- **Core records:** `L`, `S`, `M` (568–581), and in format 13 the slot-table record.
- **Invariants** stay in the core: one live copy per docid (from `D`) and per key (from
  the `keys` reader's `scan` over live ordinals, and the buffer's keys).

Records are generated from readers and buffered parts on each `digest()` call, never
stored with a segment: a record set cached per segment would go stale with every live-
docs generation, and content-derived bytes would enter the format. "Returns its digest
records" in D1 is read as "defines and enumerates them". Records depend only on docid
and content, so T2's independence of segmentation holds by construction.

**Format 13** adds the key to the core's `D` record (key length u32, key bytes), since
the key term and its `P` record leave the term dictionary (D12). The digest changes
value at format 13 and only there.

**Across instantiations.** Two layouts give different digests for the same logical
content (`document_layout` has no length field). A replication group runs one layout,
so this is never compared (open question 1).

## Statistics and lengths (L9)

The core needs a document's length in three places that are not search: the
manifest's totals (`index.hh:1295–1302`), live-docs files' live length (1257,
1321–1327), and the `D` record. A plugin that provides lengths declares
`static constexpr bool provides_length = true` and implements
`reader::length(ord)`, `reader::total_length()`, and `length(part)`; the layout allows
at most one. The core reads it through `if constexpr`; without one, every length is 0,
which the manifest's format already accepts.

The `stats` section is the core's. In format 12 it is the document count followed by
`inverted`'s 20 bytes (`writer.hh:429–434`): the format-12 core calls the provider's
`legacy_stats()` to append them and `reader.hh:106–110`'s check splits them back. In
format 13, `stats` holds only the core's fields, and each plugin's statistics live in
its own section (`inverted`'s in `lengths`' header), so a plugin's sections are
self-contained. `legacy_stats` is deleted with format 12.

## Documents, transactions, and the buffer (L10)

The document is the composition of the parts:

```cpp
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
struct keys_plugin::part {};          // format 12: the key is a term (L3)
```

`plugin_part_t` maps `docids_section` to an empty base. Inheritance keeps today's
source working: `d.terms`, `d.length`, `d.stored` (as tests and `tools/strata_engine.cc`
use them) name the same members, default construction is unchanged, and each part is a
distinct base, so the core reaches one with `static_cast<const P::part&>(doc)` and never
names a member. Phase 4's parts add `values` (slot → typed values) and points, which
are derived from values (D17).

`basic_operation<L>` and `basic_transaction<L>` are today's `operation` and
`transaction` (`index.hh:48–62`) with `document` replaced by `basic_document<L>`;
`declare_slot` (D3) is a core operation. The aliases keep today's names.

**The buffer.** `buffered` (`index.hh:746–751`) keeps `docid`, `version`, `key`,
`dead`, and the whole `basic_document<L>`, as today (`index.hh:1151` copies it in). The
core never inspects a part:

| Today | After |
|---|---|
| `valid(d)` (1040–1050) | `(P::validate(d) && ...)`, in layout order; stage 1 rejects `invalid` on the first false |
| `key_of(d, valid)` (1026–1037) | `keys_plugin::key_of(d)`; format 13 reads `d.key` |
| `doc_bytes(d)` (711–716) | `sizeof(document) + 96 + (P::bytes(d) + ...)`, the same sum as today; a `static_assert` pins `sizeof(document)` at today's value |
| `estimate_bytes(t)` (891–899) | `64 + Σ ops (16 + key and meta sizes + (P::disk_estimate(doc) + ...))`, the same sum |
| `invert` (1188–1199) | `add` on each builder for each document |
| Digest over `buffer_` (562–567) | `D` fields and `P::records` from the parts |

The sums are written so their values equal today's, which keeps flush triggers and
storage admission, and therefore every segment's contents, unchanged.

**Reads of buffered documents.** Real-time get (D13) and delete by query's buffer path
(D16) read parts directly: `stored_plugin::stored(part)`, and a per-plugin leaf
predicate for boolean matching (`inverted_plugin::matches(part, term)`,
`values_plugin::matches(part, range)`), defined with D16. Neither needs the core to know
a part's shape.

## Query-side access (L11)

```cpp
template <class L>
class basic_segment {
public:
	const core_segment& core() const noexcept;                    // docid, ordinal_of, doc_count
	template <class P> const typename P::reader& get() const noexcept;  // static_assert: P in L
	template <class P> bool holds() const noexcept;               // its structure flag
};
```

`get<P>()` returns a reference into the segment's reader tuple: a compile-time offset,
no branch, no virtual call. Search code takes the plugin's reader, not the segment:
`term_scorer`, `phrase_scorer`, `postings_iterator`, and `positions_reader` take
`const inverted_plugin::reader&` (`search.hh:219–222, 867`; `reader.hh:439, 1035`),
which carries the document count it checks ordinals against. `statistics` and
`snapshot_df` take the same reader. `segment_snapshot` (`searcher.hh:19–29`) becomes
generic over the segment type, and `search` and `search_count` (108–137) resolve hits'
docids through `core()`.

The scorer interface (`search.hh:122`) is virtual today and stays so: per-document
calls cost what they cost now. The refactor adds no dispatch on any per-posting or
per-document path; the only new step per query is `get<P>()` once per segment at
scorer setup, which compiles to an address computation. L15's measurement proves it.

## The document profile (L12)

```cpp
using document_layout = layout<docids_section, keys_plugin, stored_plugin>;
using document_index  = basic_index<document_layout>;
```

`document_index` is a sorted key-value store with versions, snapshots, and atomic
batches: `apply` with `add`, `replace_key`, `delete_key`, and conditional versions;
`find_key`; gets of the stored record by key; `keys_plugin::reader::scan` for ordered
range scans; `checkpoint`, `install`, and the digest. It links no term dictionary,
postings, scorers, or `bitpack` postings code.

It exists from format 13 only. In format 12 the key is a term, so `keys` depends on
`inverted` (L3), and format 12's open requires `lengths`; a format-12 segment without
`inverted` is invalid. Until format 13, a compile-only test instantiates
`document_layout` against a stub `keys` to keep the composition honest. With format 13,
its tests are the oracle's key-value model (`workload-profiles.md`, "What it costs")
and the gate's segment of every profile; its measurement against RocksDB is
`workload-profiles.md`'s and not this refactor's.

The runtime profiles (`search`, `document`, `graph`, `log`) are a different mechanism:
they choose which of an instantiation's plugins build sections in a given index, via
structure flags. Xapiand uses `search_layout` with runtime profiles; the compile-time
`document_layout` is for strata used as a library.

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

Each step builds and passes on every platform in CI before the next starts. Every step
keeps format 12 byte-identical and the public API source-compatible through the
aliases. The work starts after Phase 3's ratchet rows are recorded at a stated commit
(N4), and lands before Phase 3R's `strata::group` code depends on the index API, so
Phase 3R builds on `basic_index`'s interface; the group code names `strata::index`,
which keeps working whatever later layouts add.

| Step | Change | Checks beyond the full test suite |
|---|---|---|
| 0 | New fixtures pinning today's behavior: a segment with stored records across several frames (today's `small.sx` stores none, `segment_test.cc:306`), a two-segment merge's output, and the digest value of a fixed transaction script with keys, replaces, deletes, metadata, and buffered documents | They pass on the unchanged code |
| 1 | Move `output`, `spill`, `counting_output`, `section_stream` (`writer.hh:75–223`) to `section_io.hh`; add `section_sink`, `section_rule`, `region_verifier` | Moves only; fixtures |
| 2 | Open path: `core_segment` plus `inverted`, `keys`, `stored` readers; `segment` aliases `basic_segment<search_layout>` with forwarding members (`seg.term()`, `seg.stored()`, …), so callers compile unchanged | Fixtures open; `fuzz_segment` corpus replay finds the same verdicts; `heal`, `media_test` |
| 3 | Flush path: builders and the sink; `segment_writer` becomes a façade over them (the fixture test and `fuzz_segment` use it) | `small.sx` and step 0's fixtures byte-equal; flush runs (D18) byte-equal to step 0's outputs |
| 4 | `basic_document`, `basic_operation`, `basic_transaction`, `basic_index<L>`; `key_of`, `valid`, `doc_bytes`, `estimate_bytes`, and the digest through plugins | Step 0's digest value; T2's schedules; `churn_test`; manifest, live-docs, and metadata fixtures |
| 5 | Merge path through L6's interface (M2); the old `merge_segments` kept in a test as the oracle, then deleted | Merge fixture byte-equal; random merges byte-equal to the oracle; digest unchanged across merges |
| 6 | Search code on plugin readers (L11); the forwarding members removed | `verify` and `parity` on the multi-segment indexes; then L15's measurement |
| 7 | Flush memory charged to the indexing budget (L5) | Budget tests; the indexing memory row; segment bytes unchanged for unbudgeted runs |
| 8 | Format 13 work proceeds on the plugins: `keys` section, structure flags, statistics per plugin, the key in `D`, `document_layout` tests | Format 13's own fixtures and gate |

Steps 2 and 3 are the flush and open paths revision check 2 lets start; step 5 waits
for this document's acceptance of L6 (M2). Step 7 changes when budgeted indexes flush,
so it is outside the byte-identity rule and measured on its own (open question 4).

## Measurement (L15)

N4's before-and-after, run once at the end of step 6 against the recorded Phase 3
commit, on the gate VM under `tools/vm/quiet_trials.sh`:

- **Queries:** `bench/ab.sh` with variant A at the recorded commit and B at step 6,
  each serving an index built by its own binary (byte-identical, which the run checks
  by hash). Every command (`TOP_10`, `TOP_100`, `TOP_1000`, `TOP_100_COUNT`, `COUNT`)
  over both query sets (`queries`, `terms-stratified`), five trials, order alternated.
  `bench/ab.py` reports per command and class the median of B/A and its 95% bootstrap
  interval; counts must match.
- **Indexing:** both rows, single-thread and multi-thread (`STRATA_THREADS`), with
  `bench/index_trials.sh` running the two binaries as two engines in rotated order,
  five trials each, at the same `STRATA_BUDGET_MB`.
- **No regression** means every interval contains 1.0 or lies below it. A row whose
  interval lies above 1.0 blocks the merge of the refactor until it is fixed or the
  architect accepts it with its cause.
- Step 7's charge is measured the same way for the indexing rows and the indexing
  memory row.

If an earlier step touches a hot path more than expected (step 2's reader layout,
step 3's builder calls per document), the same A/B runs there too, so a regression is
attributed to its step.

## Alternatives (L16)

| Option | Per query | Per document at flush and merge | Typed parts in the buffer | Plugin set | Verdict |
|---|---|---|---|---|---|
| A. Compile-time layout (this design) | Nothing added | Inlined calls | Yes, as base classes | Fixed per binary | Recommended |
| B. Virtual readers, dispatch once per segment | One virtual call per segment at setup, then a downcast | One virtual call per plugin per document | No: parts need type erasure, an allocation or a variant per part per document | Chosen at runtime | Rejected |
| C. `std::function` registry | As B, plus no inlining | As B, plus indirect calls | No | Runtime | Rejected |
| D. One segment class, files reorganized, no interface | Nothing added | Nothing added | n/a | None | Fails D1: the core still knows search |
| E. One file per plugin (Lucene's per-format files) | Nothing added | Nothing added | n/a | n/a | Rejected: more files per commit, sync, and reclaim; format 12's single file stays |

B is the strongest alternative. Its query cost is negligible: one indirect call per
segment per query. What decides against it is the write side: a runtime registry has no
type for a document's parts, so the buffer holds them type-erased, which costs an
allocation per part per document on the indexing path that Phase 3 is ratcheting, and
moves stage-1 validation behind indirect calls. B's one advantage, choosing plugins at
runtime, is what structure flags already give within one layout (L12). A also keeps
the format's invariants (section kinds disjoint, dependencies present, file order)
checked by the compiler.

A's costs: the core becomes a template (L13); errors in plugin code surface as template
diagnostics, which the `section_plugin` concept shortens; adding a plugin changes the
layout, so it is a code change, not a configuration.

## Risks

- **File order drift.** A misplaced section changes bytes without failing any reader.
  The `small.sx` fixture and step 0's fixtures catch it; the layout lists the order in
  one place.
- **Digest drift.** A field moved between the core and a plugin changes the `D` record.
  Step 0's digest value catches it on every step.
- **Hidden coupling in `stats` and lengths.** The manifest and live-docs files carry
  total and live lengths; L9 names the provider so the core's dependence on a search
  quantity is one declared capability, not scattered calls.
- **Format 12's `keys` → `inverted` dependency** (L3) is the one exception to "plugins
  never touch each other". It is typed, checked, and deleted at format 13.
- **Parallel flush.** Builders are per run and own their state; a plugin with static
  mutable state would race. The concept cannot check it; review does, and
  `multisegment_test`, which flushes with four threads (`multisegment_test.cc:120`),
  runs under ThreadSanitizer at steps 3 and 4.
- **Merge copy paths** (P17, I7) are where correctness risk concentrates: a raw copy
  must agree with the permutation and live docs. They land after the refactor, each
  with a test against the rebuild path's decoded content and the digest.
- **Compile time** (L13), measured per step.

## Open questions for the architect

1. **Digest across layouts.** L8 makes the digest a function of the layout as well as
   the content (`document_layout` has no length field). Is that acceptable given one
   layout per replication group, or should format 13 split the `D` record so each
   plugin's per-document fields are their own record, making digests comparable across
   layouts for the content they share?
2. **A section the layout cannot read.** L7 rejects it as `unsupported_segment`,
   distinct from `corrupt_segment`, so `heal` does not report a healthy file as damaged.
   Is a new exception type the right boundary, or should opening another layout's
   index fail earlier, from a layout identifier in the manifest?
3. **Merge byte identity.** Step 5's rebuild paths are byte-identical to today's
   merge. The P17 and I7 copy paths change merge output bytes, not the format. Is
   re-pinning the merge fixture with each, checked by decoded content and digest
   equality plus T3's row, the right standard?
4. **Step 7's budget charge** changes when budgeted indexes flush, so segment layout
   under a budget changes. Is it in this refactor (measured on its own) or in Phase 3's
   remaining D17 work?
5. **`docids_section` in the layout.** It exists only to keep format 12's file order.
   Format 13 could fix docids immediately after the header and drop the marker. The
   design keeps it, since it costs nothing and keeps one layout mechanism for both
   formats; does the architect prefer the fixed position?
