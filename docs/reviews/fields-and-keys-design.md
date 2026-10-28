# strata review: fields and keys (preliminary design review)

- Date: 2026-10-27.
- Reviewed: `docs/design/fields-and-keys.md` revision 0, its probe (`docs/design/fields-and-keys/{terms.py,dictprobe.cc,probe.txt}`) and ledger `docs/status/fields-and-keys.md`, on `design/fields-and-keys` at strata:eef97f2 (no commits beyond it; `.scratch/` untracked).
- Checked against: format-13 F8 and F13-R2 (`format-13.md`), format-14 (`format-14-kv.md`), `xapiand-on-strata.md` D5, A-T5, A-KP and section 5's rows, prospect `phase-4.md` D3 (`:97-128`), `docs/reviews/calibration.md`; the code at strata:53d93fe (`include/strata/core/index.hh`), prospect:d8274b7 (`include/prospect/{reader,search,shards}.hh`), termdict:cdb8042, Xapiand master 5b865a059 and branches `phase7a/strata-engine-{write,search,callbacks,text}` (heads Xapiand:d29bbb7c7, Xapiand:e4e29fb16, Xapiand:54b3ae01d, Xapiand:256f48fb6). Owner direction: fields first-class, no reserved prefix, byte or id.
- Checked by: the design's `dictprobe` rebuilt from `dictprobe.cc` (Apple clang, `-O2`) and rerun on its sparse and bank inputs under `quiet light fields-arch`: every byte count in `probe.txt` reproduces exactly. A new probe, `.scratch/arch/dirprobe.py`, reuses `terms.py`'s `walk`/`emit` to size what the design's probe leaves out: the field directory (fixed-width and LEB) and per-field norms, on all three corpora. Code reads for every risk below. Probes kept: `.scratch/arch/dirprobe.py`, `.scratch/arch/build/dictprobe`.
  - Not run: benchmarks, fuzzing, any strata, prospect or Xapiand build or test (nothing is implemented yet), any Lucene harness. The one `clang++` compile of the probe ran before I took a hold (seconds, single job); the probe runs were under the hold. No Xapian source was opened.

## Verdict

**APPROVE WITH CONDITIONS: (field, bytes) terms, per-field dictionaries and the key as the core's own dimension are the right shape and remove every reserved byte; F1 needs FK-R1 folded into D2 first, F2 needs revision 1 for FK-R2 to FK-R5, and X6 may start now. Q1 (a) is safe under C1 to C4.**

What holds, and reproduces:

- The ambiguity is real and comes from the code: `prefixed` concatenates (Xapiand `utils.cc:49-55`), so the collisions in section 1 follow by construction.
- The dictionary bytes reproduce exactly: flat 152,639 and field 132,689 on bank, flat 118,786, field 211,318 and field1 135,635 on sparse (`probe.txt`, my rerun).
- Nothing in the term space is reserved. The key travels in `core_part::key`; the key field is a role in the table, so the check is on the declared role, not on bytes (D7). D10's varint is structural inside a term-list record, not a term byte.
- Key costs are unchanged or lower: uniqueness (`index.hh:3830`), the version map, `lookup_key` and `scan` are untouched, and `key_of` loses its prefix test (D6, D8). T5 draws keys from other fields' terms, the adversarial case.
- Ordering within one index is sound. `apply` runs one transaction at a time at a log position, and stage 1 runs inside it (`index.hh:664-668`, `writer_scope`), so "the id is the table's size at the declaration" yields the same ids on every replica that applies the same log. `install` replaces the whole content from a checkpoint's manifest (`index.hh:1411-1420`), so the table travels with the segments.
- Merges across segments written under different table sizes are well defined: ids are index-global, append-only and never reused, so a merge is a k-way merge by id, and a segment written earlier simply lacks the later ids.
- Q2: (a) is right, for D7's reason.

What blocks: FK-R1 (F1), FK-R2, FK-R3, FK-R4, FK-R5 (F2).

## Findings

