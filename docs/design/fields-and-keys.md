> Status: DRAFT (2026-10-27) designing (ready for preliminary review). Supersedes amendment A-KP
> (`xapiand-on-strata.md:193-197`, strata `53d93fe`) and F8's key-prefix rules (below).

# Fields and keys

Revision 0. Reads strata main `53d93fe`, prospect main `d8274b7` (P3, P4 landed) and branches
`phase4/values-bytes` `8a1b148`, `phase4/term-lists` `44e81ab`, Xapiand master `5b865a059` and
branches `phase7a/strata-engine-{write,search,callbacks,text}`. Designer: the fields-and-keys
workstream (ledger `docs/status/fields-and-keys.md`).

The owner's direction (2026-10-27): "If the engine itself knows about fields, that's fine too
(probably even better): Xapian's mode of concatenating fields and terms is ambiguous at best.
Figure it out correctly." No `Q`, no reserved `0xff` byte, no reserved `_id.` prefix.

Clean room: Xapiand's own MIT files were read (`schema.cc`, `utils.cc`, `query_dsl.cc`,
`search_views.cc`, `src/engine/`). Lucene (Apache-2.0, `~/.scratch/parity-src/lucene` at
`9983b7c`) and Tantivy (MIT, `~/.scratch/parity-src/tantivy` at `9e63fc5`) sources were read for
their models; no code is ported, so no notice is owed; a builder that ports any must add the
notice. Nothing of Xapian's source was opened.

## Summary

1. **Fields are first-class.** A term is (field id, bytes). Field ids are dense `u32`s that a
   replicated, append-only field table assigns per index; term bytes are any bytes. Nothing in
   the term space is reserved, so no term can be mistaken for another field's or for a key
   (D1, D2).
2. **One dictionary per field** inside the one `terms` section, with a field directory carrying
   per-field statistics, the layout Lucene and Tantivy use. Measured with the real `termdict`
   builder: 7.9% smaller than Xapiand's concatenated terms on a 100k-document Wikipedia corpus
   and 13.1% smaller on Xapiand's own bank dataset, because each field keeps its own alphabet
   (D3, T1). The cost is a fixed per-field overhead, 78% larger on an adversarial 2,000-sparse-field
   corpus, which the one-block form below cuts to 14% before the directory.
3. **Keys are the core's own dimension, with no prefix.** `key_prefix` leaves the manifest, the
   contexts, stage 1, the builders, `check()` and the planner. One field may be declared the key
   field; a term query on it is a key leaf over `keys` (D6, D7, D8).
4. **Scoring reads per-field statistics.** The Lucene preset becomes Lucene's exact BM25 (per-field
   document count and average length, one norm byte per document per field); the Xapian preset
   keeps the whole document's length, which is what Xapian scores (D4, D5).
5. **Order.** The Xapiand seam gains fields on the Xapian engine now, with no strata dependency
   (X6). strata adds the field table (F1, the first step), prospect moves to per-field terms and the
   key field (F2), strata deletes `key_prefix` (F3), and Xapiand's strata adapter moves onto fields
   (SF) before the 7a gate. P1 and P2 land as they are; S1 to S4 keep building on their pins (D13).

```
 X6 seam fields (Xapian engine) ----------------------------------+
 F1 field table (strata) -> F2 per-field terms, key field (prospect) -> F3 drop key_prefix
                                                          \       |
 S1 .. S4 (flat terms, their pins) -------------------------+---> SF strata adapter on fields -> S5 gate 7a
```

## What this depends on and changes

