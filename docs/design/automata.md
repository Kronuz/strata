# Automata: prefix, wildcard, and fuzzy term matching (Phase 5)

A multi-term query names a set of terms by a pattern: every term starting with a prefix,
every term a glob (`*`, `?`) accepts, or every term within an edit distance of a target.
strata resolves the set per segment by intersecting an automaton with the term
dictionary, merges the segments' sets into one expansion with Xapian's limits, and hands
the expansion to a combining scorer (ARCHITECTURE, "Read path", step 2). This design
builds the automata, the intersection, and the expansion. The combining scorer for
Xapian's default, `OP_SYNONYM`, is the synonym operator's own work (D9).

What it serves (FEASIBILITY, "What Xapiand uses from Xapian"): `OP_WILDCARD` and
`OP_EDIT_DISTANCE` in the query DSL; `QueryParser`'s wildcard, partial, and fuzzy flags
(the `queryparser` design maps them onto D8); and the opaque term namespace's "exists",
a wildcard on a field prefix. The benchmark class is BENCHMARKS' "Prefix, wildcard, fuzzy"
(`obam*`, `ob?ma`, `obama~2`).

Clean room: Lucene's and Tantivy's automata were reasoned about from their documentation
only (Lucene 10.4's javadoc for `LevenshteinAutomata`, `CompiledAutomaton`, `FuzzyQuery`,
`WildcardQuery`, `MultiTermQuery`'s rewrites; Tantivy 0.26's `FuzzyTermQuery` and
`AutomatonWeight` docs) and from Schulz and Mihov, "Fast string correction with
Levenshtein automata" (IJDAR 2002). Xapian's semantics come from its API documentation and,
where that is silent, from the oracle harness (O3). No Lucene, Tantivy, or Xapian source
was read.

## What exists

- `fst` (`fst.hh`): `map::intersect(automaton, f)`, a depth-first walk over every key with
  the byte-level concept `start / accept / is_match / can_match`, and `prefix_automaton`.
- `termdict` (`termdict.hh`): strata's term dictionary. Prefix-compressed blocks of 32–64
  entries with restarts every 32, and an FST **over each block's first key only**.
  Public reads: `find`, `find_run(key, visit)`, `scan(from, f)`, and `reader::cursor`.
  The index FST and block opening are private.
- strata's term values (`format.hh`, `decode_term_info`): each entry's postings and
  positions offsets follow the previous entry's unless the entry is `absolute`, which every
  restart entry is. Decoding one entry needs the run's cursor (`format::term_run`), so a
  walk decodes every value from a restart onward, as `segment::find_term` does.
- `unicode` (`unicode.hh`): `next_utf8`, the lenient decoder `analysis` uses to make
  terms: a byte that does not start a valid, shortest-form sequence decodes as itself
  (ISO-8859-1), as Xapian's `Utf8Iterator` documents.
- `string-similarity`: byte-level, case-folding, normalized metrics for ranking; no
  automaton, no code points, no transpositions. ARCHITECTURE plans that it gain Xapian's
  distance (O5).

## D1. Characters

Patterns and terms are matched as sequences of characters, each decoded by
`unicode::next_utf8`: valid UTF-8 decodes to its code point, and any other byte to the code
point of the same value. This is `analysis`'s decoding and Xapian's documented one, so `?`
and an edit count one character exactly where Xapian does.

- **Aliasing.** An invalid byte b in 0x80–0xFF decodes to U+00b, the same character as the
  valid two-byte sequence for U+00b. A pattern literal `â` (C3 A2) therefore matches a
  term's lone byte E2. That is Xapian's behavior, kept; it shows only on terms that are not
  valid UTF-8, which `analysis` never produces for text.
- **Byte ranges.** Decoding is deterministic and its characters' encodings concatenate to
  the input, so a term whose first characters equal a pattern's literal characters starts
  with their bytes, unless one of them is in U+0080–U+00FF, which two byte forms spell.
  The **literal prefix** of an automaton is therefore the bytes of its leading literal
  characters up to the first one in that range; every match starts with it, and the walk
  scans only [literal prefix, its successor).