**FK-R1. How a document part names a field it declares is undefined; predicted ids can bind terms to the wrong field. Blocks F1.** D2 assigns ids at apply ("the table's size at the declaration"), and section 5 has `inverted_plugin::part` carry a field id. A transaction that declares `y` and indexes terms in `y` must therefore know `y`'s id before the log orders it. A host that predicts it from `index::fields()` (size + k) is right only while nothing else declares in between. Two transactions built against the same table, each declaring a different new field, both predict the same id, say 5. The first applies and `x` takes 5. The second declares `y`, which takes 6, but its parts still say 5. Stage 1 accepts them, because 5 is declared by then, so `y`'s terms land in `x`. Nothing rejects this; it is silent misattribution. 7a escapes only because it runs at rf = 1 behind Xapiand's own WAL and one writer per shard (`xapiand-on-strata.md:6`). The table is a core mechanism that Phase 3R's replicated log will carry. A transaction rejected after its declarations also invalidates any id a host cached; the undo log rolls the table back (`index.hh:730`), but nothing tells the host. Fix, in D2:
1. Parts name fields by a transaction-local reference: an index into the transaction's declarations, or into a field-name list the transaction carries. Stage 1 resolves each to an id, declaring as needed, so no host ever predicts an id.
2. Or, if parts keep ids, `declare_field` carries the expected id, and stage 1 rejects a mismatch with a deterministic verdict. The `expected_schema`/`schema_conflict` pair already does this for schemas (`index.hh:72, 78, 3767`).

Recommendation: (1). It matches D12's name-carrying WAL and leaves no cache to keep coherent. Add to F1's criteria: a property test that builds transactions against stale tables (pipelined declarations of distinct and identical names), and a rejected transaction whose declarations roll back while the next one reuses the ids.

**FK-R2. T1's targets come from a probe that leaves out two things T1 counts. Blocks F2.** T1 counts `terms + term_index + norms`, and D3 puts the field directory in `term_index`. The probe counts dictionary blocks and FSTs only: no directory, no norms (`dictprobe.cc:95-103`; the ledger says "the field directory not counted"). Sizing both with `.scratch/arch/dirprobe.py`, one entry per field present (id, flags, term count, document count, sum df, sum ttf, min and max length, blocks size, index size):

| Corpus | field1 | + directory, LEB (B/field) | ratio to flat | + directory, fixed (57 B/field) | ratio | T1 target |
|---|---:|---:|---:|---:|---:|---:|
| sparse (2,000 fields) | 135,635 | 153,635 (9.0) | 1.293 | 249,635 | 2.10 | 1.25 |
| bank (39 fields) | 131,863 | 132,393 (13.6) | 0.867 | 134,086 | 0.878 | 0.87 |

Norms make bank worse. A dense byte per document per text field is 7,000 B for bank's seven text fields, against flat's single 1,000 B, so the bank ratio rises to about 0.91 with norms counted. Sparse fails its target under every directory encoding, and bank passes only with an LEB directory and no per-field norms. Neither combination is chosen in D3 or D5 ("step F2's choice"). Fix: before F2, extend the probe with the directory encoding and the norms policy F2 will build (FK-R3), and set T1's three targets from those numbers. The calibration log's row 3 applies: a target with no raw result behind it is a finding.

**FK-R3. The sparse-field cost has no bound in the design. Blocks F2.** The question was whether 2,000 fields is realistic for Xapiand. It is low. D12 maps one engine field per Xapiand (prefix, type) pair, which multiplies JSON fields:
1. A numeric field becomes 7 engine fields (the value plus six accuracy levels).
2. A date becomes as many fields as it has accuracy levels.
3. A geo field becomes one field per HTM accuracy level.
4. Each stemmed text field adds its `Z` field.
5. Each `exists` marker adds a field.
6. Namespaces add one field per partial path.