| Document | Relation |
|---|---|
| `format-13.md` F8 (`:524-726`) | Changes: the key prefix and every rule keyed on it go (D8): "the key prefix is the index's", "an empty prefix turns the rules off" (F13-R33), "every key starts with it" (F13-R25 2), "terms and keys are disjoint by construction" (F13-R2, F13-R10), routing by prefix and multi-term expansion over both readers (F13-R2, SYS-9, AUT-R4's refinement). Kept: the `keys`/`key_index` bytes, the reader interface, `scan` from a lower bound, bounded scans, the codec candidates and `keysim`, "a keyless index refuses a key" (F13-R39) |
| `format-13.md` F7 (`:497-522`), F17 (`:1027-1042`), F20 (`:1202-1203`), F21 | F7's statistics gain a field directory (D3, D4); F17's entries name a field (D10); F20 loses the key prefix and the slot reference becomes the field table's (D2); `inverted`'s digest records name a field (D9) |
| `format-13.md` F13-R11, F13-R38 | `merge_options::key_prefix`, kept as the inverted builder's backstop, is deleted: there is nothing to back stop (D8) |
| `format-14-kv.md` | Unchanged. The document layout has keys and no fields; D1a's "an index with a key prefix accepts an add without one" reads "a keyed index" |
| prospect `phase-4.md` D3 (`:97-128`) | The slot table becomes the field table's value half (D11): one replicated table of declarations per index, as Lucene's `FieldInfos`. Declarations, append-only rules, stage-1 rejections and per-segment copies are D3's, applied to fields |
| prospect `phase-4.md` D12 (`:383-390`) | "Queries on the key prefix route to `keys`" becomes "queries on the key field" (D7) |
| `xapiand-on-strata.md` D5 (`:178-191`), A-T5, A-KP, T5 (`:314`) | A-KP is superseded (D8). D5 holds: `replace_by_key` carries the key, now its bytes rather than the id term. T5's condition becomes moot on strata (no prefix) and stays a Xapian-engine census row; A-T5 stays, it fixes ranges over numeric ids on both engines |
| `xapiand-on-strata.md` section 3 (seam) and D6 | `engine::Document` and `engine::Query` name fields (D12); the Xapian implementation concatenates exactly as today |

## 1. The problem, from the code

**Xapiand's terms are concatenations.** A term is the field's prefix, a type character, then the
value (`prefixed`, Xapiand `src/database/utils.cc:49-55`); a field's prefix is its name and a dot
(`utils.cc:83-90`, with `XAPIAND_HASHED_PREFIXES` defined nowhere), and a nested field appends
its own (`schema.cc:5858-5866`). Global terms have the empty prefix, the id field `Q`
(`utils.h:61`). Two distinct terms can therefore be the same bytes (constructed from the code,
not observed in the suite):

```
field a, keyword "x.Ky"            -> "a." + 'K' + "x.Ky"  = "a.Kx.Ky"
field a.Kx (object a, child Kx), keyword "y" -> "a.Kx." + 'K' + "y" = "a.Kx.Ky"
global keyword "foo.Kbar"          -> ""   + 'K' + "foo.Kbar" = "Kfoo.Kbar"
field Kfoo, keyword "bar"          -> "Kfoo." + 'K' + "bar"   = "Kfoo.Kbar"
field Quote, text "word"           -> "Quote.Sword", which leads with the key prefix Q (A-KP)
```

The same overlap leaks into queries: `field: "*"` is a wildcard over the field's prefix
(`query_dsl.cc:633, 667`), which also matches every nested field's terms, and an edit-distance
query protects the field by counting prefix bytes (`query_dsl.cc:738`,
`prefix().size() + 1 + fuzzy_prefix_length`).

**strata inherited the key half of it.** A keyed index stores a key prefix in its manifest
(`include/strata/core/files.hh:103, 250, 295`); stage 1 refuses a key without it
(`index.hh:3810`, `reader.hh:244-256`); `inverted` refuses a term with it (prospect
`reader.hh:180`, `writer.hh:413`, `check` at `reader.hh:1626-1630`); the planner routes any term
with it to `keys` (prospect `searcher.hh:104-124, 134-136, 180`; `shards.hh:89`). The S1 adapter
works around the collision by leading every key with `0xff` and also indexing the id term as an
ordinary term (Xapiand `phase7a/strata-engine-search:src/engine/strata/shard.cc:61-68, 134, 358`),
so every id is stored twice.

**What the others do.** Lucene's `Term` is a field name and bytes, compared field first
(`index/Term.java:42, 52, 163-167`); each field has its own terms index and statistics (number of
terms, `sumTotalTermFreq`, `sumDocFreq`, `docCount`, min and max term,
`codecs/lucene103/blocktree/Lucene103BlockTreeTermsReader.java:155-180`); BM25 reads the field's
`docCount` and average length (`search/similarities/BM25Similarity.java:107-108, 142`;
`IndexSearcher.java:1146-1160`); `updateDocument(Term, doc)` deletes by a term of any field
(`IndexWriter.java:1857-1859`), and Elasticsearch's `_id` is such a field. Tantivy's term is a
`u32` field id and bytes (`src/schema/term.rs:23-55`), with one term dictionary, postings and
positions file per field (`src/index/segment_reader.rs:222-262`) and BM25 over the field's token
count (`src/query/bm25.rs:18-39`). Neither reserves bytes in the term space.

## 2. Evidence: dictionary bytes by layout

`docs/design/fields-and-keys/terms.py` turns a corpus into the same terms under three layouts,
and `dictprobe.cc` builds each with the `termdict` builder prospect uses, with the alphabet its
keys use (as prospect's inverted builder computes it, `writer.hh:187-192`), and 4-byte values.
Layouts: **flat**, Xapiand's concatenation (`path.` + type + value); **fid**, one dictionary keyed
by an order-preserving varint field id and the value; **field**, one dictionary per field, each
with its own alphabet; **field1**, as field, but a field whose terms fit one block (at most 64)
keeps no alphabet and no index, its directory entry locating the block. Raw output with conditions:
`docs/design/fields-and-keys/probe.txt` (development Mac, i9-9980HK, under a light hold; find
times are informational, the host was shared). Rerun:

```sh
D=docs/design/fields-and-keys; mkdir -p .scratch/{wiki,bank,sparse}
python3 -I $D/terms.py wiki corpus-100000.json .scratch/wiki     # search-benchmark-game corpus
python3 -I $D/terms.py bank ~/code/Kronuz/Xapiand/docs/public/assets/accounts.ndjson .scratch/bank
python3 -I $D/terms.py sparse - .scratch/sparse
clang++ -std=c++20 -O2 -DNDEBUG -I ../../termdict -I ../../fst -I ../../varint $D/dictprobe.cc -o .scratch/dictprobe
for c in wiki bank sparse; do quiet light fields 10 "dictionary probe" -- .scratch/dictprobe .scratch/$c 4 200000; done
```

| Corpus (terms, fields) | flat | fid | field | field1 | Per-field index overhead |
|---|---:|---:|---:|---:|---:|
| Wikipedia 100k: `text`, `sort_field` with six accuracy fields (769,289; 8) | 8,429,422 | 8,149,287 (−3.3%) | 7,765,012 (**−7.9%**) | 7,764,930 | none visible |
| Xapiand bank, `accounts.ndjson` (11,906; 39) | 152,639 | 145,831 (−4.5%) | 132,689 (**−13.1%**) | 131,863 (−13.6%) | 55 B a field |
| Adversarial: 1,000 docs, 20 of 2,000 keyword fields each (13,148; 2,000) | 118,786 | 131,834 (+11.0%) | 211,318 (+77.9%) | 135,635 (+14.2%) | 49 B a field |

Without values the gaps widen (Wikipedia −12.5%, bank −18.8%, field against flat). What decides:

1. **Alphabets.** One numeric field puts binary bytes into the dictionary, so a shared dictionary
   codes every text term at 8 bits a byte; a per-field dictionary codes the text field with its
   own few dozen symbols. This is why fid, though shorter than flat, loses to field.
2. **Per-field cost.** A per-field dictionary pays an alphabet and an FST, about 49 to 55 B. It
   vanishes at 8 fields and 769k terms and dominates at 2,000 fields of 6.6 terms each; field1
   removes it for one-block fields, leaving the directory (T1 bounds both).
3. **Lookups.** Flat keys are longer, and flat was the slowest in all three corpora (926 against
   767 ns on Wikipedia, 771 against 489 on bank); with a shared host these are not measurements,
   and no decision rests on them.

## 3. Decisions

**D1. A term is (field, bytes).** `field_id` is a `u32` (Tantivy's width, above Lucene's signed
`int`; every value valid, so the cap's mechanism is the width). Term bytes are 0 to
`max_term_bytes` bytes (`format.hh:99`); the empty term is valid, so a field's marker term (Xapiand's
`exists` term for a null value) needs no invented byte. Terms order by field, then bytes. No field
id and no byte is reserved: the key is not a term (D6). Alternatives: (a) flat terms with the key
moved out, rejected: the field/value ambiguity of section 1 stays the host's problem and its wildcard
and fuzzy queries keep crossing fields, there are no per-field statistics for relevance (D4), and
the shared alphabet costs 3% to 8% (section 2); (b) Tantivy's JSON-path terms (one field, the path
inside the term after a separator byte), rejected: a separator is a reserved byte.

**D2. The field table.** One table per index, owned by the core like phase-4 D3's slot table, in a
generation file `fields-<gen>.kv` that the manifest references in place of `slots` (`files.hh:104`).
It changes only through a new transaction operation, `declare_field`, so declarations are
replicated, ordered with documents, replayed and installed like them. A declaration holds:

- the name: host bytes, 1 to `max_term_bytes`, unique in the index, matched only exactly (the
  engine gives names no structure and never matches a prefix of one);
- the role: `indexed` or `key` (D7);
- for `indexed`: what postings hold (`docs`, `freqs`, `positions`, Lucene's `IndexOptions`) and
  whether the field has norms;
- the value half (D11), absent until typed slots arrive.

The id is the table's size at the declaration, so every replica assigns the same id. Declarations
only append; an identical redeclaration is a no-op that answers the existing id; a different one is
a deterministic rejection naming the field. In stage 1, a document part naming an undeclared field
rejects, as phase-4 D3 rejects an undeclared slot; a declaration earlier in the same transaction
counts, as for slots. The open index keeps a name-to-id hash map, about the names' bytes plus 32 B a
field (estimate).

**D3. One dictionary per field in `terms`, a field directory in `term_index`.** `terms` holds the
fields' `termdict` blocks one field after another, in id order; `term_index` holds F7's statistics,
then the directory, then each field's `termdict` index (alphabet and FST). A directory entry names
the field id, flags (freqs, positions, norms, one block), D4's statistics, and where the field's
blocks and index lie. A field whose terms fit one block keeps no index (field1). The directory is
pinned, read at open; a field's index is a region verified on its first use, as `lengths` is today
(F7), so an open costs the directory, not every field's FST. Directory encoding (fixed width,
searched in place, against LEB-coded and decoded at open) is step F2's choice under T1 and T6.
Seek: the directory, by binary search on the id, then the field's FST, smaller than a shared one.
Merge: fields merge in id order, each a k-way merge of its dictionaries, the comparisons a shared
dictionary would make minus the shared field bytes (an estimate; T2 measures).

**D4. Per-field statistics.** Each directory entry holds the field's term count, its document count
(documents with at least one of its terms, Lucene's `docCount`), the sum of its terms' document
frequencies, and with `freqs` the sum of their within-document frequencies (the field's total
length); with norms, its minimum and maximum length. F7's statistics stay as the document-level
figures (total, minimum and maximum document length, maximum positions). A snapshot's per-field
statistics are the sums over its segments, deleted documents included until a merge drops them, as
D13 (phase-4) does for the index's; the term table computes them per query field, once.

**D5. Norms per field; the document's length stays.** `norms` holds one byte per document per field
with norms, in directory order, the byte Lucene's preset reads today for the whole document
(prospect `reader.hh:271-278`); the builder writes a field's norms dense in step F2, and a sparse
form is a later per-field flag measured on Xapiand's corpus. `lengths` keeps each document's exact
total, the sum of its fields' lengths, which is Xapian's document length. Scoring:

| Scheme | N | Average length | A document's length |
|---|---|---|---|
| `bm25` (Lucene's preset) | the field's document count | field total / field document count | the field's norm byte |
| `bm25_xapian` | the index's document count | index total / document count | the document's exact total (`lengths`) |

For a corpus where every document has the one scored field, both rows give today's numbers, so the
parity and benchmark rows hold unchanged (T3, T4). A BM25F query (Lucene's combined fields) reads the
same per-field norms and statistics; it is a later query node, not a format change.

**D6. Keys are the core's dimension, with no prefix.** `core_part::key` (`reader.hh:89-91`) already
carries the key apart from terms. An index is keyed or keyless by its configured `keys` bit (F20),
set at creation by `index_options::keyed`, which replaces `key_prefix` (`index.hh:412, 4459-4462`).
A key is 1 to `max_term_bytes` bytes, any bytes. Unchanged: at most one live document per key
(stage 1 rejects an add whose key is live, `index.hh:3830`), versions from `versions` with the
expected-version check, `lookup_key`, `scan` from a lower bound, and a keyless index refusing a key
(F13-R39). Xapiand's key is its id's serialized value, without `Q` and without a type character.

**D7. The key field.** At most one field is declared with role `key`, and only in a keyed index. It
holds no postings: a document's key travels in `core_part::key`, and a term entry naming the key
field rejects in stage 1 (a rule on the field's declared role, not on bytes). The planner routes a
term on the key field to the `keys` reader as F8's key leaf (one ordinal, deleted copies included,
wdf 0, no positions, upper bound 0); an expansion (wildcard, prefix, edit distance) on it runs over
`keys` through `scan`, and on any other field over that field's dictionary only, so F8's rule for a
pattern spanning both readers and AUT-R4's dead-prefix refinement go. A keyed index without a key
field still has keys for `get`, replace and delete by key; they are just not queryable by term.
Alternative: a key node in the query language instead of a key field, rejected: every host query
on its id field (Xapiand's `_id`, Elasticsearch's `ids` query) would need a special case, while a
field routes by an integer comparison.

**D8. What disappears.** strata: `manifest::key_prefix` and its bytes (`files.hh:103, 250, 295`), the
corruption check against the keys bit (`index.hh:4665`), `index_options::key_prefix` and its open
check (`index.hh:412, 4587-4588`), `validate_context::key_prefix` (`reader.hh:68`), the prefix test
in `keys_plugin::key_of` (`reader.hh:249-253`) and in stage 1 (`index.hh:3810`), `merge_options` and
`builder_settings`' prefix (`merge.hh:126, 213, 298, 448`; `writer.hh:139, 462, 561`;
`index.hh:2742-2754, 2966, 4085, 4150`), `check_segment`'s (`check.hh:64, 74`;
`index.hh:945, 957, 1387, 3067`), the view's (`index.hh:1180, 1192, 2587`). prospect: the
validate refusal (`reader.hh:180-185`), the builder's (`writer.hh:97, 102, 413`), the check's
(`reader.hh:499, 1626-1630`), the routing (`searcher.hh:108-124, 134-136, 180, 193`;
`shards.hh:89`), the merge's and inverter's copies (`merge.hh:29, 107`; `inverter.hh:41`).
Xapiand: `key_lead` and `term_key_prefix()` in the strata adapter, and the duplicate id term.
Format-13 rules: F13-R2's routing and refusal, F13-R10, F13-R25 items 1 and 2, F13-R33, the
backstop of F13-R11 and F13-R38, F8's "seam's condition".

**D9. Queries and the planner.** A term leaf is `term(field, bytes)`; wildcard, prefix and
edit-distance nodes take one field and a pattern over its bytes (Xapiand's prefix-length arithmetic,
`query_dsl.cc:738`, becomes the plain `fuzzy_prefix_length`); phrase and near take terms of one
field, since positions are per field and document (Lucene's rule); a mixed-field phrase is an
invalid query. A field node matches the documents with any term in the field (Xapiand's `field:
"*"`), run in F2 as an expansion over the field's dictionary; a presence structure can replace it
later. `term_table` (`searcher.hh:131-194`) is keyed by (field, bytes) and carries per-field
statistics (D4); `snapshot_df` and the shard set's `df` take a field. P3's operators are
field-agnostic; SYNONYM and MAX weigh their subterms with their field's statistics under `bm25`.
`inverted`'s digest records add the field id before the term.

**D10. Term lists name the field.** F17's record keeps P2's shape (`[count][shared, suffix length,
suffix, wdf]`) over entries that are each term preceded by its field id as an order-preserving,
prefix-free varint (one byte below 240, else a length byte and the big-endian id). This is a
structural encoding inside a record, decoded before the term, so it reserves nothing in the term
space; front coding shares the field byte between a field's entries. `prefixed(prefix)` becomes
`prefixed(field, prefix)`. Grouping a record by field (a field count, then per field its id and
terms) is the alternative; step F2 picks the smaller on the bank corpus.

**D11. Values join the field table, later.** phase-4 D3's declarations (type, multiplicity, doc values,
points) become the value half of a field declaration, so a field can hold postings and a doc value as
in Lucene, and the dense ids phase-4 D3 plans for Xapiand are field ids. For 7a nothing changes: by
Q2 (a) values are opaque bytes under the host's 32-bit slot, and P1 builds exactly that. Xapiand's
slot is `xxh32` of the same prefix and type character that name its field (`utils.cc:59-67`,
`schema.cc:4838, 5053, 9514`), so when the value half arrives with typed slots (a later step with
its own bump), a slot becomes the value of the field of that name and the hash, with its collision
risk (`phase-4.md:119-127`), goes.

**D12. Xapiand's mapping.** An engine field is a Xapiand (prefix, type character) pair, and its name
is exactly the Xapian prefix it stands for: `name.` + `S` for a text field, `contact.city.` + `K`,
`<186a0>.` under the field's prefix for an accuracy level, the bare prefix for the null marker (with
the empty term), the type character alone for a global field, `Z` + prefix for stemmed terms. The
Xapian implementation concatenates name and term, so every Xapian term is today's byte for byte and
gate 0.5's parity carries over; strata resolves names to ids. `_id` is the key field. Stemmed terms
need one additive change in `Kronuz/analysis`: `term_generator::index_text` reports whether a term
is a `Z` stem instead of folding `Z` into its bytes (`analysis.hh:356-374`; Xapiand
`generate_terms.cc:540-556` passes the prefix), so Xapiand sends each to its field. The seam:

```cpp
class Document {
  FieldRef field(std::string name, FieldKind kind);   // document-local; kinds: text (positions,
                                                      // norms), terms (docs only), key
  void add_term(FieldRef, std::string_view term, termcount wdf = 1);
  void add_boolean_term(FieldRef, std::string_view term);
  void add_posting(FieldRef, std::string_view term, termpos, termcount wdf = 1);
  void set_key(std::string key);                      // Xapian: adds the Q term itself
  void add_value(valueno slot, std::string value);    // unchanged until typed slots (D11)
};
static Query Query::term(std::string field, std::string term, termcount wqf = 1, termpos pos = 0);
static Query Query::wildcard(std::string field, std::string pattern, ...);  // edit_distance alike
static Query Query::field(std::string field);         // any term in the field
```

A strata shard declares a document's new fields in the document's transaction, so a field and the
first document using it commit together. The WAL payload (D10 of the seam) carries field names, not
ids, so replay re-declares (a no-op when the table has them) and never depends on an id assigned by
a lost commit. Xapiand's `field: "*"` over an object field, which matches nested fields today by
prefix, becomes an OR of `Query::field` over the schema's fields under that path, built from
Xapiand's schema, not by the engine. Sites to migrate (grep at `5b865a059`, outside `src/xapian`
and `src/engine`): `prefixed(` 122 in 7 files, `Query::term(` 34, `Query::wildcard(` 6,
`Query::edit_distance(` 1, `add_term(` 38, `add_boolean_term(` 3, `add_posting(` 2.

**D13. Order.** Additive first, so every main stays green: F1 adds the table to strata with
`key_prefix` still present; F2 moves prospect to fields and stops reading the prefix; F3 deletes it
from strata. X6 changes only Xapiand and the Xapian engine. SF moves the strata adapter onto F2's
prospect and needs X6; it lands before S5.

## 4. Format changes

All pre-release (`format.hh:73-79`): F2 takes the next `pre_freeze` value after P1's and P2's
(`0x8000000B`, `0x8000000C` on their branches), F1 and F3 bump the manifest's version.

| Where | Change | Step |
|---|---|---|
| Manifest | `slots` becomes `fields` (the table's generation file); `key_prefix` removed (F3) | F1, F3 |
| `fields-<gen>.kv` | New: [count LEB][per field: name (LEB length, bytes), role u8, postings u8, flags u8][CRC32C], ids implicit by position | F1 |
| `terms` | Per-field block runs in id order | F2 |
| `term_index` | F7's 20 bytes, then the field directory, then per-field `termdict` indexes (none for a one-block field) | F2 |
| `norms` | One byte per document per field with norms, in directory order | F2 |
| `lengths`, `postings`, `positions`, `keys`, `key_index`, `versions`, `stored`, `values` | Unchanged bytes | |
| `term_lists` | Entries carry the field id's varint before the term (D10) | F2 |
| Digest | `inverted`'s records add the field id; the table's records join the core's | F1, F2 |

Every decoder change is fuzzed (`fuzz_segment`, the table's own target) and gets the decoder-safety
review before landing: the directory's offsets and sizes against the sections, ids strictly
increasing, flags against the declared options, a one-block field's block within `terms`.

## 5. API changes

**strata core.** `index_options::keyed` replaces `key_prefix`; `operation::declare_field{name,
options}` and its verdicts; `index::fields()`, a snapshot view of the table (name to id, id to
declaration); `validate_context` carries the table at the operation's position (the hook
`reader.hh:66` already names for the slot table); `keys_plugin::key_of` checks only a nonempty key
within the limit in a keyed index.

**prospect.** `inverted_plugin::part` becomes a list of field parts, each with a field id, its
`terms`, `text`/`tokens`/`split_text` and its length (`reader.hh:160-169`); the document's length is
their sum. The reader answers `find_term(field, bytes)`, `scan_dictionary(field, from, f)`
(`reader.hh:342, 376`) and `field_statistics(field)`; `statistics` (`search.hh:51-57`) gains the
per-field figures and `weight::bm25` takes them (`search.hh:72-80`); the term table, `snapshot_df`,
`shards.hh`'s global statistics and the expansions take a field; the key field's id comes from the
table, not a string. The inverter tokenizes one field's text into one field part.

**Xapiand.** Section D12's seam, in both engines.

## 6. Work in flight

| Work | Verdict |
|---|---|
| P1 values (`phase4/values-bytes`) | Stays and lands as is (D11) |
| P2 term lists (`phase4/term-lists`) | Lands as is; F2 changes its entries (D10) and re-pins its fixtures |
| P3 operators, P4 collectors (prospect main) | Stay; F2 passes a field through `term_table` and `shards.hh` (D9) |
| S1 write | Keeps its pin and `0xff` adapter until SF, which replaces `engine_key`, `key_lead` and the duplicate id term with `set_key` and the key field |
| S2 search, S3 callbacks | Keep their pins; SF maps term and expansion nodes to fields. S3's dictionary expansions become per-field |
| S4 text | Host-side `queryparser` emits (field, term) once X6 lands; its parity is unchanged on the Xapian engine |
| A-T5 (accuracy terms under `A`) | Stays; on strata they become fields of their own anyway |

Whether S1 to S4 land behind the strata engine option before SF, with A-KP's adapter, is Q1.

## 7. First buildable steps

**F1, the field table (strata, `phase4/field-table`).** Nothing blocks it. Done when:

1. `core/fields.hh`: the table (declarations, name-to-id map, id-to-declaration), its encode and
   decode with CRC, its generation file, and the manifest's `fields` reference replacing `slots`
   with the manifest's version bumped; open reads it, `check()` verifies it, the digest covers it.
2. `operation::declare_field` in transactions: stage-1 rules of D2 and D7 (name length, a known role
   and postings kind, at most one key field and only in a keyed index, a conflicting redeclaration
   rejected with the field named, an identical one answering the id); apply assigns the next id;
   `validate_context` sees declarations earlier in the same transaction.
3. Tests: a property test of random declaration sequences against a model; replay, `install` and
   a checkpoint round trip; two replicas applying the same log reach equal tables and digests;
   crash points around a commit that declares; the decoder fuzzed; a revert check per fix and a
   dying mutant per rule; ASan with UBSan and TSan in separate builds; CI green.
4. `docs/mechanisms/field-table.md`. `key_prefix` and segments untouched.

**X6, seam fields on the Xapian engine (Xapiand, `phase0.5/seam-fields`).** Nothing blocks it.
Done when D12's seam is in place on the Xapian engine, every site of D12's counts migrated, the
Xapian terms are byte-identical (the census's term shapes per document equal before and after), and
gate 0.5's T3 and T4 pass unchanged.

**F2** (prospect, `phase4/fields`) follows F1 with D3, D4, D5, D7, D9, D10, T1 to T4 and T6; **F3**
(strata) follows F2; **SF** (Xapiand) follows F2 and X6.

## 8. Gate rows

Functional rows run on the branch; size and time rows are the lead's, on `main`.

| Row | Target | Procedure |
|---|---|---|
| **T1** dictionary bytes | `terms` + `term_index` + `norms` per field, against the same index built flat: at most 0.92× on Wikipedia 100k and 0.87× on bank (the probe's 0.921 and 0.869, rounded down), at most 1.25× on the sparse corpus | Index each corpus of `terms.py` (`wiki`, `bank`, `sparse`) through `prospect_engine index` with fields (a field-aware ingest is F2's tooling), then `prospect_engine sections`; the flat baseline is the same terms concatenated into one field |
| **T2** no regression | On the single-field benchmark (search-benchmark-game corpus, one `text` field, keyed): index bytes within 0.1% of main, indexing and every query class within the noise band, merge time within the noise band | prospect `bench/index_trials.sh` and `bench/query_trials.sh` against the parent, interleaved, under an exclusive hold |
| **T3** Xapian parity | `tools/parity` passes unchanged: counts and top-10 weights at relative error at most 1e-9, 1 and 50 segments, reordered | prospect `tools/parity.cc`: `parity CORPUS N RANK_JSONL [SEGMENTS]` as today |
| **T4** Lucene parity, fields | On bank with two text fields scored separately and together, top-10 documents and scores equal Lucene 10.4's BM25 at relative error at most 1e-6 | A Lucene harness under `bench/engines/lucene-10.4.0-st` dumps rank vectors for the query set (F2's tooling); `parity` grows a Lucene mode |
| **T5** keys | A keyed index: `replace_key`, `delete_key`, expected versions and `get` behave as today for arbitrary key bytes (leading `0x00`, `0xff`, `Q`, a term's bytes); a term on the key field finds exactly the live key; a wildcard on it lists keys in order; no other field ever reaches `keys` | strata and prospect tests, with the key bytes drawn from terms of other fields |
| **T6** open cost | A segment's open reads the directory and no field's FST; resident bytes per open segment grow by at most the directory | prospect `test/format13_test.cc`'s protected-pages open (format 13's T7) over a 40-field segment; strata `bench/idle_index` before and after |
| **T7** Xapiand E2E | Gate 0.5's T3 and T4 on the Xapian engine after X6; T6 to T9 of 7a on strata after SF, with `_id` as the key field and no id term in `inverted` | As in `xapiand-on-strata.md` section 5 |

## 9. Open questions for the owner

**Q1. May S1 to S4 land behind the strata engine option with A-KP's `0xff` adapter, before SF?**
Options: (a) land them as they finish; SF deletes the adapter before the 7a gate; (b) hold them on
their branches until SF. Recommendation: (a). The adapter lives in one file
(`src/engine/strata/shard.cc:61-68, 134`), the strata engine is off unless an index asks (seam D8),
no data lives in strata indexes, and holding four branches costs daily rebases; the 7a gate (S5)
never runs on the adapter. Does not block F1 or X6.

**Q2. Does the key field's name live in the engine?** Options: (a) a declared field with role
`key` (D7); (b) a `key` query node with no field. Recommendation: (a), as D7 argues: a host's id
queries need no special case, and the routing is one integer comparison.

## 10. Risks and what is not covered

- **Many sparse fields.** A dynamic schema with thousands of rarely used fields pays a directory
  entry per field per segment (T1's sparse row, T6). Elasticsearch caps fields at 1,000 by default
  for the same reason; strata does not cap, and the step measures before choosing the encoding.
- **Cross-field queries in Xapiand.** Any query that relied on prefixes spanning fields must be
  rebuilt from the schema (D12). The census counts wildcard nodes, not their prefixes; X6's
  byte-identity check on the Xapian engine and T7 catch a missed one.
- **Phrases across prefixes.** Xapian positions are per document, so a phrase of terms from two
  prefixes can match there and is invalid here (D9). Nothing in Xapiand builds one that was found;
  T7 is the check.
- **Not measured.** Merge time, seek time and open cost of the layouts on the real builder: the
  probe measured dictionary bytes only; T2 and T6 measure the rest on `main`.
