> Status: CURRENT (2026-10-26) ledger of workstream L1 (queryparser design, steps 1 and 2 built, review fixes pushed).

# queryparser

- Design: docs/design/queryparser.md (revision 1); reviews: docs/reviews/queryparser-step1.md (REJECT; fixes below, awaiting re-check).
- Branch / worktree: design/queryparser from main 5a0299b, at ~/code/Kronuz/strata-wt/queryparser
- State: steps 1 and 2 built (T1 528/528); the review fix round is on queryparser branch `review-fixes` (from `step2`) and analysis branch `word-scanner`, awaiting the re-check; step 3 next
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - strata:2731d45 Design with D1 to D10 (incl. the Lucene/Tantivy extension survey requested by the owner's
    2026-10-26 scope change), gate rows T1 to T7, open questions Q1 to Q5, steps 1 to 5+.
  - analysis:bb718c4 (branch `word-scanner`, pushed): `analysis::word_scanner` extracted, additive;
    966 vectors and the 1,114,112-code-point sweeps unchanged; mutant (acronym not lowercased) fails it.
  - queryparser:11a6365 (local repo `~/code/Kronuz/queryparser`, no GitHub repo yet): tree, `describe`,
    iterative parser, flags boolean/phrase/lovehate, default op, NEAR/ADJ and `/n`, stem strategies,
    errors with offsets, field-resolver hook (`name:term`, `name:"phrase"`), vectors tool, grammar
    tests, deep test, random test, libFuzzer target. CMake pins analysis at bb718c4.
- T1 parity (strata-oracle a2e1d82 `parse.jsonl`, 528 rows): flag sets `default` and
  `boolean|phrase|lovehate`: **176 matched, 0 failed, 0 excepted** (the two flag sets give identical rows: 88 distinct checks, per CR3). 352 rows skipped (the other four flag
  sets, step 2). No exceptions listed (`test/vectors/EXCEPTIONS.md`).
- T5 deep nesting: 10^6 nested brackets (alternating AND/OR), 10^6 unclosed `(`, 10^6-long left and
  right NOT chains, 10^6 OR chain, parse + describe + destroy on a 512 KiB thread stack: pass.
- Random: 18 inputs of 1 MiB (raw bytes, syntax characters, operator words; 3 flag sets each): no crash;
  peak RSS 126 MiB (366 MiB under ASan).
- ASan+UBSan (Apple clang, Debug): vectors, grammar, deep, random clean. The libFuzzer runs of steps 1 and 2
  (6,647,099 runs in 120 s; 14,962,315 in 180 s) do not count: the library was not instrumented (`cov: 24`,
  review SR2). The instrumented run is in the fix round below.
- Revert evidence (failing before the fix, in the first run): `obam*` under `some` stays unstemmed
  (2 rows failed, then passed); error text names the keyword `NOT` not `AND_NOT` (8 rows); the ADJ error
  names `ADJ` (grammar test). Mutants caught: no AND/OR/XOR flattening (grammar), proximity window off by
  one (16 vector rows + grammar), `*` rule removed (2 rows), capital rule removed (2 rows + grammar),
  XOR at AND's level and AND at OR's level (grammar, after adding the two XOR/AND tests the first run
  missed), `-` not a generator (8 rows + grammar).
  - queryparser:8c2190c (branch `step2`, pushed): WILDCARD, PARTIAL, FUZZY, CJK_NGRAM, PURE_NOT,
    BOOLEAN_ANY_CASE, and the NEAR/ADJ re-read as plain words.
- **T1 parity, all 528 rows of `parse.jsonl` (strata-oracle a2e1d82): 528 matched, 0 failed, 0 excepted, 0 skipped.**
  Exception list (`test/vectors/EXCEPTIONS.md`): none.
- Step 2 checks: ASan+UBSan clean (vectors, grammar, deep, random with all nine flags: 30 inputs of
  1 MiB). Release peak RSS 161 MiB on 1 MiB inputs (405 MiB under ASan).
