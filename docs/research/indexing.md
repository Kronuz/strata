# Indexing: the bound, the survey, and the design that reaches 4.5x

The indexing pipeline from JSON lines to durable segments: parse, analysis, inversion,
flush encoding, merging, and how it scales across threads. The target is the owner's
(PLAN, "Design target"): at least 4.5x the faster baseline's throughput at one thread and
at eight. On the 5M-document corpus that is about 1:55 at one thread against Tantivy's
8:30, and about 0:57 at eight against roughly 4:10. strata today takes 18:30 at one
thread and 19:00 at eight (`docs/perf/indexing-gap.md`), so the one-thread gap to the
target is 9.7x and the eight-thread gap 20x. Parity is not the goal; the question is
what design gets to 4.5x, and the answer starts from what the hardware allows.

Numbers in this note come from the first 100,000 documents of the corpus
(`.scratch/corpus100k.json`, 117,707,867 bytes, sha256 `0a1d5344…3acebd`) on the
development Mac (Intel i9-9980HK, 8 cores at 2.4 GHz, 256 KiB L2 per core, 16 MiB L3,
two DDR4 channels; macOS, Apple clang 17, `-O3 -march=native`), the same CPU the gate
VM's 8 vCPUs run on. Every timed figure is the median of five interleaved trials.
Prototypes are under `research/indexing/`, built with `-DSTRATA_RESEARCH=ON`.

## 1. The corpus

| | 100k sample | Full corpus (5,032,104 docs, 8,327,453,114 bytes) |
|---|---:|---:|
| Bytes per document | 1,177 | 1,655 |
| Text bytes | 108.4 MB (92% of the file) | about 7.7 GB |
| Tokens | 18.30M (183 per doc; 5.92 bytes each, space included) | about 1.29G (scaled by text bytes) |
| Postings (distinct term, doc) | 8.80M (88 per doc) | about 620M |
| Distinct terms | 338,367 | several million (Heaps' law; the merged index's dictionary holds 5.03M key terms plus the vocabulary) |
| Postings in terms with df ≥ 128 | 81.5% (7,673 terms) | the same shape |
| Terms with df = 1 | 201,099 (59% of the vocabulary) | |

The corpus is pre-tokenized lowercase text separated by single spaces, so analysis here
is splitting and hashing; Lucene and Tantivy still run their tokenizers and lowercase
filters over it. Keyed indexing adds one `Q<id>` term per document (44.6 bytes on
average) and a version.

## 2. The bound

What the hardware has to do at least once per token, byte, or posting, measured stage by
stage on one thread (`research/indexing/bound.cc`, `research_bound`). The table is filled
in below from the first run; the full corpus column scales each stage by 70.7x (the byte
ratio between the corpus and the sample).

Measured on the quiet host (run 3; two earlier runs under host load were 1.5–2.3x slower
on every stage, so the quiet run is the bound). 18.30M tokens, 8.80M postings; "token"
stages are per text token, "byte" stages per byte of their input.

