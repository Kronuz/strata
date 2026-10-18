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

**The same run on the gate VM** (Linux, clang 18, `-march=native`, the VM shared at the time
with the index-size engineer's single-thread E1 run, so a first number, not the quiet one):
bandwidth 8.2 GB/s on one thread and 14.6 on eight; parse 0.17 ns a byte; SSE2 split 1.17 ns
a token; split + hash8 7.6; the big table's dependent probe 59.0 (1.9x the Mac's: nested
paging makes a dependent miss dearer); slices 46.5; LEB128 streams 67.9; partition 10.3;
group 17.8; verify 26.5 (random text reads, 2.2x); encode 16.5; the dictionary 218 ns a term.
The cache-resident path sums to 69 ns a token there (89 s over the corpus) against 49 on
the bare host; the big-table path to about 110. The ranking is the same on both platforms
and the gap between them wider where the gate measures.

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

Yes at eight threads with a wide margin, yes at one thread on the bare host with margin,
and narrowly at one thread on the gate VM, where the target is measured:

| | One thread | Eight threads |
|---|---:|---:|
| Target (4.5x the faster baseline, the VM's Tantivy) | 115 s (89 ns/token) | 57 s |
| Cache-resident path, stages as measured on the bare host | 63 s | about 10 s with linear scaling, 6 s of it bandwidth |
| The same stages as measured on the VM (first run, shared) | 89 s | about 15 s |
| strata today (VM) | 1,091 s | 1,083 s |
| Tantivy / Lucene (VM) | 512 s / 890 s | 256 s / 260 s |

On the bare host the one-thread target leaves 26 s (26 ns a token) beyond the measured
stages for the dictionary, stored records, the key filter, I/O, and merging; on the VM it
leaves 26 s too, but the stages there are 69 ns a token, so the design in section 6 lands
within a few percent of the target at one thread unless some of its stages get cheaper
than the prototypes. Four do, in order of expected gain, none prototyped yet: the verify
pass (26.5 ns a token on the VM, 11.9 on the host, all random text reads) shrinks with
2 MiB pages for the text arena and tuple buffers, since nested paging doubles the cost
of every TLB miss; the encoder's PFOR is scalar (16.5 ns) where bitpack's SIMD lanes
would pack a block in a third of that; the partition and group passes fuse into one;
and the per-term flush work falls from 1.7 µs a term (section 3) to about 0.4. The
eight-thread target is the easy one: once inversion is partitioned by term, a flush's
partitions spread across threads and produce one segment, so eight threads scale the
one-thread design nearly linearly instead of multiplying segments as today's flush does.

## 3. Where today's pipeline spends its time

Measured on the 100k sample on this host, one thread, keyed (`research/indexing/baseline.sh`,
three interleaved trials on the quiet host): today's `strata_engine index` 10.0–11.1 s
(median 10.5 s, peak RSS 1.13 GB, 43.5 MB index), Tantivy's builder 6.18–6.35 s (median
6.22 s, 418 MB, 47.9 MB): 1.69x, where the 5M run on the gate VM is 2.13x (merges, key
lookups over more segments, larger tables). Per byte, Tantivy runs at the same speed here
as in the VM (6.22 s against 512 s x 1.41%, which is 7.2 s). The 4.5x target at this size
is 1.38 s.

`research/indexing/invert.cc` feeds the same token stream (18.4M tokens, keys included)
to three inverters that write the same segment through `segment_writer`'s flush path;
their outputs are byte-identical (43,799,908 bytes). Median of five interleaved trials:

| Inverter | Add phase | Flush | Total | ns/token | MB held |
|---|---:|---:|---:|---:|---:|
| A: today (document built per `tokens_document`, copied into the buffer as `index::evaluate` does; `segment_writer::add`; `flush`) | 3,511 ms | 2,753 ms (`add` 1,655 + `flush` 1,107) | 6,264 ms | 340 | 642 |
| B: one big table keyed by term bytes, byte slices in an arena (Lucene's and Tantivy's shape), `flush_from` | 1,281 ms | 1,267 ms (sort terms 152 + read slices and `flush_from` 1,116) | 2,548 ms | 139 | 126 |
| C: 20-byte tuples in arrival order, radix partition, per-partition small tables and counting sort, bytes verified, `flush_from` | 461 ms | 1,710 ms (partition 138, group and verify 379, sort terms 230, `flush_from` 979) | 2,171 ms | 118 | 849 (495 without the transient partition copy) |