About 300 numeric JSON paths, which is ordinary for event or log documents, give 2,100 engine fields, and Elasticsearch caps fields at 1,000 because users reach it (the design's own risk note). The probe also understates bank. `terms.py` skips every `_`-prefixed key (`terms.py:59`), so bank's `checkin._point` geo terms, the namespace's partial paths, `Z` stems and global terms are absent; its 39 fields are not what Xapiand would declare. The design names the costs but bounds none of them:
1. Directory entries per segment. The design never says an entry exists only for fields present in the segment.
2. The directory format. Fixed width can be searched in place but costs 57 B a field. LEB costs 9 to 14 B but is decoded at open, which is heap per open segment and counts against T6.
3. Norms. They are dense, one byte per document per normed field (D5). Under `bm25_xapian` nobody reads them: Xapiand's scorer uses `lengths`. Yet D12 gives every `text` kind norms. On sparse text fields, dense norms grow as documents times fields.
4. One-block fields. field1 trades the index for a 256-symbol alphabet; the blocks grow from 113,117 to 135,635 B on sparse.

Fix: revision 1 states the mechanism that bounds each of these:
1. Directory entries only for fields present in the segment.
2. A directory that is compact and searchable without a full decode, for example LEB runs under a sampled fixed-width index, as termdict's restarts are.
3. Norms only where the index's scorer reads them: Xapiand under `bm25_xapian` declares none, and a sparse norms form exists before a dynamic-schema index enables them.
4. A measured choice for small fields. The probe's own `fid` layout is one candidate: fields below a term threshold pooled in one dictionary keyed by the varint id, structural as D10's is, at +11.0% on sparse before any directory. The other is field1.

T1's sparse row and T6 then measure the chosen bounds.

**FK-R4. The Xapian preset is exact on single-field corpora only; docs-only fields drop Xapiand's wdf. Blocks F2 and SF.** Xapian's document length is the sum of every term's wdf, and its BM25 reads each term's wdf. Xapiand's within-document frequencies are not all 1:
- Field terms carry the field's per-value `weight` as wdf (`schema.cc:5305-5310`), and 0 only for `bool_term` fields.
- Accuracy and global terms are `add_term` with wdf 1 (`generate_terms.cc:228, 355-356`).
- Repeated values in an array add up.

D12's `terms` kind is "docs only", which stores no frequencies. D4 defines a field's total length only "with `freqs`", and D5 makes the document's length "the sum of its fields' lengths". Under that mapping, both a keyword-term score and a document's `lengths` entry can drop the host's wdf. That breaks `bm25_xapian` against Xapian on multi-field documents. T3 cannot see it, because `tools/parity` is single-field. 7a's T7 would record the difference as a "justified" body difference (`xapiand-on-strata.md`, T7). Fix:
1. D5 defines a field part's length as the host-supplied sum of its terms' wdf, independent of the postings options. `part.length` is host-supplied today (`reader.hh:168`), so this is a definition, not a mechanism.
2. D12 maps every field without `bool_term` to `freqs`.
3. A new row, T3b, compares `bm25_xapian` with Xapian at a relative error of at most 1e-9 on bank, as Xapiand indexes it (accuracy, global and `Z` terms; values with weights), over 1 and several segments.

**FK-R5. Field ids differ across the shards of one Xapiand index; cross-shard statistics must key by name. Blocks F2.** `shards.hh` searches "the shards of one Xapiand index, each a prospect index" as one database and sums statistics across them (`shards.hh:1-5, 89`). Each shard is its own strata index with its own table. Ids follow each shard's declaration order, so the same id names different fields on different shards. D9 and section 5 say the shard set's `df` and the global statistics "take a field", and the key field's id "comes from the table". If that field is an id, cross-shard df and the per-field statistics are summed across unrelated fields: wrong scores, with no error. The same holds for 7b's remote shards. Fix:
1. The query tree the shard set receives names fields by name.
2. Each shard resolves names to its own ids at plan time, so an unknown name is an empty leaf on that shard.
3. Cross-shard statistics key by name.

Add to T5 or a new row: two shards that declare the same fields in different orders return the same top-k, scores and counts as one shard holding all the documents.

**FK-R6. Q1's adapter is wider than "one file", and X6's scope assumes S1 has not landed.** The `0xff` adapter spans more than one file:
- `shard.cc:61-68, 134, 358, 487, 527, 599, 794` on S2.
- `term_key_prefix()` in `search.cc:61`.
- `pin.h`'s `key_prefix_`, which S3 threads through planning (`search.cc:252, 295, 705-706` on `phase7a/strata-engine-callbacks`).

S2 and S3 also disagree. S2 routes nothing by prefix (an empty `term_key_prefix()`), while S3 passes the index's `\xff` prefix into `term_table` and the expansions. If S1 lands first, `StrataDocument` implements the seam's `add_term`/`add_boolean_term`/`add_posting` (`document.h:51-54` on S2), so X6 must change it. That contradicts "X6 changes only Xapiand and the Xapian engine" (D13). The step in between is an interim strata mapping that concatenates the name and the term into today's flat term. It is a second flat-term site that the design never names and that only SF removes. Not blocking; see C1 to C4.

**FK-R7. field1 needs a termdict change and a precise rule.** `termdict::reader` cannot open a dictionary without its index (`termdict.hh:637-640`), and the alphabet lives in the index bytes. An indexless one-block field is therefore new termdict API (Kronuz/termdict, its own repository and review). Builder blocks close at 64 entries or about 1,024 bytes (`termdict.hh:62-65`), so "at most 64 terms" does not guarantee one block; the rule must be "the builder produced one block". The probe's field1 find times went through FSTs (`dictprobe.cc:103` builds readers for one-block fields too), so field1's lookup cost is unmeasured. Unverified: whether every sparse field of 64 or fewer terms built exactly one block. To settle it, print `index` entries per field in `dictprobe`.

**FK-R8. Segments are not self-describing, and the design says otherwise.** The "depends on" table says phase-4 D3's per-segment copies apply to fields, and D3 (phase-4) copies declarations into every segment so frozen indexes and merges decode without the table. This design's directory holds ids, flags and statistics, but no names. Either rule works; the design must pick one and count its bytes.
1. A segment decodes only with its index's table. `install` carries the table; moving a segment between indexes (shard split, add-indexes) remaps ids by name.
2. Segments carry names. At 20 B a name, that adds about 40 KB per sparse segment, which T1 then counts.

Recommendation: (1).

**FK-R9. Statistics under deletes: per-field figures inherit an existing gap; say so.** Snapshot statistics include deleted documents until a merge (`search.hh:50-51`), while Xapian's exclude them at once. This predates this design; it is not introduced here. Still, D5's "both rows give today's numbers" holds only for corpora without deletes or replacements. For F2's criteria:
1. Per-field minimum and maximum lengths carry through merges as bounds, since exact per-field lengths are not stored.
2. Per-field document count is recomputed at merge from surviving postings, with one reusable bitset per field in id order (about `maxdoc/8` bytes).
3. The write buffer's live reads expose the same per-field statistics a flushed segment would.

**FK-R10. Decoder rules owed for D10 and the empty term.** The varint must reject:
- lead bytes `0xF5` to `0xFF`;
- non-canonical forms, such as an id below 240 in the long form, or leading zero bytes.

Otherwise equal ids compare unequal and front coding breaks. The empty term becomes valid (D1), but prospect's `validate` rejects it today (`reader.hh:182`). F2 needs a test with an empty term through termdict, the FST, `term_lists` and `find_term`.

**FK-R11. The table grows without bound and costs O(F) per open index.** A generation file rewritten whole on each declaring commit writes O(F) per commit, so O(F²) over a dynamic index's life. An open index holds a name-to-id map of about the names plus 32 B a field (D2's estimate). The many-indexes workload multiplies that by every open index. Declarations never leave the table, so churning dynamic keys accumulate forever. Condition C6.