- **Lookahead.** `next_utf8` decides a character at byte s from bytes [s, s + L), where L is
  1 below 0xC2 or above 0xF4, 2 for C2–DF, 3 for E0–EF, and 4 for F0–F4, whether or not
  the sequence turns out valid. The walk (D6) records that extent per character, so it
  knows exactly which byte prefix determined a state.

## D2. Prefix

`prefix(p)`: the terms starting with the bytes p. No decoding: it is the byte range
[p, successor(p)), where successor drops trailing 0xFF bytes and increments the last byte
(none when p is empty or all 0xFF: the range runs to the end). It is Xapian's `OP_WILDCARD`
without pattern flags, `QueryParser`'s trailing `*` and partial word, and Xapiand's
"exists". A byte prefix over terms from `analysis` equals the character prefix, since valid
UTF-8 is prefix-free; it differs only on aliased bytes, as D1 states.

## D3. Glob wildcards

`glob(pattern, flags)`: `*` matches any run of characters (`multi`) and `?` exactly one
(`single`), each live only under its flag, as Xapian's `WILDCARD_PATTERN_MULTI` and
`WILDCARD_PATTERN_SINGLE`; with neither flag the pattern is a literal. There is no escape,
as in Xapian: a live metacharacter cannot be matched literally.

- **Compilation.** The pattern decodes (D1) into elements: a literal character, `?`, or
  `*`. Consecutive `*` collapse into one, which matches the same set.
- **Simulation, not determinization.** The state is the set of pattern positions reachable
  so far, a bitset of m + 1 bits for m elements, in ⌈(m + 1) / 64⌉ words (one word up to
  63 elements). On a character c: `next = ((s & (L[c] | Q)) << 1) | (s & S)`, then
  `next |= (next & S) << 1`, where L[c] holds the positions of literal c, Q those of `?`,
  and S those of `*`; the closure is one step because stars never repeat. A match has bit
  m set. A state is dead when it is empty, and every non-empty state can still match, so
  `can_match` is exact. Each step costs O(words), whatever the pattern: a determinized glob
  can need exponentially many states (`*a??????????` needs 2^10, the classic case), which
  the simulation never builds. L[c] is a sorted array of the pattern's distinct literal
  characters, searched per character.
- **Pathological patterns** (`*a*a*a*b`, leading `*`) have no literal prefix, so the walk
  covers the whole dictionary, as in every engine; the state stays one word.

## D4. Levenshtein

`levenshtein(target, k, prefix_chars, transpositions)`: the terms within k edits of the
target, where an edit inserts, deletes, or substitutes one character, and with
`transpositions` also swaps two adjacent ones (the restricted, optimal-string-alignment
distance, as in Schulz and Mihov's transposition automaton and Lucene's `FuzzyQuery`). The
first `prefix_chars` characters of the target must match exactly, as Xapian's
`fixed_prefix_len` and Lucene's `prefixLength`; a prefix longer than the target fixes all of
it. Xapian's preset is `transpositions` on, the distance ARCHITECTURE records ("adjacent
transpositions, over code points"), to be confirmed by the oracle (O3).

- **The state** is Ukkonen's band of the dynamic program, simulated rather than compiled:
  after j characters past the fixed prefix, the row d(i, j) for target positions i in a band
  of w = min(2k + 1, n + 1) cells starting at lo(j) = clamp(j − k, 0, n + 1 − w), each clipped
  at k + 1. Every cell outside the band exceeds k (d(i, j) ≥ |i − j|), so the band holds
  every live cell. With transpositions the state also holds a second band, X(i) =
  d(i − 2, j − 1) + 1 where target character i equals the last term character, and k + 1
  elsewhere: the one value the swap rule needs from two rows back.
- **Transitions** on character c compute each new cell from the insert, delete, substitute,
  and (with transpositions) swap rules over the old band, in O(w).
- **Matching:** `is_match` when d(n, j) ≤ k; `can_match` when any cell is ≤ k, which is
  exact: from a cell i ≤ k, the rest of the target reaches d(n, ·) ≤ k. The prefix phase is
  exact matching, character by character.