On the gate VM (same caveat as section 2: the VM was shared with E1), the same program
gives A 715 ns a token (`writer.add` alone 272), B 312, and C 212 (add 33; partition 10.9,
group and verify 52.2, sort terms 27.9, `flush_from` 85.1), outputs identical: the
random-access designs lose 2–3x to virtualization, the cache-resident one 1.8x, so C's
margin over B grows from 1.17x on the bare host to 1.47x on the VM, and over A from 2.9x
to 3.4x.

What this says about today's 10.5 s:

1. **Inversion proper is 2.8 s of it** (A's `writer.add` plus `flush`); the document
   representation costs 3.5 s more (an `unordered_map` per document, a `std::string`
   and a `std::vector` per term, and the copy into the buffer). The remaining 4 s are
   the engine around it: a line read per document, `"Q" + id` strings, the `valid()`
   `std::set` over every document's terms, `key_of`, `lookup_key`, the undo log, the
   version map's string keys, the flush's docid sort, and the commit; the perf engineer's
   E0 profile attributes them in detail. None of it is inversion: 7.7 of the 10.5 s go to
   building and copying documents and to the engine's bookkeeping, 2.8 s to inverting.
2. **The flush encoder is the largest shared cost: 53–61 ns a token, 1.7 µs a term.**
   `flush_from` over 438k terms takes 1.0–1.1 s in all three variants while bitpack's
   block encoding alone is 13 ns a token (section 2). The difference is per-term work: an
   `impact` vector and `competitive()` per term, per-block `impact` vectors, `std::string`
   appends per block, `encode_term_info`, and `termdict::builder::add`. The dictionary
   builder is not the cost: measured alone over the sorted vocabulary it is 161 ns a term
   (section 5.6), 70 ms of the 860–980 ms. 59% of terms have df 1 and need none of the
   rest. Over the corpus (about 12 flushes of 3M terms, then a merge of 9M) that is 50–80 s
   at 1.7 µs a term: it has to fall to about 0.4 µs for the target.
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
   shuffle, so every flush today writes the worst case. (The gate VM gives the same bytes
   for every order; its times are 1.5–3x longer: MinHash k=2 145 ms, bisection 9.0 s.)
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

### 5.2 Inversion: hash-append against sort and partition (`research/indexing/invert.cc`)

What it breaks: the invariant every engine keeps, that a term's postings are appended to
that term's own stream as each token arrives, which costs one random probe and one random
write per token into structures that outgrow the cache. The partitioned inverter (C in
section 3) never looks a term up while documents arrive: a token becomes a 20-byte tuple
(hash, ordinal, text offset, position and length) appended in order, and the text is
kept. The flush radix-partitions the tuples by hash into 256 buckets with write-combining
buffers (a bandwidth pass), then each bucket, whose vocabulary is 1/256 of the segment's
(a 128 KiB table for a 2M-term segment), gets a cache-resident table, a stable counting
sort by term, and a byte comparison of every token against its group's first, so a hash
collision between two terms can never merge them. The result is each term's postings in
ordinal order, which `flush_from` encodes exactly as today: the three inverters' segments
are byte-identical.

Measured (section 3): 118 ns a token against 139 for the slices shape and 340 for today's,
with the add phase at 25 ns a token against 70 and 191. The flush phase is what is left to
cut, and most of it is `flush_from`'s per-term overhead (53 ns a token), shared by every
variant. The stage bound (section 2) says what C's parts cost at best: partition 5.5,
group 12.6, verify 11.9, encode 13.1.

Two refinements, not yet prototyped, follow from the numbers:

- **Partition as tokens arrive.** The add phase can append each tuple into one of 256
  per-thread bucket streams through the same write-combining buffers (80 KiB per thread,
  L2-resident), so the flush starts grouped, the partition pass disappears, and the
  transient partition copy (20 bytes a token) with it.
- **Sort terms per partition and merge.** `std::sort` over 438k `string_view`s costs 12.5 ns
  a token; each partition's terms sorted alone (1.7k of them here, 8k at a 2M-term segment)
  and a 256-way merge of sorted runs is a fraction of that, and it is what a
  partition-parallel encoder needs anyway.

A sort-based inverter in the classic sense (radix sort of every tuple by term, then
grouping) is what C becomes once partitions are grouped: the partition is the sort's
first pass and the counting sort its second. A full comparison sort is never needed.

### 5.3 Merges as byte copies (format 13)

What it breaks: the merge that decodes and re-encodes every posting. The merged 5M
segment's dictionary (`strata_engine terms` over `seg-200.sx` on the gate VM) has
8,996,854 terms: 5,032,104 key terms with df 1 and 3,964,750 vocabulary terms holding
586,083,743 postings. 103,578 terms with df ≥ 128 hold 565,652,181 of them (96.5%), and
of those only 5,941,333 (1.05%) sit in tails; the 3,861,172 terms with df < 128 hold 3.5%.
So 95% of all postings bytes are full 128-posting blocks that a docid-ordered merge
rewrites with every ordinal shifted by a constant.

A block's deltas do not change when every ordinal in it shifts by the same constant. Today
the only ordinal that is not a delta is the first of a term's list (coded as ordinal + 1,
relative to −1): if the format records each term's starting base in the term info and
each block's `last` in level 0 as it does, a merge of sources with disjoint ordinal ranges
copies every full block verbatim, rewrites the skip entries (deltas across the source
boundary and byte offsets), recomputes the level-1 impacts from level-0's (no block is
decoded), and copies the position blocks likewise, since position deltas are per document.
What stops a pure copy today is the tail: a source's last partial block (LEB128, fewer
than 128 postings) followed by the next source's full blocks would have to be re-blocked,
shifting every later block. Allowing a tail-coded block anywhere in the list (its count in
its level-0 entry, which the level-0 `last` delta already implies) removes the last
re-encode; the price is one short LEB128 run per source per term instead of full blocks,
a few bytes per boundary (a tail-coded posting costs about what a FOR-coded one does).

