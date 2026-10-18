# Inverter: one-thread indexing throughput

Targets T4's one-thread row (PLAN, "Indexing speed": at Phase 3, at least 1.0× the faster
of one-thread Lucene and Tantivy, keyed, at equal memory). At the n1017 gate run (5M
documents, `STRATA_BUDGET_MB=2056`, medians of five trials; `perf/indexing-gap`'s evidence
table) strata took 1,091 s, Tantivy-0.26-st 512 s, and Lucene-10.4.0-st 890 s: 0.47× the
faster.

Scope: the work one thread does per document (the engine's parse and tokenize, stage 1's
checks, the buffer) and the flush's inversion and encoding. Eight-thread scaling is
`perf/indexing-gap`'s, index size and merging `perf/index-size`'s.

**Design target** (PLAN, "Indexing speed", owner 2026-10-17): at least 4.5× the faster
baseline's throughput, about 1:55 at one thread on 5M documents against Tantivy's 8:30. Every
result below is stated as a ratio to Tantivy and as the distance left to 4.5×.

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

### E0 results, macOS

21,454 samples (1 ms) over one run of 21.5 s, 100,000 documents, one segment, at `4c9ebfd`
(`.scratch/runs/e0-mac.sample.txt`, aggregated by `.scratch/tools/sample_agg.py`):

| Phase | Samples | Share |
|---|---:|---:|
| Engine: read lines, find `id` and `text` | about 210 | 1.0% |
| Engine: `tokens_document` (an `unordered_map` per document, a vector and a string per distinct term) | 7,105 | 33.1% |
| Stage 1, `index::apply`: `valid()`'s `std::set` 3,256 (15.2%), the buffer's deep copy of the document 1,067 (5.0%), keys and the version map about 300 | 4,828 | 22.5% |
| Clearing the transaction (`~operation`) | 669 | 3.1% |
| Flush: inversion, `segment_writer::add` (`buffer_for` 1,835, `put_stream` 1,074) | 4,592 | 21.4% |
| Flush: encoding, `segment_writer::flush` (term sort 1.8%, postings, positions, impacts, dictionary, stored frames) | 2,712 | 12.6% |
| Flush: freeing the buffered documents, other | about 1,250 | 5.8% |
| Commit: files and sync | 85 | 0.4% |

The allocator's own frames (`malloc`, `free`, and their nanozone helpers) hold about 30% of
all samples by self time. The corpus sample has 18.3M tokens, 8.8M distinct (document, term)
pairs, and 338,367 distinct terms besides the 100,000 keys: the current path spends about
390 ns per token in `tokens_document` and 370 ns per distinct term in `valid()`.

- H1 holds: per-document allocation, copying, and grouping (the engine, stage 1, and
  inversion) hold 80% of the samples; encoding holds 12.6%.
- H2 is refuted by `tokens_document` (33.1% inclusive), the first target.
- H3 holds: the commit is 0.4%.

Linux (the gate VM): owed, while the T3 run and the indexing gap's hold have the VM.

### Calibration, macOS, 100,000 documents

Five interleaved rounds (`.scratch/tools/ab_index.py`, `runs/cal-mac`), keyed, strata at
`STRATA_BUDGET_MB=2056`:

| Engine | Median wall | Range | Peak RSS | Index bytes | Segments |
|---|---:|---:|---:|---:|---:|
| Tantivy-0.26-st | 12.10 s | 11.62–12.45 | 420 MB | 47,919,012 | 1 |
| strata `4c9ebfd` | 21.88 s | 21.35–27.44 | 1,141 MB | 43,493,711 | 1 |

strata's throughput is 0.544× Tantivy's (95% interval 0.449–0.569). 4.5× is about 2.69 s
at this sample on this host: 8.1× faster than strata today.

## Experiments

Each is written here before its A/B runs: the hypothesis, the mechanism, and the rule that
keeps or removes it.

### E1: documents as token streams, no per-term allocation

Hypothesis: the engine's grouping of tokens by term and stage 1's duplicate check, with the
heap objects per distinct term they create and free (an `unordered_map` node, a vector, and a
string per term in the engine; a `std::set` node per term in `valid()`; the buffer's deep copy
of all of them), are 50 to 60% of the one-thread time at 100,000 documents, and come out
without changing a byte of the segment.

Mechanism:

- `document` gains a token stream, additively: `text`, one string, and `tokens`, each a term
  at an offset and size of `text` and a position. Each token adds one to its term's wdf and its
  position to the term's positions, as `segment_writer::add_tokens` already inverts them
  (byte-identical to `add`, `segment_test`). `terms` keeps the terms with an explicit wdf, the
  positionless ones, and the key; a term is in one of the two.
- Stage 1 validates tokens without a set: in bounds, sized 1 to `max_term_bytes`, positions
  strictly increasing, none with the key prefix (the key stays in `terms`); a `terms` entry
  among the tokens is found through the entries' first bytes, and `terms` itself is checked by
  sorting its views when it has more than one.
- The flush inverts each buffered document's `terms` and `tokens` as one document.
- The engine builds that form: the text copied once, one token per word, the key in `terms`:
  two allocations per document instead of about three per distinct term.

Neither baseline hands the writer a document this way: Lucene's `IndexingChain` and Tantivy's
`SegmentWriter` pull each field's tokens one at a time from an analyzer's stream and insert
each into the term hash as it comes. A flat array of a document's tokens is what lets a later
step hash them all first and prefetch their table slots ahead of the probes.

Keep rule: every segment file byte-identical to `4c9ebfd`'s on the 20,000 and 100,000
samples, ctest green, and the 100,000-document A/B at least 1.3× faster than `4c9ebfd` with
the interval excluding 1.0. Otherwise removed.
