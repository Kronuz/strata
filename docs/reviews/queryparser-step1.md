# queryparser step 1: security and code review (library `queryparser:11a6365`, scanner `analysis:bb718c4`)

Reviewer: security reviewer, folded with the code review (fresh context, 2026-10-26). Read: `docs/design/queryparser.md` (revision 1,
with the owner's rulings), `docs/status/queryparser.md` (ledger at `design/queryparser` `986ab6b`), `docs/reviews/calibration.md`,
`queryparser.cc`, `queryparser.hh`, `CMakeLists.txt`, `fuzz/fuzz_parse.cc`, `test/*.cc` of `~/code/Kronuz/queryparser` at
`11a6365` (clone `~/code/Kronuz/queryparser-review`; `git status` of the original is clean, no remote, so nothing unpushed), and
`git diff origin/main...origin/word-scanner` of `~/code/Kronuz/analysis` (`bb718c4`, equals the local `origin/word-scanner`), plus
`unicode::next_utf8` at the pinned unicode commit. Clean room held: no Xapian source, no `oracle.cc`, no GPL file was opened.

## Verdict: REJECT

Scope note: while this ran the library gained step 2 (`queryparser:8c2190c`, wildcard, partial, fuzzy, CJK n-grams) and the branch its ledger
entries; neither was reviewed. SR1 to SR3 are in code step 2 builds on, so they stand against it too.

Three blockers, each small to fix, and each needs a regression test:

1. SR1: a 64 KiB query of `A.A.A.` ... `x` takes 1.9 s, 256 KiB takes 31 s; time per byte grows 4.0x for 4x the input (quadratic).
2. SR2: the libFuzzer target instruments only `fuzz_parse.cc`; the library is compiled without ASan, UBSan or coverage, so T5's
   "6,647,099 runs clean" and the 120 s run say nothing about the parser.
3. SR3: the 2^30-byte check does not bound the 32-bit `text_off`/`text_len` of the term pool: with a field prefix the pool is
   4.5x (8-byte prefix) to 32.5x (64-byte prefix) the input, so it wraps below the cap.

Everything else I attacked held: no recursion (64 KiB thread stack, ten-to-the-sixth levels, nine shapes), no hang and no
out-of-range error offset in 4,848,448 inputs under ASan and UBSan, no over-read at the end of an exact-size buffer, and the
analysis diff is additive and its suite passes under ASan. Conditions (not blockers for landing step 1, owed before step 2 or 4)
are in the table below.

| ID | Sev | Kind | One line |
|---|---|---|---|
| SR1 | high | blocker | Quadratic acronym scan: `A.B.C. ... x` (also pre-existing in indexing) |
| SR2 | high | blocker | Fuzz target does not instrument the library; T5 evidence is empty |
| SR3 | medium | blocker | Pool offsets and lengths wrap below the 2^30 input cap (field prefix amplification) |
| SR4 | medium | condition | Memory is up to 88 B per input byte; the 2^30 cap is not a bound; `bad_alloc` escapes "never throws" |
| CR1 | medium | condition | The resolver hook cannot carry `q=`: dotted names never reach it, `f:(a b)` drops the field, prefix-only result |
| CR2 | medium | condition | `a AND -b` becomes `a AND b`: marks are dropped after an explicit operator |
| CR3 | medium | condition | Four of sixteen reviewer mutants survive; the ledger's "176 rows" are 88 distinct |
| CR4 | low | condition | The seven vector-free behaviours: one inconsistent, two risky, four fine; list for V2 |
| SR5 | low | note | Output depth and size are attacker-controlled; consumers must be iterative |
| CR5 | low | note | `config` misuse, unvalidated `default_op`; CI floats at `@main`; misleading error after a dropped word |

## Attack surface results (what I ran)

All under `quiet light`, ASan and UBSan with `-fno-sanitize-recover=all` on Apple clang (Debug) unless stated; the original
repository's tests are rerun in my clone first.

| Check | Result |
|---|---|
| Repository tests under ASan+UBSan (`ctest`: grammar, vectors, deep, random) | 4 of 4 pass (`logs/ctest-asan.log`) |
| Analysis suite under ASan+UBSan at `bb718c4` (`analysis_test`, the ledger's 966 vectors and code-point sweeps) | passes, 70.6 s (`logs/analysis.out`); I did not recount its vectors |
| Recursion: parse, describe, destroy of 10^6-level shapes on a **64 KiB** thread stack (the repository tests use 512 KiB) | passes in ASan and in RelWithDebInfo, nine shapes: `(`x10^6, `(a AND `/`(a OR ` nests, `+(`, `-(`, `a NOT (`, `a XOR (b AND (c OR `, `a AND NOT ` chain, `"a-b" (` (`probe/stack.cc`, `logs/stack64-*.log`) |
| UTF-8 and hangs: every code point 0 to 0x32000 and every 97th beyond, all of 0x10FF00-0x110100 and 0x1FFF00-0x1FFFFF (past U+10FFFF), including surrogates, in 8 contexts, 2 configs; all 1- and 2-byte sequences in 18 contexts; 28 overlong, truncated, surrogate and BOM shapes in 18 contexts, at the end of the buffer; every input in a **heap buffer of exact size**, 5 s watchdog | 4,848,448 inputs, 0 hangs, 0 sanitizer reports, 0 error offsets above the input size (`probe/sweep.cc`, `logs/sweep-asan.log`). `unicode::next_utf8` bounds every continuation byte by `end` and decodes an invalid byte as itself (ISO-8859-1), one byte at a time, so it always advances; surrogates decode as three-byte scalars but are not word characters, and term text is rebuilt with `append_utf8`, so terms are always valid UTF-8 |
| Lenient recovery must not loop | Step 1 has no lenient mode or fallback (design D6, step 2). The main loop advances on every `step` (each branch returns past `p`; `scan` consumes at least the first code point of a word character); the sweep has no hang. Re-check this when step 2 lands |
| Error offsets | `<= size()` in every error of the sweep; hand cases in `probe/misc.cc` (`logs/misc.log`) |
| Time per byte, 37 adversarial generators at 64 KiB and 256 KiB (`probe/timing.cc`, `logs/timing-64k.log`) | linear (growth 0.7 to 1.2 for 4x input; `unclosed parens` 2.4 is allocator warm-up, 17 to 40 ns/B) for 34 of them, worst linear 133 ns/B (`a ` repeated). **Three are quadratic: SR1, worst ratio 119,785 ns/B at 256 KiB = 899x the next worst (133 ns/B)** |
| 2^30 limit | `parse(" " x (2^30+1))` returns `too_long` at offset 0 (`logs/misc.log`); the check itself is right (SR3 is about what it does not cover) |
| libFuzzer | the committed target is blind (SR2); instrumented rerun: 7.8 million runs, clean (below) |
| Mutants and T1 | CR3 |

### Instrumented libFuzzer (my run, the library built with `-fsanitize=fuzzer-no-link,address,undefined`)

`fuzz2.sh`: LLVM clang, global sanitizer flags so `libqueryparser.a` has coverage symbols (`nm | grep -c sanitizer_cov` prints 10),
`-timeout=10`, my dictionary (`probe/dict.txt`: keywords, brackets, quotes, generators, acronym chains, `NEAR/4294967295`, ignorables,
overlongs, truncated sequences), two seeds, two processes at once for 660 s:

| Run | max_len | Runs | cov | ft | Corpus | Result |
|---|---|---|---|---|---|---|
| A (T5's spec) | 1,048,576 | 3,696,938 | 1045 | 6923 | 2431 / 610 KB | no crash, no timeout, no leak |
| B | 2048 | 4,150,222 | 1056 | 6882 | 2399 / 433 KB | no crash, no timeout, no leak |

Against `cov: 24 ft: 35` of the committed target. So the parser is clean under ASan and UBSan for 7.8 million guided runs, and
the stated T5 row can be met by fixing the build. Two caveats: libFuzzer's input length ramps up slowly (`lim` was 5062 at the
end of A), so it never reached the sizes where SR1 bites, which is why SR1 needs its own timing test; and `fuzz_parse.cc` maps
only the first byte to flags and strategy, with `default_op` limited to `and_` and `or_`.


## Blocking

### SR1. Quadratic acronym scan: a 64 KiB query takes 1.9 s, a 1 MiB query takes minutes (high, blocker)

Attacker: anyone who can send a query string (an HTTP client), and for the same reason anyone who can write a document to an
index. `analysis::word_scanner::acronym` (`analysis.hh`, `bb718c4`) walks the whole chain `A.B.C.` and returns `nullptr` when a
word character follows the chain (`A.B.Cx`), so the caller falls back to `word()`, which takes the single letter `a`. The caller
then reaches `B.C.Dx`, and `acronym` walks the same chain again to the same failure, once per letter: n/2 restarts of an
O(n) walk. In `queryparser.cc` this happens twice per letter (`word_group`'s generator loop and the main loop), and with the
phrase flag off as well.

- Probe: `probe/timing.cc`, rows `acronym-chain-then-word` (`"A." x n + "x"`), `acronym-chain-AB-then-word`, `acronym-chain no
  phrase`: **29,778 ns/B at 64 KiB, 119,785 ns/B at 256 KiB, growth 4.02 for 4x the input**; that is 1.95 s and 31 s of one
  core for one request, about 4.5e-10 s per byte squared: 8 KiB costs 30 ms, 32 KiB 0.5 s, 1 MiB about 8 minutes, 2^30 bytes
  years. Every other generator costs at most 133 ns/B, so this is 899x the next worst case.
- It is not an artifact of the parser: `probe/idx.cc` runs `analysis::term_generator::index_text` (code that predates the
  branch) on the same text: 16 KiB 0.12 s, 32 KiB 0.47 s, 64 KiB 1.9 s (`logs/idx.log`). The extraction made the bug reachable
  from untrusted queries; indexing a hostile document stalls an indexer thread the same way. Fix it in `analysis`, once.
- The random test and the libFuzzer run cannot see it: the input has to be a long, regular chain, and the fuzz target is blind
  (SR2).
- Fix: remember in the scanner the end of the last failed chain (`acronym` fails identically for every start inside one chain,
  because the chain end and the byte after it are the same), or stop scanning a chain once it is longer than any acronym that
  can survive `max_word_length` (a 64-letter acronym is 128 bytes; keep index parity by testing that a longer chain yields the
  same terms as before for chains up to the cap and linear time beyond). Add the 64 KiB chain to `test/` as a bound on time
  (for example, under 100 ms) and to the analysis suite, and add `"A.B."` and `"A."` to the fuzz dictionary.

### SR2. The fuzz target does not instrument the library (high, blocker for T5)

`CMakeLists.txt` applies `-fsanitize=fuzzer,address,undefined` to the `queryparser_fuzz` target only, and the library
(`queryparser.cc`, with `analysis.hh` inlined in it), `unicode` and `snowball` are built without any of it.

- Probes: `nm .scratch/build-fuzz/libqueryparser.a | grep -c sanitizer_cov` prints 0, and the libFuzzer lines of the
  uninstrumented build show `cov: 24 ft: 35` for the whole run, flat from the first second (`logs/fuzz-a-uninstrumented.log`,
  `logs/fuzz-b-uninstrumented.log`, 2.8 and 2.6 million runs before I stopped them, which guided nothing). The same build was the ledger's: "libFuzzer
  (LLVM, ASan+UBSan, `-max_len=1048576 -timeout=10`, 120 s): 6,647,099 runs, no crash". With no coverage feedback the corpus
  never leaves the seed, so nothing a mutator found there reaches a branch of the parser; with no ASan or UBSan in the library, an
  overflow in it is not reported either. T5's row ("libFuzzer clean under ASan and UBSan for a stated run") is not met.
- Fix: build the whole tree with `-fsanitize=fuzzer-no-link,address,undefined` when `QUERYPARSER_FUZZ` is on (a global
  `CMAKE_CXX_FLAGS`/`CMAKE_C_FLAGS`, as my `fuzz2.sh` does) and link the target with `-fsanitize=fuzzer`; check in the build
  log that `libqueryparser.a` has coverage symbols; ship the dictionary; rerun for the stated time and put the coverage figure
  (`cov:`) in the ledger. My instrumented run is below; it is not a substitute for the developer's.

### SR3. Pool offsets wrap below the input cap (medium, blocker by the lead's ruling "checked, not wrapped")

The lead's ruling: keep 32-bit links, but the 2^30-byte limit must be checked, not wrapped. The check on input bytes is correct,
and it bounds three of the four things that are 32-bit:

- `node_id` (nodes): at most one node per input byte (`a-a` is three nodes in three bytes), so at most 2^30 < 2^32 - 1. Safe.
- `pos`/`termpos_`: at most the number of terms. Safe. `window`: clamped at 2^32 - 1 in `finish`. Safe.
- **`text_off` and `text_len`: not bounded.** `builder::term` does `std::uint32_t(q_.pool_.size())` and `std::uint32_t(text.size())`
  with no check, and the pool is the sum of the term texts, not of the input. A resolver prefix is copied into every term of
  a group (`finish` and `make_term`), and `Z` precedes stemmed terms. `probe/misc2.cc` (`logs/misc2.log`), query `t:a-a-a-...`:
  1 MiB of input gives a 1.0x pool with a 1-byte prefix, **4.5x with 8 bytes, 32.5x with 64 bytes**. A query at the cap with an
  8-byte prefix therefore needs a 4.5 GiB pool, and one of 132 MB with a 64-byte prefix reaches 4 GiB. Past 2^32 the offset wraps
  to a smaller value, so `query::text()` returns the wrong slice (a different term: the query silently means something else),
  or, when the wrapped offset lies beyond the pool, `std::string_view::substr` throws `std::out_of_range` inside a `noexcept`
  function and the process terminates.
- Attacker: a client whose body reaches the parser at that size; the HTTP layer's limit is the only guard (SR4). The wrap itself
  I did not run (it needs a pool above 4 GiB, 10 GiB of RAM on this shared host): the arithmetic and the amplification factors
  are measured, the wrap is by reading `queryparser.cc:106-113` (`term`) and `queryparser.hh:88` (`text`).
- Fix: in `builder::term`, fail with `too_long` when `pool_.size() + text.size()` would exceed `UINT32_MAX` (the parse stops,
  like every other error), or make `text_off`/`text_len` 64-bit only if the lead rules the other way. Test it with a prefix large
  enough to reach the check at a few MiB (a resolver returning a 1 MiB prefix does it), and state in the header that the limit
  is "2^30 input bytes and 2^32 term-text bytes".

## Conditions

### SR4. Memory amplification and the cap that bounds nothing (medium, condition)

Attacker: an HTTP client. `probe/misc2.cc`, peak RSS growth over the input, 4 MiB of `(a ` repeated: **353 MiB, 88 B per input
byte** (36 B per node, 32 B per open bracket, 16 B per operand, vectors that double); `a-` repeated 7.9 B/B; `a ` 2.7 B/B; `+(a `
7.2 B/B (`logs/misc2.log`). `describe()` adds up to 11 bytes per input byte (`logs/timing-64k.log`, `outx`). So 12 MiB of
`(a ` costs about 1 GiB, and the 2^30 cap would need 88 GiB: the cap bounds the integers, not the host. Design D9 leaves bytes,
nesting and clauses unbounded and says "the HTTP body bounds it upstream", which is the contract to write down, with the number.

- `parse` "never throws on input" is false under memory pressure: `std::bad_alloc` from the vectors or the pool escapes, and so
  does an exception from the host's resolver. Callers on a reactor thread must catch both; the header says neither.
- Condition: (a) add `config::max_query_bytes` (host-set, default well below 2^30, or documented as the HTTP limit times 88) and a
  count of nodes if the lead prefers, each checked in the parser and returning `too_long`; (b) catch `bad_alloc` inside `parse`
  and return an error, or state in the header that `parse` may throw `bad_alloc`; (c) put 88 B/B and 11 B/B in the README's
  limits. Per the ratchet, any cap must stay at or above Lucene's, Tantivy's and Xapian's (none documents one in the parser).

### CR1. The resolver hook cannot carry `q=` (medium, condition)

The owner ruled that `q=` moves to this parser after 7a. The hook is `bool(string_view name, field_spec&)` with
`field_spec{std::string prefix}`, called once per `word:` in `word_group`. Against what `boolean-parser` serves in Xapiand
today (design section 1 and Q2):

1. **Dotted and dashed names never reach it.** `probe/misc2.cc`: `user.name:foo`, `a.b.c:foo`, `a-b:foo` and `user@host:x`
   call the resolver zero times and become the phrase `user name foo`; only a name that is one scanner word (`a_b`, `title`,
   `A.B` the acronym, `日本`) is passed. Xapiand's nested field paths are dotted. The name must be read as a field name (up to `:`
   over `[word-chars . - _ @]`, host-configurable) before the word scanner sees it.
2. **`f:(a b)` drops the field and widens the query.** `t:(a b)` gives `(a OR b)` with no prefix; `t:-x` and `t:+x` give `x`.
   With `q=` this turns a field-restricted query into a search of the default field. Scope the field over a group (a field in the
   frame), or return an error; never ignore it.
3. **The result is a prefix string only.** A `q=` field needs a type or mode (text, exact/keyword, numeric, date, geo), a
   per-field analysis choice (stemmer, stopper, strategy, CJK, default operator), and the raw value for non-text types: `date:2020-01-01`
   or `id:Abc-1` is lowercased, split on `-` into a phrase and stemmed as text today. Return a field handle (a `uint32_t` the host
   owns) and let the node carry it instead of copying the prefix string into every term text: that also removes the prefix
   amplification of SR3.
4. **Unknown field handling is fixed** (`name:x` becomes a phrase). `q=` wants an error or the choice per call: make the return a
   tri-state (field, not a field, error with message).
5. Prefix applies to every word of a `NEAR` chain and generator group (`t:a NEAR b` is `ta NEAR 11 tb`); say whether that is the
   intended scope of a field.
6. The name is a view of the host's buffer, up to the whole query long, and the call can throw; document both.

Changing the hook now costs nothing (no caller exists; the owner's rule is to migrate callers, not forward). Recording the six
points in the design before step 4 is enough; none of them blocks step 1.

### CR2. Marks are dropped after an explicit operator (medium, condition)

`reduce()` keeps the left item's mark and discards the right one's (`queryparser.cc:563-570`), so a `+` or `-` that follows an
operator vanishes. `probe/misc.cc` (`logs/misc.log`): `a AND -b` gives `(a AND b)`, `a OR -b` gives `(a OR b)`, `+a AND b` gives
`(a AND b)`, `a --b` gives `(a OR b)`, `+-a` gives `a`. An exclusion silently becoming an inclusion is the worst failure a query
parser can have for a host that appends `AND -deleted` or similar to a user's text. D1's list (`+`/`-` bind tighter than AND/NOT)
says `a AND -b` should be `a AND_NOT b` or an error; none of the seven vector-free behaviours covers it. Fix: handle the mark on
the right operand (`AND_NOT` for `-`) or raise the syntax error, add the cases to `test/test.cc`, and list them for V2. Related:
`a NEAR/2 b NEAR/20 c` silently uses 2 for both while a NEAR/ADJ mix is an error (CR4, item 3).

### CR3. Tests: four reviewer mutants survive; "176 rows" are 88 distinct (medium, condition)

Two claimed mutants, rerun: window off by one (`+ count - 1` to `+ count`): 16 vector rows fail and the grammar test fails
(killed, as claimed); `*` rule removed: 2 vector rows fail (killed by vectors only, as claimed; the grammar test passes). Sixteen
mutants in all (the two above and fourteen of mine), `mutants.sh` (`logs/mutants.out`, builds in `mut-repo/`): 11 killed (`<=` to `<` on the word cap, no window
clamp, `+`/`-` boundary check, left-associativity, error offset, proximity kind mixing, flattening, keyword boundary, capital
rule, window, star). **Survivors: m4** (no clamp on the digits of `NEAR/n`: the test checks `4294967295`, not `4294967296`);
**m10** (`Z` and the field prefix swapped: no test combines a resolver and a stem strategy, and T2 will also need the order
checked against the oracle); **m11** (a `+`/`-` mark left stale after an operand dropped by `max_word_length`: it moves to the
next operand); **m14** (`all_required` off: `+a b` under default AND must be `(a AND b)`, and no test mixes marks with default
AND). m6 (a stray `)` closes the base frame, undefined behaviour) passes `test.cc` and the vectors and is killed only because
`queryparser_random` crashes: add `a) b` and `)))` under ASan as a grammar test.

T1 compares the whole `describe()` string, or `ERROR` plus the message, with `got == want` for every supported row, and fails
on an unused exception (`test/vectors.cc`): the check is full. But the two flag sets it covers, `default` and
`boolean|phrase|lovehate`, are the same flags (`default_flags`) and their 88 rows are identical in `parse.jsonl` (checked row by
row with a script), so **176 matched is 88 distinct checks of 22 queries x 4 strategies**; the 22 queries have one bracket query
(`(barack OR michelle) AND obama`) and no three-operand chain, `NEAR/n`, `+`/`-` on a phrase or bracket, unbalanced input, or default
operator other than OR. Say
88, and treat the grammar tests (hand-written expectations from the developer's reading) as the only check of CR4's behaviours.

### CR4. The seven vector-free behaviours (low, V2 list)

Checked against the grammar the design states (D1: SYN, `+`/`-`, NEAR/ADJ, AND/NOT, XOR, OR) and the vectors:

1. Juxtaposition at the lowest level, chains flattened: **risky.** `a OR b c` is one OR under OR; under default AND it is
   `((a OR b) AND c)` and `a b OR c` is `a AND (b OR c)` (`test.cc`), which reads as the opposite of "OR binds loosest". No vector
   has three juxtaposed items. V2 must pin `a OR b c`, `a b OR c` and `a b AND c d` under both default operators.
2. `+a b`, `+a -b c`, lone `-a` an error: consistent with LOVEHATE as documented and with `NOT obama`; the default-AND case is
   untested (m14).
3. Window `n + terms - 1`, mixed NEAR/ADJ an error, the first distance wins: **inconsistent** in the last part (CR2); the
   three-term window is an extrapolation of two rows.
4. Generators `- : / . @ \`: only `-` and `:` have vectors; fine as a choice, V2 rows needed for the rest. `a.b` after the
   scanner's acronym rule (`A.B.Cx` is a three-word phrase, matching what the indexer emits) is consistent.
5. Keywords need BOOLEAN, `AND NOT`, the operator named in the error as written: fine, and `Syntax: <expression> OP <expression>`
   carries no user text, so errors cannot inject into an HTTP body.
6. Unclosed `(` closes at the end, stray `)` ignored, `()` vanishes: **risky** for an engine that may later reject them; list for
   V2 (`a AND (b OR c` succeeding silently is a choice a vector should confirm). `a AND ()` errors but `a AND () b` succeeds.
7. `obam*` is not Z-stemmed: vector-backed.

### SR5. The tree and `describe` are attacker-sized (low, note)

Depth is attacker-controlled up to a sixth of the input (a left-deep `a NOT a NOT ...` is 10^6 deep at 6 MB). `describe` and the
destructor are iterative; the adapter in Xapiand (D7) and any consumer must be too. The header should say so. Output of
`describe` is up to 11.1 bytes per input byte (phrase generators), so a 1 MiB query describes into 11 MiB.

### CR5. Smaller things (low)

- `config::default_op` is not validated: `term` gives `Query(@0)`, `phrase` and `near` give window-0 nodes (`logs/misc2.log`).
  Reject anything but `or_`, `and_`, `xor_`, `and_maybe` with an `assert` or clamp.
- `config::max_word_length = 0` drops every word; fine, say so. A dropped word gives a misleading error
  (`a AND <70-byte word>` raises the `AND` syntax error) and silently drops an exclusion (`-<70-byte word>`); the index side
  drops the same words, so this is consistent and should be documented.
- `query::text()` returns views into `pool_`; a moved or copied `query` with a small pool (std::string SSO) invalidates them. Say
  "valid while the same object lives".
- `stem_strategy::all` with no stemmer emits unstemmed terms on the query side while `index_text` emits nothing; harmless, noted.
- CI calls `Kronuz/cpp-libraries/.github/workflows/cmake.yml@main` (floating, the family's convention), and the dependency pins
  are exact commits (`analysis` `bb718c4`, which pins `unicode` and `snowball` by commit): no float in the build.
- The analysis diff reviewed: `word()`, `suffix()` and `acronym()` moved unchanged into `word_scanner` (the diff shows renamed
  members only), `term_generator` sets `scanner_.cjk_ngram` per call and calls `scan`; additive, README updated, and the
  suite passes under ASan. The only new exposure is SR1.

## Probes, mutants and logs

Clone `~/code/Kronuz/queryparser-review` at `11a6365`, analysis at `.scratch/analysis` (checked out `bb718c4`); all under
`.scratch/` (kept, untracked):

- `probe/CMakeLists.txt`, `probe/timing.cc` (37 generators, ns/B at N and 4N, growth, output and node bytes per input byte),
  `probe/sweep.cc` (code points, invalid shapes, contexts, exact-size heap buffers, watchdog, offset check), `probe/stack.cc`
  (nine 10^6 shapes on a thread stack of N KiB), `probe/misc.cc` (grammar and mark cases, resolver cases, `too_long`),
  `probe/misc2.cc` (resolver names, `default_op` misuse, RSS per byte, pool amplification), `probe/idx.cc` (indexing path),
  `probe/dict.txt` (the fuzz dictionary). Builds: `probe/build-rel/`, `probe/build-asan/`, `build-asan/` (repository tests).
- `run_all.sh`, `run2.sh`, `run3.sh`, `run4.sh`, `run_analysis.sh` (what ran), `fuzz.sh` (the uninstrumented repro of the
  ledger's setup), `fuzz2.sh` (instrumented, A at 1 MiB and B at 2 KiB), `mutants.sh` with `mut-repo/` and `build-mut/`.
- `logs/`: every output named above. `seeds/` fuzz seeds, `fuzz2-a/`, `fuzz2-b/` corpora.

## Not run

- The SR3 wrap at 2^32 pool bytes (needs 10 GiB on a shared host); Linux and FreeBSD (CI); TSan (no concurrency; the stemmer is
  caller-owned); a fuzz run by the developer's instrumented target (SR2 is theirs to close); the oracle's other four flag sets
  (step 2); WORD_BREAKS; any Xapiand-side use of the tree (step 4); behaviour against Xapian itself (clean room, no oracle).

## Re-check

Scoped re-check of SR1, SR2, SR3, SR4, CR2 and CR3 (2026-10-26), by the re-checker, not a re-review. Library `queryparser:c0a1584` (branch
`review-fixes`), then `queryparser:b732d09`, which only makes `test/oom.cc` skip itself under TSan (`SKIP_RETURN_CODE 77`); the library
and every other test are byte-identical, so nothing below was redone for it (checkout `~/code/Kronuz/queryparser-review`; the fuzz build and
the fuzz runs used `b732d09`). Scanner `analysis:9b15abf` (`.scratch/analysis-fix`). Clean room held. Everything ran under `quiet`;
no benchmark beyond the reviewer's own `timing` probe, which the task asked for (under `quiet strict`). Logs are `.scratch/logs/recheck-*`,
`mutants-fix.out`, `mutants-extra.out`, `fix-fuzz-*.log`; scripts `recheck_build.sh`, `recheck_analysis.sh`, `recheck_fuzz.sh`,
`mutants-fix.sh`, `mutants-extra.sh` next to the reviewer's, which are untouched.

| ID | Ruling |
|---|---|
| SR1 | CLOSED |
| SR2 | CLOSED |
| SR3 | CLOSED |
| SR4 | CLOSED (the scoped part; two items of the original condition stay with the host, below) |
| CR2 | CLOSED (the `-` after OR or XOR as an error is accepted; V2 rows requested) |
| CR3 | CLOSED (15 of 16 mutants killed; m10 as scripted is an equivalent mutant, the real swap is killed) |

Repository tests at `c0a1584`: `ctest` 5 of 5 pass in RelWithDebInfo and under ASan+UBSan (grammar, vectors, deep, oom, random); the vectors
runner matches 528 rows, 0 failed, 0 excepted, 0 skipped.

### SR1: CLOSED

- Fix: `analysis.hh:256-282` (`analysis:9b15abf`). `word_scanner` remembers the last failed chain as `[fail_from_, fail_to_)` and its `end`;
  `acronym` returns `nullptr` at once for a start inside it at an even offset, before it touches `text_`. `reset()` (`analysis.hh:256`)
  clears it and `term_generator::index_text` calls it per text; a `queryparser` parse builds a fresh scanner. The skip is sound: every start
  on one chain has the same chain end and the same byte after it, so it fails identically.
- Timing (`probe/build-fix-rel/timing 65536`, 64 KiB and 256 KiB, `logs/recheck-timing-64k.log`, 38 rows): `acronym-chain-then-word`
  **124.3 and 152.6 ns/B (growth 1.23)** against 29,778 and 119,785 before; `acronym-chain-AB-then-word` 101.8 and 114.2; `acronym-chain no phrase`
  168.3 and 168.0 (growth 1.00); `acronym-chain-spaced` 98.0 and 103.5. The worst row is `single letters` at 186.6 ns/B and the worst
  growth is `unclosed parens` 1.87 (25.6 to 47.8 ns/B, the allocator warm-up noted in the review). Nothing is quadratic any more; the
  256 KiB chain costs about 40 ms where it cost 31 s.
- Tests, with the fix reverted (`analysis.hh` taken from `bb718c4`): the analysis suite fails at `test/test.cc:337` (`acronym chain of 64 KiB:
  1.983 s`) and `queryparser_test` against analysis `bb718c4` fails at `test/test.cc:311` (`slow 1.159293`). Restored, both pass.
- Tokens unchanged: the analysis suite at `9b15abf` passes in RelWithDebInfo (3.5 s) and under ASan+UBSan (54 s) with its 966 vectors and
  1,114,112 code-point sweeps untouched, and the new test compares the scanner with an unmemoized copy of the old rule on 20,000 random strings
  over `AAABBb.. x1` (the alphabet that exercises chains), equal output on all.
- Not covered by a test, by reading only: the memo is keyed by the `end` pointer, so a caller that scanned one buffer, changed its bytes in
  place and scanned again with the same pointers must call `reset()`; `index_text` does, and the header comment says so.

### SR2: CLOSED

- `CMakeLists.txt:6-9` puts `-fsanitize=fuzzer-no-link,address,undefined` on the whole tree through `add_compile_options` and links only the
  target with `-fsanitize=fuzzer` (`:53`). `fuzz/run.sh` builds, then refuses to run when `nm libqueryparser.a | grep -c sanitizer_cov` is 0.
- My build through `fuzz/run.sh` itself (LLVM clang): `nm build-fix-fuzz/libqueryparser.a | grep -c sanitizer_cov` prints **10** (it was 0).
- My own 600 s run, two processes at once, the reviewer's dictionary `probe/dict.txt` and seeds, `-timeout=10`, fresh corpora:

| Run | max_len | Runs | cov | ft | Corpus | Result |
|---|---|---|---|---|---|---|
| A | 1,048,576 | 3,065,717 | 1636 | 10465 | 2535 / 462 KB | no crash, no timeout, no leak |
| B | 2048 | 2,997,198 | 1636 | 10525 | 2559 / 450 KB | no crash, no timeout, no leak |

  cov 1636 against 24 uninstrumented and 1045 to 1056 in my earlier run (the target now also drives the resolver, `max_term_bytes`, the
  step 2 flags and a tree-consistency walk, so more of the code is reachable). Runs are lower than the 7.8 million of the last round because
  the target does more per input. libFuzzer's input length stayed small again (`lim` 3167 in A), so large inputs are the timing tests' job, as
  said in SR1.
- `fuzz/queryparser.dict` is shipped; I did not diff it against `probe/dict.txt`.

### SR3: CLOSED

- The only write to the pool is `builder::store` (`queryparser.cc:216-222`); `grep` finds one `pool_.append`. It throws `limit_exceeded` before
  any 32-bit offset is taken when `text.size() > max_pool_ - pool_.size()`, and `max_pool_` is `min(config::max_term_bytes, 0xFFFFFFFF)`
  (`:129`). Every term goes through `store`: `term` (`:134`, used by `make_term` for the prefix and the `Z` marker, and by the n-gram runs),
  and `leaf` (`:142`, used by the wildcard, the edit distance and the partial wildcard, whose text is `prefix + word`). Quoted phrases and
  generator groups end in `finish`, then `make_term`. The resolver prefix is therefore counted once per term, in every path. `push` rejects the
  node count at `no_node` (`:224`). `parse` maps `limit_exceeded` to `too_long` at offset 0 (`:785`).
- Lowered-cap tests (`test/test.cc:208-247`): caps of 4, 0 and 90 bytes on plain words, wildcard, edit distance and a 30-byte resolver prefix
  (`t:a-a` passes at 2 x 31 bytes, `t:a-a-a` fails at 3 x 31, phrases too), the clamp of a 2^40 cap, and offset 0. They pass. With the check
  removed (mutant x1), `queryparser_test` fails; with the node check removed (x5) nothing fails, which is expected, since 2^32 nodes cannot be
  built in a test.
- The wrap itself still cannot be run here (needs 4 GiB of term text); the proof is the check being on the only append, plus the lowered-cap tests.

### SR4: CLOSED (scoped part)

- `parse` wraps the whole build in one `try` (`queryparser.cc:769-793`): the parser, its re-parse with plain flags (which copies the config and
  its `std::function`), and the `limit_exceeded` and `std::bad_alloc` handlers. `bad_alloc` gives `error_kind::out_of_memory`, "Out of memory"
  at offset 0. Nothing allocates outside it: `res` is empty when constructed, the early `too_long` return builds a 14-byte message and the
  `out_of_memory` handler a 13-byte one, both inside the small-string buffer of libc++ and libstdc++. `describe()` can still throw and the
  header says so (`queryparser.hh:156-158`), as it does for an exception thrown by the host's resolver.
- `test/oom.cc` replaces `operator new` and fails each allocation in turn for five queries that reach every step 2 path (re-parse, n-grams,
  wildcard and partial, a resolver prefix past the small-string size): **186 failure points, each returns `out_of_memory`, none throws**, in
  RelWithDebInfo and under ASan+UBSan (no leak). With the `bad_alloc` handler broken (mutant x2) it fails.
- Still the host's, as the review said: there is no `config::max_query_bytes` (only `max_term_bytes` bounds the term text), and the 88 B and
  11 B per input byte are in the header, not in the README's limits. I take the header as the stated contract; say so in the README before
  step 4 if the owner wants it there.

### CR2: CLOSED

- `queryparser.cc:655-670` (`push_operand`): a mark on the right operand of an explicit operator changes `AND` to `AND_NOT` for `-`, and a `-` after
  anything but AND is a syntax error naming the operator and its offset.
- V2 group `plusminus` in `parse-v2.jsonl`: `a AND -b` is `(ZSa AND_NOT ZSb)`, `a NOT -b` is the error `Syntax: <expression> NOT <expression>`,
  `a OR +b` is `(a OR b)`, `+a AND b` is `(a AND b)`, `+a OR b` is `(a OR b)`, `+a NOT b` is `(a AND_NOT b)`. Mine (`probe/misc`): `a AND -b`
  gives `Query((a@1 AND_NOT b@2))`, `a NOT -b` and `a AND NOT -b` give the NOT error, `a OR +b` and `+a AND b` give the OR and AND of V2. Tests at
  `test/test.cc:106-122` cover these and the precedence cases (`a OR b AND -c`, `a AND -(b c)`, `a AND -"b c"`). Without the `AND_NOT` change
  (mutant x3) or the error (x4) `queryparser_test` fails. The exclusion no longer turns into an inclusion.
- The judgement: **accepted**. V2 has no row for `a OR -b`, `a XOR -b` or `a AND NOT -b` (I searched all groups). V2 does answer the same shape
  for NOT (`a NOT -b` is an error), so an error after OR and XOR is the same stance and the fail-closed one; a host that appends `AND -deleted`
  is unaffected and one that appends `OR -x` gets an error instead of an inclusion. The cost is a possible mismatch with V2 on those two shapes.
  Ask the oracle for `a OR -b`, `a XOR -b`, `a AND NOT -b` and `a AND --b`, and relax the parser only if V2 accepts them. The test comment says
  they are not in V2.

### CR3: CLOSED

`mutants-fix.sh` is the reviewer's `mutants.sh` pointed at a copy of `c0a1584` (`mut-repo-fix/`), with the m14 pattern updated for the renamed
`default_op_` and m4 also split by occurrence (m4a: the `NEAR/n` digits, m4b: the `~n` digits, since the clamp now appears twice). Each runs
vectors, `queryparser_test` and `queryparser_random` (`logs/mutants-fix.out`):

| Mutant | Killed by |
|---|---|
| m1 window off by one | vectors (40 rows) and test |
| m2 `*` rule removed | vectors (5 rows) only, as before |
| m3 word cap `<=` to `<` | test |
| m4, m4a, m4b no clamp on the digits | test (`NEAR/4294967296`, `ab~4294967296`), the old survivor |
| m5 no window clamp | test |
| m6 stray `)` closes the base frame | test (the new `)))`, `)a`, `a)))` cases abort; `random` aborts too) |
| m7 mark boundary | test |
| m8 left associativity | test |
| m9 error offset | test |
| m10 `Z` and prefix swapped | **survives as scripted: equivalent mutant**, see below |
| m11 stale mark after a dropped word | test, the old survivor |
| m12 proximity kind mixing | test |
| m13 no flattening | vectors (12 rows) and test |
| m14 `all_required` off | test, the old survivor |
| m15 keyword boundary | test |
| m16 acronym capital rule | vectors (8 rows) and test |

m10 as the reviewer scripted it rewrites `if (z) { t = "Z"; }` to `t = ""` and then `t += prefix;` to `t += (z ? "Z" : ""); t += prefix;`, which
builds the same string `Z` + prefix, so it was equivalent at `11a6365` too (my error, not a test gap). The real swap (m10b: `t += prefix; t += Z`,
giving `SZrun`) is killed by the new `title:running` row (`ZSrun@1`) at `test/test.cc:203`. Extras (`logs/mutants-extra.out`): the pool check
removed (x1), the `bad_alloc` handler broken (x2, killed by `oom`), the `-` rewrite (x3) and the OR error (x4) are all killed; the node-count
check (x5) is not testable. The ledger's "176 rows" no longer applies: the runner matches 528 rows now, and I did not recount how many are
distinct.

### New findings

None. Two notes for the lead, outside this scope: the fuzz target now reads a three-byte header, so the reviewer's old seeds under
`.scratch/seeds` start as noise (fine, they only seed); and the juxtaposition grammar (CR4 item 1) is untouched, as ruled.

### Not run

TSan, and the `b732d09` skip path under it (CI's job); Linux and FreeBSD; the SR3 wrap at 4 GiB of term text; the reviewer's sweep, stack and
misc2 probes against `c0a1584` (only `timing` and `misc` were rerun); my 11-minute instrumented fuzz of the old round was not repeated at the
old settings; a diff of `fuzz/queryparser.dict` against the reviewer's; the distinct count of the 528 vector rows; any V2 oracle query; benchmarks
beyond the `timing` probe.