- Step 2 revert evidence: before the fix the grammar test failed for `foo* ADJ bar` (the `*` hid the
  operator); first-run vector failures were none. Mutants, each caught by a vector row or grammar test:
  partial without the 2-character floor (4 rows), partial pair merging into its parent (24 rows), no
  re-read (8 rows), default fuzzy distance 1, n-gram positions advancing (4 rows), pure NOT off for a lone
  `-a`, any-case ignored, `*` not skipped before NEAR.
- Step 2 used V2 for three questions the 528 rows could not settle (read of groups `partial` and `stars`,
  not T2): partial needs two or more characters (`a b` gets none, `ba` does); NEAR next to an expanding
  word re-reads the text (V2: `a NEAR b*` under WILDCARD, `barack NEAR obama` under PARTIAL), while
  `a NEAR b c*` does not; `~n` is fuzzy only when the digits end the text (`ba~2 bc` is not). V2 shows
  more (re-read applies only from the offending operator; stop words; `fixed_prefix_len=1`) for step 3.
- Step 2 behaviour not pinned by T1 (step 3 settles it with V2): wildcard on a phrase-generator group
  ignores the `*`; partial applies only when the word ends the text (no trailing space); the re-read
  drops boolean, wildcard and partial for the whole text; lone `-a` under PURE_NOT gives the same
  `0 * <alldocuments>` tree as `NOT a`; BOOLEAN_ANY_CASE covers AND, OR, NOT, XOR only.
- Not done in step 2: glob wildcards (`?` WILDCARD_SINGLE, `*` WILDCARD_MULTI) and `**`, which no
  parse.jsonl row exercises (V2 does); they go with step 3.
