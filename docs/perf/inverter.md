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

Result (`d5468e4`): kept. Every segment file, manifest, and metadata file byte-identical to
`4c9ebfd`'s at 20,000 and 100,000 documents; ctest 33 of 33. Five interleaved rounds at
100,000 documents (`runs/e1-ab`, the host's load falling during the run, Tantivy 9.31 → 7.00 s):

| Engine | Median wall | Range | Peak RSS | Index bytes |
|---|---:|---:|---:|---:|
| Tantivy-0.26-st | 8.09 s | 7.00–9.31 | 421 MB | 47,919,012 |
| strata `4c9ebfd` | 13.88 s | 11.84–17.25 | 1,141 MB | 43,493,711 |
| strata E1 | 6.73 s | 5.59–8.12 | 829 MB | 43,493,711 |

E1 is 2.12× faster than `4c9ebfd` (per-round ratios 2.01–2.34, interval 2.01–2.34) and
1.20× Tantivy's throughput (interval 1.15–1.26), up from 0.58×; 4.5× is 3.74× away. Peak RSS
falls 27%, since a token document is smaller than its terms grouped.

What is left (`runs/e1-mac.sample.txt`, about 4,900 active samples): the flush holds 84% of
them, inversion (`segment_writer::add`) 49% and encoding (`flush_from`) 28%; the per-document
path (parse, stage 1, the buffer copy) 15%. In inversion, `buffer_for`'s hash, probe, and
`memcmp` take 22% and the LEB128 streams in `std::string`s 18%; in encoding,
`write_postings` 21%, decoding the streams back 17%, positions 11%, competitive pairs 9.5%,
the dictionary 7%, checksums 7%.

### E2: inversion on arrival

Hypothesis: buffering documents and inverting them at flush costs one copy and one free of
every document (about 7% at 100,000 documents) and, at the gate's scale, holds the buffer in
a form about five times larger than its postings (a token document is about 3.3 KB here; its
postings, positions, and columns about 0.6 KB), so a budget holds a fifth of the documents a
flush could, and 5M documents make five times the segments and the merges.

Mechanism: stage 2 inverts each accepted transaction's documents into the index's active
`segment_writer` as `apply` returns (D18's stage 2 at one thread: the design's diagram
inverts on arrival; the code inverted at flush). Stage 1 evaluates on pointers into the
transaction, so a rejected transaction still leaves nothing. A document replaced or deleted
while buffered is marked dead by ordinal. The flush writes the writer's postings as they are
when the buffer's docids increase and none died (the case for appends); otherwise it rebuilds
them in docid order without the dead, as a merge writes its output (`add_document` and
`flush_from`), so the segment is byte for byte what flushing the live documents in docid
order writes today. The budget charges the writer's own allocations (D17) instead of an
estimate per document.

Neither baseline does the slow half this way: Lucene and Tantivy also invert on arrival, but
keep a replaced document in the flushed segment as a deleted one; strata's segments never
hold a document dead at birth, so the format is unchanged.

Keep rule: byte-identical segments at 20,000 and 100,000 documents; a test that replaces,
deletes, and reorders docids inside one buffer checks the flushed segment against the same
live documents flushed in docid order (equal bytes and digests); ctest green; the 100,000
A/B not slower than E1 (a win at the gate's scale is checked by the confirmation run, since
100,000 documents never fill the buffer). Otherwise removed.

Result: kept, as the scaffold for E3. Byte-identical segments, manifests, and metadata at
20,000 and 100,000 documents; ctest 33 of 33, with a new index_test case (12 seeds of 900
transactions: replaces and deletes inside the buffer, explicit docids out of order,
rejected transactions, term and token documents mixed) whose segments equal, byte for byte,
an index that buffers whole documents and inverts them at flush; replacing the rebuild with
the as-is flush makes it abort ("docids must be distinct"). The term table and arena now
start small (1,024 slots, 4 KiB chunks), since a buffer holds them from its first document.
Five interleaved rounds at 100,000 documents under host load (`runs/e2-ab`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 8.47 s | 7.32–9.05 | 418 MB |
| strata E1 | 6.89 s | 6.11–7.72 | 817 MB |
| strata E2 | 6.28 s | 5.40–7.17 | 446 MB |

E2 is 1.077× faster than E1 (interval 1.034–1.131) at 45% less peak RSS, and 1.26× Tantivy
(interval 1.17–1.57); 4.5× is 3.57× away. The memory is the larger effect: 446 MB is about
4.5 KB a document in RSS for the whole process, where E1 held 8.2 KB.

### E3: the tuple-stream inverter (the research note's C)

The research note (`research/indexing`, `docs/research/indexing.md` sections 3, 5.2, 6)
measured three inverters writing byte-identical segments through `flush_from`: today's
hash-append (A, 340 ns a token on this host), Lucene's and Tantivy's slices (B, 139), and
tuples in arrival order with a radix-partitioned flush (C, 118; its add phase 25 against
A's 191). The lead's direction (2026-10-17): build C into the index path, take out the
per-document representation and bookkeeping, then the encoder's per-term cost.

Hypothesis: with C as the active buffer, the add phase (now `segment_writer::add` on
arrival, about half of E2's time) falls to the cost of hashing each token and appending a
tuple, and the flush's grouping stays in cache; 100,000 documents at least 1.5× faster
than E2.

Mechanism, as the note's section 6 lays it out, with what E2 built kept around it:

- On arrival each token becomes a 20-byte tuple (hash, ordinal, text offset, position,
  length) appended in arrival order, the token's bytes copied once into the buffer's text
  arena; nothing is looked up.
- At flush the tuples are partitioned by the hash's top byte into 256 runs; each run gets
  an L2-sized table keyed by hash and a stable counting sort by term, and every token's
  bytes are compared with its term's first, so two terms whose hashes collide are never
  merged. Terms sort, and each term's postings go to `flush_from` in ordinal order, so the
  encoder and the segment are unchanged.
- E2's dead ordinals and docid order become a relabelling during the grouping: one lookup
  per tuple through the live documents' new ordinals.

Keep rule: byte-identical segments at 20,000 and 100,000 documents; the E2 equivalence test
and ctest green; the 100,000 A/B at least 1.3× faster than E2 with the interval excluding
1.0. Otherwise removed.

Result (`include/strata/tuple_buffer.hh`): below its own keep rule on time (1.21×, not 1.3×),
kept on the lead's direction as the base of the research design (the partition on arrival,
the term-partitioned flush at more threads, document order at flush); its memory is the open
cost. Byte-identical segments, manifests, and metadata at 20,000 and 100,000 documents;
ctest 34 of 34, including `tuple_buffer_test` (6 seeds of 2,000 random documents against
`segment_writer::add`, equal bytes with the hash whole and narrowed to masks that make terms
collide, tokens of 254, 255, and 300 bytes, positions past 2^24, terms given whole with an
explicit wdf, positionless, or long, documents left out and docids shuffled; with the
collision split disabled it fails) and E2's equivalence test, now over the tuple buffer.
E2's `segment_writer` additions (the rebuild, scans, per-document keys) are gone; the
writer is E1's again, with `no_more_docs` now declared in it.

Five interleaved rounds at 100,000 documents (`runs/e3-ab2`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 6.52 s | 5.79–6.77 | 420 MB |
| strata E2 | 5.33 s | 4.82–5.47 | 447 MB |
| strata E3 | 4.22 s | 3.75–4.39 | 1,229 MB |

E3 is 1.214× faster than E2 (interval 1.156–1.459) and 1.544× Tantivy (interval
1.494–1.634); 4.5× is 2.91× away. The first build's time was 1.086× E2 until the zeroing
went: value-initialized tuple chunks, arena chunks, and the flush's partition copy cost 13%
of the samples in `bzero`; the hash now reads a zeroed word written after each document's
bytes instead.

Peak RSS is 2.75× E2's: 20 bytes a token, the text, and the flush's partition copy of every
tuple (368 MB here). At the gate's equal memory that means fewer documents per flush and
more segments at 5M than E2. Where the time goes now (`runs/e3-mac.sample.txt`, about 3.0 s
active): the flush 61% (`flush_from` 28%, grouping 16%, the term sort about 8%, partition
8.5%, assembling postings 5%), stage 1 and the buffer's add 19%, the engine's tokenizing
7.5%.
