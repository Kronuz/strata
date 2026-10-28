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

## Re-check 1

- Date: 2026-10-27. Scope: FK-R1 to FK-R5, the lead's rulings (Q1, Q2) and FK-R6, against `docs/design/fields-and-keys.md` revision 1 on `design/fields-and-keys` at strata:42b004f (`git diff 20bd77b..HEAD -- docs/`, nothing else read for design).
- Checked by: the designer's whole probe rerun end to end into a separate directory (`.scratch/arch/recheck/run.sh`, a copy of `run-r1.sh` with the scratch path changed) under `quiet light fields-recheck`: Xapiand `7fcbf0000` indexing bank, sparse-kw and sparse-num, `xapian-delve` dumps, `fieldprobe` built against termdict cdb8042. The dump's own assertion (each shard's wdf sum equals its total length) passed. The architect's `dirprobe.py` rerun on `terms.py`'s corpora. Code reads: prospect:d8274b7 `shards.hh` and `searcher.hh:199`, the `phase7a/strata-engine-{search,callbacks}` adapter lines the design cites, one `xapian-delve` term-list dump. No Xapian source was opened. Probes kept: `.scratch/arch/recheck/` (run.sh, run.log, out/, the shards.hh copy).
- Not run: benchmarks, any strata, prospect or Xapiand build or test (nothing is implemented), fuzzing, sanitizers, any Lucene harness, the Wikipedia key-file build (the 0.921 was recomputed from the raw sizes in `probe-r1.txt`, not rebuilt).

### Verdicts

| Finding | Verdict |
|---|---|
| FK-R1 | **CLOSED** (design). One suggestion for F1's tests below. |
| FK-R2 | **CLOSED** |
| FK-R3 | **CLOSED** |
| FK-R4 | **CLOSED**, with a procedure gap in T3b (new FK-R12) |
| FK-R5 | **CLOSED**, with an incomplete change list (new FK-R13) |
| FK-R6 | **CLOSED** (folded into SF; Q1 and Q2 recorded) |

**FK-R1, closed.** `fields-and-keys.md:219-246` (D2): new fields are named `declared(i)`, an index into the transaction's own `fields`; stage 1 resolves each by name, so no host predicts an id. Known ids travel as `id(n)` under `expected_fields = {size, chain}`; stage 1 rejects `fields_conflict` unless the table holds at least `size` declarations and its chain at `size` matches (`:234-237`), as `expected_schema` does (`index.hh:72, 78`). The four races (`:248-265`):
1. Two transactions each declaring a new field: both say `declared(0)`; the second resolves `y` by name and takes the next id. My failure case from FK-R1 cannot arise, because no id exists to predict.
2. Same name twice: identical declaration answers the same id; a different one rejects `invalid`, deterministically on every replica.
3. A rejected declaring transaction: its declarations roll back, and `fields()` never shows an in-flight one (`:245-246`), so no host holds a rolled-back id.
4. Ids read before a lost tail: the chain at `size` differs after the ids are reused under other names, so the transaction rejects; a table shorter than `size` rejects too. Concurrent appends beyond `size` leave the prefix chain unchanged, so ordinary concurrency never rejects.

F1's tests (`:531-539`) would catch a wrong assignment: a property test against a model that pipelines stale-table transactions with distinct and identical new names and mixed `id`/`declared` references, asserting every resolved term lands in the field the host named; the rolled-back-then-reused case; the lost tail with a named dying mutant (chain check skipped); a revert check per fix and a dying mutant per rule. Suggestions for F1's criteria, not blocking: name the mutant "`declared(i)` resolved as the build-time table size plus `i`" (the revision-0 behaviour) as one the property test must kill; and add an out-of-range `declared(i)` to stage 1's rejections (`:526-530` lists name length, role, key-field rules and conflicts, not that). The chain's hash `H` is left to F1; 64 bits is within the trusted-cluster model.

**FK-R2, closed.** Rerun, 18 of 18 `report.txt` files (bank, sparse-kw, sparse-num; all shards and the five-shard splits) are identical to `probe-r1.txt`'s inputs after the path header line. The numbers include the directory (`fieldsize.py:117-145`, LEB entries under 20-byte samples every 16, entries only for own-dictionary or normed fields), the pooled dictionary (`fieldprobe.cc:78-88`, keys are the D10 varint plus the term, alphabet computed from the pool's own bytes) and norms (second column set of each report):

