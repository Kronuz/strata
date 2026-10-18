# Automata: prefix, wildcard, and fuzzy term matching (Phase 5)

Revision 1 (2026-10-17) folds the preliminary review (`docs/reviews/automata-design.md`,
AUT-R1 to AUT-R14) and the owner's rulings on its C5 (key scans from a lower bound) and O6
(one automaton interface). Each changed decision cites the finding it answers.

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
where that is silent, from the oracle harness's recorded vectors (O3), never from a
sentence whose source is Xapian's code (AUT-R9). No Lucene, Tantivy, or Xapian source was
read.

Rulings earlier reviews addressed to this phase, each answered here: Phase 1's automaton
check over `fst::map::intersect` (phase-1.md items 3 and 4, D5 and T1), Phase 2's
automaton-driven seeking (phase-2-design.md D14, D6 and D7), and Phase 1's postings-tail
question (phase-1.md:239-244, T9).

## What exists

- `fst` (`fst.hh`): `map::intersect(automaton, f)`, a depth-first walk over every key with
  the byte-level concept `start / accept / is_match / can_match`, and `prefix_automaton`.
  Nothing calls either; the owner retires both once this branch lands (O6).
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
  valid UTF-8, which `analysis` never produces for text. Xapiand's keyword and
  binary-valued terms are not UTF-8 in general, so aliasing and encoded surrogates
  (`next_utf8` accepts ED A0 80 as U+D800) are oracle questions (O3) with vectors over a
  keyword corpus (T4, AUT-R9).
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
  covers the whole dictionary, pruned by D6's seek targets; the state stays one word.
- **Anchoring.** The pattern matches the whole term. Xapiand strips a keyword term's
  trailing `*` before building a pattern with flags, so whether Xapian anchors the end
  under pattern flags decides which terms its users get; it is an O3 question (AUT-R5), and
  an unanchored end is one trailing star added at compilation.
- **Next character** (D5): any character when a live position holds `?` or `*`, otherwise
  the smallest literal at a live position at or above the asked one: exact.
- **Memory.** The masks hold one bitset per distinct literal character, so a pattern of m
  elements costs up to m × ⌈(m + 1) / 64⌉ words; construction charges it to the query's
  budget before allocating (D11).

## D4. Levenshtein

`levenshtein(target, k, prefix_chars, transpositions)`: the terms within k edits of the
target, where an edit inserts, deletes, or substitutes one character, and with
`transpositions` also swaps two adjacent ones (the restricted, optimal-string-alignment
distance, as in Schulz and Mihov's transposition automaton and Lucene's `FuzzyQuery`). The
first `prefix_chars` characters of the target must match exactly, as Xapian's
`fixed_prefix_len` and Lucene's `prefixLength`; a prefix longer than the target fixes all of
it. The `transpositions` preset is whatever O3's vector shows Xapian's `OP_EDIT_DISTANCE`
does; the design cites the vector once recorded, not ARCHITECTURE's sentence, whose source
is Xapian's code (AUT-R9, C7). k takes every value: a k at or above the longest key's
length plus the target's accepts every key with the fixed prefix, so the simulation clamps
k at 2^62 internally and changes no answer (AUT-R6).

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
  implementation per mechanism"): the compiled DFA is a Track P candidate, chosen by a
  branch A/B under a hold the lead grants, recorded in `docs/perf/` (C3), never by a gate
  row.
- **Next character** (D5): any character while a live cell is below k; otherwise the
  smallest at or above the asked one among the target characters after cells equal to k and
  the swap band's live cells. It may name a character that turns out dead, never skip a live
  one: conservative, which seeking needs.

## D5. The automaton interface

strata's one automaton interface (AUT-R1; the owner's O6):

```cpp
struct automaton {                                        // glob, levenshtein
	std::size_t words() const;                            // the state's size in uint64 words
	void start(std::uint64_t* s) const;
	void step(const std::uint64_t* s, char32_t c, std::uint64_t* out) const;
	bool is_match(const std::uint64_t* s) const;
	bool can_match(const std::uint64_t* s) const;
	// The smallest character >= c whose step keeps s alive, or none; never above the
	// smallest live one (AUT-R2).
	std::optional<char32_t> next(const std::uint64_t* s, char32_t c) const;
	std::string_view literal() const;                     // D1's literal prefix
};
```

