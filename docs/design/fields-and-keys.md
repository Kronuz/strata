> Status: DRAFT (2026-10-27) designing (ready for preliminary review): revision 1, for the re-check of
> FK-R1 to FK-R5. Supersedes amendment A-KP (`xapiand-on-strata.md:193-197`, strata `53d93fe`) and
> F8's key-prefix rules (below).

# Fields and keys

Revision 1. Reads strata main `53d93fe`, prospect main `d8274b7` (P3, P4 landed) and branches
`phase4/values-bytes` `8a1b148`, `phase4/term-lists` `44e81ab`, termdict `cdb8042`, Xapiand master
`5b865a059` and branches `phase7a/strata-engine-{write,search,callbacks,text}`. Designer: the
fields-and-keys workstream (ledger `docs/status/fields-and-keys.md`). Review:
`docs/reviews/fields-and-keys-design.md` (approve with conditions); revision 1 closes FK-R1 to FK-R5,
folds FK-R6 into SF and FK-R7 to FK-R11 into F1's and F2's criteria, and records the owner's rulings
on Q1 and Q2 (section "Review findings folded in").

The owner's direction (2026-10-27): "If the engine itself knows about fields, that's fine too
(probably even better): Xapian's mode of concatenating fields and terms is ambiguous at best.
Figure it out correctly." No `Q`, no reserved `0xff` byte, no reserved `_id.` prefix.

Clean room: Xapiand's own MIT files were read (`schema.cc`, `utils.cc`, `query_dsl.cc`,
`search_views.cc`, `src/engine/`). Lucene (Apache-2.0, `~/.scratch/parity-src/lucene` at
`9983b7c`) and Tantivy (MIT, `~/.scratch/parity-src/tantivy` at `9e63fc5`) sources were read for
their models; no code is ported, so no notice is owed; a builder that ports any must add the
notice. Nothing of Xapian's source was opened; revision 1's term dumps use the installed
`xapian-delve` 2.1.0 binary on indexes Xapiand wrote.

## Summary

1. **Fields are first-class.** A term is (field id, bytes). Field ids are dense `u32`s that a
   replicated, append-only field table assigns per index; term bytes are any bytes. Nothing in
   the term space is reserved (D1, D2). A transaction names the fields it declares by name, so no
   host predicts an id, and the ids it reuses are checked against the table they were read from
   (D2, FK-R1).
2. **Per-field dictionaries, small fields pooled.** Inside the one `terms` section a field with at
   least 256 terms in the segment has its own dictionary and alphabet; smaller fields share one
   pooled dictionary keyed by the field id's varint. A compact directory has entries only for
   fields with a dictionary of their own or norms (D3). Measured with the real `termdict` builder on
   Xapiand's own terms (49 engine fields on bank, geo, stems and partial paths included), counting
   the directory: 12.5% smaller than flat terms on bank, 6.4% smaller on 300 sparse numeric paths
   (2,100 engine fields), 11.0% larger on 2,000 sparse one-word fields (T1, section 2).
3. **Keys are the core's own dimension, with no prefix.** `key_prefix` leaves the manifest, the
   contexts, stage 1, the builders, `check()` and the planner. One field may be declared the key
   field; a term query on it is a key leaf over `keys` (D6, D7, D8).
