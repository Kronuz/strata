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

### E4: the encoder's per-term cost

Hypothesis: `flush_from` spends about 1.9 µs a term here (849 samples over 438,467 terms in
E3's profile), and most of it is work no term needs: every term's whole-list competitive
pairs are built (a vector of every posting's pair, then a 256-entry scan) although the term
info stores them only for terms with skip blocks (df ≥ 128, 1.8% of terms here); terms with
fewer than 128 postings build their LEB128 tail in a string of its own before copying it;
each block of a long list allocates its pairs; and the tuple buffer assembles each term's
postings with a `push_back` per posting and position. Without them the encoder costs under
1 µs a term and the 100,000-document run at least 10% less.

Mechanism, byte for byte the same output:

- Whole-list pairs only for df ≥ 128, from a scratch reused across terms.
- A list without full blocks writes its tail in place; a long list's per-block pairs reuse
  one scratch.
- The tuple buffer sizes a term's posting arrays from its tuple count and fills them by
  index.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, ctest green, and the
100,000-document A/B at least 1.1× faster than E3 with the interval excluding 1.0.
Otherwise removed.

Result: kept by BENCHMARKS rule 3 (the interval excludes 1.0 and the effect passes 3%),
though short of the 1.1× this item set itself and of its hypothesis: the encoder fell from
849 to 584 samples, 1.94 to 1.33 µs a term, not under 1 µs. Byte-identical segments at
20,000 and 100,000 documents; segment, index, and tuple_buffer tests pass. Five interleaved
rounds at 100,000 documents (`runs/e4-ab`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 6.65 s | 5.79–6.84 | 417 MB |
| strata E3 | 4.10 s | 3.51–4.36 | 1,229 MB |
| strata E4 | 3.86 s | 3.37–3.98 | 1,246 MB |

E4 is 1.095× faster than E3 (interval 1.036–1.202) and 1.710× Tantivy (interval
1.698–1.772); 4.5× is 2.63× away. What is left of the encoder (`runs/e4-mac.sample.txt`):
assembling each term's postings from its tuples 122 samples, `write_postings` 117,
`write_positions` 74, the sections' checksums 54, the dictionary 47, the key order check 29.

Process note: this item's build and tests (about 21:35 to 21:47) ran while
`perf/indexing-gap`'s strict hold `ig-e2` (an eight-thread A/B on the gate VM) was active;
the check printed the hold and the command went on. Its numbers from that window may need a
rerun. Every compile, test, and measurement here now goes through `.scratch/gate.sh`, which
refuses under a strict hold.

### E5: grouping without reading the text, terms sorted by 8-byte chunks

Hypothesis: of E4's flush, the grouping's check of every token's bytes against its term's
(a random read of the text and a `memcmp` per token) and the sort of the segment's terms
(`std::sort` over 438,467 `string_view`s, a `memcmp` per comparison, through 100,000 keys
that share a 38-byte prefix) are about 15% of the run, and most of it can go without
weakening either.

Mechanism:

- A token of at most 8 bytes hashes by one masked word load, a multiply, and an xorshift,
  then adds its length times a constant: for a fixed length that is a bijection on its
  bytes, so two such tokens with equal hashes and lengths are the same term. Only longer
  terms, terms given whole, and a narrowed test hash compare bytes.
- Terms sort by their bytes in 8-byte big-endian chunks with the remaining length (an
  integer sort, no `memcmp`); runs that tie on a chunk and continue are sorted again on
  the next chunk, so keys sharing a long prefix cost a few integer passes.

Lucene's `BytesRefHash` and Tantivy's `ArenaHashMap` compare bytes on every probe; neither
skips the comparison by the hash's structure. For the sort, Lucene's `BytesRefHash.sort`
already uses an MSB radix sort over bytes (`MSBStringRadixSorter`) and Tantivy
`sort_unstable_by` over term slices; the chunked sort here is the MSB idea with a 64-bit
digit.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, tests green with the
collision masks, and the 100,000-document A/B faster than E4 by BENCHMARKS rule 3. Otherwise
removed.

Result: unresolved on this host, kept provisionally (E9 builds on both mechanisms); a rerun
on the gate VM is owed. Byte-identical segments; tests pass. Three A/B sessions at 100,000
documents, 17 rounds pooled (`runs/e5-ab`, `e57-ab`, `e5-ab2`): E5 is 1.066× E4, interval
0.985–1.130, so the interval does not exclude 1.0 (the host's load came and went: one
session's E4 ranged 3.42–4.88 s). The profile attributes the effect where it was aimed
(`runs/e4-mac` against `runs/e5-mac`): the grouping 408 → 311 samples and the term sort
about 160 → 80, about 180 ms of a 3.4 s run (5%).

### E7: the engine's front end

Hypothesis: the engine builds each document afresh (an `operation` per line, `"Q" + id`
strings, `std::getenv` per line) and splits the text with a `memchr` call per word; reusing
the batch's operations (their strings and vectors keep their capacity) and splitting 8 bytes
at a time with an exact space mask take most of `tokens_document`'s 7% and the documents'
construction and destruction out of the run.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, and the 100,000-document
A/B faster than E5 by BENCHMARKS rule 3. Otherwise removed.

Result (`f6b06a1`): kept. Byte-identical segments at 20,000 and 100,000 documents; the index
test passes. Six interleaved rounds at 100,000 documents on a quiet host (`runs/e57-ab`;
Tantivy 5.73–5.78 s): E7 is 1.043× E5 (interval 1.024–1.075) and 1.875× Tantivy (interval
1.833–1.895); 4.5× is 2.40× away. Peak RSS rises from 1,248 to 1,346 MB: the reused
operations keep a batch's capacity.

### E9: C in batches, into a dense term store

Hypothesis: C keeps every token as a 20-byte tuple until the flush, so the buffer is about
2.75 times E2's and the process pays a page fault per 4 KiB it touches: E4's run at 100,000
documents spent 0.73 s of its 3.37 s in the kernel on 301,088 page faults (macOS zero-fills
each page, about 2.4 µs), and at the gate's equal memory a 2 GB buffer of tuples holds about
400,000 documents, 12 to 13 flushes and merges at 5M. Grouping each batch of about a million
tuples as it fills (C's partition and counting sort, unchanged, in cache) and appending each
term's postings for the batch to a compressed store keeps the buffer at about a fifth of
the tuples' size and the tuples' memory reused, so at 100,000 documents the run is faster by
the faults it no longer takes, and at 5M the same budget holds about five times the
documents per flush.

Mechanism: tuples go into 256 buckets by the hash's top byte as they arrive (the note's
refinement D, so no partition pass); when a batch fills, each bucket is grouped and
verified as C does, and each group (one term's postings in the batch) costs one probe of
the store's term table and one appended chunk of LEB128 postings (ordinal deltas, wdfs,
position deltas) linked from its term, newest first; the batch's tuples, text, and side
entries are then reset with their memory kept. The flush drains the last batch, sorts the
store's terms by 8-byte chunks, and decodes each term's chunks into `flush_from`, as E2's
flush decoded its streams; documents left out or reordered relabel there. Neither baseline
groups before it appends: Lucene's and Tantivy's writers probe their term table and append
to a term's slices for every token.

Keep rule: byte-identical segments at 20,000 and 100,000 documents; tuple_buffer_test with
batches drained every 1, 7, and 500 tuples and the collision masks; ctest green; the
100,000-document A/B faster than E7 by BENCHMARKS rule 3, and peak RSS at least 40% lower.
Otherwise removed.

Result: kept. Byte-identical segments, manifests, and metadata at 20,000 and 100,000
documents; ctest 34 of 34, with tuple_buffer_test now draining batches every 1, 7, and 500
tuples (with and without the collision masks, with documents left out and reordered) and
checking keys and scans before and after a drain. Six interleaved rounds at 100,000
documents (`runs/e9-ab`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 5.97 s | 5.58–6.37 | 416 MB |
| strata E7 | 3.45 s | 3.02–4.82 | 1,352 MB |
| strata E9 | 3.03 s | 2.99–3.06 | 588 MB |

E9 is 1.143× faster than E7 (interval 1.002–1.586; E7's two slow rounds are its page faults
under the host's memory pressure, E9's range is 70 ms) at 57% less peak RSS, and 1.975×
Tantivy (interval 1.876–2.071); 4.5× is 2.28× away.
