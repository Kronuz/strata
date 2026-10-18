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

BOUND_TABLE

Reading the table:

BOUND_READING

### Memory bandwidth and I/O on 8 cores

BANDWIDTH

### Is 4.5x reachable?

REACHABLE

## 3. Where today's pipeline spends its time

TODAY

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

IDEAS

## 6. The design

DESIGN

## 7. Recommendations

RECOMMENDATIONS