| Stage (one thread, median of 5 trials) | ns/unit | GB/s | Full corpus, 1 thread |
|---|---:|---:|---:|
| Copy the text (memcpy, warm) | 0.14 /byte | 7.3 | 1.1 s |
| Parse: line ends, `id` and `text` fields (memchr, find) | 0.12 /byte | 8.4 | 1.0 s |
| Split tokens, memchr per token | 7.32 /token | 0.81 | 9.4 s |
| Split tokens, SSE2 space mask | 0.74 /token | 8.0 | 1.0 s |
| Split (memchr) + hash, wordwise (strata's) | 17.9 /token | 0.33 | 23 s |
| Split (memchr) + hash, xxh64 | 19.8 /token | 0.30 | 26 s |
| Split (SSE2) + hash8: one masked 8-byte load, a multiply | 5.8 /token | 1.02 | 7.5 s |
| Big term table (25 MB, 2^20 slots): intern every token | 28.5 /token | 0.21 | 37 s |
| Big term table: split + hash + probe, dependent | 31.5 /token | 0.19 | 41 s |
| Big term table: the same, hashed 16 ahead with prefetch | 33.7 /token | 0.18 | 43 s |
| Append LEB128 streams per term (ordinals + positions; strata's `term_buffer`) | 36.0 /token | 0.16 | 46 s |
| Append byte slices per term in one arena (Lucene's, Tantivy's) | 25.5 /token | 0.23 | 33 s |
| Tuples (hash, ordinal, offset, position) from the hashes, 20 bytes each | 7.4 /token (6.6 is the memchr split) | 0.80 | 9.5 s |
| Radix partition the tuples by hash, 256 ways, write-combined | 5.5 /token | 1.07 | 7.1 s |
| Per partition: small hash table (L2) + stable counting sort by term | 12.6 /token | 0.47 | 16 s |
| Verify every token's bytes against its term's (random text reads, prefetched) | 11.9 /token | 0.50 | 15 s |
| Encode: FOR ordinal blocks, PFOR wdf and position blocks, LEB128 tails | 13.1 /token | 0.45 | 17 s |
| Write the encoded bytes (37.2 MB, 4.22 bytes per posting) to a file | 0.83 /byte | 1.2 | 2.5 s for 3 GB |

Memory bandwidth on this host: 12.6 GB/s on one thread, 25.6 GB/s on eight (a sum over
1 GiB; the VM's 8 vCPUs share the same two channels). `research_bound` prints the table.

Reading the table:

1. **Parsing is not the problem.** Finding each line's `id` and `text` runs at 8.4 GB/s;
   splitting tokens with a 16-byte SIMD space mask at 8.0 GB/s. The whole 8.33 GB corpus
   parses and splits in about 2 s on one thread. A memchr call per token costs ten times
   the SIMD split (7.3 against 0.74 ns), which is what today's `tokens_document` does.
2. **Hashing is 6 ns a token when done right.** Tokens average 5.9 bytes; one masked
   8-byte load and a multiply hash 5.8 ns per token, split included; strata's
   byte-tail `wordwise` through a memchr split costs 17.9 ns. Hashing the corpus: 7.5 s
   against 23 s.
3. **The big term table is the wall.** One probe per token into a table that does not fit
   the cache (25 MB here; a 500k-document segment's would be 100 MB) costs about 25 ns
   on top of the hash, two dependent misses each (the slot, then the term bytes it
   points at), and prefetching the slot does not help because the second miss is the
   compare. Appending the posting to the term's stream is another random write: 25 ns
   into arena slices, 36 ns into per-term `std::string` LEB128 streams. Lucene, Tantivy,
   and strata all pay this: about 60 ns a token, 80 s of the corpus, before anything is
   encoded. It is also why none of them scales with segment size.
4. **The cache-resident path costs 49 ns a token in total**, every stage included: SIMD
   split and hash8 (5.8), tuple append (about 1), partition (5.5), group (12.6), verify
   (11.9), encode (13.1): 63 s for the corpus on one thread, unoptimized C++ with no SIMD
   beyond the split. Partition and group are bandwidth passes over 366 MB of tuples per
   100k documents; verify is the price of never trusting a hash (a 20-byte tuple carries
   the token's text offset); encode is bitpack's scalar PFOR.
5. **The gap to the bound.** At 1.29G tokens the one-thread target of 115 s is 89 ns per
   token for everything: parse, inversion, encoding, the dictionary, stored records, keys,
   and merging. Today's engine spends 845 ns per token (1,091 s).

### Memory bandwidth and I/O on 8 cores

- **Read the corpus once:** 8.33 GB at 2–3 GB/s from NVMe (about 3–4 s), 0.7 s from the
  page cache at one thread's 12.6 GB/s. One reader thread hands 1 MiB line-aligned blocks
  to the workers; the benchmark harnesses read stdin, which is what strata's engine does.
- **Tuple traffic:** 20 bytes a token, written once (26 GB over the corpus), partitioned
  once (read 26 GB, write 26 GB), grouped once (read, sort, write back: about 78 GB).
  At 25.6 GB/s shared across eight cores that is about 6 s of pure bandwidth for the
  whole corpus, 10% of the eight-thread target of 57 s, so eight threads stay compute-bound
  on this host. On one thread the same bytes take about 12 s at 12.6 GB/s, 10% of the
  one-thread target.
- **Write the index:** about 3 GB of segments plus their rewrite by merging. A copy merge
  (section 5) writes each byte about twice; today's re-encoding merges wrote 6.1 GB for
  a 4.85 GB result at one thread (`indexing-gap.md`). At 1.2 GB/s (APFS, measured) that is
  5 s; the gate VM's virtio disk is slower, and the merge is the only I/O that matters.
- **Memory:** the tuple buffer holds a flush's tokens at 20 bytes each plus the text
  (about 5 KB a document, so a 2 GB budget holds about 400k documents, 12–13 flushes of
  the corpus). Today's un-inverted buffer holds 11.4 KB a document (1.14 GB peak RSS at
  100k documents); Tantivy's arena holds about 4 KB.

### Is 4.5x reachable?

Yes, at one thread with margin, and at eight threads easily:

| | One thread | Eight threads |
|---|---:|---:|
| Target (4.5x the faster baseline) | 115 s (89 ns/token) | 57 s |
| Hardware bound, cache-resident path, stages as measured | 63 s | about 10 s with linear scaling, 6 s of it bandwidth |
| strata today | 1,091 s | 1,083 s |
| Tantivy / Lucene | 512 s / 890 s | 256 s / 260 s |

The one-thread target leaves 26 s (26 ns a token) beyond the measured stages for the
dictionary build, stored records, the key filter, I/O, and merging. Section 3 shows the
dictionary and per-term flush work is today 1.7 µs per term (53 ns a token), which must
come down about 4x; everything else is small. The design in section 6 is the one that
pays only these stages. The eight-thread target is not the binding one: once inversion
is partitioned by term, a flush's partitions spread across threads and produce one
segment, so eight threads scale the one-thread design nearly linearly instead of
multiplying segments as today's flush does.

## 3. Where today's pipeline spends its time

Measured on the 100k sample on this host, one thread, keyed (`research/indexing/baseline.sh`,
three interleaved trials, host partly loaded): today's `strata_engine index` 16.8–21.2 s
(median 18.2 s, peak RSS 1.14 GB, 43.5 MB index), Tantivy's builder 8.7–12.7 s (median
11.5 s, 420 MB, 47.9 MB). The 4.5x target at this size is about 2.0–2.5 s.

`research/indexing/invert.cc` feeds the same token stream (18.4M tokens, keys included)
to three inverters that write the same segment through `segment_writer`'s flush path;
their outputs are byte-identical (43,799,908 bytes). Median of five interleaved trials:

| Inverter | Add phase | Flush | Total | ns/token | MB held |
|---|---:|---:|---:|---:|---:|
| A: today (document built per `tokens_document`, copied into the buffer as `index::evaluate` does; `segment_writer::add`; `flush`) | 3,511 ms | 2,753 ms (`add` 1,655 + `flush` 1,107) | 6,264 ms | 340 | 642 |
| B: one big table keyed by term bytes, byte slices in an arena (Lucene's and Tantivy's shape), `flush_from` | 1,281 ms | 1,267 ms (sort terms 152 + read slices and `flush_from` 1,116) | 2,548 ms | 139 | 126 |
| C: 20-byte tuples in arrival order, radix partition, per-partition small tables and counting sort, bytes verified, `flush_from` | 461 ms | 1,710 ms (partition 138, group and verify 379, sort terms 230, `flush_from` 979) | 2,171 ms | 118 | 849 (495 without the transient partition copy) |

What this says about today's 18.2 s:

1. **Inversion proper is 2.8 s of it** (A's `writer.add` plus `flush`); the document
   representation costs 3.5 s more (an `unordered_map` per document, a `std::string`
   and a `std::vector` per term, and the copy into the buffer). The remaining 12 s are
   the engine around it: a line read per document, `"Q" + id` strings, the `valid()`
   `std::set` over every document's terms, `key_of`, `lookup_key`, the undo log, the
   version map's string keys, and the flush's docid sort; the perf engineer's E0 profile
   attributes them in detail. None of it is inversion.
2. **The flush encoder is the largest shared cost: 53–61 ns a token, 1.7 µs a term.**
   `flush_from` over 438k terms takes 1.0–1.1 s in all three variants while bitpack's
   block encoding alone is 13 ns a token (section 2). The difference is per-term work: an
   `impact` vector and `competitive()` per term, per-block `impact` vectors, `std::string`
   appends per block, `encode_term_info`, and `termdict::builder::add`. 59% of terms have
   df 1 and need none of it. Over the corpus (about 12 flushes of 3M terms, then a merge
   of 9M) that is 50–80 s at 1.7 µs a term: it has to fall to about 0.4 µs for the target.
3. **The add phase is where the designs differ.** C adds a document in 25 ns a token
   (hash, 20-byte tuple, the text copied); B in 70 (the big table and the slices); A in
   191 before the writer even sees it. In an engine the add phase is what the owner core
   does per document and what bounds refresh latency; C's flush then runs off the core
   and, being partitioned, across cores.
4. **Memory.** B's slices are the most compact in-memory form (1.26 KB a document, 2.6x
   less than C's tuples and text, 5x less than A's documents). C trades memory for
   cache-friendliness; its tuples compress (a 48-bit hash, 24-bit ordinals and positions)
   and the text is needed anyway for stored records in Xapiand's case. Today's engine
   holds 11.4 KB a document.

## 4. Survey

What the best open implementations and the literature do, read from permissive sources
only (Lucene, Apache-2.0; Tantivy, MIT; PISA, Apache-2.0; papers).

### Lucene 10 (`org.apache.lucene.index`)

- One `DocumentsWriterPerThread` (DWPT) per indexing thread, each an independent
  in-memory segment: `TermsHashPerField` interns terms in a `BytesRefHash` (open
  addressing over MurmurHash3, term bytes in a `ByteBlockPool`), and appends each
  posting to per-term byte slices in `ByteBlockPool` (slices grow through
  `LEVEL_SIZE_ARRAY` = 5, 14, 20, 30, 40, 40, 80, 80, 120, 200 bytes; a full slice ends
  with a forwarding address) and `IntBlockPool` for the stream heads. Doc deltas, freqs,
  and position deltas are VInts written at index time; the flush
  (`FreqProxTermsWriter`) sorts the terms and re-reads the slices to write
  `Lucene912PostingsWriter` blocks of 128 (FOR for docs, PFOR for freqs and positions).
- `FlushByRamOrCountsPolicy` flushes the largest DWPT when the global RAM buffer
  (`setRAMBufferSizeMB`) is exceeded; DWPTs flush concurrently on the indexing threads,
  and `ConcurrentMergeScheduler` merges on background threads. Every DWPT holds its own
  term table; nothing is shared across threads.
- `updateDocument` buffers the delete term; deletes are resolved at flush by seeking each
  term in each segment's `TermsEnum` (`FrozenBufferedUpdates`), which is why keyed
  indexing costs Lucene more than adding.
- Merges decode and re-encode postings through `PostingsEnum`; only stored fields and doc
  values bulk-copy. `BPIndexReorderer` (Lucene 10, `misc`) reorders at merge by recursive
  graph bisection, on several threads.

### Tantivy 0.26 (`src/indexer`, `src/postings`, `tantivy-stacker`)

- One `SegmentWriter` per indexing thread with a `MemoryArena`; terms interned in
  `ArenaHashMap` (open addressing, MurmurHash2 of the term bytes, bytes in the arena);
  each term's postings in an `ExpUnrolledLinkedList` of arena blocks starting at 8
  bytes and doubling to 32 KiB (`tantivy-stacker/src/expull.rs`), holding VInt doc
  deltas, term frequencies, and positions as the `Recorder` writes them
  (`src/postings/recorder.rs`).
- A segment closes when the arena reaches the per-thread budget minus a 1 MB margin
  (`index_writer.rs`, `index_documents`); `serialize_postings` sorts the term table's
  entries and writes blocks of 128 (bitpacked docs and freqs, positions in their own
  stream). Deletes (`delete_term`) resolve at flush by term lookups per segment, as in
  Lucene. `LogMergePolicy` merges run on background threads and re-encode postings
  through doc-id mapping (`merger.rs`).
- The harness's single-thread builder (`bench/engines/tantivy-0.26-st`) parses each line
  with `TantivyDocument::parse_json`, which allocates the document's field values before
  indexing them.

### PISA 0.8 (`tools/invert`, `tools/compress_inverted_index`)

A batch pipeline, not an engine: `invert` reads a forward index (term-id sequences per
document) in batches, inverts each batch in memory, writes it, and merges the batches
into one uncompressed inverted index, which `compress_inverted_index` encodes with the
chosen codec (SIMD-BP128, PEF, …) and `reorder-docids` (recursive graph bisection)
permutes before compression. It shows the two-pass shape (forward index first, inversion
second) at its simplest.

### Literature

- Sort-based and single-pass inversion: Witten, Moffat, and Bell, *Managing Gigabytes*
  (ch. 5): in-memory inversion as a sort of (term, doc) pairs with a multiway merge of
  runs; Heinz and Zobel, "Efficient single-pass index construction for text databases"
  (JASIST 2003): one pass, a hash table (or burst trie) of terms with compressed
  in-memory postings, runs written when memory fills and merged at the end; Büttcher
  and Clarke, "Indexing time vs. query time trade-offs in dynamic information retrieval
  systems" (CIKM 2005) and "A hybrid approach to index maintenance" (ECIR 2006):
  logarithmic merging against in-place updates. The merge-based family is what Lucene,
  Tantivy, and strata do.
- Parallel and pipelined inversion: Melnik, Raghavan, Yang, and Garcia-Molina, "Building
  a distributed full-text index for the Web" (WWW 2001): a software pipeline of loading,
  processing (sorting), and flushing stages, with the memory split between stages so
  that the three overlap; Ribeiro-Neto, Moura, Neubert, and Ziviani, "Efficient
  distributed algorithms to build inverted files" (SIGIR 1999): term-partitioned against
  document-partitioned construction. GPU inverted-index construction (Wei and JaJa,
  "An optimized high-throughput strategy for constructing inverted files", TPDS 2012) is
  sort-based: radix sort of (term, doc) pairs, then segmented reductions.
- Cache-conscious partitioning: Polychroniou and Ross, "A comprehensive study of
  main-memory partitioning and its application to large-scale comparison- and radix-sort"
  (SIGMOD 2014): radix partitioning with software write-combining buffers streams at
  memory bandwidth where a scatter into thousands of destinations would miss on every
  write; the same technique drives hash joins (Balkesen, Teubner, Alonso, and Özsu,
  "Main-memory hash joins on multi-core CPUs", ICDE 2013) and columnar sorts.
- Parsing and analysis at bandwidth: Langdale and Lemire, "Parsing gigabytes of JSON per
  second" (VLDB J. 2019): a structural pass over 64-byte blocks with SIMD classification
  before any per-token work; the same column-at-a-time idea applies to whitespace and
  case classes. Short-string hashing (wyhash, xxh3) runs at a few nanoseconds per key.
- Encoding: Lemire and Boytsov, "Decoding billions of integers per second through
  vectorization" (SPE 2015), SIMD-BP128 and its encoder; Pibiri and Venturini,
  "Techniques for inverted index compression" (ACM CSUR 2020) for the codec landscape
  and partitioned Elias-Fano.
- Document ordering: Silvestri, "Sorting out the document identifier assignment problem"
  (ECIR 2007): sorting documents by URL gives most of the compression gain of clustering
  at no cost on web corpora; Dhulipala et al., "Compressing graphs and indexes with
  recursive graph bisection" (KDD 2016); Mackenzie et al., "Compressing inverted indexes
  with recursive graph bisection: a reproducibility study" (ECIR 2019) and "Tradeoff
  options for bipartite graph partitioning" (TKDE 2022): cheaper variants that keep most
  of the gain; MinHash-based ordering as a one-pass approximation (Chierichetti et al.,
  "On compressing social networks", KDD 2009, for the shingle-ordering idea).

## 5. Ideas and prototypes

Each idea states what it breaks, the prototype, and the numbers (winners and losers both).

### 5.1 Document order at flush (`research/indexing/order.cc`)

What it breaks: the rule that flushes keep docid order and only merges reorder (P1), and
the assumption that a good order needs a graph algorithm. Each document's MinHash
signature falls out of the hashes the inverter computes anyway (k minima of a mixed
hash over the document's distinct terms, about 2 ns a token), and sorting documents by
the signature puts similar documents together. The same 100,000 documents written by
`segment_writer` in each order, with the order's cost on one thread:

| Order | Order time | Postings | Positions | Postings + positions |
|---|---:|---:|---:|---:|
| Corpus order (curid, as the harness indexes) | 0 | 15.97 MB | 22.75 MB | 1.000 |
| Random shuffle | 0 | 15.94 MB | 22.78 MB | 1.000 |
| MinHash k=1, every term | 30 ms | 15.68 MB | 22.68 MB | 0.991 |
| MinHash k=1, terms with 2 ≤ df ≤ N/10 | 372 ms | 15.17 MB | 22.25 MB | 0.966 |
| MinHash k=2, every term | 48 ms | 15.14 MB | 22.26 MB | 0.966 |
| MinHash k=4, every term | 64 ms | 15.21 MB | 22.22 MB | 0.967 |
| Bisection (P1 as built), min_df 4096, 1 thread | 5,941 ms | 14.80 MB | 21.71 MB | 0.943 |
| Bisection, min_df 82 (4096 scaled to the sample) | 16,766 ms | 14.74 MB | 21.72 MB | 0.942 |

1. The corpus order carries no locality at all: it compresses exactly like a random
   shuffle, so every flush today writes the worst case.
2. MinHash with two minima over every term takes 60% of bisection's gain at 1/120 of its
   cost (48 ms against 5.9 s per 100k documents; 0.5 µs a document, which is 2.6 ns a
   token). k = 4 and the df filter add nothing over k = 2.
3. At 5M documents bisection saved 10.2% of postings plus positions (the gate's T3 row), more
   than the 5.8% here, since clusters grow with the corpus; MinHash should scale the same
   way. The order's second cost is in the inverter: tuples carry arrival ordinals, so the
   flush relabels them through the permutation (one L2-resident lookup a tuple, about 2 ns)
   and each partition sorts by the new ordinal before grouping (an in-cache radix pass,
   about 5 ns a token). About 10 ns a token in all, 11% of the one-thread budget, for a
   3.4–6% smaller index and part of bisection's 0.80x query time (P1's T2 row).
4. The catch for the designer: a flush that reorders writes a permuted `docids` section in
   every segment (format 12's kind 2, 29 MB at 5M in the merged index) and gives up the
   docid-ordered tie skip on flushed segments. Bisection at merge keeps its place for the
   final segment; MinHash at flush is the cheap order for everything before it.

IDEAS

## 6. The design

DESIGN

## 7. Recommendations

RECOMMENDATIONS
