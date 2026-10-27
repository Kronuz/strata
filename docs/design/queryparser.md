> Status: DRAFT (2026-10-26) designing (ready for preliminary review); workstream L1 of
> `xapiand-on-strata.md`; ledger `docs/status/queryparser.md`.

# queryparser: a clean-room, MIT query-string parser for Xapiand and strata

Revision 1 (2026-10-26). Reads strata `5a0299b`, strata-oracle `a2e1d82`, Xapiand
`engine-query` `a808ecc9d`, analysis `a0825b1`, Lucene `9983b7c`, Tantivy 0.26.0.

Every `text` field query in Xapiand goes through Xapian's `QueryParser`
(`engine/xapian/query.cc:97-113`), so strata cannot serve Xapiand without a replacement
(`xapiand-on-strata.md` Q3, S4). The design: one iterative parser whose **compat preset**
reproduces today's syntax, proved against the oracle vectors and then inside Xapiand on Xapian
itself, plus **extensions** from Lucene and Tantivy (boosts, escaping, phrase slop, lenient
errors; later regex and field grouping) behind flags. It depends only on `analysis`, `snowball`
and `unicode`, and returns its own tree, which Xapiand adapts to `engine::Query` in one place.

Clean room: no Xapian source and no GPL file was opened, `strata-oracle/vectors/oracle.cc`
included (the repository is GPL, its README says). Behaviour comes from `parse.jsonl` and
xapian.org's `queryparser.html`. Lucene (Apache-2.0) and Tantivy (MIT) sources were read for the
survey; nothing is ported yet, and any port carries its notice (T7).

## What this depends on and changes

| Document | Relation |
|---|---|
| `xapiand-on-strata.md` L1, S4, D6 | Implements L1. S4 calls this library through the adapter of D7. Proposes changing D6 once T4 passes (Q4): parse host-side on both engines and drop Xapian's parser from Xapiand. |
| `limits.md` | Adds the rows of D9 when this design is approved. |
| PLAN Phase 5 ("`queryparser` vectors pass") | T1 and T2 are that row. Regex waits for Phase 5's `automata`. |
| `analysis` (library) | D3 makes its word scanner public; its 966 vectors prove nothing changed. |
| `boolean-parser` (library) | Untouched now. Q2 proposes that `queryparser` later replaces it in Xapiand's `q=` layer. |

## 1. What Xapiand asks of a parser today

Xapiand builds one `Text` node per text field value (`query_dsl.cc:742-841`): the parser sees a
field's value, never the `field:value` query string, which Xapiand splits itself with
`boolean-parser` (`query_dsl.cc:940-1142`).

| Setting | Values Xapiand passes (`query_dsl.cc`, `engine/query.h:93-124`) |
|---|---|
| Flags always set | PHRASE, LOVEHATE, FUZZY (`:834-836`) |
| Flags sometimes set | BOOLEAN when the default op is AND or OR (`:805-808`); WILDCARD for a trailing `*`, WILDCARD_SINGLE and WILDCARD_MULTI for `?`/`*` inside (`:745-779`); PARTIAL for `**` or `_partial` (`:744-746`, `:344`); CJK_NGRAM per field (`:826-828`); WORD_BREAKS only with ICU (`:829-833`) |
| Default operator | unset (OR), AND, OR, NEAR, PHRASE, ELITE_SET, SYNONYM, MAX (`:797-810`) |
| Stopper, stemmer, strategy | set only when the default op is not NEAR, PHRASE, ELITE_SET, SYNONYM or MAX (`:818-824`); strategy none, some, all, all_z |
| Default prefix | field prefix plus type character (`:796`), passed to `parse_query` (`xapian/query.cc:113`) |
| Expansion | partial 50, most frequent; fuzzy 0 (no limit), error (`:66-69`, `:838-839`) |
| Not used | `add_prefix`, range processors, spelling, synonyms, PURE_NOT, BOOLEAN_ANY_CASE |

Errors become a client error carrying Xapian's message (`handler.cc:1323-1324`; engine kind
`invalid`, `convert.cc:152`). The E2E suite's text queries live in nine pages under
`leaf-queries/text-queries/`.

## 2. The oracle and what it does not cover

`parse.jsonl` holds 528 rows: 22 queries (`inputs/queries.txt`) × 6 flag sets × 4 stemming
strategies, each a `Query::get_description()` string or an error message. Its flag sets are
`default`, `boolean|phrase|lovehate`, `default|wildcard|partial`, `default|fuzzy`,
`default|boolean_any_case|pure_not`, `default|cjk_ngram`. Observed behaviour the parser must
reproduce includes: Z stems only for uncapitalised words under `some`; no stems inside
phrases, NEAR and ADJ; a window of 11 for two-word NEAR and ADJ; `1990s-era` and `title:obama`
as phrases; CJK n-grams ANDed at one position; `NOT obama` raising `Syntax: <expression> NOT
<expression>` (20 rows) unless PURE_NOT.

