# strata testing

strata's tests cover everything the Lucene, Tantivy, and Xapian test suites cover
for the features strata has, and every phase gate measures coverage. Speed claims
are only as good as the correctness behind them, so every optimization re-proves
equality with an exhaustive or oracle answer. The search tests, fuzz targets, `parity`, and
`strata_engine verify` named below live in [prospect](https://github.com/Kronuz/prospect) since
the repository split; strata's suite is the core's (`core_kv`, `files`, `faultfs`,
`merge_policy`, the `files` fuzz corpus, and the boundary checks).

## Layers

| Layer | What it proves | Where |
|---|---|---|
| Unit and model tests | Each component against a brute-force model: postings, skips, scorers, collectors, counts | `test/` |
| Pruned against exhaustive | Top-k with every pruning path equals a full scan, ties by ordinal, on the Wikipedia segment and on random segments | `strata_engine verify`, `test/search_test.cc` |
| Oracle parity | Match sets, BM25 weights, analyzer output, and parser descriptions equal Xapian's recorded vectors | `tools/parity.cc`, the oracle repository |
| Cross-engine differential | Counts equal Tantivy's and Lucene's on every gate query with equal analysis | the benchmark harness |
| Media faults | Reads that fail or return wrong bytes, transiently or for good, at offsets across every file: transient faults heal, damage fails closed or is reported, no answer is ever wrong | `media_test` on faultfs media faults; `docs/design/media-faults.md` |
| Fuzzing | Every decoder and parser: corrupt input fences or answers, never crashes, hangs, or reads out of bounds | `fuzz/`, built from CMake; one CPU-hour at the gate's commit for each decoder or parser added or changed in the phase; every target's checked-in corpus replayed as a CTest |
| Sanitizers | ASan, UBSan, and TSan over every test | every gate |
| Crash and power loss | Every barrier of randomized workloads, with `faultfs` | Phase 3 onward |
| Churn | Long add, replace, and delete cycles against a model: small repeated changes to the same keys, bursts of many adds then mass deletes, documents of megabytes and tens of thousands of terms alternating with tiny ones. Every cycle: contents and digest equal the model, queries equal brute force, `index::check` passes, with reopens and crash points between | `test/churn_test.cc`, Phase 3 onward |
| Steady state | Churn at a constant live set for many cycles stays bounded: disk within a stated multiple of a fresh index of the same documents, segment count and deleted fraction within the merge policy, query time within a stated factor of the fresh index, writer memory flat, write amplification within the T3 bound | `test/churn_test.cc` (bounds), the gate VM (time and memory), Phase 3 onward |
| Comparative soak | The same churn workload through strata, Lucene, Tantivy, and Xapian: throughput, disk, memory, and query latency over time | `bench/soak` (each engine's `churn` mode, `bench/engines`), on the gate VM and the bare-metal machine; a ratchet row from Phase 3 |
| Deterministic simulation | Clusters with crashes, partitions, skew, corruption, membership changes; linearizability checked | Tracks C and D |
| End to end | Xapiand's documentation examples as an E2E suite | Phase 7 |
| Mutation testing | Mutants of the core's decoders and scorers are killed by the suite | Mull (LLVM), each phase gate; the report row is the mutation score (killed over total) |

## Coverage

Source-based coverage (`llvm-cov`) over the phase's code, every file and function
added or changed in the phase. It runs over the unit tests, `parity`, `verify` on a
sample index, and the checked-in fuzz corpora replayed:

| Code | Lines | Branches |
|---|---|---|
| Decoders, readers, format | ≥ 98% | ≥ 95%, and every corruption check reached by a unit test or by the fuzz corpus replayed as a test |
| Scorers, collectors, counting | ≥ 95% | ≥ 90% |
| Everything else in strata | ≥ 90% | ≥ 80% |

A line left uncovered is listed in the gate report with its reason.

## Test parity

prospect's [`docs/testing/parity.md`](https://github.com/Kronuz/prospect/blob/main/docs/testing/parity.md) is the inventory: one row per test area of each baseline,
the strata tests covering it, and its status (ported, covered differently, not
applicable with a reason, or pending with its phase). It is built first for the
Phase 2 scope, before Phase 3's gate, and each phase gate requires its rows ported
and passing.

- **Lucene** (Apache-2.0) and **Tantivy** (MIT). Their tests are read for what they
  check and re-implemented as strata tests. Code is not copied; a test that must keep
  its original form keeps its license header and notice.
  - Lucene areas: the postings, term-dictionary, stored-field, and norms format test
    cases; `CheckIndex`-style integrity checks; boolean, phrase, sloppy-phrase, and
    span queries against brute force (`TestBoolean2`-style random queries);
    block-max and MaxScore scorers; top-docs collectors and tie-breaking; index
    writers under deletes, updates, merges, concurrency, crashes, and corruption;
    directory and checksum behavior; analysis; packed integers and FSTs.
  - Tantivy areas: postings serialization and skip readers; block-WAND and boolean
    queries, including its property-based tests; phrase queries and positions;
    collectors; merges and deletes; the document store; the term dictionary.
- **Xapian** (GPL). Under the clean-room rule its source, test suite included, is
  never read. Its behavior enters strata as oracle vectors that the GPL oracle
  records from Xapian's public API: query operators (`AND_MAYBE`, `AND_NOT`, `NEAR`,
  `PHRASE`, `ELITE_SET`, `SYNONYM`, `MAX`, `SCALE_WEIGHT`, `XOR`), weighting schemes,
  `TermGenerator`, `QueryParser`, stemmers, `_percent`, estimates, and value ranges.
  The areas come from Xapian's public documentation and the features Xapiand uses.