- Fix round for the review (SR1 to SR4, CR2 to CR5; CR1 as a note), 2026-10-26:
  - analysis:9b15abf (branch `word-scanner`, pushed, additive): **SR1** `word_scanner` remembers the last failed
    `A.B.C.` chain (every start inside it fails identically) and `index_text` resets it per call; `reset()` is public.
    Tokens unchanged: 966 vectors and 1,114,112 code points; a 20,000-input comparison against an unmemoized copy of
    the rule; a 64 KiB chain bound of 0.5 s. Revert: without the fix the bound fails (1.5 s); mutant (memo ignores the
    chain end) fails the vectors and the comparison.
  - queryparser:c0a1584 (branch `review-fixes`, pushed), pinned to analysis:9b15abf:
    - **SR1**: worst ns/B over the reviewer's 37 generators at 64 and 256 KiB (`probe/timing.cc`, Release, `quiet strict`):
      before 119,785 (acronym chain, 899x the next worst); after 163 (`a ` repeated; the chain generators 107, 92, 97 and
      147 ns/B at 256 KiB; worst growth 2.06, `unclosed parens` allocator warm-up, as before). A 64 KiB chain test in
      `test.cc` fails with the old pin (1.14 s against the 0.5 s bound).
    - **SR2**: `QUERYPARSER_FUZZ` instruments the whole tree (`fuzzer-no-link,address,undefined`, global flags before
      the dependencies); only the target links `-fsanitize=fuzzer`; `fuzz/run.sh` fails if `nm` shows no `sanitizer_cov`
      in `libqueryparser.a` (10 symbols); `fuzz/queryparser.dict` is the reviewer's dictionary plus `AND -`, `OR -`,
      `NEAR/18446744073709551617`, `~4294967296`, `A.`. The target also varies the default operator (six values), a
      resolver, `max_term_bytes` and `max_word_length`, and checks that every node's text lies in the pool and that
      the tree has no repeated node. Run: LLVM clang 23.1.2, `-max_len=1048576 -timeout=10`, 661 s: **3,103,987 runs,
      cov 1639, ft 10654, corp 2872 / 590 KB, no crash, timeout or leak** (was `cov: 24`).
    - **SR3**: mechanism: `config::max_term_bytes` (default and ceiling 2^32 - 1, `max_term_bytes_limit`) bounds the
      bytes of the term pool; `builder::store` checks it before the offset is taken and throws a private
      `limit_exceeded`, which `parse` returns as `too_long`. The node index is checked against 2^32 - 1 in `push`.
      Chosen over 64-bit fields because it costs one compare per term, keeps the node at its size, and gives hosts a
      memory knob. Tests at caps of 0, 4 and 90 bytes: exact fit passes, one byte over fails, wildcard and edit-distance
      leaves count, a 30-byte resolver prefix copied into every term of a phrase, a cap above 2^32 clamps.
    - **SR4**: chosen: catch `bad_alloc` into `error_kind::out_of_memory`, and document 88 B per input byte and the
      attacker-sized tree in the header. A byte cap would duplicate the HTTP body limit the host already owns and
      would still leave `bad_alloc` for any other reason; the catch makes "never throws on input" unconditional
      (only a resolver exception escapes). `test/oom.cc` fails each of the 186 allocations of five queries in turn
      (all flags, a resolver with a long prefix): every one returns `out_of_memory`, none throws, no leak under ASan.
    - **CR2**: V2 (group plusminus, default AND, no stopper) gives `a AND -b` = `(a AND_NOT b)`, `a OR +b` = `(a OR b)`,
      `+a AND b` = `(a AND b)`, `a NOT -b` = the NOT syntax error. A mark on the right operand of an explicit operator
      now turns AND into AND_NOT, is ignored for `+`, and raises the operator's syntax error for `-` after OR, XOR or
      NOT. V2 has no row for `a OR -b` and `a XOR -b`: they are errors (a choice; the alternative, a silent inclusion,
      is what the review called the worst failure). Behaviour 3 of the list below is unchanged.
    - **CR3**: all 16 reviewer mutants are now killed (m4 `NEAR/4294967296` and `~4294967296`; m10 `title:running`
      under `some` gives `ZSrun`; m11 `+<70-letter word> b c`; m14 `+a b` and `+a b -c d` under default AND; m6 by
      `test.cc` alone, because the library builds with `_GLIBCXX_ASSERTIONS` and libc++ hardening in the test build).
      The "176 rows" are 88 distinct checks (see T1 above).
    - **CR4**: pinned from V2 (stopper none, so no stop-word noise): unclosed `(` closes at the end (`(a`, `+(a`,
      `(a OR (b`, `((a OR b) AND c`, `NOT (a`, `(a AND`, `(`), and `+a b -c d` under AND. **V2 contradicts behaviour 1**
      (see "V2 differences" below); not changed here.
    - **CR5**: an unsupported `default_op` reads as `or_` (test); the header documents `max_word_length = 0`, a
      dropped word being no operand, the lifetime of `text()` views, and the iterative-consumer duty (SR5).
    - **CR1**: not built, as ruled. Step 4 needs these API changes: (1) read a field name up to `:` over
      `[word-chars . - _ @]` (dotted and dashed names such as `user.name:x`, `a-b:x` never reach the resolver today);
      (2) scope a field over a group, `t:(a b)` and `t:-x` (today the field is dropped and the query widens), or
      return an error; (3) a resolver result with a field handle (`uint32_t` the host owns) carried by the node
      instead of a prefix copied into every term (this also removes the prefix amplification of SR3), a type or mode
      (text, keyword, numeric, date, geo), the per-field analysis (stemmer, stopper, strategy, CJK, default operator)
      and the raw value for non-text types; (4) a tri-state return (field, not a field, error with message);
      (5) say whether a field covers every word of a NEAR chain or generator group (today it does); (6) the name is a
      view of the host's buffer and the call can throw.
  - Evidence: ASan+UBSan (Apple clang, Debug) 5 of 5 tests clean (grammar, vectors, deep, random, oom); T1 **528 of
    528** (`matched 528, excepted 0, failed 0, skipped 0`); Release 5 of 5. Revert check per fix: SR1 above; CR2
    (`else if (false)`), SR3 (check removed), SR4 (catch removed: `oom` fails), CR5 (clamp removed) each fail a test; the
    reviewer's 16 mutants plus 8 mutants of the fixes (no AND_NOT, `-` allowed after OR, check off by one, leaf
    unchecked, and the four reverts) are all killed (`.scratch/mut.py` in the queryparser clone).
  - CI dispatched with `gh workflow run ci.yml --ref`: queryparser `review-fixes` (run 40448279053), analysis
    `word-scanner` (40448282075), queryparser `step2` (40448285275); not watched.