The contract:

- A state is `words()` plain 64-bit words, copyable with `memcpy`, with no pointers into
  the automaton; `start` and `step` write every word.
- An automaton is immutable after construction, so one serves every segment of a query on
  any thread at once; T5 runs matchers on several threads over one automaton under TSan.
- `can_match` is exact for glob and Levenshtein; `next` is exact for glob and conservative
  for Levenshtein (D4).
- Construction takes an optional charge callback (bytes before they are allocated), which
  throws to refuse; the walk's recorded states use the same callback (D6, D11).

Automata step over characters, not bytes. fst's byte concept (`accept` returning a state
by value) is not implemented: it would copy a Levenshtein state of 4k + 3 words or a
multi-word glob state at every transition, and D1's lenient decoding needs characters with
lookahead. The owner's O6 retires `fst::map::intersect` and `fst::prefix_automaton` once
this branch lands; Phase 1's carried check over them (phase-1.md items 3 and 4) is retired
with them, and T1 carries the brute-force check for strata's walk. The lead amends
PLAN.md:223-224 and 236 accordingly.

## D6. The walk over sorted keys

`matcher<A>` takes keys and answers, per key, whether it matches and, when it cannot, the
byte prefix every key that shares it also fails on, and a seek target. Its answers are
correct for keys in any order: it resumes from the previous key's shared bytes and recorded
`seen`, and answers a dead prefix from bytes alone; increasing order only makes resuming
pay. Any ordered source uses it: a sorted vector (tests), a dictionary (D7), a merged
cursor, the keys section (D8a).