Format 13 needs, for the designer's list:

1. The term info carries the list's base ordinal (the first block's deltas are relative to
   it); flushes write −1 as today, merges write the source's offset.
2. A postings list is a sequence of blocks each holding at most 128 postings: a 128-posting
   FOR/PFOR block, or a tail-coded block of fewer, anywhere, with its count derivable from
   level 0 (`last` deltas) or stored as one byte.
3. Position and count blocks follow the same rule, aligned with their postings blocks as
   now, so a source's positions copy as one byte range per term.
4. A merged segment's ordinal order is the concatenation of its sources' orders, which is
   docid order only when the sources' docid ranges are disjoint and increasing; otherwise
   the merge writes the permuted `docids` kind (format 12's kind 2), as a reordering merge
   does today.
5. Sources with deleted documents keep today's re-encoding merge (or P17's rebase copy for
   the blocks without deletes), since removing a document renumbers every later ordinal;
   append-only merges, and merges after a reclaiming merge, take the copy path.

The cost model: the 5M force merge in docid order took 404 s on the gate VM (T3) and
wrote 4.4 GB; a copy merge reads about 2.5 GB and writes about 2.5 GB (a few seconds of
bandwidth), walks 12 sources' dictionaries (36M cursor steps), rewrites skip data, and
builds the merged dictionary of 9M terms, whose cost is `termdict::builder`'s per term
(section 5.6, measured). Tens of seconds at one thread, against 404. It matters twice: the
baselines force-merge to one segment inside the timed run while strata's `index` leaves 9
to 14 segments, and the eight-thread row's extra segments are merge work. Merges become
I/O-bound, so they also stop competing with inversion for cores.

### 5.4 Threads: term-partitioned flushes, one segment per flush

