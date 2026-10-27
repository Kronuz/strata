> Status: CURRENT (2026-10-26) ledger of workstream L1 (queryparser design, step 1 built).

# queryparser

- Design: docs/design/queryparser.md (revision 1); reviews: none yet.
- Branch / worktree: design/queryparser from main 5a0299b, at ~/code/Kronuz/strata-wt/queryparser
- State: steps 1 and 2 built (T1 528/528), awaiting code and security review; step 3 next
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
  `boolean|phrase|lovehate`: **176 matched, 0 failed, 0 excepted**. 352 rows skipped (the other four flag
  sets, step 2). No exceptions listed (`test/vectors/EXCEPTIONS.md`).
- T5 deep nesting: 10^6 nested brackets (alternating AND/OR), 10^6 unclosed `(`, 10^6-long left and
  right NOT chains, 10^6 OR chain, parse + describe + destroy on a 512 KiB thread stack: pass.
- Random: 18 inputs of 1 MiB (raw bytes, syntax characters, operator words; 3 flag sets each): no crash;
  peak RSS 126 MiB (366 MiB under ASan).
- ASan+UBSan (Apple clang, Debug): vectors, grammar, deep, random clean. libFuzzer (LLVM, ASan+UBSan,
  `-max_len=1048576 -timeout=10`, 120 s): 6,647,099 runs, no crash or timeout. Probes: queryparser `.scratch/`.
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
  1 MiB); libFuzzer (LLVM, ASan+UBSan, all flags selected by the first bytes, 180 s): 14,962,315 runs
  clean. Release peak RSS 161 MiB on 1 MiB inputs (405 MiB under ASan).
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
- Doing: nothing.
- Next: reviews of steps 1 and 2 (fold findings in as a separate fix round); step 3 (stopper, default ops,
  prefix, expansion limits, glob wildcards; T2 against V2 with the 2.0-versus-2.1 and stemmer exceptions counted
  separately); the lead lands `step2` after review and CI.
- Blocked on: nothing. Step 3 can run T2 against V2 (copy `parse-v2.jsonl` into `test/vectors/`).
- Behaviour chosen without a vector (the V2 matrix must cover each; all in `test/test.cc`):
  1. Juxtaposed items are the lowest-precedence level: `a OR b c` is one OR; an AND/OR/XOR chain
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