- V2 differences found while pinning (for T2 in step 3; a census of 1,209 V2 rows with boolean-class flags and default
  operator AND or OR gave 11 mismatches in 65 stopper-none rows, the rest hidden by stop words):
  - Juxtaposed items form a group that binds tighter than every explicit operator: V2 `a OR b c` = `(a OR (b AND c))`,
    `a b OR c` = `((a AND b) OR c)`, `a b XOR c d` = `((a AND b) XOR (c AND d))`, `a NOT b c` = `(a AND_NOT (b AND c))`;
    ours reads `a OR b c` as `((a OR b) AND c)` (behaviour 1).
  - Flattening applies to the left operand only: V2 `a AND (b AND c)` and `a AND b c` stay nested, `(a AND b) AND c`
    and `a b AND c` flatten; ours flattens both sides (T1 does not exercise it).
  - `+a b c -d` under AND is `((a AND (b AND c)) AND_NOT d)` but `+a b -c d` is flat; ours flattens both.
  - A stray `)` after an operator makes the whole text plain words with nesting kept: `(a OR b)) AND c` =
    `(a AND or AND b AND (and AND c))` (ours ignores the `)`); `a( b` and `a(b)` drop a stop word differently.
  - NEAR and ADJ are plain words when they do not sit between two words of a valid chain: `a NEAR b c`, `NEAR a`,
    `a NEAR +b`, `title:a NEAR b`; `a ADJ/3 b ADJ/5 c` takes the last distance; `a NEAR/x b` is plain words.
- Doing: nothing.
- Next: the re-check of the fix round; the lead lands `step2` plus `review-fixes`; step 3 (stopper, default ops,
  prefix, expansion limits, glob wildcards; T2 against V2 with the 2.0-versus-2.1 and stemmer exceptions counted
  separately); the lead lands `step2` after review and CI.
- Blocked on: nothing. Step 3 can run T2 against V2 (copy `parse-v2.jsonl` into `test/vectors/`).
- Behaviour chosen without a vector (the V2 matrix must cover each; all in `test/test.cc`):
  1. (V2 differs, see above) Juxtaposed items are the lowest-precedence level: `a OR b c` is one OR; an AND/OR/XOR chain
     and nested same-operator brackets flatten into one n-ary node.
  2. `+a b` is `(a AND_MAYBE b)`; `+a -b c` is `((a AND_MAYBE c) AND_NOT b)`; a lone `-a` raises the `NOT` syntax error.
     With default AND all unmarked items are required.
  3. Window = n + terms - 1 for NEAR and ADJ (n defaults to 10 for both, as the vectors show for two terms); a
     NEAR/ADJ chain of mixed kinds is an error; `NEAR/n` operands are words (generator groups flatten in).
  4. Phrase generators are `- : / . @ \` between two word characters; the vectors fix only `-` and `:`.
  5. Operator keywords need BOOLEAN; `AND NOT` is AND_NOT; a missing operand raises
     `Syntax: <expression> OP <expression>` with OP as written (the vectors fix only `NOT`).
  6. Unclosed `(` closes at the end; an unmatched `)` is ignored; an empty `()` vanishes.
  7. A word followed directly by `*` is not Z-stemmed under `some` (vector: `obam*`).
- Deviation from D9 ("no text limit"): text over 2^30 bytes returns `error_kind::too_long`, because node
  indexes and pool offsets are 32 bits. Larger inputs need 64-bit links; say if the row should change.
- Not done: the stopper hook (step 3), `describe` of `wqf != 1` terms, the cpp-libraries catalog entry
  (waits for the GitHub repo).
- Last verified: 2026-10-26, macOS x86_64, Apple clang, Release and ASan+UBSan; nothing on Linux or
  FreeBSD yet (CI will).

## Log

- 2026-10-26: Clean room held: no Xapian source, no GPL file (`oracle.cc` not opened);
  xapian.org `queryparser.html` read as public documentation.
- 2026-10-26: Step 1 built. Clean room held: only parse.jsonl rows and general operator syntax were used; no Xapian source, no `oracle.cc`.
