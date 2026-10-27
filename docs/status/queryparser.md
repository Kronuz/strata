> Status: CURRENT (2026-10-26) ledger of workstream L1 (queryparser design, steps 1 to 3 built; step 3 on main, step 3b on branch `step3b` awaiting review).

# queryparser

- Design: docs/design/queryparser.md (revision 1); reviews: docs/reviews/queryparser-step1.md (REJECT; fixes below, awaiting re-check).
- Branch / worktree: design/queryparser from main 5a0299b, at ~/code/Kronuz/strata-wt/queryparser
- State: steps 1 to 3 built. Main (queryparser:56d7e08) holds step 3. Step 3b is on branch `step3b` (from main, pushed): T1 528/528, T2 3,482 matched + 6 excepted of 3,488 (no open row), T3 1,995/1,995 (no open row); awaiting review.
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
- **Step 3 (V2 parity), 2026-10-26**, queryparser branch `step3` (pushed, from main b732d09):
  - queryparser:c18feee the V2 grammar; queryparser:56d7e08 the T2 runner. CI dispatched
    (run 40456097226), not watched.
  - **T1: 528 matched, 0 excepted, 0 failed, 0 skipped.**
  - **T2 (`parse-v2.jsonl`, 3,488 rows, each under its recorded settings): 3,398 matched, 6 excepted, 84 failed.**
    Exceptions by kind: unreachable (word breaks without ICU) 6; Xapian 2.1 against 2.0 0; stemmer-only 0 (no row
    differed on a stem). The 84 failures are `open` rows in `test/vectors/EXCEPTIONS-v2.md` (they do not fail the run;
    a new failure or a stale open row does). By group: partial 31, parens 22, operands 8, near 7, plusminus 5, misc 5,
    stars 3, stopwords 2, prefix 1. Shapes, about 45 distinct: stop words beside brackets and stray brackets
    (`a( b`, `a(b)`, `a) b`, `a )b`, `a (b) c)`, `(a) NEAR (b)`: V2 drops them all, which the run model does not
    explain), NEAR or ADJ beside a quote, bracket, `+b`, `-b` or an expanding word (`"a b" NEAR c`, `a NEAR +b`,
    `a-b NEAR c*`, `NEAR a*`, `NEAR/3 a*`), a lone hated stop word under PARTIAL (`-the`, `-a -an`), `a NEAR/3 AND b`
    (AND reads as a word), `a~-1`, `ba~2 bc~1*`, `ba*~ OR~ bc*~`, CJK in a quoted phrase (V2 keeps every n-gram at
    position 1), `-"a b" c`.
  - T2 also applies the caps that wildcard, partial and edit-distance nodes carry to the oracle's expansion database
    (rebuilt in the runner from the terms the rows matched) and compares the matched terms or the error. 41 rows whose
    `most_frequent` cut ties among equally frequent terms are compared by tree only: which tied terms stay is the
    engine's choice (V2 kept `baa03` and `baa58` and cut `baa14`, `baa25`, `baa36`, `baa47`).
  - **Grammar changes** (each has a mutant below):
    1. Juxtaposed items form a group that binds tighter than every operator (`a OR b c` = `(a OR (b AND c))`,
       `a NOT b c` = `(a AND_NOT (b AND c))`). The group is runs of adjacent plain words (one element each) plus
       standalone items (phrases, brackets, wildcards, edit distances, words followed by `*` or `~`); required items,
       and under AND every element, fold with AND, the rest with the default operator, then AND_MAYBE and AND_NOT
       (`+a b c -d` = `((a AND (b AND c)) AND_NOT d)`).
    2. Flattening is left only (`a AND (b AND c)` and `a AND b c` stay nested).
    3. Brackets group with or without BOOLEAN. A stray `)` after any operator re-reads the whole text as plain words
       (no boolean, wildcard, partial, fuzzy; brackets and lovehate stay); otherwise it separates two parts joined by
       the default operator (`(a OR b)) AND c` under OR is `(a OR or OR b OR (and OR c))`). `a OR )` is the OR error.
    4. NEAR and ADJ are plain words unless between two plain words; operands that are generator groups, brackets,
       phrases or operator words, `/0`, `/x`, `/` and a trailing NEAR leave them plain. A mixed chain, an expanding
       operand, a plain word beside the chain, an expanding word after a NEAR that is not a chain, re-read the whole
       text. The distance is the last explicit `/n`; digits wrap as a 32-bit integer; a wrapped zero means 10; the
       window wraps and is at least the term count.
    5. **Re-read from the offending operator onward: V2 does not show it.** Every re-read row in V2 reads the whole
       text as plain words (`a NOT b NEAR c*` turns the NOT into a word too). The one row that reads like a tail
       (`a NEAR/3 AND b`: the AND is a word) is a single open row.
    6. Glob `?`, `*` inside a word, and `**`: the whole token is one `WILDCARD` over the pattern text; a wildcard,
       glob and edit distance take a position (`banna~ banana` puts `banana` at 2).
    7. Edit distance carries `fixed_prefix_len` = the bytes of the term prefix (`1` for `S`, `7` for `title.S`, none
       for no prefix), not a constant 1.
    8. `config::default_prefix`; default operators NEAR, PHRASE (window 10 + words - 1), ELITE_SET (10), SYNONYM, MAX,
       elements of a group join pairwise (`((a MAX phrase) MAX b)`), a run is one n-ary node; NEAR and PHRASE over a
       non-term raise `OP_NEAR and OP_PHRASE only currently support leaf subqueries` (`error_kind::unsupported`).
    9. Stop words (`config::stopper`, `stop`): dropped from a run that has other words, or beside a wildcard or edit
       distance; kept in a run of two or more stop words, or alone in a group; STOP_ALL drops them everywhere (an
       operand that vanishes removes its operator; positions stay; `"cat the dog"` is `PHRASE 3` over positions 1
       and 3). V2's STOP_NONE and STOP_STEMMED rows are identical. With a stop list a group of only exclusions reads as
       if none were marked (`-running` is `running`, `a NEAR -b` drops `b`).
    10. Smaller facts the rows showed: a word before `* = < > [ { / @ \ "` is not Z-stemmed; `+` and `#` suffixes
        belong to one-character words (`c++`, `ab+` is `ab`); a doubled quote inside a phrase is part of it; control
        characters and Unicode spaces separate runs, other punctuation breaks a run; `-ba*` and `-banna~` do not expand
        (PARTIAL still does); a word followed by `~` that is not an edit distance stands alone; descriptions escape
        control bytes as `\xNN`; a partial pair drops its term when it is a stop word; an operator keyword followed
        by `*` under WILDCARD is a word; `AND`, `OR`, `NOT` in a row name the tighter one (`AND NOT` as written).
  - **CR2's four shapes are still absent from V2** (checked: no row for `a OR -b`, `a XOR -b`, `a AND NOT -b`,
    `a AND --b`). Question for a future oracle run: those four under default AND and OR with and without a stop list.
    Today `a OR -b`, `a XOR -b` and `a AND NOT -b` are syntax errors; `a AND --b` is `(a AND b)` (`--b` is a plain
    word, so the old error is gone); the grammar test pins all four.
  - **V3 recorded** (strata-oracle:699ee7d): `vectors/out/parse-v3.jsonl`, 1,995 rows in 50 groups of 15 to 48 rows, one per open shape of `EXCEPTIONS-v2.md` and CR2's four (`cr2-*`, with `-forms` neighbours), settings per row; read the README "parse-v3" section.
  - Compatibility: today's syntax is reproduced where T1 (528 rows) pins it; the juxtaposition change moves `a OR b c`,
    `a b XOR c d` and every mixed explicit/implicit text, as the lead ruled.
  - Evidence: ASan+UBSan (Apple clang Debug) 6 of 6 clean (grammar, vectors, t2, deep, oom, random; deep nesting of
    10^6 passes on 512 KiB). Release random: 30 inputs of 1 MiB, peak RSS 158 MiB (161 before). Time-per-byte probe
    (the reviewer's 37 generators plus 17 for the new shapes, 64 KiB against 256 KiB, Release, `quiet strict`): worst
    growth 1.47 (`unclosed parens`, the known allocator warm-up); no superlinear case. A quadratic scan of the
    anchors per run, found by reading the group code, was fixed before the probe.
  - Mutants, each killed by T2 or the grammar test (`.scratch/mut3.py`): right operand flattens (51 rows), no runs
    (363), glob off (61), no fixed prefix (133), stray `)` never re-reads (5), stop words never dropped (446), re-read
    keeps fuzzy (7), last distance wins removed (2), caps not stored (170), default op always n-ary (16), no
    exclusion demotion (7 rows plus a stale open row), stray `)` not a separator (2 stale open rows), `AND NOT` label
    (5), mixed chain an error again (14); and operand may be NEAR, which survived until `a NEAR NEAR` was added to the
    grammar test (the T2 rows reach the same tree through another guard).
  - Fuzz: instrumented (`fuzz/run.sh`, LLVM clang, whole tree under `fuzzer-no-link,address,undefined`), `-max_len=1048576`,
    661 s: **2,817,047 runs, cov 2284, ft 14546, corp 3681 / 579 KB**, no crash, timeout or leak. The target now varies the
    glob flags, stop list and strategy, default prefix, expansion caps and ten default operators; the dictionary gains
    18 step 3 shapes.
- **Step 3b (T2 open rows closed through the V3 vectors), 2026-10-26**, queryparser branch `step3b`
  (queryparser:2226c88, 5976767, 114e09f, fccaab8, 4e14ccd; CI dispatched, run 40461287087, not watched):
  - T3 = `test/t2.cc` over `test/vectors/parse-v3.jsonl` (ctest `t3`, open rows in `EXCEPTIONS-v3.md`, none left).
    Counts after each cluster (T2 matched/open, T3 matched): start 3,398/84, 1,375 of 1,995; NEAR/ADJ 3,434/48,
    1,568; brackets and stray `)` 3,468/14, 1,843; minus and stop-word exclusions 3,473/9, 1,903; tilde, CJK, rest
    **3,482 + 6 excepted/0, 1,995**. No cluster hit the three-attempt cap.
  - **Rules found** (every rule is a V3 row; a mutant for each is in `.scratch/mut4.py`, 31 of 31 killed):
    1. A re-read now means plain words with no BOOLEAN, LOVEHATE, PARTIAL, wildcard, fuzzy, quotes or brackets
       (phrase generators stay). Triggers: a stray `)` anywhere; an unclosed phrase holding `*` or `?` under
       WILDCARD; a minus-marked phrase that reads as unmarked; a minus-marked item before AND, OR, XOR or NOT; a sign
       apart from the AND it follows (`a AND - -b`). The earlier note that brackets and signs stay in a re-read was wrong.
    2. NEAR or ADJ that forms no chain re-reads when the next word is an operator word, an expanding word (also at the
       end of a generator group: `NEAR b.c.d*`), a `+b` or `-b`, or a quote; or when a quoted phrase precedes it;
       `-a NEAR b` re-reads, `+a NEAR b` is a chain; a lone `+` between NEAR and its operand is skipped. `NEAR` and
       `ADJ` as a last word never expand under PARTIAL. `near` is a stop word, `adj` is not.
    3. A bracket needs BOOLEAN, or a `+`/`-` sign in front of it. A bracket holding one stop word goes beside a heavy item
       (non-stop word or operator word) that white space separates from it, or after a word glued to it (`a(b)`,
       `a.(b)`); otherwise it stays (`(a)(b)`, `(a)b`). A word glued to `(` is not stemmed, keeps a `+` suffix, and
       PARTIAL stops expanding after a call and a `)`.
    4. An exclusion that reads as unmarked also applies without BOOLEAN or a stop list; a kept run of stop words counts
       as a positive item; PARTIAL never expands a demoted exclusion (its term stands); a hated bracket of one stop word vanishes.
    5. A bare `~` is an edit distance unless `-` or `~` follows; a run in a group may end with a solo word
       (`foo~2 bar~1*` = `foo`, `(2 bar)`, `1`); `OR~` is a word; an operator word before a CJK character is still an operator;
       a CJK run in a phrase is its units at one position (no bigrams); a stray `)` or none is punctuation inside a group.
  - **CR2 settled by V3**: `a OR -b`, `a XOR -b`, `a AND NOT -b`, `a NOT -b`, `OR -(b)` and `OR -"b c"` are syntax errors
    in Xapian too (errors name the operator as written); `a OR - b`, `a OR --b`, `a OR ---b`, `a OR +b` parse. No change was needed.
  - Evidence: ASan+UBSan (Apple clang Debug) 7 of 7 clean (grammar, vectors, t2, t3, deep, oom, random; deep 10^6 nesting
    passes). Fuzz (LLVM clang, whole tree instrumented, `-max_len=1048576`, 661 s, 19 new dictionary shapes, seeded from
    the step 3 corpus): **3,188,287 runs, cov 2547, ft 16080, corp 3981 / 634 KB**, no crash, leak or timeout.
    Time-per-byte probe (reviewer's generators, step 3's and 16 for these shapes, 71 in all, 64 KiB against 256 KiB,
    Release, `quiet strict`): worst growth 1.28 (`unclosed parens`, the known warm-up); no superlinear case.
    Mutants (32 tried): 2 survived at first, both dead code (an internal bracket flag that the missing BOOLEAN already
    implied, and a duplicated retry condition); both removed (queryparser:4e14ccd) and the other 31 are killed by T2 and/or T3.
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
- Next: review of `step3b` and CI (run 40461287087); step 4 (Xapiand A/B, needs the CR1 API changes below).
- Blocked on: nothing.
- Behaviour chosen without a vector in steps 1 and 2 (items 1, 3 (mixed chains), 5 (NEAR errors) and 6 are replaced by V2 in
  the Step 3 block above; the rest stand):
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
- Not done: `describe` of `wqf != 1` terms, the cpp-libraries catalog entry (waits for the GitHub repo).
- Last verified: 2026-10-26, macOS x86_64, Apple clang, Release and ASan+UBSan; nothing on Linux or
  FreeBSD yet (CI will).

## Log

- 2026-10-26: Clean room held: no Xapian source, no GPL file (`oracle.cc` not opened);
  xapian.org `queryparser.html` read as public documentation.
- 2026-10-26: Step 1 built. Clean room held: only parse.jsonl rows and general operator syntax were used; no Xapian source, no `oracle.cc`.