What it breaks: document-partitioned parallelism (one in-memory segment per thread, as
Lucene's DWPTs, Tantivy's indexing threads, and today's `flush_threads` do), which
multiplies segments by the thread count and leaves every thread its own term table and
its own dictionary to build and merge. In C the flush is partitioned by term hash: thread k
takes partitions k, k + T, …, groups, verifies, and encodes them, and one assembler
concatenates the 256 encoded runs in dictionary order. One segment per flush at any
thread count; the add phase parallelizes by document batch with a reader handing out
line-aligned blocks, each batch's ordinals assigned at admission.

Measured (`research_invert … 3 0 8`, 100k documents, three trials under a strict hold, the
encoder serial; the first run of each thread count pays the buffers' page faults and the
median hides it):

| C on T threads | Add phase | Partition | Gather, group, verify | Sort terms (serial) | `flush_from` (serial) | Total | ns/token |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 407 ms | 124 ms | 496 ms | 242 ms | 858 ms | 2,117 ms | 115 |
| 2 | 274 | 152 | 332 | 246 | 919 | 1,934 | 105 |
| 4 | 118 | 89 | 287 | 251 | 925 | 1,702 | 92.5 |
| 8 | 85 | 71 | 261 | 248 | 903 | 1,568 | 85 |

Every thread count writes the identical segment. The add phase scales 4.8x at eight
threads (hashing and tuple writes are compute); the flush's parallel part reaches 1.9x
because the serial encoder and term sort are 73% of the eight-thread total (Amdahl), and
because this first version allocates each partition's table and scratch anew (page faults
on 256 KiB allocations, since fixed; the rerun is in the ledger). The design's assembler
makes the encoder partition-parallel and the dictionary range-parallel, which removes the
serial 1.15 s; with it the eight-thread flush is bounded by the measured parallel parts,
about 300 ms here, and the add phase by the reader.

### 5.5 Positions compressed during inversion

The question assumes positions are appended per term as they arrive (strata's LEB128
`positions` stream, Lucene's prox slices, Tantivy's recorder). In the tuple design a
position travels in its tuple at 4 bytes until the flush groups it, when each term's
positions are already in order and go straight to PFOR blocks; nothing is compressed
twice. The memory trade is in section 3: slices hold 1.26 KB a document, tuples and text
4.95 KB, today's documents 6.4 KB (11.4 KB of RSS). A 16-byte tuple (48-bit hash, 24-bit
ordinal within the flush, 24-bit position, 8-bit length, 32-bit offset) is 4.2 KB a
document; the text is needed anyway where it is the stored record. Compressing positions
inside tuples (delta from the previous token, 1–2 bytes) would make them variable-length
and the partition and counting sort byte-oriented; the measured grouping cost is 25 ns a
token at 20 bytes, so the room is small and the complexity real. Not recommended.

### 5.6 A shared or sharded term dictionary across threads

Per-thread tables duplicate the vocabulary T times and cost T dictionaries to merge.
Sharing one table needs synchronization on every token. The partition by hash is the
sharding: during the add phase no table exists at all (tokens are hashed, not resolved),
and at flush each partition's table is private to the thread that owns the partition, so
no term is ever interned twice and nothing is locked. The dictionary is built once per
flush from 256 sorted runs. The remaining serial cost is the dictionary builder itself:

`termdict::builder` over the sample's 338,367 sorted terms with a 4-byte value each:
161 ns a term (54 ms); sorting the vocabulary with `std::sort` over `string_view`s 286 ns
a term (97 ms). At the merged index's 9M terms the build is 1.5 s and the flushes' 12 x
3M terms 6 s over the corpus: not the problem, and parallel by lexicographic range if it
ever is. The 1.7 µs a term of `flush_from` (section 3) is therefore the encoder's own
per-term work, not the dictionary's.

### 5.7 Column-at-a-time analysis

On this pre-tokenized corpus analysis is splitting and hashing. The bound (section 2)
measured the per-token loop against the batch form: `memchr` per token 7.3 ns, a 16-byte
SSE2 space mask 0.74 ns; `wordwise` over a memchr split 17.9 ns, the SIMD split with an
8-byte masked load and one multiply 5.8 ns. Three times less per token for the two steps
that touch every byte, and the analyzer of a real engine (classification, case folding,
UAX #29 boundaries in the `analysis` library) gains the same way from classifying 16 or
32 bytes per instruction before any per-token work. The parsing of the JSON lines is
0.12 ns a byte with `memchr` and `find`; a simdjson-style structural pass would not pay
here, since only two fields are read. Recommended as the shape of the tuple writer's
front end, with the `analysis` library's tokenizer given a batch interface later.

## 6. The design

The design that reaches the target, stated as what the writer does from a document to a
durable segment, and where it replaces code. A clean replacement of the write path's
inversion; the read path, the segment format (until format 13), `index`'s stage 1 (docids,
versions, keys), commits, and the manifest stay.

