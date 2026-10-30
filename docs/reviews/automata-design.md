# strata review: automata design (preliminary)

- Date: 2026-10-17.
- Reviewed:
  - `docs/design/automata.md` at `fa544a6` on `phase5/automata` (D1 to D12, T1 to T8, O1 to
    O5), unchanged at the branch head `a29d702`, and the ledger `docs/status/automata.md` at
    `cc8823e`, unchanged at `a29d702`.
  - `git log main..phase5/automata`, five commits, and every diff: the ledger `ae8a226` and
    `cc8823e`; the design `fa544a6`; D1 to D6 `d84bfc2` (`include/strata/automata.hh`,
    `test/automata_reference.hh`, `test/automata_test.cc`, 4 lines of `CMakeLists.txt`); D7 to
    D9 `a29d702` (`include/strata/expand.hh`, `test/expand_test.cc`, 3 lines of
    `CMakeLists.txt`). `a29d702` is local: `origin/phase5/automata` is at `d84bfc2`.
  - The worktree's uncommitted changes, read for where the build is going and not reviewed as
    branch content: `fuzz/fuzz_automata.cc` and its corpus (T3), 9 lines of `CMakeLists.txt`, a
    comment in `automata.hh`, and a fix to `automata_test.cc:118`.
  - The libraries at strata's pins, which equal their local heads: `fst` `24da59a` (`fst.hh`,
    `test/test.cc`, `fuzz/fuzz_map.cc`), `termdict` `eaf83b3`, `unicode` `c93026c`
    (`next_utf8`), and `string-similarity` `cbc0a79` (`levenshtein.h`; Xapiand pins the same).
  - `docs/design/layers.md` revision 3 and `include/strata/{reader,searcher,search}.hh` on
    `origin/phase4/layers-refactor` at `1d60098`; `docs/reviews/format-13-design.md`,
    `docs/design/format-13.md`, and `include/strata/keys.hh` on `origin/phase4/format13` at
    `4266374`.
  - Xapiand `master` at `7e8cccb7d`, MIT files only: `src/query_dsl.cc:58-69, 636-690, 696-880`
    (its header is Dubalu's MIT notice) and the `string_similarity` pin in `CMakeLists.txt:923`.
- Checked against:
  - `docs/reviews/calibration.md` at main `0c9090d`, all three rows applied: every new mechanism
    searched for in strata's dependencies and the whole Kronuz catalog (`gh repo list Kronuz
    --limit 1000`, 257 repositories: no term automaton, glob matcher, or Levenshtein automaton;
    AUT-R1, AUT-R3, AUT-R12); CI on every platform for the reviewed commits (AUT-R14); every row
    cited as a baseline traced to a raw file (the design cites none; T6 has no recorded number).
  - PLAN at main `0c9090d`: the process and "What waits" (PLAN.md:3-20), the owner's decisions
    from the system review (22-55), Goals, Ratchets (131), "Every gate" (171-185), Phase 1's `fst`
    API and automaton test (223-224, 236), Phase 2's term class (272-273), Phase 3's
    multi-segment rows (325), Phase 5 (543-552), Phase 6's stall row (559).
  - FEASIBILITY.md:29, 109, 119, 126, 146, 162-173; BENCHMARKS.md rule 5 (42-45), the class row
    (99), "Query speed" (133-137); `docs/design/limits.md`; TESTING.md:31-37;
    ARCHITECTURE.md:170-181, 221-238, 322-345; `docs/design/phase-4.md` D6 (265-275) and D13a
    (373-381); `docs/testing/parity.md`.
  - The system review on main (`system-2026-10-17.md`): SYS-5 (324-352), SYS-9 (432-455),
    SYS-15 (587-607), SYS-16 (609-634).
  - Items earlier reviews addressed to Phase 5 or to this component: phase-1.md:239-247 (items 3
    and 4); phase-2-design.md:117-122 (D14) with phase-2.md:96-100; stage-1.md:403, 516-518,
    1102-1110, 1144-1153.
  - Lucene and Tantivy from their documentation only.
- Checked by:
  - Reading every diff and the code each claim cites, at the commits above.
  - CI through `gh`: run 39059258216 at `d84bfc2` with its failed-job logs and the ASan job's
    log; main's run 39071019926 at `5691ec7`; the layers branch's run 39070316303 (running at
    15:05).
  - A static count of the bytes the tests' inputs can produce: `automata_test.cc:94-99` and
    `expand_test.cc:183`.
  - Probes written, not compiled (the hold), in `strata-wt/automata/.scratch/review-automata/`:
    `probe_lookahead.cc` and `probe_depth.cc` (AUT-R10), `probe_deleted.cc` (AUT-R5), and
    `run.sh`, which builds them against `a29d702` from `git archive`, builds a mutant
    `automata.hh` whose `lookahead()` returns 3 for F0 to F4, and runs both `automata_test` and
    the probe on it. Each finding states the outcome as read from the code.
  - Not run: no build, test, sanitizer, fuzzing, or benchmark; `quiet check` exits 1 at 14:44
    and at 15:07 (the gate VM's `night_gate.sh n1017` holds the host strictly until about 01:17).
    No VM or other host. The oracle repository's sources were not opened; only the file names in
    `strata-oracle/vectors/out` were listed. No Xapian source and no GPL Xapiand file.

Citations: `AUT:N` is `docs/design/automata.md` at `fa544a6`; `automata.hh`, `expand.hh`,
`automata_test.cc`, `expand_test.cc`, and `automata_reference.hh` are at `a29d702`; `LAY:N` is
`layers.md` at `1d60098`, and "layers `reader.hh`" is that branch's file; `F13R:N` is
`format-13-design.md` at `4266374`; `SYS:N` is `system-2026-10-17.md` at main.

## Verdict

**APPROVE WITH CONDITIONS: the automata (D1 to D4) and the sorted-key matcher (D6) are right
and well tested; the expansion layer (D7 to D9) reads main's `segment`, which the layers refactor
removes, has no path for format 13's keys, seeks more weakly than Phase 2's ruling, and runs
without a budget or a cancellation point; T4 and T6 cannot run as written. Revision 1 answers
AUT-R1 to AUT-R5 before `expand.hh` gains code beyond AUT-R3's port, and AUT-R6 to AUT-R8 and
AUT-R11 before T6's harness work.**

What holds:

- D1's literal prefix is exact. Under `next_utf8` only U+0080 to U+00FF have two spellings: a
  multi-byte sequence decodes only in shortest form, at or above 0x80, 0x800, or 0x10000
  (`unicode.hh:98-124`). Both automata stop their literal at the first such character
  (`automata.hh:118, 228`).
- `lookahead()` (`automata.hh:49-51`) never understates what `next_utf8` examines: E0 to EF read
  both continuation bytes before deciding, F0 to F4 all three (`unicode.hh:105, 114`), so each
  recorded `seen` is exact or conservative.
- The glob is the bit-parallel NFA. After collapsing, no two stars are adjacent
  (`automata.hh:117`), so one closure pass is exact (182-189), and `can_match` is exact because
  every live position can still reach the accepting bit.
- The Levenshtein band holds [j − k, j + k] ∩ [0, n] in every row (`lo`, `automata.hh:303-306`);
  every cell outside it exceeds k; the swap band carries d(i − 2, j − 1) + 1 exactly when target
  character i equals the last term character read (283), which the swap rule needs (280) for the
  optimal-string-alignment distance.
- The matcher is correct for keys in any order, not only increasing: it resumes from the
  previous key's shared bytes and recorded `seen`, and answers a dead prefix from bytes alone
  (`automata.hh:344-377`). Increasing order buys speed only. A termdict walk (AUT-R3) can probe
  it with block prefixes.
- The limit algebra is right: `error` stops a segment at max + 1 matches, `first` at max (a term
  among the union's first max is among each holding segment's first max), and `most_frequent`
  walks everything (`expand.hh:173-181, 203-213`).
- The run-cursor replay gives every matched term `find_term`'s info after any seek: the first
  entry `scan` returns at or past `from` is either one `find_run` visited or a restart entry,
  which is absolute (`format.hh:446-461`; `termdict.hh:392-434, 479-494`). `expand_test` seeks
  at every dead entry and compares (`expand_test.cc:101-108`).
- The tests are built the right way: a brute force independent of the simulations
  (`automata_reference.hh:27-69`), every {a, b} string to length 10, globs of 80 elements across
  two state words, k from 0 to 3, fixed prefixes past the target, transpositions both ways,
  every matched info against `scan_terms` and `find_term`, limits of 0, 1, 3, and 20 over
  segments sharing terms, and `or` counts with deletes.
- The design cites only documentation and Schulz and Mihov (AUT:17-23), and routes its owner
  questions instead of deciding them (AUT:323-336).

What blocks:

- AUT-R1: two automaton interfaces remain, one frozen at Phase 1 and owed a check nobody holds.
- AUT-R2: D7 seeks past one dead subtree at a time; Phase 2 ruled automaton-driven seeking, and
  D5 has no operation for it.
- AUT-R3: `expand.hh` does not compile against layers' `basic_segment`, and it copies
  `find_term`'s run-cursor replay.
- AUT-R4: no path for keys: after format 13, `QK*` expands to nothing, and every pattern with an
  empty literal scans every key.
- AUT-R5: terms held only by deleted documents fill `first` slots and trip `error`; Xapiand's
  constructions make two unasked semantics load-bearing.
- AUT-R6: no D11 row names a mechanism that bounds anything.
- AUT-R7: the walk never yields, checks a deadline, or observes cancellation.
- AUT-R8: T6 has no query set, no engine support, and contradicts itself on equal counts.
- AUT-R11: the combiner every Xapiand path uses is not built, and constant score, which T6
  needs, is deferred on a wrong citation.

## Rulings on D1 to D12

| Decision | Ruling | Findings |
|---|---|---|
| D1 Characters | Sound. Aliasing and encoded surrogates (`unicode.hh:104-111` accepts ED A0 80) get oracle vectors | AUT-R9 |
| D2 Prefix | Sound: a byte range, Xapian's OP_WILDCARD without pattern flags; under format 13 the range is routed to `inverted`, `keys`, or both | AUT-R4 |
| D3 Glob | Sound with changes: end-anchoring under pattern flags becomes an oracle question, since Xapiand strips a trailing `*` before building patterns; `next` for seeking; the state is charged to the query budget | AUT-R2, R5, R6 |
| D4 Levenshtein | Sound with changes: the simulation as the one implementation, the compiled DFA a Track P candidate chosen by a branch A/B; the transposition preset is the oracle's; the cap is not load-bearing; `next` for seeking | AUT-R2, R6, R9 |
| D5 Automaton interface | Sound with changes: strata's one automaton interface, with `next` and a stated contract; fst's concept goes to the owner | AUT-R1, R2 |
| D6 Sorted-key walk | Sound with changes: state that any key order is correct; return a seek target; test depth invariance; restate the memory bound and charge it | AUT-R2, R6, R10 |
| D7 Dictionary intersection | Wrong as placed: it reads main's `segment` and decodes `inverted`'s term infos itself. The alternative: an inverted-reader parameter now, one seekable term cursor in the reader after layers lands, termdict's seek now (AUT-R3); keys (AUT-R4); seeking (AUT-R2); budget and cancellation (AUT-R7) | AUT-R2, R3, R4, R7 |
| D8 Expansion and Xapian's limits | Sound with changes: a rule for terms with no live document; a k-way merge; key leaves; the limit counts both sources | AUT-R4, R5 |
| D9 Scoring | Sound with changes: constant score built here; O4 decided now; one per-segment info store | AUT-R11 |
| D10 Where the code lives | Sound with changes | AUT-R12 |
| D11 Limits | Wrong as stated: the edit-distance mechanism is not the code's, and the "no limit" rows name sizes, not bounds. The alternative: an uncapped k and D13a's query budget | AUT-R6 |
| D12 Errors | Sound with changes: the k error goes with the cap; D13a's budget error and cancellation join | AUT-R6, R7 |
| Scale | Sound with changes: a global-statistics synonym needs the candidate round's term set | AUT-R5 |

## Findings

### What the branch builds on

1. **AUT-R1. D5 is the right interface for strata, but fst's frozen concept stays a second one,
   and Phase 1's carried check has no owner** (D5; SYS-15).
   - fst's concept is `start()`, `accept(state, byte)`, `is_match`, `can_match`, with the state
     held by value in each stack frame (`fst.hh:404-433`, frame at 409), plus
     `prefix_automaton` (`fst.hh:662-675`). PLAN freezes `intersect(automaton)` as Phase 1 API
     (PLAN.md:223-224) and promised brute force on 100,000 random patterns (236). fst's tests
     and fuzz target use prefix automata only (`test/test.cc:75, 169, 188`;
     `fuzz/fuzz_map.cc:20`). The Phase 1 review carried the rest here: "Phase 5 repeats the
     brute-force check with Levenshtein and wildcard automata" (phase-1.md:245-247).
   - D5 defines a second concept, over characters, with caller storage (AUT:129-138), and states
     fst's as unused (AUT:143-145). That answers SYS-15's literal ask (SYS:606-607). Nothing in
     strata or termdict calls `fst::map::intersect`, and stage 1 already shrank fst's role to the
     block-tree index (stage-1.md:516).
   - D5 is the better of the two for strata. D1's lenient decoding needs characters with
     lookahead, and fst's by-value state would copy a Levenshtein state of up to 4k + 3 words
     (`automata.hh:238`), or a multi-word glob state, at every transition. A second prefix
     matcher exists as well: `fst::prefix_automaton` beside `automata::prefix`
     (`automata.hh:87-100`).
   - Fix:
     1. D5 is strata's one automaton interface. It states the contract: the state's size from
        `words()`, states copyable as plain words, the automaton immutable and shareable across
        threads (which T5 then tests, AUT-R10), and AUT-R2's `next`.
     2. A new owner question, O6: retire `fst::map::intersect` and `prefix_automaton`
        (recommended: no consumer, and a frozen API tested only with prefixes), or keep them with
        a byte adapter over D5 and the check phase-1.md:245-247 promised. The lead amends
        PLAN.md:223-224 and 236 with the answer.
     3. Until O6 is answered, T1 states that it carries phase-1.md's item 4 for strata's walk only.

2. **AUT-R2. D7 seeks past one dead subtree at a time; Phase 2 ruled automaton-driven seeking,
   and D5 has no operation for it** (D5, D6, D7).
   - The design that fixed termdict's first-key index committed to it: "compute the smallest
     string the automaton can accept at or after the current position, `floor_value` to its
     block, scan ... and repeat from the next string past the block. First-term keys are enough
     for this" (phase-2.md:96-100). The Phase 2 review ruled the same, D14: "Use
     automaton-driven seeking instead" (phase-2-design.md:117-122). The automata design cites
     neither.
   - D7 seeks to the successor of a dead prefix after 64 consecutive entries under it
     (AUT:188-191; `expand.hh:115-125`). For `ob?ma` the literal is `ob`; every key `obXY` with
     Y ≠ `m` dies at its fourth byte under its own 4-byte prefix, so `dead_run` restarts at 1 for
     each (`expand.hh:115`), and a seek fires only where 64 keys share one such prefix;
     otherwise the walk decodes every entry of [`ob`, `oc`). Automaton-driven seeking jumps from
     `obaa` to `obam`, and from `obama` past the rest to `obb` + `ma`: about two entries per
     spelling of the `?`.
   - The live next characters are computable without determinizing. For a glob: any character
     when a live position holds `?` or `*`, else the live positions' literals. For Levenshtein:
     any character while a live cell is below k, else only the target characters at cells equal
     to k and the swap band's (`automata.hh:274-284`).
   - D5 offers `step`, `is_match`, and `can_match` (AUT:129-138). Nothing returns the smallest
     live character, so neither the matcher nor termdict (O2) can compute a seek target.
   - Why it matters: T6's wildcard and fuzzy rows with a short or empty literal, the design's own
     first risk (AUT:316-317).
   - Fix:
     1. D5 gains `next(state, c)`: the smallest character at or above c that keeps the state
        alive, or none.
     2. D6's matcher returns a seek target with each dead result: the bytes up to the deepest
        boundary whose `seen` does not pass its offset, then the smaller spelling of
        `next(state, c + 1)` (for U+0080 to U+00FF, the smaller of the lone byte and the
        two-byte form); with none, it backs up one boundary.
     3. D7 seeks to that target on every dead result once termdict's seek is cheap (AUT-R3);
        `seek_after` remains a preset until then.
     4. Or D7 keeps dead-prefix seeking and records the deviation from phase-2.md:96-100 with a
        branch A/B under a granted hold (C3): entries visited and time per query on the 5M
        segment for `ob?ma`, `obama~2`, and `*ma`.

3. **AUT-R3. `expand.hh` reads main's `segment`, which layers step 6 removed, and its
   run-cursor replay is a second copy of `find_term`'s** (D7, D10; SYS-9).
   - `expand.hh` calls `seg.dictionary()` (58), `seg.doc_count()` (105), `seg.complete_impacts()`
     (235), and `term_scorer(seg, ...)` (237) on main's `segment`, and D10 says layers'
     `basic_segment` "takes it unchanged" (AUT:260-263). At `1d60098`, `basic_segment` exposes
     `core()`, `get<P>()`, `deps<P>()`, `holds<P>()`, and `verify_all()` (layers
     `reader.hh:751-768`); `term_scorer` takes `const inverted_plugin::reader&` (layers
     `search.hh:312-315`); `snapshot_statistics` reads `get<inverted_plugin>()` (layers
     `searcher.hh:94-108`). None of `expand.hh`'s calls compile there.
   - Term-info decoding is `inverted`'s format (LAY:97). `expand.hh` decodes it itself
     (`expand.hh:72-104`), replaying the run cursor with `find_run` exactly as `find_term` does
     (main `reader.hh:324-342`; layers `reader.hh:325-343`). That is one mechanism twice: the
     term info at an arbitrary dictionary position.
   - SYS-9 ruled "puts `intersect` behind the inverted reader ... before writing `expand.hh`"
     (SYS:451-453), and F13-R2's fifth point repeats it (F13R:282-283). The system review read
     origin at `d84bfc2` (SYS:12); `a29d702` had been committed at 11:48 and is still unpushed.
   - termdict's `scan(from)` decodes and builds every key of `from`'s block below it
     (`termdict.hh:479-494`, the skip at 488), so a seek costs up to 64 entry decodes plus the
     `find_run` replay (AUT:180-187), where a seek by restart binary search and `find_from`'s
     skipping (`termdict.hh:392-434, 466-478`) costs one restart run.
   - Why it matters: layers is the critical path (the index's queue, README.md:37) and lands
     before this branch; format 13's F10 and F13 may change the term info D7 decodes (SYS-9).
   - Fix:
     1. Now: `intersect` takes the inverted reader, and `expand` and `expansion_or` take a
        segment-to-reader accessor. Main's `segment` and layers' `seg.get<inverted_plugin>()`
        both provide `dictionary()`, `doc_count()`, and `complete_impacts()`, so one source
        compiles on both.
     2. After layers lands: one seekable term cursor in `inverted_plugin::reader` (`seek(from)`
        on `term_cursor`), used by `find_term`, `scan_terms`, and `intersect`; `expand.hh`
        keeps no term-info decoding.
     3. O2 now, not after T6: termdict gains an additive `reader::cursor::seek(from)` (the FST
        floor, the restart binary search, `find_from`'s skipping without building keys). It
        makes AUT-R2's seeks cheap and serves the termdict-based key candidates K1, K2, and K4
        (format-13.md:198-205). Block skipping by the first keys' common prefix is an option on
        top. Only strata links termdict, so no other consumer is affected. O2's alternative,
        exposing the FST and block opening, puts termdict's block format in strata and is wrong.

4. **AUT-R4. The design has no path for keys: after format 13, `QK*` expands to nothing, and
   every pattern with an empty literal scans every key** (D7, D8; F13-R2; layers L3).
   - Format 13 takes every key out of `terms`; dictionary walks never see one (F13R:248-252).
     Xapiand lists indexes with OP_WILDCARD over `QK<path>` (`search_views.cc:2292-2294`, as
     F13R:250-251 cites).
   - Both designs adopted one routing rule (LAY:281-284; F13R:271-279): a literal that starts with
     the key prefix expands over `keys` only; a literal that is a proper prefix of it, the empty
     one included, over both, merged in byte order; any other over `inverted` only. Limits count
     both sources, and each expanded key is a key leaf whose df counts the segments holding it,
     deleted copies included (LAY:272-276).
   - `automata.md` never mentions keys; `expand` walks the dictionary only (`expand.hh:169-181`),
     and `expansion::segments` holds term infos only (157-167).
   - The keys walk has two interfaces that disagree. Layers' concept scans a prefix, "not a lower
     bound" (LAY:238-249; layers `reader.hh:493-496`); format 13's reader scans from a lower
     bound (`keys.hh:186-195` at `4266374`). A prefix scan cannot seek.
   - On the benchmark corpus every document has a key, `Q` + id (`strata_engine.cc:101, 152`),
     about 5 million (BENCHMARKS.md:67), beside 3,964,752 terms (phase-2.md:219-220).
     `obama~2` with no fixed prefix, `*ma`, and `?bama` all have an empty literal, so each walks
     every key under the rule, on top of the dictionary. With a lower-bound scan, the matcher's
     dead prefixes skip whole key ranges: under `obama~2`, a key whose two bytes after `Q` are
     not among `o`, `b`, `a`, `m` is dead at its third byte.
   - Why it matters: Xapiand's index listing returns nothing once format 13 lands, and T6 on the
     keyed corpus pays a whole key scan per fuzzy query unless the keys walk seeks.
   - Fix:
     1. A decision for keys: F13-R2's routing exactly, refined by one exact test that costs a
        few steps: run the matcher over the key prefix itself and skip `keys` when it reports a
        dead prefix within the key prefix.
     2. The keys walk uses the same matcher and seek targets as the dictionary walk (D6,
        AUT-R2), so `keys`' scan takes a lower bound, as `keys.hh:186-195` does. The lead rules
        L3's concept to that form (C5).
     3. `expansion` carries key leaves (the key, and per segment its ordinal) beside term infos.
        The limits apply to the merged set.
     4. Format 12 keeps keys in the dictionary, so the source is chosen by layout
        (`search_layout_12` against `search_layout`, LAY:187-188).
     5. T2 gains a row: an expansion over a keyed format-13 index equals the format-12 expansion
        of the same documents.

5. **AUT-R5. D8 follows Xapian's documented limit types, but terms with no live document count
   and get selected, and Xapiand's constructions make two unasked semantics load-bearing**
   (D3, D8, D9, Scale).
   - D8 counts and ranks by snapshot df, deleted documents included (AUT:208-212;
     `expand.hh:199`). A term whose every posting is deleted stays in its segment's dictionary
     until a merge, so it takes a `first` slot, counts toward `error`, and ranks in
     `most_frequent`. `probe_deleted.cc`: from the code, with `obamaa` held only by a deleted
     document and `obamab` by a live one, `first` 1 selects `obamaa`, the query then counts 0
     live documents, `most_frequent` 1 also selects `obamaa` (a df tie, broken by term order),
     and `error` 1 throws over two terms. A Xapian database whose deletes are committed holds no
     posting for `obamaa`. Xapiand's partial matching selects the 50 most frequent
     (`query_dsl.cc:67, 732-743`), so an updated document's old terms take those slots. O3 does
     not ask how Xapian treats such terms.
   - D3 anchors a pattern at both ends under pattern flags (AUT:77-80). Xapiand strips a keyword
     term's trailing `*` before it builds a pattern with flags (`query_dsl.cc:710-717,
     745-759`), so `f?o*` reaches the engine as `f?o` with WILDCARD_PATTERN_SINGLE and MULTI.
     Whether Xapian anchors the end decides which terms Xapiand's users get. O3 does not ask.
   - Every multi-term query Xapiand builds combines with OP_SYNONYM (`query_dsl.cc:741, 758,
     770`, and the `*` queries at 647 and 681, which take the constructor's default, AUT:235).
     D9 builds `or` (AUT-R11).
   - Per-shard limits (AUT:294-299) are routed to O3; sound. A global-statistics synonym needs one
     term set across shards, which per-shard expansion does not give.
   - `expand` copies every matched term once per segment as a `std::string` (`expand.hh:177`),
     then sorts every (term, segment) pair (191). A k-way merge of the segments' ordered walks
     copies each term once and stops `first` and `error` at the merged count.
   - Fix:
     1. D8 states the rule for a term with no live document. Recommended: on segments with
        deletes, exclude it from selection with a liveness test that stops at its first live
        posting (a single posting's ordinal is inline), and keep D13's df for ranking and
        weights, so the remaining difference is the statistics drift the parity gate already
        measures. The alternative is to state the divergence and measure it in T4 with deletes.
     2. O3 gains five questions: end-anchoring under pattern flags; terms with no live document
        under each limit type; aliasing and encoded surrogates; OP_WILDCARD and OP_EDIT_DISTANCE
        inside OP_PHRASE and OP_NEAR; and a byte count as `fixed_prefix_len`, which is what
        Xapiand passes (`query_dsl.cc:772`).
     3. The expansion merges the segments' walks in order, copies a term once, and stops at the
        merged limit.
     4. "Scale" states that a global-statistics synonym needs the candidate round's term set.

### The rest

6. **AUT-R6. No D11 row is load-bearing as written** (D6, D11, D12).
   - Edit distance: "Band cells hold k + 1 in 32 bits" (AUT:279). Cells are 64-bit words
     (`using word = std::uint64_t`, `automata.hh:46`; the band at 274-283), and the cap (217) is
     enforced at 221 and needed by nothing. D11 gives Xapian's distance as unsigned (AUT:279), whose
     largest is 2^32 − 1, so 2^32 − 2 is one below it and "At the largest" is false. A k past
     the longest key plus the target accepts every key, so no cap above that changes an answer.
     Tantivy's cell holds no value (AUT:279).
   - Pattern length, "No limit", mechanism "the glob state is ⌈(m + 1) / 64⌉ words; the
     Levenshtein band is bounded by k" (AUT:280). That sizes a state; it bounds nothing. Each
     character of each visited key costs O(words) for a glob (`automata.hh:160-169`) and O(w)
     for Levenshtein (274-284), and the matcher keeps up to 4,097 states (369-374). A glob of
     10^6 `?` keeps every key alive to its end, at 15,626 words per step and a 125,008-byte state
     per recorded boundary. A 10,000-character target at k = 10,000 has w = 10,001 and 20,003
     words with transpositions (238): 160,024 bytes per boundary, 16 MB for a 100-character key
     and 656 MB at D6's 4,097 boundaries (AUT:162-166). Both come from one query.
   - Expanded terms, "No limit", mechanism "Memory O(matched terms)" (AUT:281): a size again.
     Phase 4 names the mechanism such state uses: D13a's query budget, a `byte_budget` charged
     with decoder state, whose exhaustion fails the query (phase-4.md:373-381). `byte_budget`'s
     permits exist today (`budget.hh:15-60`, `try_grow`).
   - Why it matters: limits.md:3-8, and Goal 5. An untrusted query allocates and computes without
     a named bound.
   - Fix:
     1. Edit distance: accept every `std::uint32_t` k, Xapian's whole range (the cells are
        already 64-bit), and name the row's mechanism as "none needed". Or pack cells in 32 bits,
        which halves the state, and name that.
     2. Pattern length and expanded terms: D13a's budget is the mechanism. The matcher's recorded
        states, the expansion's terms and infos, and the scorers built from them draw on a permit;
        a failed `try_grow` fails the query with D13a's budget error, which D12 gains.
     3. D6's sentence states the bound as 4,097 × (words + 2) words, with words from the
        pattern, charged to the permit.
     4. Tantivy's cell gets a value from its documentation, or the reason none is published.

7. **AUT-R7. The walk never yields, checks a deadline, or observes cancellation** (D7, D8).
   - `intersect` and `expand` run to completion (`expand.hh:56-134, 169-223`). An empty literal
     walks the whole dictionary: 3,964,752 terms on the 5M corpus, plus about 5 million keys while
     keys are terms (format 12) or under AUT-R4's rule.
   - ARCHITECTURE's suspension points are query start, segment boundaries, window boundaries, and
     stored fetches, with a 100 to 250 µs slice read once per window (ARCHITECTURE.md:231-238).
     The rewrite step (180-181) has none. FEASIBILITY promises cancellation and time limits at
     yield points (FEASIBILITY.md:146), and Phase 6 gates a 1 ms p99 reactor stall
     (PLAN.md:559).
   - The hooks exist: the executor's `checkpoint(stop)` (`executor.hh:45`) and the per-iteration
     `cancel_token` check in `bisection.hh:454`.
   - Fix:
     1. D7 checks the query's cancellation and deadline every N entries (a preset, measured) and
        can resume at the last key, so a time-sliced query yields inside a walk; the walk's
        slice joins ARCHITECTURE's budget rule.
     2. A test cancels a whole-dictionary walk and checks it stops within N entries.

8. **AUT-R8. T6 cannot run as written, and its equal-semantics clause contradicts its count
   clause** (T6, T7).
   - No query set: BENCHMARKS gives three examples (99). The harness's query files live on the
     gate VM (`query_trials.sh:13-17`), the repository holds no sample of the class, and the quick
     tier samples by tag (`sample_queries.py:1-34`).
   - Engines: the Lucene harness parses with the classic QueryParser (`DoQuery.java:30, 37`),
     which builds PrefixQuery, WildcardQuery, and FuzzyQuery from `obam*`, `ob?ma`, and
     `obama~2`. The Tantivy harness uses its QueryParser (`do_query.rs:143-147, 161`). T6's
     "regex-free wildcard" (AUT:310) is not one of Tantivy's documented query types; RegexQuery
     and FuzzyTermQuery are the ones that apply, so the harness must build the queries and T6
     must say how. The Xapian
     harness has no OP_WILDCARD, OP_EDIT_DISTANCE, or QueryParser (a grep over
     `bench/engines/xapian-2.1.0`). `strata_engine`'s syntax is words, `+`, `-`, and phrases
     (`engine_query.hh:4`).
   - "Each with its default rewrite" against "counts equal every engine's" (AUT:310;
     BENCHMARKS.md:137): Lucene's FuzzyQuery rewrites by default to the 50 best terms by edit
     distance (its documented `TopTermsBlendedFreqScoringRewrite`, `defaultMaxExpansions` 50),
     so its match set differs from every other engine's whenever `obama~2` expands past 50 terms.
   - Scoring: Lucene's prefix and wildcard rewrites and Tantivy's automaton queries score a
     constant (AUT:246-247); strata builds `or`; Xapian's default is OP_SYNONYM. BENCHMARKS rule 5
     asks for the same scoring function or count-only collectors (42-45). Only the COUNT rows
     meet it today.
   - Segments: PLAN's ratchet rows run queries before and after merging (PLAN.md:325), and an
     expansion walks each segment (`expand.hh:175-181`). T6 names neither.
   - "T6 also decides D4's compiled DFA, D7's termdict change (O2), and D9's bitset union"
     (AUT:310). T6 runs on main only (PLAN.md:16-20); a choice between builds is a branch A/B
     under a hold the lead grants, evidence, never a gate row (SYS-6).
   - T7's procedure, "the allocation counter (BENCHMARKS' property row)" (AUT:311), names no
     build, binary, or query file.
   - Fix:
     1. A committed, tagged query set for the class: prefix, glob, and fuzzy at k 1 and 2, with
        fixed prefixes 0 and 2, drawn by seed from the 5M vocabulary and stratified by expansion
        size, as Phase 2's term class is by df (PLAN.md:272-273).
     2. Per-engine configuration per row so match sets are equal: Lucene's FuzzyQuery with its
        constant-score rewrite rather than the default; Tantivy's RegexQuery for prefixes and
        globs, and FuzzyTermQuery; Xapian's OP_WILDCARD and OP_EDIT_DISTANCE with max_expansion 0;
        `strata_engine` parsing the three forms. A query whose counts differ is dropped with its
        reason in the report.
     3. Scoring per comparison: constant score on all three engines against Lucene and Tantivy
        (strata's from AUT-R11); against Xapian, its OP_OR combiner against strata's `or` now,
        and OP_SYNONYM against strata's synonym once O4's scorer lands.
     4. Before and after merging, as Phase 3's rows.
     5. The choices T6 "decides" move to branch A/Bs (C3); T6 measures what landed.
     6. T7 names the allocation-counting build, the binary, and the query file.

9. **AUT-R9. T4 names tooling that does not exist, and D4's preset rests on a sentence the
   clean room cannot use** (T4, O3, D4).
   - The oracle records `chars`, `infix`, `parse`, `rank`, `termgen`, `termgen-xapiand`,
     `unicode`, and `values` vectors (`strata-oracle/vectors/out`); none is an expansion.
     `tools/parity.cc` compares counts and top-10 weights from rank vectors (`parity.cc:1-11`)
     and parses no wildcard or edit-distance description.
   - T4's corpus is analyzed text, valid UTF-8, so D1's aliasing (AUT:51-54) is never compared
     with Xapian, while Xapiand's keyword and binary-valued terms are not UTF-8 in general.
   - D4's transposition preset cites ARCHITECTURE.md:345 ("adjacent transpositions, over code
     points"), whose source is stage-1.md:1144-1146, which cites a Xapian source file. Under
     FEASIBILITY's procedure a behavior comes from documentation and observed output, never from
     source (FEASIBILITY.md:162-173); D4 already defers to the oracle (AUT:105-106).
   - Fix:
     1. T4's procedure names the oracle's expansion mode (its inputs: patterns, flags, limits, k,
        fixed prefixes; its output: each expansion's terms in order, per limit type), the corpora
        (the oracle's, a keyword corpus with invalid UTF-8, and a variant with deletes), and
        `parity`'s comparator. The lead assigns the GPL-side work.
     2. O3 gains AUT-R5's five questions, each answered by a vector before T4 counts.
     3. D4's preset is whatever O3's vector shows, and the design cites the vector, not
        ARCHITECTURE's sentence.

10. **AUT-R10. The brute-force tests catch most of what T1 claims, with five gaps** (T1, T2,
    T3, T5).
    1. **A use-after-free in the committed test.** `automata::decode(any_term())` keeps views of
       a temporary (`automata_test.cc:118-120`). CI run 39059258216 at `d84bfc2` fails
       `linux-tsan` with "heap-use-after-free ... automata_test.cc:120:82". The worktree fixes the
       test, uncommitted; `decode`'s views into its argument (`automata.hh:59-70`) invite the same
       mistake elsewhere.
    2. **The 4-byte lookahead is not pinned.** No two keys in `automata_test` share a proper
       prefix of a valid 4-byte sequence: the bytes 9D and 84 occur only inside the U+1D11E piece
       (`automata_test.cc:97-99`), the only other F0 sequence, F0 then 82, is overlong whatever
       follows, and no F1 to F4 byte occurs. `expand_test`'s pieces hold no 4-byte character
       (`expand_test.cc:183`). A `lookahead()` returning 3 for F0 to F4 never takes a wrong resume
       there; the dead-prefix extension check (`automata_test.cc:80-88`) reaches it only if a
       random glob or target accepts U+00F0 where a key holds U+1D11E. `probe_lookahead.cc` adds
       every cut of a valid sequence per lead class with each tail. From the code, under
       `glob("x?")` the mutant reports the whole key `x F0 9D 84` as a dead prefix, where the
       correct walk reports none (deciding F0 read past the key's end), and then answers
       `x F0 9D 84 80`, which decodes as `x` and U+1D100 and matches, as dead.
    3. **Depth is never varied.** Every walk uses the default depth (`automata.hh:335, 402`).
       The only key sure to pass 4,096 characters is a run of `a` (`automata_test.cc:201`); the
       keys sharing a long base pass it only when `rng(5)` draws a base above about 4,096 pieces
       from a uniform [0, 4,500] (`automata_test.cc:101-104, 199`). `probe_depth.cc` runs the
       matcher at depths 0, 1, 2, 3, and 4,096 over keys sharing 4,100 pieces; by D6 all five
       agree with the reference.
    4. **The segment half.** T1 claims terms past one block's 1 KiB and the empty term
       (AUT:305). `expand_test`'s terms hold at most 24 bytes (`expand_test.cc:190-199, 223`) and
       it erases the empty term (224). No test reaches the intersection's two corruption checks, a
       df above the document count and a failed region check (`expand.hh:105, 131-132`), and no
       fuzz target runs an automaton over a corrupted segment.
    5. **Threads.** D5 says one automaton serves every segment on any thread (AUT:140-142); no
       test runs matchers on several threads over one automaton, and PLAN asks for TSan where
       threads are involved (PLAN.md:173).
    - Procedures: T1 names `automata_test.cc` only, and T2 names it though T2 lives in
      `expand_test.cc` (AUT:305-306). T5 places `expand.hh` with "everything else" (AUT:309),
      but the intersection decodes on-disk term infos, which TESTING holds to the reader targets:
      at least 98% of lines and 95% of branches, every corruption check reached (TESTING.md:35).
    - Fix: commit the test fix and delete `decode`'s rvalue `std::string` overload; put
      `probe_lookahead`'s set and `probe_depth`'s invariance into `automata_test`; give
      `expand_test` a 4-byte piece, a 2 KiB term, and corrupt-block and corrupt-df cases; run an
      intersection over corrupted segments in a fuzz target, with its hour at the gate
      (PLAN.md:174-176); add a TSan test; correct T1's and T2's procedures and T5's class.

11. **AUT-R11. D9 builds the one combiner no Xapiand path uses, defers the one they all use, and
    rests constant score on the wrong cover** (D9; O4).
    - Xapiand's multi-term queries combine with OP_SYNONYM (AUT-R5). D9 builds `or`
      (`expand.hh:229-242`) and leaves synonym to O4 (AUT:239-241, 332-333), so T4's scores, T6
      against Xapian, and every Xapiand path wait on a scorer nobody is assigned.
    - "Xapiand needs it for 'exists' as a boolean filter, which Phase 4's scored filters cover"
      (AUT:247-249). Phase 4's scored filters are iterators over values, ranges and geo, with
      `weight()` and `max_weight()` (phase-4.md:265-275); none takes a term set. "Exists" is
      OP_WILDCARD over the field prefix with OP_SYNONYM (`query_dsl.cc:642-647`).
    - T6 needs constant score to compare top-k rows with Lucene and Tantivy under equal
      semantics (AUT-R8).
    - `expansion_or` completes the impacts of every expanded term at setup (`expand.hh:235`),
      decoding each short list's postings, where `term_table` defers that until a scorer asks
      (`searcher.hh:116-145`). Two per-query, per-segment term-info stores now exist:
      `term_table`'s columns and `expansion::segments`.
    - Fix:
      1. O4 decided now. Recommended: a synonym-operator item ahead of `queryparser`, which also
         serves the DSL's explicit `_synonym` (`query_dsl.cc:332-334`).
      2. Constant score built in this branch: a union that does not score (a disjunction without
         weights, or a `docset` bitset as the Track P choice), for T6 and for FILTER contexts.
         The citation is corrected.
      3. One store: the expansion fills `term_table` through a keyed insert (its `column()` scans
         linearly, `searcher.hh:154-155`, which is quadratic in expanded terms), or multi-term
         leaves use the expansion as their store. The design says which.

12. **AUT-R12. Where the code lives** (D10; O1, O5).
    - `automata.hh` depends only on `unicode` (`automata.hh:30`), as D10 says. ARCHITECTURE lists
      `automata` as a new standalone library (ARCHITECTURE.md:324), and stage 1 merged the
      planned `levenshtein` into it (stage-1.md:518). The catalog holds nothing it duplicates.
    - The test reference's distance (`automata_reference.hh:46-58`) is the one ARCHITECTURE
      assigns to `string-similarity` (ARCHITECTURE.md:345; stage-1.md:1148-1151), whose
      Levenshtein is byte-level and normalized (`levenshtein.h`'s `_distance`), as the design
      notes (AUT:40-42). Calibration's first row makes the private copy a finding. Xapiand pins
      the library (Xapiand `CMakeLists.txt:923`), so the change is additive.
    - D10 puts termdict's intersection "over its interface" (AUT:266), which makes termdict
      depend on automata; a seekable cursor (AUT-R3) or a concept-typed parameter needs no
      dependency.
    - Ruling:
      1. `automata.hh` stays in strata, library-shaped, until AUT-R1 and AUT-R2's interface and
         termdict's seek exist; then it moves to `Kronuz/automata`. Creating that repository is
         O1, the owner's; recommended yes, at that point.
      2. `expand.hh` stays in strata, without term-info decoding (AUT-R3).
      3. termdict's addition does not depend on automata.
      4. O5 is decided by ARCHITECTURE and stage 1: `string-similarity` gains an integer,
         code-point, optimal-string-alignment distance, additively, and the test reference calls
         it. It stays independent of the automaton, which T1 needs.

13. **AUT-R13. Phase 1 left the postings-tail question for this phase; it must be answered
    before format 13 freezes** (T6; format 13's F11).
    - "LEB128 stays on size ... Revisit it under the format version in Phase 5 if tail decode
      dominates expansion queries" (phase-1.md:239-244). Stage 1's reason: most terms have fewer
      than 128 postings, "and prefix, wildcard, and fuzzy expansions touch thousands of them"
      (stage-1.md:1102-1104); 54% of the 3,964,752 terms have one posting (phase-2.md:219-220).
    - Format 13 is that format version. Its tail candidate, E4, is chosen by size and by format
      13's own count and intersection rows (format-13.md:274-298), not by expansion queries; after
      the freeze a tail change is format 14. The automata design does not mention the item.
    - Fix: before format 13 freezes, a branch profile under a granted hold (C3: evidence) of the
      class's queries on the 5M segment, `or` and count mode, states the share of time in tail
      decode. If it dominates, E4's decision weighs the class. The lead adds this to format 13's
      freeze criteria (C4).

14. **AUT-R14. The ledger, the criteria, and CI** (process; SYS-16).
    - The ledger has no done criteria (`docs/status/automata.md:1-13`).
    - It names design commit `0557fa3`, the pre-rebase twin of `fa544a6`; it says "Touches
      existing files: none yet" while `d84bfc2` and `a29d702` add 4 and 3 lines to
      `CMakeLists.txt`; it says "State: designing" and "Doing: implementing D1-D6" while D1 to D9
      are committed; and "Last verified: nothing built yet" (`automata.md:2, 4, 8, 12, 13`). The
      index says "designing" (README.md:16). SYS-16 found the first two (SYS:613-615); they
      stand.
    - CI at `d84bfc2`, run 39059258216: `linux-tsan` fails on the branch's test (AUT-R10 1.);
      `macos-x86_64`, `macos-arm64`, and `freebsd-x86_64` fail to build on main's `std::jthread`
      (`bisection.hh:541`), which main fixed in `adb329e` (run 39071019926 at `5691ec7`: seven
      jobs green, TSan running at 15:05); `linux-asan-ubsan` stops at its 60-minute limit after
      26 of 34 tests, before `automata`. `a29d702` has never run CI.
    - D10 sends the parity rows to "T9" (AUT:271); the test-parity row is T8 (AUT:312).
      ARCHITECTURE's table says "Levenshtein DFAs" and "edit distances come from
      string-similarity" (ARCHITECTURE.md:324), and D4 simulates instead (AUT:120-125).
    - Fix: the lead writes the criteria (below, or its own); the contributor corrects the ledger,
      rebases onto main past `adb329e`, pushes, and gets all eight jobs green at every commit a
      criterion cites; "T9" becomes T8; the lead corrects ARCHITECTURE.md:324 with revision 1.

## Gate rows

| Row | Target | Procedure | Ruling |
|---|---|---|---|
| T1 Brute force | Yes | Partly: names `automata_test.cc` only, while `expand_test.cc` holds the segment half | AUT-R10 |
| T2 Expansion | Yes | Names the wrong file; no keys or deleted-only cases | AUT-R4, R5, R10 |
| T3 Fuzzing | Yes | Yes once `fuzz_automata.cc` is committed; a corrupted-segment path is owed | AUT-R10 |
| T4 Oracle parity | Yes | No: the oracle records no expansion vectors, and `parity` has no comparator | AUT-R9 |
| T5 Sanitizers and coverage | The coverage class is wrong for the intersection; no TSan | A targeted run; CI's ASan job has not reached the test | AUT-R10, R14 |
| T6 Speed | Contradictory: default rewrites against equal counts | No: no query set, and three engines and `strata_engine` lack the class | AUT-R8 |
| T7 Memory | Yes | No build, binary, or query file named | AUT-R8 |
| T8 Test parity | Yes | Yes | — |
| Missing | — | — | A keys-equivalence row (AUT-R4), a cancellation row (AUT-R7), the tail-decode profile (AUT-R13), and Phase 1's fst check or its retirement (AUT-R1) |

## Done criteria

The ledger has none. Proposed, for the lead to write or replace:

- K1. Rebased onto main past `adb329e`; all eight CI jobs green at every commit a criterion cites,
  ASan and UBSan included (calibration, second row).
- K2. T1 passes with AUT-R10's additions: the 4-byte cut set, depth invariance at 0, 1, 2, 3, and
  4,096, the use-after-free fixed, and `decode`'s rvalue overload deleted.
- K3. T2 passes with deleted-only terms under revision 1's rule (AUT-R5), and with keys under
  F13-R2's routing once format 13's keys reach the planner (AUT-R4).
- K4. `expand.hh` passes against main's `segment` now, and against layers' `basic_segment` with no
  term-info decoding of its own once layers lands (AUT-R3).
- K5. Seeking as revision 1 decides (AUT-R2): the same answers with seeks at every dead entry, and
  fewer entries visited than the dead-prefix walk on a committed set.
- K6. The walk draws on a budget permit and checks cancellation every N entries; tests exhaust a
  budget and cancel a whole-dictionary walk (AUT-R6, R7).
- K7. T3: `fuzz_automata` and its corpus committed and replayed in CI; an intersection over
  corrupted segments in a fuzz target (AUT-R10).
- K8. T5: ASan with UBSan, and TSan, over `automata`, `expand`, and the replays; coverage at the
  reader targets for the intersection and at "everything else" for the rest (AUT-R10).
- K9. Constant score built, its counts equal to brute force with deletes (AUT-R11).
- K10. `strata_engine` parses the class, and `verify` is clean on its committed query set (AUT-R8).
- K11. `limits.md` carries D11's revised rows (AUT-R6) and `parity.md` the T8 rows.
- K12. The ledger matches the repository.

## Open questions

- **O1 (owner).** Recommended yes, once AUT-R1 and AUT-R2's interface and termdict's seek exist
  (AUT-R12); until then the code stays in strata.
- **O2.** Decided by the standing rule to extend a Kronuz library additively: termdict gains a
  seekable cursor now; exposing its FST and block opening is wrong (AUT-R3).
- **O3 (oracle).** Sound; five questions more and the tooling to answer them (AUT-R5, R9).
- **O4 (lead).** Decide now: a synonym-operator item ahead of `queryparser` (AUT-R11).
- **O5.** Decided by ARCHITECTURE.md:345 and stage-1.md:1148-1151: extend `string-similarity`
  additively (AUT-R12).
- **O6 (owner, new).** `fst::map::intersect` and `prefix_automaton`: retire them (recommended), or
  keep them with a byte adapter and phase-1.md item 4's check (AUT-R1).

## Required changes

Revision 1 of the design, each item checked by a re-check appended here:

- **Before `expand.hh` gains code beyond AUT-R3's port:** AUT-R1, AUT-R2, AUT-R3, AUT-R4, and
  AUT-R5.
- **Before T6's harness work, and before an expansion reaches a query planner:** AUT-R6, AUT-R7,
  AUT-R8, and AUT-R11.
- **Before the full review:** AUT-R9, AUT-R10, AUT-R12, AUT-R13, and AUT-R14; then a full review
  of revision 1 with its code.

## Conditions

1. **C1.** The lead writes done criteria into the ledger (K1 to K12 or its own) before the next
   automata commit.
2. **C2.** All eight CI jobs, ASan and UBSan included, are green at every commit a criterion
   cites; `a29d702` and the fuzz work are pushed so CI sees them (calibration, second row).
3. **C3.** Every choice T6 was to decide (D4's compiled DFA, O2's form, D9's bitset union,
   `seek_after`, the cancellation interval) is a `docs/perf/` table from a branch A/B under a hold
   the lead grants, never a T6 row (PLAN.md:16-20, SYS-6).
4. **C4.** Before format 13 freezes: AUT-R13's profile, and the lead adds it to format 13's freeze
   criteria.
5. **C5.** The lead rules the form of `keys`' scan, LAY:238-249's prefix against `keys.hh:186-195`'s
   lower bound, between the layers and format-13 workstreams before format 13's key routing
   reaches the planner (layers' step 7c). This review recommends the lower bound.
6. **C6.** Two calibration rows, for the lead:
   1. Rulings addressed to a later phase reached its design unread: Phase 2's D14
      (phase-2-design.md:117-122) and Phase 1's items 3 and 4 (phase-1.md:239-247) were
      addressed to Phase 5, and the Phase 5 design cites none. The check: a design review lists
      every earlier review item that names its phase or its components, and rules each.
   2. The system review read `phase5/automata` on origin at `d84bfc2` while `a29d702`, with
      `expand.hh`, was committed locally, so SYS-9's "before writing `expand.hh`" arrived after
      the fact. The check: read each worktree's local head and `git status` beside origin.
7. **C7.** The lead decides whether stage-1.md:1144-1146's citation of a Xapian source file needs
   remediation under FEASIBILITY.md:162-173. D4's preset already waits for the oracle (AUT-R9).

## Which work starts when

| Work | Starts | Waits for |
|---|---|---|
| The test fix, AUT-R10's tests, `fuzz_automata` committed, the TSan test | Now; CI validates under the hold | — |
| D5's `next` and D6's seek target (AUT-R2) | Now | — |
| termdict's `cursor::seek` on a termdict branch (O2) | Now | — |
| `string-similarity`'s distance, additively (O5) | Now | — |
| `expand.hh` over an inverted-reader parameter (AUT-R3, step 1) | Now | — |
| Constant score (AUT-R11) | Now | — |
| The budget permit and cancellation checks (AUT-R6, R7) | Now | — |
| The seekable cursor in `inverted_plugin::reader` (AUT-R3, step 2) | After layers lands on main | Revision 1 |
| The keys source (AUT-R4) | After format 13's keys reach the planner | C5's ruling |
| The synonym scorer | When O4 assigns it | O4 |
| T6's harness: query set, engine configurations, `strata_engine` syntax | After revision 1's T6 | AUT-R8 |
| Branch A/Bs for C3's choices and AUT-R13's profile | Under a hold the lead grants | The pieces above |
| Landing on main | After the full review | Every required change |

## Verdict and findings, in one line each

- Verdict: APPROVE WITH CONDITIONS; revision 1 answers AUT-R1 to R5 before more `expand.hh` code,
  and AUT-R6 to R8 and R11 before T6's harness work.
- AUT-R1: D5 is strata's one automaton interface, with a stated contract; fst's frozen concept
  goes to the owner as O6, and Phase 1's carried check is closed or retired with it.
- AUT-R2: D5 gains `next` and D6 a seek target, so D7 seeks as Phase 2's D14 ruled, or the
  deviation is recorded with a branch A/B.
- AUT-R3: the intersection takes the inverted reader now and moves into its one seekable term
  cursor after layers lands; termdict gains `cursor::seek` now.
- AUT-R4: keys by F13-R2's routing, refined by the matcher over the key prefix; a lower-bound
  keys scan; key leaves in the expansion; a keys-equivalence row in T2.
- AUT-R5: a rule for terms with no live document; five more oracle questions; a k-way merge;
  per-shard sets under global statistics.
- AUT-R6: an uncapped k, and D13a's budget as the mechanism for pattern size, matcher states, and
  expanded terms.
- AUT-R7: cancellation and deadline checks every N entries inside the walk, resumable.
- AUT-R8: T6 gets a query set, per-engine configurations with equal match sets, equal scoring
  per comparison, multi-segment runs, and its choices moved to branch A/Bs; T7 gets a procedure.
- AUT-R9: T4 gets the oracle's expansion mode and a comparator; D4's preset comes from a vector.
- AUT-R10: commit the use-after-free fix; pin the 4-byte lookahead and depth invariance; complete
  the segment half; a corrupt-segment fuzz path; a TSan test; reader-class coverage.
- AUT-R11: O4 decided now; constant score built here on a correct citation; one per-segment
  term-info store.
- AUT-R12: `automata.hh` moves to `Kronuz/automata` after the interface settles; O5 extends
  `string-similarity`; termdict does not depend on automata.
- AUT-R13: the tail-decode profile before format 13 freezes.
- AUT-R14: criteria written, ledger corrected, the branch rebased, pushed, and green on all eight
  jobs.
- Blocking: AUT-R1 to AUT-R8 and AUT-R11, at the points "Required changes" names.
- Conditions: C1 (criteria in the ledger), C2 (CI green, everything pushed), C3 (branch A/Bs for
  T6's choices), C4 (the tail profile before format 13 freezes), C5 (the keys scan's form), C6 (two
  calibration rows), C7 (stage 1's citation).

Not run: no build, test, sanitizer, fuzzing, or benchmark (the host's strict hold until about
01:17), so the three probes and `run.sh` are written and not compiled, and their outcomes are read
from the code; `a29d702` and the worktree's uncommitted fuzz target have never run in CI; no VM,
bare-metal host, or Xapiand node; the oracle's sources and Lucene's and Tantivy's sources were not
read; no Xapian or GPL Xapiand file was opened.
