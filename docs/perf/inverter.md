# Inverter: one-thread indexing throughput

Targets T4's one-thread row (PLAN, "Indexing speed": at Phase 3, at least 1.0× the faster
of one-thread Lucene and Tantivy, keyed, at equal memory). At the n1017 gate run (5M
documents, `STRATA_BUDGET_MB=2056`, medians of five trials; `perf/indexing-gap`'s evidence
table) strata took 1,091 s, Tantivy-0.26-st 512 s, and Lucene-10.4.0-st 890 s: 0.47× the
faster.

Scope: the work one thread does per document (the engine's parse and tokenize, stage 1's
checks, the buffer) and the flush's inversion and encoding. Eight-thread scaling is
`perf/indexing-gap`'s, index size and merging `perf/index-size`'s.

Every experiment reports the four axes: wall time, peak RSS, index bytes, and segments,
plus `verify` and oracle parity on its index. A/Bs run strata's two builds and
Tantivy-0.26-st (the anchor, and the drift check) interleaved, at least five trials, on
the first 100,000 documents of the gate corpus, after a 20,000-document dry run, under a
light hold.

## E0: where one thread's time goes (profile, no code change)

Hypotheses, each with what would refute it:

- H1: at 100,000 documents (no flush before the commit, no merges) more than half the
  time is per-document allocation, copying, and hashing ahead of encoding: the engine's
  `tokens_document`, stage 1's `valid` and the buffer's copy of the document, the version
  map, and `segment_writer::add`. Refuted if `flush_from`'s encoding (postings, positions,
  competitive pairs, the dictionary, stored frames) holds more than half the samples.
- H2: no single function holds more than a quarter of the samples. Refuted by one that
  does; it is then the first experiment's target.
- H3: the commit's sync is under 5% of the wall time on both platforms. Refuted if it is
  more.

Procedure: `strata_engine` at `4c9ebfd`, Release with `-march=native -g
-fno-omit-frame-pointer`, indexing the first 100,000 documents of the gate corpus with
`STRATA_BUDGET_MB=2056` (the gate's budget), a 20,000-document dry run first. macOS
(i9-9980HK, APFS): `sample` at 1 ms over the whole run; Linux (the gate VM): `perf record
-F 999 -g`. Reported: inclusive shares of each phase (read and parse, tokenize, stage 1 and
its parts, inversion, each encoder, the commit), and the hottest functions by self time.

## Experiments

Each is written here before its A/B runs: the hypothesis, the mechanism, and the rule that
keeps or removes it.