- **Resuming.** For the previous key the matcher records, per character boundary: its byte
  offset, `seen` (the furthest byte any character so far examined, D1's lookahead), and the
  state. A new key sharing l bytes with the previous one resumes at the last boundary
  whose `seen` ≤ l: every character before it was decided by shared bytes alone. Keys in a
  dictionary share most of their bytes with their predecessor, so most keys cost a few
  steps.
- **Dead prefixes.** When a state dies at a boundary whose `seen` lies within the key, every
  key starting with the key's first `seen` bytes dies there too. The matcher reports that
  prefix, and answers the following keys that share it without decoding them.
- **Seek targets** (AUT-R2; Phase 2's D14). When a key dies at character boundary o with
  state S, and every character before o was decided by the bytes before o (`seen` = o), the
  characters that keep S alive at o form a finite set A (S accepted the dead character only
  because it accepts few). Every live key after this one that starts with the bytes P before
  o starts with P + a spelling of some a in A: its valid UTF-8 encoding, and for U+0080 to
  U+00FF also the lone byte (D1). The target is the smallest such P + spelling above the
  key, bounded above by the successor of P (keys leaving P) and below by the successor of
  the dead prefix; no key between the dying key and the target can match. A spelling that is
  a proper prefix of the key (a lone byte whose valid form the key continues) yields no
  skip. When `seen` passes o, the target is the dead prefix's successor alone. `next` builds
  A by asking from U+0000 upward, so a conservative `next` only shortens a seek.
- **Recorded depth.** Boundaries are recorded up to 4,096 characters a key (preset
  `resume_depth`); a longer key's tail is decoded on every visit, which reading its bytes
  already costs. The matcher's memory is at most 4,097 × (words + 2) words, words from the
  pattern (D3, D4), whatever a term's length (terms reach 2^32 − 1 bytes, `limits.md`), and
  it draws on the query's budget as it grows (D11). A tuning preset, not a limit: it changes
  no answer, which T1 tests at depths 0, 1, 2, 3, and 4,096.

## D7. Intersecting a segment's dictionary

`intersect(inverted, automaton, f, options)` calls f(term, info) for every matching term
in order, each info decoded as `find_term` decodes it (impacts of a term below 128
postings left for `complete_impacts`). `inverted` is the segment's inverted reader (AUT-R3):
main's `segment` today, layers' `seg.get<inverted_plugin>()` once the refactor lands; both
provide `dictionary()` and `doc_count()`, so one source compiles on both.

- **Why not fst traversal.** termdict's FST indexes block first keys, about one key in 32 to
  64, so it cannot enumerate terms; `fst::map::intersect` over it would visit block starts,
  not matches. The walk scans blocks.
- **The range.** The scan covers [literal prefix, successor) (D1, D2), so `obam*` and `ob?ma`
  read only the `obam` and `ob` ranges; `obama~2` with no fixed prefix reads the whole
  dictionary, pruned by D6's seek targets.
- **The term-info cursor across seeks.** `termdict::reader::scan(from, f)` skips the
  entries of `from`'s block below it without showing their values, and a value's offsets
  follow its predecessors'. A seek therefore runs `find_run(from, visit)` first, recording
  the run cursor before each value it sees, then `scan(from, f)`; the first entry the scan
  returns takes the cursor recorded for that same value, or the cursor past every recorded
  value when it is the next restart. This replay is `find_term`'s mechanism a second time;
  it leaves `expand.hh` when the inverted reader gains one seekable term cursor after layers
  lands (`term_cursor::seek(from)`, used by `find_term`, `scan_terms`, and `intersect`),
  built on termdict's additive `reader::cursor::seek(from)` (O2, decided: the FST floor, the
  restart binary search, `find_from`'s skipping, forward only). Only strata links termdict.
- **Seeking** (AUT-R2). Entries under a dead prefix cost one entry decode each and no
  automaton step. After `seek_after` consecutive dead entries (a preset, 64 =
  `termdict::max_block_entries`, since a seek through today's `scan` re-decodes up to one
  block and one run), the walk seeks to the matcher's target (D6). With termdict's
  `cursor::seek`, a seek never re-decodes behind the cursor, and the walk seeks on every dead
  result; the preset's value is a branch A/B (C3): entries visited and time per query on the
  5M segment for `ob?ma`, `obama~2`, and `*ma`.
- **Cancellation and time slices** (AUT-R7). Every `check_every` entries (a preset, 1,024,
  measured by a branch A/B, C3) the walk reads the query's `task::cancel_token`, throwing
  `task::cancelled` when it is set, and asks the caller's `pause()`, which returns true when
  the slice or deadline is spent. A paused walk returns the key to resume from; the caller
  yields (`task::budget::checkpoint`) and calls `intersect` again from it, rebuilding the
  matcher, which is correct for any key order (D6). The walk's slice joins ARCHITECTURE's
  budget rule (100 to 250 µs between suspension points).
- **Budget** (AUT-R6). The matcher's recorded states and the automaton's construction draw
  on the query's `byte_budget` permit through the charge callback (D5).
- **Corruption.** A dictionary block that fails its region check throws `corrupt_segment`,
  as `scan_dictionary` does; a decoded df above the segment's document count is corrupt, as
  in `find_term`. T1 reaches both.
- **Block skipping.** Every key in a block lies between its first key and the next block's,
  so it shares their common prefix; when the automaton is dead on that prefix, the block can
  be skipped unread. With `cursor::seek`, a seek target past a block skips it through the
  FST floor; skipping by the first keys' common prefix is an option on top, measured by a
  branch A/B (C3). termdict gains no dependency on automata (AUT-R12).

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

- **Terms with no live document** (AUT-R5). A term whose every posting in a segment is
  deleted stays in that segment's dictionary until a merge; a Xapian database whose deletes
  are committed holds no posting for it. On a segment with deletes, a matched term is tested
  for liveness by walking its postings to the first live one (a single posting's ordinal is
  inline); a term live in no segment is not selected, counted toward `error`, or ranked.
  A selected term's df stays D13's, deleted documents included, and every segment holding
  it keeps its info, marked live or not, so weights and dfs are the snapshot's; the
  remaining difference from Xapian is the statistics drift the parity gate already
  measures. How Xapian treats such terms under each limit type is an O3 question.
- **The merge.** Segments are walked in turn into one ordered map of distinct terms; a term
  is copied once, and each segment adds its df and info. Under `first`, once n eligible
  terms are held, the n-th is the cutoff: larger terms are dropped and every later walk
  stops past it, so each segment stops at the merged limit, and every term up to the final
  cutoff was seen by every segment (its df is exact). Under `error`, the first eligible
  term past the limit throws.
- `max_expansion` 0 means no limit, Xapian's default for both operators. `QueryParser`'s
  defaults (Xapian documents 100 most frequent for partial terms, unlimited for wildcards)
  are the `queryparser` design's to pass in.
- An empty expansion matches nothing (Xapian's `MatchNothing`), and is not an error.
- **Memory.** An expansion holds its terms' bytes and per-segment infos, O(matched terms),
  charged to the query's budget permit as it grows (D11); `error` and `first` bound it by
  the limit, plus the terms below the cutoff with no live document. Allocation grows with
  expanded terms, never with matching documents (the Phase 3 allocation property row).

## D8a. Keys (AUT-R4)

Format 13 takes every key out of `terms`, so a dictionary walk never sees one; Xapiand
lists indexes with `OP_WILDCARD` over `QK<path>`. The expansion follows F13-R2's routing
(layers' L3) exactly, refined by one exact test:

1. The source is chosen by layout: format 12 keeps keys in the dictionary
   (`search_layout_12`), so D7 alone covers them; format 13 (`search_layout`) routes.
2. A literal that starts with the key prefix expands over `keys` only; a literal that is a
   proper prefix of it, the empty literal included, over both, merged in byte order; any
   other over `inverted` only. Before walking `keys` for a proper-prefix literal, the
   matcher runs over the key prefix itself: a dead prefix within it skips `keys`, so
   `obama~2` never walks the keys of a corpus whose key prefix it cannot extend.
3. The keys walk uses the same matcher and seek targets as D7, over `keys`' scan from a
   lower bound (the owner's ruling on C5; layers' prefix-scan text follows it).
4. `expansion` carries key leaves beside term infos: the key and, per segment, its ordinal.
   A key's df counts the segments holding it, deleted copies included (LAY:272-276). The
   limits apply to the merged set of terms and keys.
5. T2 gains a row: an expansion over a keyed format-13 index equals the format-12 expansion
   of the same documents.

This waits for format 13's keys reaching the planner (layers' step 7c); until then
`expand` takes the inverted source only, which is format 12's complete answer.

## D9. How expanded terms score

Selection (D8) and combination are separate, as in Xapian's `OP_WILDCARD(pattern,
max_expansion, flags, combiner)`.

- **`synonym`, the default** (Xapian's default combiner; ARCHITECTURE, step 2). The union
  scores as one term: a document's wdf is the sum of its expanded terms' wdfs, weighted by
  the union's statistics. This keeps a rare misspelling from outscoring the intended word,
  the failure of a plain sum. Its scorer, with Xapian's `OP_SYNONYM` term-frequency
  estimate taken from the oracle's vectors, is the synonym operator's work (FEASIBILITY:
  "`OP_SYNONYM` statistics"), shared with explicit `SYNONYM` queries and the DSL's
  `_synonym`; this branch does not build it. Every Xapiand multi-term path combines with
  `OP_SYNONYM` (AUT-R11), so O4 asks the lead to decide now; recommended: a
  synonym-operator item ahead of `queryparser`.
- **`or`**: each expanded term scores as its own term and the scores sum (Xapian's `OP_OR`
  combiner). Built here, from `term_scorer`s under a `disjunction_scorer` weighted by each
  term's snapshot df, so expansions run end to end and their counts are tested.
- **`max`** (Xapian's `OP_MAX`) follows with a max-combining disjunction, not in this branch.
- **Constant score** (AUT-R11), built here: Lucene's default rewrite for prefix and
  wildcard queries and Tantivy's for its automaton queries, which T6 needs to compare top-k
  rows under equal semantics, and the FILTER context's union. `expansion_constant` unions
  the segment's expanded live terms without scoring them: every match scores the given
  constant, and once the threshold reaches it the scorer ends. Phase 4's scored filters are
  iterators over values, ranges, and geo, none over a term set, so they do not cover it.
  A union into a `docset` bitset instead is a Track P candidate, chosen by a branch A/B
  (C3).
- **One store** (AUT-R11). A multi-term leaf uses its `expansion` as its per-segment
  term-info store; it does not fill `term_table`, whose `column()` scans linearly and would
  be quadratic in expanded terms. Impacts are completed only by the scorers that need them
  (`or`), never for constant score.
- **Top n by frequency** is the `most_frequent` limit (D8), not a combiner. Lucene's fuzzy
  default rewrite, the top 50 terms by similarity with blended frequencies, has no Xapian
  counterpart and is not built.

## D10. Where the code lives

- `include/strata/automata.hh`: `prefix`, `glob`, `levenshtein`, `matcher` (D1–D6),
  namespace `strata::automata`, depending only on `unicode`. It is library-shaped: it moves
  verbatim to a library.
- `include/strata/expand.hh`: `intersect` over an inverted reader (D7), `expand` over a
  snapshot through a snapshot-to-reader accessor (D8), and the `or` and constant builders
  (D9). It keeps term-info decoding only until the inverted reader's seekable term cursor
  exists (AUT-R3), and stays in strata, since it knows strata's term values and snapshots.
- **The library** (AUT-R12). `automata.hh` stays in strata, library-shaped, until D5's
  interface with `next` and termdict's `cursor::seek` exist; then it moves to
  `Kronuz/automata` with its brute-force tests. Creating that repository is the owner's
  call (O1; recommended yes at that point). ARCHITECTURE's library table already lists
  `automata`; its "Levenshtein DFAs" and "edit distances come from string-similarity" are
  the lead's to correct to D4's simulation (AUT-R14).
- **The test reference's distance** (O5, decided by ARCHITECTURE and stage 1):
  `string-similarity` gains an integer, code-point, optimal-string-alignment distance,
  additively (Xapiand pins it), and `automata_reference.hh` calls it. It stays independent
  of the automaton, which T1 needs. The library change is a branch of its own, which the
  lead assigns; the private copy stays until it lands.
- **Touches to existing files** are additive: `CMakeLists.txt` gains the test and fuzz
  targets in lines of their own; `docs/testing/parity.md` gains the Phase 5 rows (T8).

## D11. Limits

Rows for `limits.md` (AUT-R6); Tantivy's edit-distance value is from `FuzzyTermQuery`'s
documented signature (docs.rs, read 2026-10-17), which states no bound in prose:

| Limit | strata | Mechanism | Lucene 10.4 | Tantivy 0.26 | Xapian | Status |
|---|---|---|---|---|---|---|
| Edit distance | Every value | None needed: cells are 64-bit, and k clamps at 2^62, past any key's length, which changes no answer (D4) | 2 (`LevenshteinAutomata`'s tables) | 255 by type (`FuzzyTermQuery::new` takes `distance: u8`); its documentation states no other bound | Unsigned (`unsigned`), no fixed limit | At the largest |
| Pattern length | No limit | The query's byte budget (Phase 4's D13a): construction charges the glob masks (up to m × ⌈(m + 1) / 64⌉ words) and the Levenshtein target before allocating; the walk charges each recorded state, at most 4,097 × (words + 2) words; a refused charge fails the query with the budget error (D12) | Determinization work limit, 10,000 states (throws) | A regex size limit | No fixed limit | At the largest |
| Expanded terms | No limit (the caller's `max_expansion`) | The query's byte budget: each distinct term's bytes and each segment's info are charged as the expansion grows (D8) | `IndexSearcher` max clause count, 1,024, for scoring rewrites; none for constant score | No fixed limit | No fixed limit (`max_expansion` 0) | At the largest |
| Walk time | No limit | Cancellation and pause checks every `check_every` entries (D7) | — | — | — | Bounded by the query's deadline |
| `resume_depth` 4,096 characters, `seek_after` 64 entries, `check_every` 1,024 entries | Tuning presets (D6, D7) | None: they change no answer | — | — | — | To measure (presets rule, C3) |

## D12. Errors

`expansion_error` (a `std::runtime_error`) when an `error`-limited expansion exceeds its
limit, carrying the limit. `query_budget_error` when a charge to the query's byte budget is
refused (AUT-R6): Phase 4's D13a budget error, defined in `expand.hh` until D13a's home
exists, then moved there. `task::cancelled` when the query's cancellation token is set
(AUT-R7). No edit distance is refused. A pattern is never malformed: every byte string is a
valid glob, prefix, or target.

## Scale

Expansion runs inside one shard, over that shard's snapshot: its cost is the dictionary
walk of each segment, independent of cluster size, and it sends nothing over the network.
Across shards each shard expands on its own and applies the limits to its own terms, as
each Xapian database in a multi-database search is expected to (O3); a global `first` or
`most_frequent` across shards needs a candidate round (each shard's top candidates with
dfs, merged at the coordinator, then sent down), one extra round trip, which Track D's query
design decides with the global-statistics mode (DISTRIBUTION, "Query"). The synonym
combiner's statistics are global statistics like any term's, and a global-statistics
synonym needs one term set across shards, the candidate round's, which per-shard expansion
does not give (AUT-R5).

## Gate rows

| Row | Target | Procedure |
|---|---|---|
| T1 Correctness against brute force | Every automaton's matches over every term equal a brute-force scan's, exactly, as sorted term lists, and every matched term's info equals `find_term`'s: random term sets over small alphabets (dense matches and long dead runs), ASCII, Unicode (2-, 3-, 4-byte characters), invalid and aliasing bytes (E2 82 41 beside E2 82 AC, lone C3, F0 at a term's end), every cut of a valid 2-, 3-, and 4-byte sequence per lead class with each tail; adversarial sets: the empty term, terms past `resume_depth`, terms of 2 KiB (past one block's 1 KiB); patterns: empty, `*`, `?`, `**`, `*a*a*a*b`, `*a??????????`, more than 64 elements, literal metacharacters with flags off, k = 0 to 3 and k past every key, prefix lengths 0 to past the target, transpositions on and off; matcher depths 0, 1, 2, 3, and 4,096 agree; every seek target skips no match; over sorted vectors, one segment, and many segments, through seeks, restart runs, a corrupt block, and a corrupt df. It carries Phase 1's automaton check for strata's walk (O6 retires fst's) | `test/automata_test.cc` (sorted vectors, the matcher) and `test/expand_test.cc` (segments): brute force decodes each term with `next_utf8` and checks a recursive glob matcher and a full OSA dynamic program, independent of D3 and D4 |
| T2 Expansion | `error`, `first`, `most_frequent`, and no limit over several segments sharing terms equal a brute-force union of terms with a live document, with summed dfs; terms held only by deleted documents are never selected; `error` throws exactly when the distinct live count passes the limit; ties by term order; empty expansions; the `or` and constant builders' counts equal brute-force document counts, with deletes; a budget too small fails with `query_budget_error`; a cancelled whole-dictionary walk stops within `check_every` entries, and a paused one resumes to the same answer; after format 13's keys reach the planner, an expansion over a keyed format-13 index equals the format-12 expansion of the same documents (D8a) | `test/expand_test.cc` |
| T3 Decoder fuzzing | `fuzz_automata` (pattern, parameters, and a term set from the input; the matcher and its seek targets against brute force; an intersection over the segment built from the terms, and over the same segment with input-chosen bytes flipped, which must answer or throw `corrupt_segment`) one CPU-hour at the gate's commit with zero findings and a hang timeout; its corpus replayed as a test in every build | `fuzz/fuzz_automata.cc`, `fuzz/corpus/automata` |
| T4 Oracle parity | Expansions of `OP_WILDCARD` (prefix and glob) and `OP_EDIT_DISTANCE` (k 1 and 2, fixed prefixes 0 and 2) equal Xapian's recorded vectors with each limit type, over the oracle's corpus, a keyword corpus with invalid UTF-8, and a variant with deletes; O3's questions answered by vectors before T4 counts | The oracle harness gains an expansion mode (its inputs: patterns, flags, limits, k, fixed prefixes; its output: each expansion's terms in order, per limit type), GPL-side work the lead assigns; `tools/parity.cc` gains the comparator |
| T5 Sanitizers and coverage | `automata_test`, `expand_test`, and the fuzz replay pass under ASan with UBSan and under TSan (matchers on several threads over one automaton); the intersection at TESTING's reader targets (≥ 98% lines, ≥ 95% branches, every corruption check reached), since it decodes on-disk term infos; the rest of `automata.hh` and `expand.hh` at "everything else" (≥ 90% lines, ≥ 80% branches) | CI's `linux-asan-ubsan` and `linux-tsan` jobs; `tools/coverage.sh` |
| T6 Speed (the lead measures, on `main` only) | The prefix, wildcard, and fuzzy class, top 10 and count, ≤ 1.0× the faster of Lucene and Tantivy and faster than Xapian (Phase 5's gate), each query's counts equal across engines, before and after merging as Phase 3's rows | A committed, tagged query set: prefix, glob, and fuzzy at k 1 and 2 with fixed prefixes 0 and 2, drawn by seed from the 5M vocabulary and stratified by expansion size, as Phase 2's term class is by df. Per-engine configurations with equal match sets: Lucene's `PrefixQuery`, `WildcardQuery`, and `FuzzyQuery` with the constant-score rewrite, not FuzzyQuery's default top-50 rewrite; Tantivy's `RegexQuery` for prefixes and globs and `FuzzyTermQuery`; Xapian's `OP_WILDCARD` and `OP_EDIT_DISTANCE` with `max_expansion` 0; `strata_engine` parsing the three forms. Scoring per comparison (BENCHMARKS rule 5): constant score against Lucene and Tantivy; against Xapian, its `OP_OR` combiner against strata's `or`, and `OP_SYNONYM` against strata's synonym once O4's scorer lands. A query whose counts differ is dropped with its reason in the report. `bench/query_trials.sh` with the class's query file, `bench/phase2/gate.py`. The choices this row once decided (D4's compiled DFA, `seek_after`, `check_every`, block skipping, the bitset union) are branch A/Bs (C3) |
| T7 Memory | Heap bytes per query by class; growth with expanded terms only, none with matching documents | The allocation-counting build of `strata_engine` (BENCHMARKS' property row) over T6's query file, one query at a time, with a budget permit's `bytes()` reported beside the counter |
| T8 Test parity | The inventory gains and passes the rows for Lucene's `TestLevenshteinAutomata`, `TestFuzzyQuery`, `TestWildcardQuery`, `TestPrefixQuery`, `TestAutomatonQuery`, and `TestTermsEnum`'s intersection, and Tantivy's fuzzy and automaton-weight tests; regex queries n/a (no Xapian operator) | `docs/testing/parity.md` |
| T9 Postings-tail profile (AUT-R13) | Before format 13 freezes: the share of the class's query time spent decoding postings tails, `or` and count modes, on the 5M segment; if it dominates, format 13's tail decision (E4) weighs the class | A branch profile under a hold the lead grants (evidence, C3); the lead adds it to format 13's freeze criteria (C4) |

## Risks

- The fuzzy row (T6) before termdict's `cursor::seek`: `obama~2` with no fixed prefix
  decodes every entry D6's seek targets cannot skip within `seek_after`. D7 names the fix
  (O2) and keeps it to one function.
- Xapian's exact semantics at the edges (O3) can move T4; each is one parameter or one
  comparison in D3, D4, or D8.
- `or` scoring of a large expansion is slow and ranks rare terms high; it exists for
  tests and for callers that ask for `OP_OR`, not as the default.

## Open questions

- **O1 (owner).** A public `Kronuz/automata` repository for `automata.hh` (D10)?
  Recommended yes, once D5's `next` and termdict's `cursor::seek` exist (AUT-R12).
- **O2.** Decided by the standing rule to extend a Kronuz library additively: termdict gains
  `reader::cursor::seek(from)` (D7). Exposing its FST and block opening is wrong (AUT-R3).
  The termdict branch is the lead's to assign.
- **O3 (oracle).** Xapian's choices where its documentation is silent, each answered by a
  recorded vector before T4 counts: whether `OP_EDIT_DISTANCE` counts a transposition as
  one edit (D4's preset); whether `fixed_prefix_len` counts characters or bytes (Xapiand
  passes a byte count); `most_frequent`'s tie order; whether a multi-database search
  applies limits per database; empty patterns under each flag; whether a pattern is
  anchored at its end under pattern flags (D3); terms with no live document under each
  limit type (D8); aliasing and encoded surrogates (D1); and `OP_WILDCARD` and
  `OP_EDIT_DISTANCE` inside `OP_PHRASE` and `OP_NEAR` (AUT-R5, AUT-R9).
- **O4 (lead).** Who builds the synonym scorer (D9). Recommended: a synonym-operator item
  ahead of `queryparser`, which also serves the DSL's `_synonym` (AUT-R11).
- **O5.** Decided by ARCHITECTURE and stage 1: `string-similarity` gains the distance
  additively (D10).
- **O6 (owner, decided 2026-10-17).** strata's D5 interface is the one; fst's unused
  byte-level interface is retired once automata lands (D5).