| Corpus | flat | pool < 256 (bm25_xapian) | Ratio | Rerun here |
|---|---:|---:|---:|---|
| bank, 49 fields, one segment | 147,188 | 128,723 (dicts 128,617 + directory 106) | 0.875 | yes |
| bank, five shards of 200 | 300,231 | 263,949 | 0.879 | yes |
| sparse-kw, 2,000 fields | 118,786 | 131,825 | 1.110 | yes |
| sparse-kw, five shards | 178,990 | 191,839 | 1.072 | yes |
| sparse-num, 2,100 fields | 722,643 | 676,695 | 0.936 | yes |
| sparse-num, five shards | 938,092 | 826,924 | 0.881 | yes |
| Wikipedia 100k, 8 fields (`terms.py`, not Xapiand's terms) | 8,429,422 | 7,765,092 (74-byte directory) | 0.921 | recomputed from raw sizes, not rebuilt |

With norms on every text field: bank 148,928 against flat plus one norm byte per document 148,188 (1.005, five shards 0.946), sparse-kw 1.629 and 1.745. T1 (`:573`) is each of these rounded up to the next hundredth, checked row by row; the sparse-kw 1.11 and 1.08 targets are above 1 and stated as such (`:618-623`). `dirprobe.py` on `terms.py`'s corpora reproduces 530 B and 18,000 B. The mechanism choice (pool < 256 smallest or tied on every corpus at both segment sizes) holds in the reports.

**FK-R3, closed.** Bank is now Xapiand's own: 49 engine fields in `stats.json`, with `checkin.G` and six `checkin.<x>.G` levels, `Zpersonality.S`, the `style.*` partial paths (20 text fields), and the 170,931 non-`Q`/`V` postings of which 21,121 have wdf above 1 recount exactly from `bank.terms`. The four bounds: directory entries only for fields with their own dictionary or norms (`:282-286`); a sampled LEB directory decoded 16 entries at most, which T6 (`:580`) measures; norms a declaration flag, none for Xapiand (`:319-323`, `:332`); small fields pooled by measurement, no termdict change. Nits, no action beyond a line in F2's notes: the directory's byte count does not include the pool's own blocks and index sizes (a handful of bytes), and T1 counts `terms`, `term_index` and `norms` only, so D10's field varint in `term_lists` is not in any ratio; D10 already assigns F2 to measure it.

**FK-R4, closed.** D5 (`:310-317`): a field part's length is the host's sum of its terms' wdf, independent of the postings options; the document's length is the host's total, defaulting to the parts' sum, kept exactly in `lengths`; Xapiand adds the id term's wdf 1 (so document length is the fields' sum plus 1; `V` has wdf 0). D12 (`:408-410`) maps every field with a wdf to `freqs` and only `bool_term` fields to docs-only, so a term's wdf survives. The wdf-sum assertion held on my rerun for all three corpora. T3b is defined (`:576`) with a target (1e-9), corpus, query classes and segment counts. Remaining gap, new FK-R12 below.

**FK-R5, closed.** D14 (`:449-465`): the query tree names fields; each shard resolves a name to its own id at plan time and an unknown name is an empty leaf; document frequency and per-field statistics sum by name; remote shards likewise. `shards.hh` changes are listed with lines that match prospect:d8274b7 (`shard_input` `:50`, `stats()` `:88`, `df` `:89`, `search` `:278`). T5b (`:579`) is defined: two shards declaring the same fields in different orders (and one field on one shard only), same top-k, scores at 1e-12 and counts as one shard, under both schemes, with a sum-by-id mutant that must fail it. Remaining gap, new FK-R13 below.

**FK-R6 and the rulings, closed.** SF's scope (`:559-565`) names the whole adapter, and every cited line exists on the branch tips: `shard.cc:61, 68, 134` and `search.cc:61, 540` on `phase7a/strata-engine-search`; `pin.h:51`, `search.cc:252, 295, 705-706`, `shard.cc:917` on `phase7a/strata-engine-callbacks`. The S2/S3 routing disagreement is named, and C4's interim mapping is in X6 (`:547-549`) and D13. Q1 (a) is recorded with C1 to C4 as I stated them (`:586-596`), including the C2 grep and C3's non-increasing count; Q2 is the key field with role `key` (D7, `:598`). C5 (FK-R7 to FK-R10) is in F2's criteria (`:551-556`, FK-R7 dropped because pooling needs no termdict API), and C6 is T8 and F1's criterion 4.

### New findings

**FK-R12. T3b's phrase queries cannot be built from the file it names. Low; fix before F2 closes, does not block F1, X6 or F2's start.** T3b (`:576`) has the oracle index `bank.terms` and `parity` read the same file, for "term, OR, AND and phrase queries". `xapiand_terms.py` writes `shard:doc`, wdf and term only; `xapian-delve -1 -v -r N` prints "(wdf, termfreq)" and no positions (positions need `-t TERM -r N`; checked on one bank document). A phrase test from this file has no positions to index. Fix: either drop phrase from T3b and keep it in T4 and T7, or extend the dump with positions per (document, term) for the phrase query set's terms only. Also reword "within and across fields": D9 makes a cross-field phrase invalid.

**FK-R13. D14's `shards.hh` change list omits the builder and the exhaustive path. Low; add to F2's criteria.** `scorer_builder` is `scorer_ptr(const segment&)` (`searcher.hh:199`), which names neither a shard nor a table, and it is called from both `search` (`shards.hh:278`) and `exhaustive_search::scan` (`:189-267`, the key, visitor and descending-docid path). The list names only `search`. Resolving names per shard needs the shard (or its table) passed to the builder, so the type changes and both call sites with it. T5b should also run once with `search_options::key` or a descending order, so the sum-by-id mutant dies on the exhaustive path too.

### Decision

- F1 may start now: FK-R1 is closed and F1's criteria carry the property, rollback, lost-tail and mutant tests.
- X6 may start now (it was not blocked; its scope, byte-identity check and C4 mapping are as the review asked).
- F2 may start once F1 lands; FK-R2 to FK-R5 are closed. FK-R12 and FK-R13 enter F2's done criteria with C5.