```
 host / harness            per thread (receiving core)                owner core
 ─────────────            ───────────────────────────                ──────────
 bytes of a document ──▶ split + hash8 (SIMD) ──▶ text arena, tuples ──▶ stage 1: docid, version,
                         (hash, ord, off, pos|len) into 256            key map; ordinal base
                         write-combined bucket streams                 per batch; admit
                                                                             │ budget / refresh
                                                                             ▼
 flush, T worker threads: partition k, k+T, …: gather ─▶ small table ─▶ counting sort ─▶ verify
                          ─▶ encode blocks, tails, skip data, impacts ─▶ sorted terms + bytes
 assembler: 256-way merge of sorted terms ─▶ dictionary; postings and positions by memcpy;
            per-document columns (lengths, norms, docids, versions, pk filter, stored) ─▶ segment
 merges: copy blocks and rewrite skip data (format 13); re-encode only sources with deletes
```

1. **Documents arrive as tokens, never as `document`s.** The writer's hot entry takes a
   token span (term bytes and position per token; Xapiand's `TermGenerator` and the
   harness's whitespace split both produce exactly that) or a prepared document as spans
   into one buffer. Per token: the SIMD split and `hash8` (5.8 ns), a copy of the term
   bytes into the flush's text arena, and a 20-byte tuple appended to the bucket stream
   of the hash's top byte through a write-combining buffer. About 12 ns a token; C's
   prototype did 25 with `memchr` and `wordwise`. The `document`/`term_entry` API stays
   for callers that have it, converted into tokens at the boundary.
   Replaces: `tools/strata_engine.cc`'s `tokens_document`; `index::evaluate`'s copy into
   `buffer_` and the `buffered` struct's `document`; `segment_writer::add`'s term table
   (`buffer_for`, `slot`, the arena) and `term_buffer`'s LEB128 streams.
2. **Stage 1 unchanged in meaning, lighter in cost.** Docids, version checks, and the key
   map run on the owner core per document as now; the document's bytes are already in the
   arena, so admission moves a (batch, ordinal base) pair, not strings. `valid()`'s
   `std::set` over a document's terms goes: a repeated term is legal in a token stream
   (it is a wdf of two), and term length is checked at the split.
3. **The flush is term-partitioned and runs off the owner core on T threads.** Each thread
   takes partitions k, k + T, …: gathers the batches' bucket streams (arrival order is
   ordinal order), interns each hash in a 2^⌈log₂(2·terms)⌉-slot table that stays in L2,
   counting-sorts by term, verifies every token's bytes against the term's first, sorts
   the partition's terms, and encodes each term's postings, positions, skip data and
   impacts into the partition's buffer: exactly `flush_from`'s per-term output, by the
   same block encoders, with the per-term fixed cost taken out (no `impact` vector per
   term for df 1, no per-block vector allocations, no `std::string` growth per block).
   Measured parts: group and verify 25 ns a token, blocks 13.
   Replaces: `index::flush`'s per-run `segment_writer::add` loop and `flush_threads`'
   one-segment-per-run split; `segment_writer::flush`'s sort and decode of the streams.
   Keeps: `flush_from`'s section layout and the encoders behind it.
4. **The assembler writes one segment.** A 256-way merge of the partitions' sorted terms
   feeds `termdict::builder` in order, each term's encoded bytes are appended by `memcpy`
   from its partition buffer, and the per-document sections are written as today. Where
   the dictionary builder's serial time matters (section 5.6), the block tree is built in
   T lexicographic ranges and one FST over the block prefixes closes it.
5. **Refresh, commit, manifest, deletes, versions, key lookups** stay as built in
   `index.hh`; a flush publishes one segment whatever T is, so the eight-thread run no
   longer multiplies segments or the key lookups over them.
6. **Merges copy** (section 5.3) once format 13 lands; until then the tiered policy runs
   today's merge, which the design makes rarer (one segment per flush) but not cheaper.
7. **Document order** (section 5.1): MinHash signatures per document are available for
   free in step 1; whether a flush sorts by them is a preset the architect decides against
   the permuted-`docids` cost. Bisection stays the force-merge order.
8. **Memory.** The flush buffer is text plus tuples, charged to the budget as today's
   `flush_charge` does: about 5.1 KB a document at 20-byte tuples (12–13 flushes of the
   corpus at 2 GB), 4.2 KB at 16. Flushing a batch's buffers frees them whole; nothing is
   allocated per token or per term during the add phase except the bucket streams' chunks.