## Blocking items

- **L1 (F1):** D2 defines how parts reference fields: transaction-local references resolved in stage 1 (recommended), or an expected id with a deterministic conflict verdict. F1's criteria gain the stale-table property test and the rollback test (FK-R1).
- **L2 (F2):** The probe counts the directory, in the encoding F2 will build, and norms under the norms policy. T1's three targets are reset from those numbers (FK-R2).
- **L3 (F2):** Revision 1 names the bound for the directory, the norms and small fields, as listed in FK-R3, with a bank row built from Xapiand's real terms (X6's seam can emit them) rather than `terms.py`'s approximation (FK-R3).
- **L4 (F2, SF):** D5 defines a field part's length as the host's wdf sum; D12 maps non-`bool_term` fields to `freqs`; row T3b is added (FK-R4).
- **L5 (F2):** Fields cross the shard set by name, and cross-shard statistics key by name, with the two-shard declaration-order test (FK-R5).

## Conditions

- **C1 (Q1 (a)):** S1 to S4 may land behind the engine option before SF only if gate 7a's strata and prospect pins include F3. F3 deletes `index_options::key_prefix`, so the adapter cannot compile on the gate's pins. That is the mechanical guarantee that `0xff` never reaches 7a.
- **C2:** `xapiand-on-strata.md`'s S5 entry criteria gain a script check: `git grep -nE 'key_lead|engine_key|term_key_prefix|key_prefix' -- src/engine/strata` is empty, and T7's "no id term in `inverted`" is run, not inferred.
- **C3:** Between S1's landing and SF, no new caller of the adapter lands. The grep count at S1's merge is recorded in the ledger and may only fall.
- **C4:** X6 names its interim mapping in `StrataDocument` (FK-R6), deleted by SF. If S1 has not landed when X6 lands, the S branches rebase onto X6 with that mapping.
- **C5:** FK-R7 to FK-R10 enter F2's done criteria: termdict's indexless reader is reviewed in its own repository; the one-block rule is stated; the varint and empty-term decoder tests exist; the segment rule from FK-R8 is chosen; the statistics criteria from FK-R9 are added.
- **C6:** F1 or F2 adds a row for the field table: bytes written and per-open-index memory after declaring 2,000 and 100,000 fields one per commit. The table's generation encoding is chosen against that row, for example delta generations compacted occasionally (FK-R11).

Not run: benchmarks, fuzzing, sanitizers, any build or test of strata, prospect or Xapiand, any Lucene harness. Hosts other than the development Mac were not touched.