Two gaps make a second vector set necessary:

1. **Xapiand's configurations are absent.** No row sets a stopper, a default prefix, a default
   op other than OR, PHRASE, LOVEHATE and FUZZY without BOOLEAN, WILDCARD_SINGLE/MULTI, or an
   expansion limit.
2. **One behaviour is undetermined.** Under `default|wildcard|partial`, `barack NEAR obama`
   gives `(barack@1 OR near@2 OR obama@3)` (8 rows): operators read as words and no partial
   expansion, which looks like a reparse after a failure. Yet `NOT obama` raises instead. The
   rule that separates them cannot be read from these rows.

**V2** (`parse-xapiand.jsonl`) is the matrix of section 1 over the existing queries plus inputs
for stop words, `+`/`-` on phrases and brackets, `NEAR/n`, `ADJ/n`, `word~`, `"a b"~3`,
acronyms, `c++`, `3.14`, a 70-byte word, deep brackets, unbalanced quotes and brackets, empty
and all-stop-word texts, and the fallback cases. Q1 decides who records it.

## 3. Decisions

**D1. One parser, presets of it.** A hand-written lexer and an operator-precedence parser
(Xapian's documented order: SYN, `+`/`-`, NEAR/ADJ, AND/NOT, XOR, OR) driven by a flag word.
`preset::xapian` is the compat flag set; extensions (D10) are more flags. No second parser per
dialect (one implementation per mechanism). Alternative: a generated LALR parser, rejected: a
generator dependency, and table parsers recover errors poorly (D6).

**D2. Iterative, with no recursion anywhere.** Parsing uses an explicit operator and operand
stack on the heap; destruction and `describe` walk with an explicit stack, as `engine::QueryNode`
already does (`engine/query.h:199`). Depth is then bounded by input length alone (D9).

**D3. Words come from `analysis`.** Query words must equal index terms, so the scanner
(`term_generator::word`, `suffix`, `acronym`, `ngrams`, `analysis.hh:227-314`) becomes a public
`analysis::word_scanner` that both call. Stop words use `analysis::stopper`; stems, a
caller-owned per-thread `snowball::stemmer*`. Query-only rules (operators, quotes, `*`, `?`, `~`,
phrase generators such as `-` and `:`, the capital heuristic) live in `queryparser`.

**D4. Output: the library's own tree.** Nodes: `term{text, wqf, pos}`, `op{kind, children,
window}` for AND, OR, AND_NOT, XOR, AND_MAYBE, NEAR, PHRASE, SYNONYM, ELITE_SET, MAX,
`scale{factor}`, `wildcard{pattern, max_expansion, limit, pattern_flags, combiner}`,
`edit_distance{pattern, distance, max_expansion, limit, combiner, prefix_length}`, `match_all`,
`match_nothing`. Each maps one-to-one onto an `engine::Query` factory (`engine/query.h:137-163`).
`describe(tree)` prints `get_description()`'s format, which is how rows compare. Building
`engine::Query` directly was rejected: it ties the library to Xapiand's headers.

**D5. Errors.** `parse` returns the tree or a `queryparser::error{kind, byte offset, message}`;
it never throws on input. In the compat preset the message equals the oracle's text where a
row records one (Xapiand puts it in the HTTP body).

**D6. Fallback and lenient mode are one mechanism.** The compat preset reproduces the reparse
of section 2 exactly as far as V2 determines it. The `lenient` extension (Tantivy's
`parse_query_lenient`) returns the best tree plus every error with its offset. Both are
"on error, retry with fewer syntax flags", one code path with two policies.

**D7. Xapiand integration, before strata.** `src/engine/text.{h,cc}` (MIT) maps `TextConfig` to
a parser config and the tree to `engine::Query`. Two switches use it on Xapian:

1. `ENGINE_QUERYPARSER_AB` (build option): `parse_text` also runs `queryparser`, compares
   `describe()` with Xapian's description, and counts mismatches per request into the census
   file of `xapiand-on-strata.md` D12.
2. `XAPIAND_TEXT_PARSER=queryparser` (environment): the `Text` node is expanded host-side by
   `queryparser`, so the whole E2E suite runs our parser on Xapian.

At S4 the strata implementation calls the same adapter.

**D8. 2.0 against 2.1.** The vectors record Xapian 2.1.0; Xapiand vendors 2.0.0. Where the
in-Xapiand A/B and a vector disagree, the compat preset follows Xapiand's 2.0 (what users get
today) and lists the row as an exception (Q5).

**D9. Limits.** No reference engine documents a cap on query length, nesting, or clause count
inside its parser; each limit below names its mechanism.

| Limit | queryparser | Mechanism | Lucene `9983b7c` | Tantivy 0.26 | Xapian (docs) |
|---|---|---|---|---|---|
| Query text bytes | none (`size_t`) | single pass, O(n) memory; the HTTP body bounds it upstream | none in the parser | none | none documented |
| Nesting depth | none | D2: heap stacks; each level costs ≥ 1 input byte | JVM stack (recursive JavaCC) | process stack (recursive nom) | none documented |
| Clauses | none | the engine's query budget | 1,024 (`IndexSearcher.java:80`), at query build | none | none documented |
| Word bytes | 64, then dropped | must equal the index side (`analysis.hh:192`); V2's 70-byte row checks the query side | 32,766 per term | 65,530 per token | 245 per term (backend) |
| NEAR/ADJ window, `~N` distance | 2^32 − 1 | `termpos` and `unsigned` in `engine::Query` | slop `int`; edits ≤ 2 (`LevenshteinAutomata.java:37`) | slop `u32` | none documented |

Expansion caps belong to the nodes (Xapiand's 50 and 0) and the engine, not the parser.

**D10. Extensions, judged by value against complexity and what strata can run.** Each is a flag,
off in `preset::xapian`.

| Feature | Lucene | Tantivy | Verdict | Runs on |
|---|---|---|---|---|
| Boost `term^2`, `"..."^2`, `(...)^2` | classic `QueryParser.jj:311,338` | `boost`, `query_grammar.rs:837` | **adopt** | SCALE_WEIGHT (P3) |
| Escaping `\` | `_ESCAPED_CHAR` | in quoted strings | **adopt**: `c++`, `-x`, `NOT`, `a:b` become searchable | terms |
| Phrase slop `"a b"~N` | `FUZZY_SLOP` | `slop_or_prefix_val`, `:439` | **adapt** to NEAR, window words + N; Lucene's slop counts position moves, not a window | NEAR |
| Lenient errors | `SimpleQueryParser` ignores syntax errors (`SimpleQueryParser.java:44`) | `parse_query_lenient` | **adopt** (D6) | n/a |
| `&&`, `\|\|`, `!` | classic `:196-198` | no | **skip**: no value over words | |
| Regex `/re/` | `REGEXPTERM` | `regex`, `:699` | **adapt later**, behind Phase 5 `automata`; Lucene's determinize cap (10,000 work units) becomes a load-bearing row then | dictionary scan |
| Ranges `[a TO b}`, `>=x`, `*` bounds | standard parser, `StandardSyntaxParser.jj:154-199` | `range`, `:451-453` | **adapt at the `q=` layer** (Q2): a range is typed, so the host builds it through a field resolver | Xapiand range sources |
| Field `f:x`, grouping `f:(a b)` | both | `f:x` | **adapt at the `q=` layer** (Q2) via a resolver callback | per-field prefixes |
| Sets `f: IN [a b]`, exists `f:*` | no | `set`, `exists` | **skip**: a set is an OR; `f:*` already exists in Xapiand (`query_dsl.cc:631`) | |
| Phrase prefix `"a b"*` | no | yes | **skip** for now: PARTIAL covers type-ahead | |

## 4. Gate rows

Functional rows, on the branch or `main`; no row measures speed.

| Row | Target | Procedure |
|---|---|---|
| **T1** oracle parity | Every one of the 528 rows of `parse.jsonl` at strata-oracle `a2e1d82` matches (`describe()` or error message), or is listed in `test/vectors/EXCEPTIONS.md` with its reason | `queryparser_vectors test/vectors/parse.jsonl` exits 0 and prints matched, excepted, failed |
| **T2** Xapiand parity | The same for every V2 row | Same tool on `parse-xapiand.jsonl` |
| **T3** A/B in Xapiand | Zero description mismatches over the E2E suite, or each listed | Xapiand built with `ENGINE_QUERYPARSER_AB`; `harness/e2e_capture.sh`; mismatch count from the census file |
| **T4** E2E on our parser | Zero assertion regressions against the Xapian-parser baseline | `XAPIAND_TEXT_PARSER=queryparser`, then `e2e_diff.py` as in `xapiand-on-strata.md` T3 |
| **T5** robustness | libFuzzer clean under ASan and UBSan for a stated run with `-max_len=1048576 -timeout=10`; 10^6 nested `(` and a 10^6-long `NOT` chain parse, describe, and destroy without stack overflow | `fuzz/fuzz_parse.cc`; `test/deep.cc` under a 512 KiB thread stack |
| **T6** extensions inert | With every extension flag off, T1 and T2 results are unchanged; each extension has its grammar tests | CI runs T1, T2 per flag |
| **T7** license | No GPL file in the repository; `NOTICE` names each ported Lucene or Tantivy piece and its license | `git ls-files` review; a `grep -ri gpl` check in CI |

## 5. Open questions for the owner

Rulings (2026-10-26): Q1 (a), the lead's call. Q2 (b), after the 7a gate (owner). Q3 (c): breaking
syntax ships with a major Xapiand version, with no setting or flag (owner); the extended grammar is
the default from that release, and the compat preset stays for T1. Q4 yes, at T4 (owner). Q5 as
recommended, the lead's call.

**Q1. Who records V2?** It extends the GPL `oracle.cc`, which the parser's developer must not
read. Options: (a) a separate agent, never assigned to `queryparser`, working from Xapian's
public API; (b) the lead. Recommendation: (a); the developer sees only `.jsonl` files. Blocks
step 3, not steps 1 and 2.

**Q2. Should `queryparser` later parse the whole `q=` string?** Xapiand has two boolean grammars:
`boolean-parser` for `q=` (unary NOT, a MAYBE operator, NOT > AND > MAYBE > XOR > OR, its README)
and Xapian's inside each value (binary NOT, no MAYBE, AND/NOT > XOR > OR). Options: (a) text
values only; (b) after S4, `queryparser` with a field-resolver callback replaces `boolean-parser`
in `make_dsl_query`, bringing `f:(a b)`, ranges and one grammar. (b) breaks `q=` strings that use
NOT or MAYBE. Recommendation: (b) after the 7a gate, behind Q3's setting, with its change list in
its own design; the resolver hook is in the API from step 1.

**Q3. How do breaking extensions reach users?** Escaping, boosts and slop change answers:
`\`, `^` and `~` after a quote are separators today. Options: (a) an index setting
`query_syntax: xapian | extended`, fixed at creation like the engine (`xapiand-on-strata.md`
D8); (b) a per-request flag; (c) change for everyone. Recommendation: (a) with (b) as an
override; new indexes default to `extended` after the 7a gate.

**Q4. Parse host-side on both engines once T4 passes?** That is D6's "cleaner end state" in
`xapiand-on-strata.md`: the `Text` node and Xapian's parser leave Xapiand before Phase 8.
Recommendation: yes, at T4.

**Q5. Which Xapian is the reference when 2.0 and 2.1 differ?** Recommendation: 2.0 for the
compat preset (D8), the differing rows listed.

## 6. First steps

Sizes are estimates for pacing: S small, M medium (one launch under about 300k tokens).

| Step | Where | Done when | After | Size |
|---|---|---|---|---|
| **1** word scanner and parser core | `analysis` (one commit), new local repo `~/code/Kronuz/queryparser` (published on the owner's word) | `analysis::word_scanner` extracted, analysis's 966 vectors unchanged; tree, `describe`, iterative lexer and parser, default op, BOOLEAN with brackets, LOVEHATE, PHRASE with generators, NEAR/ADJ and `/n`, stemming strategies, errors; the field-resolver hook declared; T1 passes on the 176 rows of `default` and `boolean\|phrase\|lovehate`; T5's deep-nesting test | none | M |
| **2** compat remainder | queryparser | WILDCARD, glob, PARTIAL, FUZZY, CJK_NGRAM, PURE_NOT, BOOLEAN_ANY_CASE, fallback; T1 complete; fuzz target | 1 | M |
| **3** V2 parity | queryparser | stopper, default ops, prefix, expansion fields; T2 | 2, V2 (Q1) | M |
| **4** Xapiand A/B | Xapiand `phase0.5/queryparser` | D7's adapter and both switches; T3, T4 | 3, X3 | M |
| **5+** extensions | queryparser | one flag per step (escaping, boosts, slop, lenient), T6 each | 2, Q3 | S each |

Step 1 starts now: no open question blocks it, and it needs no strata or Xapiand build.

## 7. Limits of this design

- **Silent divergence:** a query shape no vector and no E2E request covers can parse
  differently from Xapian with every row passing. T3 narrows this; nothing closes it.
- **WORD_BREAKS (ICU) is not covered**, as at index time (`generate_terms.cc:503`).
- **Nothing is built or run.** Xapian behaviour rests on `parse.jsonl` rows and xapian.org
  documentation; Xapiand facts on the cited lines.