9. **Limits stay load-bearing.** Ordinals are 32-bit as now (2^32 − 2 documents a segment);
   a position above 2^24 or a term above 255 bytes takes an escape tuple (a second tuple
   carrying the high bits), so the limits of `limits.md` do not change.

Where it stands against the target, one thread, the corpus (1.29G tokens; prototype
numbers scaled by bytes; the key terms and per-document columns add about 3%):

| Step | ns/token | Corpus, 1 thread |
|---|---:|---:|
| Parse lines and fields | 0.8 (0.12 a byte) | 1 s |
| Split, hash, text copy, bucketed tuple append | 12 | 15 s |
| Stage 1 per document (keys, versions, admission), today's cost | 3 | 4 s |
| Group and verify (measured) | 25 | 32 s |
| Encode blocks, tails, skip data, impacts (measured blocks 13; per-term work at 0.4 µs a term, 2.5M terms a flush) | 18 | 23 s |
| Dictionary (161 ns a term, 2.5M terms a flush) and assembly (memcpy of 3 GB) | 2 | 3 s |
| Stored records, columns, file writes (3 GB at 1.2 GB/s) | 3 | 4 s |
| Copy merges to one segment (format 13) | — | 20–30 s |
| **Total** | **64** | **about 105–115 s against 115 s** |

Tight at one thread, which is why the three cuts in section 7 are ranked as they are: the
measured prototype without them (C as run: 118 ns a token of inversion alone) would land
at about 160 s plus merges. At eight threads the add phase, the flush's partitions, and
the assembler's ranges all scale; the serial remainder is stage 1 and the reader (about
10 s), so the projection is 25–35 s against 57 s, with merges I/O-bound beside it.

## 7. Recommendations

Ranked by distance to 4.5x, each with its prototype numbers; the first three are the
design, the fourth is a preset.

1. **Replace the inverter with the tuple-stream, term-partitioned flush (sections 5.2, 5.4,
   6).** Prototype: 118 ns a token against today's 340 with byte-identical segments
   (`research_invert`, 100k documents, five trials), the add phase 25 against 191; the
   stage bound puts the finished form at about 55–60 ns a token. Expected at 5M, one
   thread: inversion from about 440 s (today's A shape at 340 ns a token) to about
   75 s, before merges. Eight threads: one segment per flush, the flush parallel by
   partition (measured 1.35x at eight threads with the encoder still serial; the parallel parts alone scale 1.9–4.8x). This is the step that makes 4.5x reachable; nothing that
   keeps a per-token term probe gets there.
2. **Cut `flush_from`'s per-term cost from 1.7 µs to about 0.4 µs** (section 3, item 2; a
   precondition for every design, and the perf/inverter engineer's natural item). The
   dictionary builder is 161 ns of it (section 5.6); the rest is per-term and per-block
   allocation in `flush_from`, `write_postings`, and `competitive()`. Over the corpus:
   12 flushes of about 2.5M terms plus a 9M-term merge, 50–80 s today, 12–18 s at the
   target. Measurable in isolation with `research_invert`'s B or C, whose `flush_from`
   line is this cost.
3. **Copy merges in format 13** (section 5.3): 95% of postings bytes are full blocks that
   shift by a constant; the format needs a stored list base, tail-coded blocks anywhere,
   and the same for positions. Expected: the 5M force merge from 404 s (docid order, one
   thread) to tens of seconds, I/O-bound; write amplification near 2 instead of a
   re-encode per tier. It makes the eight-thread row's extra segments and the baselines'
   in-run force merge affordable. Prototype owed: a two-segment copy merge on the sample
   against `merge_segments`, once the format decision is taken.
4. **MinHash order at flush as a preset** (section 5.1): 3.4% smaller postings plus
   positions at 100k documents (more at 5M) for 0.5 µs a document plus about 10 ns a
   token in the flush, against bisection's 5.8% at 120x the cost; every flushed segment
   becomes permuted. A disk and query-speed item, not a throughput one; for the
   architect with P1's owners.

Killed or deferred, with their numbers: Lucene/Tantivy-shape slices (B) win 2.5x over
today but keep the per-token probe (70 ns a token in the add phase, 139 in all) and
document-partitioned threads; prefetching the big table's slots (33.7 against 31.5 ns a
token, no gain, the second miss is the key compare); MinHash k = 4 and the df-filtered
variants (no gain over k = 2); compressing positions inside tuples (section 5.5).