- **No determinization.** Schulz–Mihov's parametric DFA (Lucene's tables, Tantivy's
  builder) turns each step into one table lookup, at the cost of precomputed tables per
  (k, transpositions) that grow quickly with k: Lucene stops at k = 2. The simulation costs
  O(min(k, n)) per character for any k. One implementation ships (ARCHITECTURE, "One
  implementation per mechanism"): the compiled DFA is a Track P candidate, kept only if
  the Phase 5 fuzzy row measures it faster (T6).

## D5. The automaton interface

```cpp
struct automaton {                                        // glob, levenshtein
	std::size_t words() const;                            // the state's size in uint64 words
	void start(std::uint64_t* s) const;
	void step(const std::uint64_t* s, char32_t c, std::uint64_t* out) const;
	bool is_match(const std::uint64_t* s) const;
	bool can_match(const std::uint64_t* s) const;
	std::string_view literal() const;                     // D1's literal prefix
};
```

Automata step over characters, not bytes, and are immutable after construction, so one
automaton serves every segment of a query on any thread. States live in caller storage
of a fixed size, so the walk keeps one per character boundary without allocating per step.
fst's byte concept (`accept` returning a state by value) is not implemented: nothing in
strata intersects an FST of terms (termdict's FST holds block first keys, D7), and an
adapter can follow if one does.

## D6. The walk over sorted keys

`matcher<A>` takes keys in increasing order and answers, per key, whether it matches and,
when it cannot, the byte prefix every key that shares it also fails on. Any ordered source
uses it: a sorted vector (tests), a dictionary (D7), a merged cursor.

- **Resuming.** For the previous key the matcher records, per character boundary: its byte
  offset, `seen` (the furthest byte any character so far examined, D1's lookahead), and the
  state. A new key sharing l bytes with the previous one resumes at the last boundary
  whose `seen` ≤ l: every character before it was decided by shared bytes alone. Keys in a
  dictionary share most of their bytes with their predecessor, so most keys cost a few
  steps.
- **Dead prefixes.** When a state dies at a boundary whose `seen` lies within the key, every
  key starting with the key's first `seen` bytes dies there too. The matcher reports that
  prefix, and answers the following keys that share it without decoding them.
- **Recorded depth.** Boundaries are recorded up to 4,096 characters a key (preset
  `resume_depth`); a longer key's tail is decoded on every visit, which reading its bytes
  already costs. It bounds the matcher's memory at 4,096 × (words + 2) words whatever a term's
  length (terms reach 2^32 − 1 bytes, `limits.md`). A tuning preset, not a limit: it changes
  no answer.

## D7. Intersecting a segment's dictionary

`intersect(segment, automaton, f)` calls f(term, info) for every matching term in order,
each info decoded as `find_term` decodes it (impacts of a term below 128 postings left for
`complete_impacts`).

- **Why not fst traversal.** termdict's FST indexes block first keys, about one key in 32 to
  64, so it cannot enumerate terms; `fst::map::intersect` over it would visit block starts,
  not matches. The walk scans blocks.
- **The range.** The scan covers [literal prefix, successor) (D1, D2), so `obam*` and `ob?ma`
  read only the `obam` and `ob` ranges; `obama~2` with no fixed prefix reads the whole
  dictionary, pruned by D6.
- **The term-info cursor across seeks.** `termdict::reader::scan(from, f)` skips the
  entries of `from`'s block below it without showing their values, and a value's offsets
  follow its predecessors'. A seek therefore runs `find_run(from, visit)` first, recording
  the run cursor before each value it sees (from the restart up to the first entry at or
  past `from`), then `scan(from, f)`; the first entry the scan returns takes the cursor
  recorded for that same value, or the cursor past every recorded value when it is the next
  restart. Every entry visited after that is decoded in order, a non-matching one without
  its impacts.
- **Seeking past dead prefixes.** Entries under a dead prefix cost one entry decode each and
  no automaton step. After 64 consecutive entries under one dead prefix (preset
  `seek_after`, `termdict::max_block_entries`: a seek re-decodes up to one block and one run,
  so a dead run that long has paid for it), the walk seeks to the prefix's successor.
- **Corruption.** A dictionary block that fails its region check throws `corrupt_segment`,
  as `scan_dictionary` does; a decoded df above the segment's document count is corrupt, as
  in `find_term`.
- **Block skipping belongs in termdict** (O2). Every key in a block lies between its first
  key and the next block's, so it shares their common prefix; when the automaton is dead on
  that prefix, the block can be skipped unread. The index FST and block opening are private
  to termdict, so strata cannot do this through the public API. The proposal is an
  additive `termdict::reader::intersect(matcher, f)`: block skipping by first keys, dead
  prefixes inside runs, and suffixes skipped unread (`suffixes::skip`), with values
  visited in order so the run cursor needs no replay. strata's `intersect` is the one
  function that changes when it lands. Until then the walk decodes every entry of the
  range outside long dead runs; whether that meets the fuzzy row (T6) is the lead's
  measurement, and the termdict change is the expected fix if it does not.

## D8. Expansion and Xapian's limits

`expand(snapshot, automaton, limit)` resolves the expansion over every segment of a
snapshot: the sorted distinct terms, each with its snapshot document frequency (the sum
of the segments' dfs, deleted documents included, Phase 3's D13), and each segment's term
infos for its own terms. A snapshot is one Xapian database: its segments' expansions
merge, and the limits apply to the merged set.

`limit` is Xapian's `max_expansion` and limit type (`WILDCARD_LIMIT_*`):

| Type | Xapian | strata |
|---|---|---|
| `error` (Xapian's default) | Expanding to more than `max_expansion` terms throws `WildcardError` | Throws `strata::expansion_error`. A segment stops at `max_expansion` + 1 matches, which already decides it; otherwise the merged count decides |
| `first` | The first `max_expansion` terms in term order | Each segment stops at `max_expansion` matches (the merged first n are among each segment's first n); the merge keeps the first n |
| `most_frequent` | The `max_expansion` terms with the highest term frequency | Every segment walks its whole range; the merge keeps the n highest snapshot dfs, ties by term order (O3), returned in term order |

- `max_expansion` 0 means no limit, Xapian's default for both operators. `QueryParser`'s
  defaults (Xapian documents 100 most frequent for partial terms, unlimited for wildcards)
  are the `queryparser` design's to pass in.
- An empty expansion matches nothing (Xapian's `MatchNothing`), and is not an error.
- **Memory.** An expansion holds its terms' bytes and per-segment infos, O(matched terms),
  bounded by the dictionaries; `error` and `first` bound it by the limit. Allocation grows
  with expanded terms, never with matching documents (the Phase 3 allocation property row).

## D9. How expanded terms score

Selection (D8) and combination are separate, as in Xapian's `OP_WILDCARD(pattern,
max_expansion, flags, combiner)`.

- **`synonym`, the default** (Xapian's default combiner; ARCHITECTURE, step 2). The union
  scores as one term: a document's wdf is the sum of its expanded terms' wdfs, weighted by
  the union's statistics. This keeps a rare misspelling from outscoring the intended word,
  the failure of a plain sum. Its scorer, with Xapian's `OP_SYNONYM` term-frequency
  estimate taken from the oracle's vectors, is the synonym operator's work (FEASIBILITY:
  "`OP_SYNONYM` statistics"), shared with explicit `SYNONYM` queries; this branch does not
  build it (O4).
- **`or`**: each expanded term scores as its own term and the scores sum (Xapian's `OP_OR`
  combiner). Built here, from `term_scorer`s under a `disjunction_scorer` weighted by each
  term's snapshot df, so expansions run end to end and their counts are tested.
- **`max`** (Xapian's `OP_MAX`) follows with a max-combining disjunction, not in this branch.
- **Constant score** is Lucene's default rewrite for prefix and wildcard queries and
  Tantivy's for its automaton queries. Xapiand needs it for "exists" as a boolean filter,
  which Phase 4's scored filters cover; a large expansion in count or filter mode is better
  served by a union into a `docset` bitset than by a disjunction of thousands of term
  scorers, a Track P candidate measured on T6.
- **Top n by frequency** is the `most_frequent` limit (D8), not a combiner. Lucene's fuzzy
  default rewrite, the top 50 terms by similarity with blended frequencies, has no Xapian
  counterpart and is not built.

## D10. Where the code lives

- `include/strata/automata.hh`: `prefix`, `glob`, `levenshtein`, `matcher` (D1–D6),
  namespace `strata::automata`, depending only on `unicode`. It is library-shaped: it moves
  verbatim to a library.
- `include/strata/expand.hh`: `intersect` over a segment (D7), `expand` over a snapshot (D8),
  and the `or` builder (D9). It uses only `segment`'s public reads (`dictionary`,
  `find_term`'s value format through `format::decode_term_info`, `doc_count`) and is templated
  on the segment type, so `phase4/layers-refactor`'s `basic_segment` takes it unchanged.
- **Recommendation:** a new public library, `Kronuz/automata`, holding `automata.hh` and its
  brute-force tests (it is general: any ordered key source, any byte strings), with
  `termdict::reader::intersect` (O2) in termdict over its interface, and `expand.hh`
  staying in strata, since it knows strata's term values and snapshots. ARCHITECTURE's
  library table already lists `automata`. Creating a public repository is the owner's
  call (O1); the code is built inside strata meanwhile.
- **Touches to existing files** are additive: `CMakeLists.txt` gains the test and fuzz
  targets in lines of their own; `docs/testing/parity.md` gains the Phase 5 rows (T9).

## D11. Limits

Rows for `limits.md`:

| Limit | strata | Mechanism | Lucene 10.4 | Tantivy 0.26 | Xapian | Status |
|---|---|---|---|---|---|---|
| Edit distance | 2^32 − 2 | Band cells hold k + 1 in 32 bits; the state is min(2k + 1, n + 1) cells | 2 (`LevenshteinAutomata`'s tables) | Not in its documentation; read with the other reference values | Unsigned, no fixed limit | At the largest |
| Pattern length | No limit | The glob state is ⌈(m + 1) / 64⌉ words; the Levenshtein band is bounded by k | Determinization work limit, 10,000 states (throws) | A regex size limit | No fixed limit | At the largest |
| Expanded terms | No limit (the caller's `max_expansion`) | Memory O(matched terms), D8 | `IndexSearcher` max clause count, 1,024, for scoring rewrites; none for constant score | No fixed limit | No fixed limit (`max_expansion` 0) | At the largest |
| `resume_depth` 4,096 characters, `seek_after` 64 entries | Tuning presets (D6, D7) | None: they change no answer | — | — | — | To measure (presets rule) |

## D12. Errors

`expansion_error` (a `std::runtime_error`) when an `error`-limited expansion exceeds its
limit, carrying the limit; `std::invalid_argument` for k above 2^32 − 2. A pattern is never
malformed: every byte string is a valid glob, prefix, or target.

## Scale

Expansion runs inside one shard, over that shard's snapshot: its cost is the dictionary
walk of each segment, independent of cluster size, and it sends nothing over the network.
Across shards each shard expands on its own and applies the limits to its own terms, as
each Xapian database in a multi-database search is expected to (O3); a global `first` or
`most_frequent` across shards needs a candidate round (each shard's top candidates with
dfs, merged at the coordinator, then sent down), one extra round trip, which Track D's query
design decides with the global-statistics mode (DISTRIBUTION, "Query"). The synonym
combiner's statistics are global statistics like any term's.

## Gate rows

| Row | Target | Procedure |
|---|---|---|
| T1 Correctness against brute force | Every automaton's matches over every term equal a brute-force scan's, exactly, as sorted term lists, and every matched term's info equals `find_term`'s: random term sets over small alphabets (dense matches and long dead runs), ASCII, Unicode (2-, 3-, 4-byte characters), invalid and aliasing bytes (E2 82 41 beside E2 82 AC, lone C3, F0 at a term's end); adversarial sets: the empty term, terms past `resume_depth`, terms past one block's 1 KiB; patterns: empty, `*`, `?`, `**`, `*a*a*a*b`, `*a??????????`, more than 64 elements, literal metacharacters with flags off, k = 0 to 3, prefix lengths 0 to past the target, transpositions on and off; over sorted vectors, one segment, and many segments, through seeks and restart runs | `test/automata_test.cc`: brute force decodes each term with `next_utf8` and checks a recursive glob matcher and a full OSA dynamic program, independent of D3 and D4 |
| T2 Expansion | `error`, `first`, `most_frequent`, and no limit over several segments sharing terms equal a brute-force union with summed dfs; `error` throws exactly when the distinct count passes the limit; ties by term order; empty expansions; the `or` builder's counts equal brute-force document counts, with deletes | `test/automata_test.cc` |
| T3 Decoder fuzzing | `fuzz_automata` (pattern, parameters, and a term set from the input; the matcher against brute force) one CPU-hour at the gate's commit with zero findings and a hang timeout; its corpus replayed as a test in every build | `fuzz/fuzz_automata.cc`, `fuzz/corpus/automata` |
| T4 Oracle parity | Expansions of `OP_WILDCARD` (prefix and glob) and `OP_EDIT_DISTANCE` (k 1 and 2, fixed prefixes) on the oracle corpus equal Xapian's recorded vectors, with each limit type; O3's questions answered by vectors | The oracle harness records them (Phase 0's GPL-side repository); `tools/parity.cc` compares |
| T5 Sanitizers and coverage | `automata_test` and the fuzz replay pass under ASan and UBSan; `automata.hh` and `expand.hh` at TESTING's "everything else" targets (≥ 90% lines, ≥ 80% branches) | A `-fsanitize=address,undefined` build with Homebrew LLVM; `tools/coverage.sh` |
| T6 Speed (the lead measures, on `main` only) | The prefix, wildcard, and fuzzy class (`obam*`, `ob?ma`, `obama~2`, and the harness's sample of the class), top 10 and count, ≤ 1.0× the faster of Lucene and Tantivy and faster than Xapian (Phase 5's gate); counts equal every engine's. Engines run equal semantics: Lucene `PrefixQuery`, `WildcardQuery`, `FuzzyQuery(maxEdits 2, prefixLength 0, transpositions)`; Tantivy's prefix, regex-free wildcard, and `FuzzyTermQuery(distance 2, transposition)`; each with its default rewrite, stated beside the row | `bench/query_trials.sh` with the class's queries, `bench/phase2/gate.py`; T6 also decides D4's compiled DFA, D7's termdict change (O2), and D9's bitset union |
| T7 Memory | Heap bytes per query by class recorded with the allocation-counting build; growth with expanded terms only, none with matching documents | The allocation counter (BENCHMARKS' property row) |
| T8 Test parity | The inventory gains and passes the rows for Lucene's `TestLevenshteinAutomata`, `TestFuzzyQuery`, `TestWildcardQuery`, `TestPrefixQuery`, `TestAutomatonQuery`, and `TestTermsEnum`'s intersection, and Tantivy's fuzzy and automaton-weight tests; regex queries n/a (no Xapian operator) | `docs/testing/parity.md` |

## Risks

- The fuzzy row (T6) without block skipping: `obama~2` with no fixed prefix decodes most of
  the dictionary. D7 names the fix (O2) and keeps it to one function.
- Xapian's exact semantics at the edges (O3) can move T4; each is one parameter or one
  comparison in D3, D4, or D8.
- `or` scoring of a large expansion is slow and ranks rare terms high; it exists for
  tests and for callers that ask for `OP_OR`, not as the default.

## Open questions

- **O1 (owner).** A public `Kronuz/automata` repository for `automata.hh` (D10)?
- **O2 (owner; termdict).** The additive `termdict::reader::intersect` (D7), or exposing the
  index FST and block opening so strata does it.
- **O3 (oracle).** Xapian's choices where its documentation is silent: whether
  `OP_EDIT_DISTANCE` counts a transposition as one edit; whether `fixed_prefix_len` counts
  characters; `most_frequent`'s tie order; whether a multi-database search applies limits per
  database; and empty patterns under each flag.
- **O4 (lead).** Who builds the synonym scorer (D9): the `queryparser` workstream, or a
  synonym-operator item ahead of it.
- **O5 (owner; string-similarity).** ARCHITECTURE has `string-similarity` gain Xapian's
  distance and serve as the tests' reference. Its metric is byte-level and normalized; the
  tests carry their own reference meanwhile, and the library change is the owner's.