4. **Scoring.** `bm25_xapian` reads the document's exact length, which is the host's sum of every
   wdf (Xapiand's id term included), and needs no norms and no per-field statistics, so Xapiand
   declares none. A field declared with norms carries one norm byte per document (dense or sparse)
   and per-field statistics, and `bm25` scores it as Lucene does (D4, D5, FK-R4).
5. **Shards resolve names.** Field ids are per index, so the shard set's statistics and the query
   tree name fields; each shard resolves names to its own ids at plan time (D14, FK-R5).
6. **Order.** X6 (seam fields on the Xapian engine), F1 (field table), F2 (prospect on fields), F3
   (drop `key_prefix`), SF (Xapiand's strata adapter on fields, FK-R6's scope) before S5. S1 to S4
   may land behind the engine option before SF under C1 to C4 (Q1, ruled).

```
 X6 seam fields (Xapian engine) ----------------------------------+
 F1 field table (strata) -> F2 per-field terms, key field (prospect) -> F3 drop key_prefix
                                                          \       |
 S1 .. S4 (flat terms, their pins, C1-C4) ------------------+---> SF strata adapter on fields -> S5 gate 7a
```

## What this depends on and changes

| Document | Relation |
|---|---|
| `format-13.md` F8 (`:524-726`) | Changes: the key prefix and every rule keyed on it go (D8): "the key prefix is the index's", "an empty prefix turns the rules off" (F13-R33), "every key starts with it" (F13-R25 2), "terms and keys are disjoint by construction" (F13-R2, F13-R10), routing by prefix and multi-term expansion over both readers (F13-R2, SYS-9, AUT-R4's refinement). Kept: the `keys`/`key_index` bytes, the reader interface, `scan` from a lower bound, bounded scans, the codec candidates and `keysim`, "a keyless index refuses a key" (F13-R39) |
| `format-13.md` F7 (`:497-522`), F17 (`:1027-1042`), F20 (`:1202-1203`), F21 | F7's statistics gain a field directory (D3, D4); F17's entries name a field (D10); F20 loses the key prefix and the slot reference becomes the field table's (D2); `inverted`'s digest records name a field (D9) |
| `format-13.md` F13-R11, F13-R38 | `merge_options::key_prefix`, kept as the inverted builder's backstop, is deleted: there is nothing to back stop (D8) |
| `format-14-kv.md` | Unchanged. The document layout has keys and no fields; D1a's "an index with a key prefix accepts an add without one" reads "a keyed index" |
| prospect `phase-4.md` D3 (`:97-128`) | The slot table becomes the field table's value half (D11): one replicated table of declarations per index, as Lucene's `FieldInfos`. Declarations, append-only rules and stage-1 rejections are D3's, applied to fields. Changed: segments do not carry copies of the declarations; a segment decodes with its index's table (D3, FK-R8) |
| prospect `phase-4.md` D12 (`:383-390`) | "Queries on the key prefix route to `keys`" becomes "queries on the key field" (D7) |
| prospect `multi-index-search.md` (`shards.hh`) | Changes: the shard set sums statistics by field name, and each shard resolves names to its ids (D14) |
| `xapiand-on-strata.md` D5 (`:178-191`), A-T5, A-KP, T5 (`:314`), S5's entry criteria | A-KP is superseded (D8). D5 holds: `replace_by_key` carries the key, now its bytes rather than the id term. T5's condition becomes moot on strata (no prefix) and stays a Xapian-engine census row; A-T5 stays. S5's entry criteria gain C2's grep check (Q1) |
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

**Xapiand's real terms.** `docs/design/fields-and-keys/xapiand_terms.py` indexes a corpus with a
real Xapiand on the Xapian engine (5 shards, the default), PUTting each line at `/NAME/<n>` as the
docs suite does with bank, and dumps every document's terms and wdf with `xapian-delve`; it checks
each shard's sum of wdf against the shard's total length. `fieldsize.py split` names each term's
engine field as D12 does (the prefix through the type character; a leading `Z` keeps the stem field
apart) and leaves out `Q` (the key, which goes to `keys`) and `V` (the Xapian engine's version term,
`src/engine/xapian/shard.cc:56`; strata keeps versions in `versions`). `fieldprobe.cc` builds each
field's dictionary with the `termdict` builder prospect uses (own alphabet, 4-byte values), counts
blocks through the builder's sink, and builds the pooled dictionary for each threshold;
`fieldsize.py report` adds the directory and norms. Raw output: `probe-r1.txt` (development Mac,
under a light hold; bytes only). Rerun:

```sh
XAPIAND=~/code/Kronuz/Xapiand-wt/value-compare/build/bin/xapiand \
ACCOUNTS=~/code/Kronuz/Xapiand/docs/public/assets/accounts.ndjson \
  quiet light fields-rev 20 "fields probe" -- docs/design/fields-and-keys/run-r1.sh
```

The binary is Xapiand `7fcbf0000` (`explore/value-compare`), whose `schema.cc`, `generate_terms.cc`
and `utils.cc` equal master `5b865a059`'s. Corpora (`sparse_corpora.py`): **bank**, the 1,000
accounts; **sparse-kw**, 1,000 documents each with 20 of 2,000 one-word string fields (revision 0's
sparse); **sparse-num**, 1,000 documents each with 20 of 300 integer fields, which Xapiand indexes
with six accuracy levels each, so 300 paths become 2,100 engine fields (FK-R3's case).

What Xapiand really declares on bank: 49 engine fields, against revision 0's 39 from `terms.py`
(which skipped `_` keys and typed short strings as keywords). Every string is a text field (20);
`personality` adds its stem field `Zpersonality.S`; the `style` namespace indexes each leaf twice,
by full path and by partial path (`style.clothing.pants.S` and `style.pants.S`); `checkin._point`
becomes `checkin.G` and six HTM levels `checkin.<3>.G` to `checkin.<f>.G`; each of the three numbers
becomes seven fields. Every document has one `Q` term (wdf 1) and one `V` term (wdf 0); 21,121 of
bank's 170,931 other postings have wdf above 1.

| Corpus, one segment of 1,000 documents (fields) | flat | field | field1 | pool < 64 | **pool < 256** | pool < 1,024 |
|---|---:|---:|---:|---:|---:|---:|
| bank, Xapiand's terms (49) | 147,188 | 130,375 (0.886) | 129,469 (0.880) | 129,376 (0.879) | **128,723 (0.875)** | 132,330 (0.899) |
| sparse-kw (2,000) | 118,786 | 221,821 (1.867) | 144,138 (1.213) | 131,825 (1.110) | **131,825 (1.110)** | 131,825 (1.110) |
| sparse-num (2,100) | 722,643 | 837,531 (1.159) | 726,974 (1.006) | 732,125 (1.013) | **676,695 (0.936)** | 676,695 (0.936) |
| Five shards of 200 documents, summed | | | | | | |
| bank | 300,231 | 269,804 (0.899) | 265,053 (0.883) | 265,229 (0.883) | **263,949 (0.879)** | 275,251 (0.917) |
| sparse-kw (about 1,736 a shard) | 178,990 | 577,908 (3.229) | 257,000 (1.436) | 191,839 (1.072) | **191,839 (1.072)** | 191,839 (1.072) |
| sparse-num (2,100 a shard) | 938,092 | 1,314,314 (1.401) | 876,861 (0.935) | 826,924 (0.881) | **826,924 (0.881)** | 826,924 (0.881) |

Bytes are `terms` + `term_index` (dictionaries and the directory) under `bm25_xapian` (no norms, D5),
against the same terms built flat (Xapiand's concatenation, one dictionary). field1 is revision 0's
one-block form (a field the builder closed in one block keeps no alphabet and no index); pool < T
puts every field with fewer than T terms in the segment into one dictionary keyed by the field id's
varint (D10's form) and the value. Wikipedia 100k (revision 0's `terms.py` key files, 8 fields):
flat 8,429,422, pool < 256 7,765,092 with a 74-byte directory (computed from `fieldprobe`'s raw sizes
in `probe-r1.txt`), 0.921.

What decides:

1. **Alphabets.** One numeric field puts binary bytes into a shared dictionary, so it codes every
   text term at 8 bits a byte; a dictionary of its own codes the text field with its few dozen
   symbols. Larger fields therefore want their own dictionary: at the 1,024 threshold, bank's
   medium text fields (`name.lastName.S` with 495 terms, `contact.city.S` with 646) join the pool
   and bank grows 2.8% over pool < 256.
2. **Per-field cost.** A dictionary of its own pays an alphabet, an FST and block headers, about 49
   to 55 B (revision 0's probe).
   Pooling removes them for small fields and needs no termdict change (FK-R7): pool < 256 is the
   smallest on all three corpora at both segment sizes, and the pool's alphabet stays a full byte
   wide, which is why sparse-kw's 30-symbol flat dictionary still wins by 11%.
3. **The directory.** An entry per present field with statistics costs 8 to 14 B a field (revision 0's
   rule, my rerun: 13.5 B on bank, 8.3 B on sparse-kw; the architect's `dirprobe.py` gives 13.6 B
   and 9.0 B on `terms.py`'s corpora, reproduced in `probe-r1.txt`). That alone was +14% on
   sparse-kw. Under D3's rule a pooled field without norms has no entry: 3 B of directory on
   sparse-kw and sparse-num, 106 B on bank.

**Norms (the Lucene preset).** With norms on every text field, dense or sparse per field (sparse:
count, ordinal gaps and one byte per present document, written when smaller), against flat plus one
document norm byte: bank 148,928 against 148,188 (1.005), five shards 0.946; sparse-kw 1.629 and
1.745 (2,000 normed text fields, 46,767 B of norms where dense would be 2,000,000 B); sparse-num has no
text field (0.935, 0.881); Wikipedia about 0.922 (its one text field's dense norms equal flat's).

**Revision 0's figures** (`probe.txt`, `terms.py`'s approximation, no directory, no norms) stay as
written for the record; they are no longer T1's basis.

## 3. Decisions

**D1. A term is (field, bytes).** `field_id` is a `u32` (Tantivy's width, above Lucene's signed
`int`; every value valid, so the cap's mechanism is the width). Term bytes are 0 to
`max_term_bytes` bytes (`format.hh:99`); the empty term is valid, so a field's marker term (Xapiand's
`exists` term for a null value) needs no invented byte. Terms order by field, then bytes. No field
id and no byte is reserved: the key is not a term (D6). Alternatives: (a) flat terms with the key
moved out, rejected: the field/value ambiguity of section 1 stays the host's problem and its wildcard
and fuzzy queries keep crossing fields, there are no per-field statistics for relevance (D4), and
flat terms cost 6% to 13% more than per-field ones on bank and sparse-num (section 2); (b) Tantivy's JSON-path terms
(one field, the path inside the term after a separator byte), rejected: a separator is a reserved
byte.

**D2. The field table, and how a transaction names fields (FK-R1).** One table per index, owned by
the core like phase-4 D3's slot table, in a generation file `fields-<gen>.kv` that the manifest
references in place of `slots` (`files.hh:104`). A declaration holds:

- the name: host bytes, 1 to `max_term_bytes`, unique in the index, matched only exactly (the
  engine gives names no structure and never matches a prefix of one);
- the role: `indexed` or `key` (D7);
- for `indexed`: what postings hold (`docs`, `freqs`, `positions`, Lucene's `IndexOptions`) and
  whether the field has norms (D5);
- the value half (D11), absent until typed slots arrive.

The id is the table's size when the declaration applies, so every replica applying the same log
assigns the same ids. Declarations only append and ids are never reused, except that a rejected
transaction's declarations roll back with it (the undo log, `index.hh:730`).

A transaction (`basic_transaction`, `index.hh:69-74`) gains two members:

```cpp
std::vector<field_declaration> fields;        // the fields its parts name by declaration
std::optional<field_mark> expected_fields;    // {size, chain}: the table its ids were read from
```

and a document part names its field by a `field_ref`, either `declared(i)`, an index into the
transaction's `fields`, or `id(n)`, an id the host read from `index::fields()`. Stage 1, inside
`apply` (one transaction at a time, `index.hh:664-668`):

1. With `id` references, `expected_fields` is required. The table must hold at least `size`
   declarations and its chain at `size` must equal `chain`; otherwise the verdict is the new
   `fields_conflict`, carrying the table's current size and chain, as `expected_schema` yields
   `schema_conflict` (`index.hh:72, 78, 3766-3767`). Every `id(n)` must be below `size`.
2. Each entry of `fields`, in order, resolves by name: a declared name with an identical declaration
   answers its id; a declared name with a different one rejects the transaction `invalid` naming the
   entry; an absent name is declared, taking the next id, and logged in the undo log.
3. Parts resolve their references to ids; `validate_context` carries the resolution (strata
   `reader.hh:66-67`, where Phase 4 adds the slot table), and the buffer stores ids.

The chain is c₀ = 0, cᵢ₊₁ = H(cᵢ ‖ the encoded declaration i), a 64-bit hash held in memory per
declaration (8 B a field, recomputed at open in O(F), not stored). `index::fields()` answers the
table as of the last applied transaction with its mark, never an in-flight one.

The races, and why none misattributes a term:

1. **Two transactions built against one table each declare a new field** (revision 0's failure:
   both would predict id 5). Each names its field by `declared(0)`. The first to apply declares `x`,
   id 5; the second resolves `y` by name and declares it, id 6. No id was predicted, so none is
   wrong.
2. **Both declare the same name.** The first declares it; the second's identical declaration
   resolves to the same id, and a different declaration rejects deterministically on every replica.
3. **A declaring transaction is rejected.** Its declarations roll back and the next transaction
   reuses those ids. No host held them: `fields()` never shows an in-flight declaration, and a
   transaction's own references are `declared(i)`.
4. **Ids read before a lost tail.** A crash at rf = 1 (WAL durability is asynchronous by default) or
   a failover that truncates a log can lose applied declarations; later declarations then reuse
   their ids under other names. A host still holding marks from before sends `expected_fields =
   {6, c₆}`; the table's chain at 6 differs, so the transaction rejects `fields_conflict`. The host
   re-reads `fields()` and rebuilds. A concurrent declarer only appends beyond `size` and leaves the
   prefix's chain unchanged, so ordinary concurrency never rejects. A 64-bit chain collides with
   probability about 2⁻⁶⁴ per check, within the trusted-cluster model (honest faults only).

Alternatives: names only, revision 0's recommendation (1): correct and simplest, but every
transaction then carries every name it uses, 794 B for one bank document (49 names and their
lengths, from `stats.json`) against its 1,292 B of JSON; with the mark, a transaction carries 16 B
(estimate) plus the names of new fields only. Ids with an expected id per declaration (option 2
alone): a host must still predict the new ids and retry under every concurrent declarer. Xapiand's
own WAL keeps carrying names (D12), so its replay re-declares and never depends on an id.

**D3. Dictionaries in `terms`, a field directory in `term_index` (FK-R3, FK-R7, FK-R8).** In each
segment, a field with at least 256 terms has its own `termdict` dictionary (blocks in `terms`, its
alphabet and FST in `term_index`); every other field's terms go into one pooled dictionary keyed by
the field id's D10 varint and the term, its blocks after the fields' and its index after theirs.
The threshold is the segment's, so a field can be pooled in one segment and on its own in another;
the directory says which. Section 2 chose 256 over 16, 64, 1,024 and the one-block form (field1),
which also needed an indexless termdict reader (FK-R7); pooling needs none.

The directory follows F7's statistics in `term_index`: LEB-coded entries in id order, one for each
field present in the segment that has a dictionary of its own or norms, and no entry for a pooled
field without norms. An entry holds the id's delta from the previous entry, a flags byte (freqs,
positions, norms dense or sparse, pooled), D4's statistics when the field has norms, and the sizes
of its blocks and index when it has its own. A fixed-width sample every 16 entries (id `u32`, entry
offset, blocks offset, index offset: 20 B) indexes the entries, so a lookup binary-searches the
samples and decodes at most 16 entries; open reads nothing else and builds no map (T6). Seek: the
directory, then the field's FST, or the pool's with the id's varint before the term. A field absent
from the directory and the pool is absent from the segment. Merge: fields merge in id order, each a
k-way merge of that field's terms across the inputs' own and pooled dictionaries; the output
re-decides pooled against own by its term count.

A segment decodes only with its index's field table (FK-R8, the review's option 1): the directory
holds ids, flags and statistics, no names. `install` carries the table with the segments
(`index.hh:1411-1420`); moving a segment between indexes (a shard split, add-indexes) remaps its ids
by name, a rewrite of the directory and the pool's keys.

**D4. Per-field statistics, where a scorer reads them.** A directory entry of a field with norms
holds its document count (documents with at least one of its terms, Lucene's `docCount`), the sum of
its terms' document frequencies, the sum of its within-document frequencies (the field's total
length), and its minimum and maximum length. A field without norms has none: `bm25_xapian` reads the
index's figures and each term's own statistics, and `bm25` scores a field without norms by the
document's length (D5). F7's statistics stay as the document-level figures (total, minimum and
maximum document length, maximum positions). A snapshot's per-field statistics are the sums over its
segments, deleted documents included until a merge drops them, as phase-4 D13 does for the index's
(the gap with Xapian, which excludes them at once, predates this design, FK-R9); the term table
computes them per query field, once.

**D5. Lengths are the host's; norms by declaration (FK-R4).** A field part carries its length, the
host's sum of the wdf of its terms, independent of what the field's postings store. The document
carries its length, the host's total, which defaults to the sum of its parts' lengths; `lengths`
keeps it exactly, and it is Xapian's document length. Xapiand passes the sum of every wdf it gives
Xapian, the id term's included: every bank document's `Q` term has wdf 1 and `V` wdf 0, and each
shard's total length equals the sum of all its terms' wdf (`xapiand_terms.py`'s check), so strata's
document length is the fields' sum plus 1. `part.length` is already host-supplied
(`reader.hh:166-172`); this makes it per field and adds the document's.

Norms are a declaration flag, not implied by a kind. A field with norms has one norm byte per
document that has the field, the byte Lucene's preset reads today for the whole document (prospect
`reader.hh:271-278`), in directory order, dense (one byte per document) or sparse (count, ordinal
gaps, bytes), whichever is smaller for that field in that segment. Xapiand declares no norms: its
scorer is `bm25_xapian`. Scoring:

| Scheme | N | Average length | A document's length |
|---|---|---|---|
| `bm25`, field with norms | the field's document count | field total / field document count | the field's norm byte |
| `bm25`, field without norms | the index's document count | index total / document count | the document's exact total (`lengths`) |
| `bm25_xapian` | the index's document count | index total / document count | the document's exact total (`lengths`) |

`bm25_xapian` reads each term's wdf from its postings, so every field Xapiand scores stores `freqs`
(D12). On a corpus whose one scored field every document has, `bm25` with norms and `bm25_xapian`
give today's numbers, so T2, T3 and T4 hold unchanged; T3b checks the multi-field case against Xapian. A BM25F query (Lucene's
combined fields) reads the same per-field norms and statistics; it is a later query node, not a
format change.

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
`keys` through `scan`, and on any other field over that field's terms only, so F8's rule for a
pattern spanning both readers and AUT-R4's dead-prefix refinement go. A keyed index without a key
field still has keys for `get`, replace and delete by key; they are just not queryable by term.
Alternative: a key node in the query language instead of a key field, rejected (Q2, ruled): every
host query on its id field (Xapiand's `_id`, Elasticsearch's `ids` query) would need a special case,
while a field routes by an integer comparison.

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
Xapiand: `key_lead`, `term_key_prefix()` and `key_prefix_` in the strata adapter, and the duplicate
id term (SF). Format-13 rules: F13-R2's routing and refusal, F13-R10, F13-R25 items 1 and 2,
F13-R33, the backstop of F13-R11 and F13-R38, F8's "seam's condition".

**D9. Queries and the planner.** A term leaf is `term(field, bytes)`, the field named by name in
the query tree and resolved to the segment's id at plan time (D14); wildcard, prefix and
edit-distance nodes take one field and a pattern over its bytes (Xapiand's prefix-length arithmetic,
`query_dsl.cc:738`, becomes the plain `fuzzy_prefix_length`); phrase and near take terms of one
field, since positions are per field and document (Lucene's rule); a mixed-field phrase is an
invalid query. A field node matches the documents with any term in the field (Xapiand's `field:
"*"`), run in F2 as an expansion over the field's terms; a presence structure can replace it later.
`term_table` (`searcher.hh:131-194`) is keyed by (field, bytes) and carries per-field statistics
(D4); `snapshot_df` (`searcher.hh:120`) takes a field. P3's operators are field-agnostic; SYNONYM
and MAX weigh their subterms with their field's statistics under `bm25`. `inverted`'s digest records
add the field id before the term.

**D10. Term lists and the pool name the field by a varint (FK-R10).** F17's record keeps P2's shape
(`[count][shared, suffix length, suffix, wdf]`) over entries that are each term preceded by its field
id as an order-preserving, prefix-free varint: one byte below 240, else a lead byte `0xF0 + n`
(n of 1 to 4) and the id's n big-endian bytes. The pooled dictionary's keys (D3) use the same form.
It is structural, decoded before the term, so it reserves nothing in the term space; front coding
shares the field byte between a field's entries. The decoder rejects lead bytes `0xF5` to `0xFF`
and non-canonical forms (an id below 240 in the long form, a long form with a leading zero byte), so
equal ids always have equal bytes. `prefixed(prefix)` becomes `prefixed(field, prefix)`. Grouping a
record by field is the alternative; step F2 picks the smaller on bank's real terms.

**D11. Values join the field table, later.** phase-4 D3's declarations (type, multiplicity, doc values,
points) become the value half of a field declaration, so a field can hold postings and a doc value as
in Lucene, and the dense ids phase-4 D3 plans for Xapiand are field ids. For 7a nothing changes: by
`xapiand-on-strata.md`'s Q2 (a) (`:333, 345`) values are opaque bytes under the host's 32-bit slot, and P1 builds exactly that. Xapiand's
slot is `xxh32` of the same prefix and type character that name its field (`utils.cc:59-67`,
`schema.cc:4838, 5053, 9514`), so when the value half arrives with typed slots (a later step with
its own bump), a slot becomes the value of the field of that name and the hash, with its collision
risk (`phase-4.md:119-127`), goes.

**D12. Xapiand's mapping.** An engine field is a Xapiand (prefix, type character) pair, and its name
is exactly the Xapian prefix it stands for: `name.firstName.S` for a text field, `<186a0>.` under the
field's prefix for an accuracy level (`accountNumber.<186a0>.N`), an HTM level for a geo field
(`checkin.<a>.G`), the bare prefix for the null marker (with the empty term), the type character
alone for a global field, `Z` + prefix for stemmed terms (`Zpersonality.S`); a namespace's partial
paths are fields of their own (`style.pants.S`). `_id` is the key field; the Xapian engine's `V`
version term has no strata field. Kinds: `text` (freqs, positions), `terms` (freqs) for every
field with a wdf, and `boolean` (docs only) for `bool_term` fields, whose wdf is 0
(`schema.cc:5305-5310`); none has norms. The Xapian implementation concatenates name and term, so
every Xapian term is today's byte for byte and gate 0.5's parity carries over; strata resolves names
to ids. Stemmed terms need one additive change in `Kronuz/analysis`: `term_generator::index_text`
reports whether a term is a `Z` stem instead of folding `Z` into its bytes (`analysis.hh:356-374`;
Xapiand `generate_terms.cc:540-556` passes the prefix), so Xapiand sends each to its field. The
seam:

```cpp
class Document {
  FieldRef field(std::string name, FieldKind kind);   // document-local; kinds: text, terms,
                                                      // boolean, key
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

A strata shard's transaction names a document's new fields by `declared(i)` and the known ones by
id under the shard's mark (D2), so a field and the first document using it commit together. The WAL
payload (D10 of the seam) carries field names, not ids, so replay re-declares (a no-op when the table
has them) and never depends on an id assigned by a lost commit. Each field part's length is the sum
of the wdf Xapiand gives its terms, and the document's is their sum plus the id term's 1 (D5).
Xapiand's `field: "*"` over an object field, which matches nested fields today by prefix, becomes an
OR of `Query::field` over the schema's fields under that path, built from Xapiand's schema, not by
the engine. Sites to migrate (grep at `5b865a059`, outside `src/xapian` and `src/engine`):
`prefixed(` 122 in 7 files, `Query::term(` 34, `Query::wildcard(` 6, `Query::edit_distance(` 1,
`add_term(` 38, `add_boolean_term(` 3, `add_posting(` 2.

**D13. Order.** Additive first, so every main stays green: F1 adds the table to strata with
`key_prefix` still present; F2 moves prospect to fields and stops reading the prefix; F3 deletes it
from strata. X6 changes only Xapiand and the Xapian engine, plus the interim strata mapping C4 names
when S1 has landed (FK-R6). SF moves the strata adapter onto F2's prospect, needs X6, and lands
before S5.

**D14. Fields across shards (FK-R5).** Each shard of a Xapiand index is its own strata index with its
own table, so the same id names different fields on different shards. The shard set therefore never
sees an id:

1. The query tree names fields by name. Each shard resolves a name to its id from its snapshot's
   table at plan time; a name the shard does not know is an empty leaf on that shard.
2. Cross-shard statistics key by name. A term's document frequency is the sum over shards of the
   term's frequency under that shard's id for the name; a field's statistics (D4) sum the same way;
   the document-level figures are summed as today.
3. Remote shards (7b) exchange statistics by name for the same reason.

`shards.hh` changes: `shard_input` (`shards.hh:50-53`) gains the shard's table snapshot;
`multi_snapshot::df` (`:89`) becomes `df(field name, term)`, summing `snapshot_df` per shard over
`segments(s)` with that shard's id instead of over the flattened `all_`; a `field_stats(name)`
joins `stats()` (`:88`); the term table over a shard set is keyed by (name, bytes) with one id
column per shard; the per-shard `search` (`:278-297`) builds each shard's scorer with that shard's
ids and the set's statistics. Statistics over segments of one index keep keying by id.

## 4. Format changes

All pre-release (`format.hh:73-79`): F2 takes the next `pre_freeze` value after P1's and P2's
(`0x8000000B`, `0x8000000C` on their branches), F1 and F3 bump the manifest's version.

| Where | Change | Step |
|---|---|---|
| Manifest | `slots` becomes `fields` (the table's generation file); `key_prefix` removed (F3) | F1, F3 |
| `fields-<gen>.kv` | New: [count LEB][per field: name (LEB length, bytes), role u8, postings u8, flags u8][CRC32C], ids implicit by position; the generation encoding is chosen under T8 | F1 |
| `terms` | Own dictionaries' blocks in id order, then the pool's blocks | F2 |
| `term_index` | F7's 20 bytes, the directory (LEB entries under 20-byte samples every 16), the own dictionaries' indexes, the pool's index | F2 |
| `norms` | Per field with norms, in directory order: dense or sparse (count, LEB ordinal gaps, bytes) | F2 |
| `lengths`, `postings`, `positions`, `keys`, `key_index`, `versions`, `stored`, `values` | Unchanged bytes | |
| `term_lists` | Entries carry the field id's varint before the term (D10) | F2 |
| Digest | `inverted`'s records add the field id; the table's records join the core's | F1, F2 |

Every decoder change is fuzzed (`fuzz_segment`, the table's own target) and gets the decoder-safety
review before landing: the directory's offsets and sizes against the sections, ids strictly
increasing, flags against the declared options, samples against the entries, a pooled field's keys
within the pool, the varint's canonical form (D10).

## 5. API changes

**strata core.** `index_options::keyed` replaces `key_prefix`; `basic_transaction` gains `fields`
and `expected_fields`, and `verdict` gains `fields_conflict` with the table's mark (D2);
`index::fields()`, a snapshot view of the table with its mark (name to id, id to declaration);
`validate_context` carries the transaction's resolved references; `keys_plugin::key_of` checks only
a nonempty key within the limit in a keyed index.

**prospect.** `inverted_plugin::part` (`reader.hh:166-172`) becomes the document's length and a list
of field parts, each with a `field_ref`, its `terms`, `text`/`tokens`/`split_text` and its length.
The reader answers `find_term(field, bytes)`, `scan_dictionary(field, from, f)` (`reader.hh:342,
376`) and `field_statistics(field)`; `statistics` (`search.hh:51-57`) gains the per-field figures
and `weight::bm25` takes them (`search.hh:72-80`); the term table, `snapshot_df` and the expansions
take a field id within one index; `shards.hh` takes names (D14); the key field's id comes from the
table, not a string. The inverter tokenizes one field's text into one field part.

**Xapiand.** Section D12's seam, in both engines.

## 6. Work in flight

| Work | Verdict |
|---|---|
| P1 values (`phase4/values-bytes`) | Stays and lands as is (D11) |
| P2 term lists (`phase4/term-lists`) | Lands as is; F2 changes its entries (D10) and re-pins its fixtures |
| P3 operators, P4 collectors (prospect main) | Stay; F2 passes a field through `term_table` and `shards.hh` (D9, D14) |
| S1 write | May land behind the engine option before SF under C1 to C4 (Q1); keeps its pin and `0xff` adapter until SF |
| S2 search, S3 callbacks | As S1; SF maps term and expansion nodes to fields and reconciles their routing (FK-R6). S3's dictionary expansions become per-field |
| S4 text | Host-side `queryparser` emits (field, term) once X6 lands; its parity is unchanged on the Xapian engine |
| A-T5 (accuracy terms under `A`) | Stays; on strata they become fields of their own anyway |

## 7. First buildable steps

**F1, the field table (strata, `phase4/field-table`).** Nothing blocks it. Done when:

1. `core/fields.hh`: the table (declarations, name-to-id map, id-to-declaration, the chain), its
   encode and decode with CRC, its generation file, and the manifest's `fields` reference replacing
   `slots` with the manifest's version bumped; open reads it and recomputes the chain, `check()`
   verifies it, the digest covers it.
2. Transactions carry `fields` and `expected_fields`; stage 1 applies D2's three steps and D7's rules
   (name length, a known role and postings kind, at most one key field and only in a keyed index, a
   conflicting declaration rejected naming the entry, an identical one answering the id); the undo
   log rolls declarations back; `fields()` answers the last applied table with its mark;
   `validate_context` carries the resolution.
3. Tests (FK-R1): a property test that pipelines transactions built against stale tables, with
   distinct and identical new names and mixed `id`/`declared` references, against a model, checking
   that every resolved term's field is the one the host named; a rejected declaring transaction
   followed by one that reuses the ids; a lost tail (a declaring commit dropped, the ids redeclared
   in another order, then a transaction under the old mark) that rejects `fields_conflict` and never
   misattributes, with a mutant that skips the chain check failing it; replay, `install` and a
   checkpoint round trip; two replicas applying the same log reach equal tables, chains and digests;
   crash points around a declaring commit; the decoder fuzzed; a revert check per fix and a dying
   mutant per rule; ASan with UBSan and TSan in separate builds; CI green.
4. T8 measured, and the generation encoding chosen against it (C6).
5. `docs/mechanisms/field-table.md`. `key_prefix` and segments untouched.

**X6, seam fields on the Xapian engine (Xapiand, `phase0.5/seam-fields`).** Nothing blocks it. Done
when D12's seam is in place on the Xapian engine, every site of D12's counts migrated, the Xapian
terms are byte-identical (the census's term shapes per document equal before and after, and
`xapiand_terms.py`'s dump of bank equal before and after), and gate 0.5's T3 and T4 pass unchanged.
If S1 has landed, X6 also implements the seam in `StrataDocument` with one named interim mapping
that concatenates name and term into today's flat term, deleted by SF (C4); if it has not, the S
branches rebase onto X6 with that mapping.

**F2** (prospect, `phase4/fields`) follows F1 with D3, D4, D5, D7, D9, D10, D14, T1 to T6. Its done
criteria carry C5: the varint and empty-term decoder tests (D10; the empty term through termdict,
the FST, `term_lists` and `find_term`, today refused at `reader.hh:182`); the segment rule of D3
(FK-R8); FK-R9's statistics criteria (per-field minimum and maximum carried through merges as
bounds; per-field document count recomputed at merge from surviving postings with one reusable
bitset per field; the write buffer's live reads exposing the statistics a flushed segment would).
**F3** (strata) follows F2.

**SF** (Xapiand) follows F2 and X6. Its scope includes FK-R6's whole adapter: `key_lead`, `engine_key`
and the duplicate id term in `shard.cc` (`phase7a/strata-engine-search:src/engine/strata/shard.cc:61,
68, 134`), `term_key_prefix()` (`search.cc:61, 540`), `pin.h`'s `key_prefix_` and the planning that
threads it (`phase7a/strata-engine-callbacks:src/engine/strata/pin.h:51`, `search.cc:705-706`,
`shard.cc:917`), and C4's interim mapping; S2 and S3 disagree on routing (S2 passes an empty
`term_key_prefix()`, S3 the index's `\xff`), and SF replaces both with the key field. Done when C2's
grep is empty and T7 passes.

## 8. Gate rows

Functional rows run on the branch; size and time rows are the lead's, on `main`.

| Row | Target | Procedure |
|---|---|---|
| **T1** dictionary bytes | `terms` + `term_index` + `norms`, per-field against the same terms built flat. Under `bm25_xapian` (no norms): Wikipedia 100k at most 0.93×; bank (Xapiand's terms) 0.88× in one segment and 0.88× over five shards; sparse-kw 1.11× and 1.08×; sparse-num 0.94× and 0.89×. With norms on every text field (flat plus one document norm byte): Wikipedia 0.93×, bank 1.01× and 0.95×, sparse-kw 1.63× and 1.75×, sparse-num 0.94× and 0.89×. Each target is section 2's ratio rounded up to the next hundredth | `run-r1.sh` writes the terms (`xapiand_terms.py`, `fieldsize.py split`); F2's tooling indexes each corpus's terms through `prospect_engine index` per field and flat into one field, one segment and five, then `prospect_engine sections`. Wikipedia uses `terms.py wiki` |
| **T2** no regression | On the single-field benchmark (search-benchmark-game corpus, one `text` field declared with norms, keyed): index bytes within 0.1% of main, indexing and every query class within the noise band, merge time within the noise band | prospect `bench/index_trials.sh` and `bench/query_trials.sh` against the parent, interleaved, under an exclusive hold |
| **T3** Xapian parity | `tools/parity` passes unchanged: counts and top-10 weights at relative error at most 1e-9, 1 and 50 segments, reordered | prospect `tools/parity.cc`: `parity CORPUS N RANK_JSONL [SEGMENTS]` as today |
| **T3b** Xapian parity, fields | On bank as Xapiand indexes it (every field, accuracy, geo, stem and partial-path terms, wdf as given, the id term's wdf in the document length), `bm25_xapian` counts and top-10 weights equal Xapian's at relative error at most 1e-9, for term, OR, AND and phrase queries within and across fields, 1 and 5 segments | The oracle (`strata-oracle`) indexes `bank.terms` (`xapiand_terms.py`) as Xapian documents and records rank vectors for the query set; `parity` grows a terms mode that reads the same file, splits fields with `fieldsize.py`'s rule and indexes them as field parts with D5's lengths |
| **T4** Lucene parity, fields | On bank with two text fields declared with norms, scored separately and together, top-10 documents and scores equal Lucene 10.4's BM25 at relative error at most 1e-6 | A Lucene harness under `bench/engines/lucene-10.4.0-st` dumps rank vectors for the query set (F2's tooling); `parity` grows a Lucene mode |
| **T5** keys | A keyed index: `replace_key`, `delete_key`, expected versions and `get` behave as today for arbitrary key bytes (leading `0x00`, `0xff`, `Q`, a term's bytes); a term on the key field finds exactly the live key; a wildcard on it lists keys in order; no other field ever reaches `keys` | strata and prospect tests, with the key bytes drawn from terms of other fields |
| **T5b** shards by name | Two shards that declare the same fields in different orders (and one field only one shard has) return the same top-k documents, scores at relative error at most 1e-12, and counts as one shard holding all the documents, under `bm25` and `bm25_xapian` | prospect `test/shards_test.cc`: build both, compare per query; a mutant that sums by id fails it |
| **T6** open cost | A segment's open reads the directory's header and no field's FST and builds no per-field map; resident bytes per open segment grow by at most the directory | prospect `test/format13_test.cc`'s protected-pages open (format 13's T7) over a 49-field bank segment and a 2,100-field sparse-num segment; strata `bench/idle_index` before and after |
| **T7** Xapiand E2E | Gate 0.5's T3 and T4 on the Xapian engine after X6; T6 to T9 of 7a on strata after SF, with `_id` as the key field and no id term in `inverted` | As in `xapiand-on-strata.md` section 5 |
| **T8** field table growth | Bytes written to `fields-*` and resident table bytes per open index after declaring 2,000 and 100,000 fields one per commit, recorded; the encoding chosen keeps bytes written within 4× the final table's size (rewriting it whole on each commit writes O(F²)) | strata `bench/field_table` (F1's tooling) |

## 9. Owner rulings

**Q1, ruled (a) (lead, 2026-10-27).** S1 to S4 may land behind the strata engine option with A-KP's
`0xff` adapter before SF, under the architect's conditions:

- **C1.** Gate 7a's strata and prospect pins include F3, which deletes `index_options::key_prefix`,
  so the adapter cannot compile on the gate's pins.
- **C2.** `xapiand-on-strata.md`'s S5 entry criteria gain a script check:
  `git grep -nE 'key_lead|engine_key|term_key_prefix|key_prefix' -- src/engine/strata` is empty, and
  T7's "no id term in `inverted`" is run, not inferred.
- **C3.** Between S1's landing and SF, no new caller of the adapter lands; the grep count at S1's
  merge is recorded in the ledger and may only fall.
- **C4.** X6 names its interim mapping in `StrataDocument`, deleted by SF (section 7).

**Q2, ruled (a).** The key is a declared field with role `key` (D7).

## Review findings folded in

| Finding | Where |
|---|---|
| FK-R1 how parts name fields | D2: `declared(i)` for new fields, ids under a chained mark; the four races; F1's criteria 2 and 3 |
| FK-R2 T1 counted the directory and norms out | Section 2 measures both; T1 restated from those numbers |
| FK-R3 sparse cost unbounded | Section 2 on Xapiand's real terms (bank with `_` keys, sparse-num's 2,100 fields); D3 (entries only for fields with their own dictionary or norms, sampled LEB directory, pool < 256 chosen by measurement), D4 and D5 (statistics and norms only where read; none for Xapiand) |
| FK-R4 Xapian scoring across fields | D5 (part and document lengths are the host's; the id term's wdf), D12 (every field with a wdf stores freqs), T3b |
| FK-R5 cross-shard statistics | D14, `shards.hh`'s changes, T5b |
| FK-R6 adapter spans three files | SF's scope (section 7), C4 |
| FK-R7 field1 needs termdict API | Dropped: pooling needs no termdict change |
| FK-R8 segments not self-describing | D3: a segment decodes with its index's table; moves remap by name |
| FK-R9 statistics under deletes | D4 names the gap; F2's criteria (C5) |
| FK-R10 decoder rules | D10; F2's criteria (C5) |
| FK-R11 table growth | T8, F1's criterion 4 (C6) |

## 10. Risks and what is not covered

- **Sparse one-word fields still cost 11%.** sparse-kw's flat dictionary has a 30-symbol alphabet;
  the pool's keys carry binary ids, so its alphabet is 256 symbols (section 2). A pool coding ids
  with fewer symbols is a later termdict experiment, not this design.
- **Norms on many sparse fields.** A host that declares norms on 2,000 text fields pays 63% to 75%
  over flat on sparse-kw, mostly norms and statistics (T1's second set). Xapiand declares none;
  Lucene would pay 2,000,000 B of dense norms on that corpus.
- **Transaction size.** A transaction that declares many fields carries their names (794 B for a new
  bank schema); after the first documents, only the mark (estimate, F1 measures).
- **Cross-field queries in Xapiand.** Any query that relied on prefixes spanning fields must be
  rebuilt from the schema (D12). The census counts wildcard nodes, not their prefixes; X6's
  byte-identity check on the Xapian engine and T7 catch a missed one.
- **Phrases across prefixes.** Xapian positions are per document, so a phrase of terms from two
  prefixes can match there and is invalid here (D9). Nothing in Xapiand builds one that was found;
  T7 is the check.
- **Not measured.** Lookup, merge and open time of pooled against own dictionaries, the directory's
  decode cost, and the table's memory: the probes measured bytes only; T2, T6 and T8 measure the
  rest on `main`.

**Amendment A-X6 (lead, 2026-10-28, from X6's review CR5).** X6 keeps Xapiand's `set_key(key)`
with the host building the key term, instead of D2's `FieldKind::key` with the Xapian engine
adding the `Q` term. SF moves the shard key path to the key field (role `key`) and deletes the
host-built key term; until then the strata engine reads keys through `keys.h` (C3: no new
callers).

**Lead ruling on F2's T1 (2026-10-28).** Measured on the real builders with the directory: bank
0.861x/0.863x (target 0.88), sparse-num 0.929x/0.861x (0.94/0.89), Wikipedia 0.922x (0.93) all beat
their targets; the adversarial sparse-kw case is 1.128x/1.084x against 1.11/1.08, over by 0.018 and
0.004, because pooled keys carry a 2–3 byte field id and a full-byte alphabet. Accepted for F2: the
miss is 1.6% on a synthetic worst case and real data gains 7–14%; revisit only if a real corpus
shows it.

