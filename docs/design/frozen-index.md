# Frozen indexes: a read-optimized mode, and back

Status: exploration, not yet scheduled. The owner's idea, first meant for Xapian's honey
backend: an index can be **frozen**, which locks it against writes and rebuilds it for
reads only, and **unfrozen**, which makes it writable again. PISA shows the ceiling: a
static index needs none of the structures that updates require, and its builder can spend
time that an updating engine cannot.

## What freezing removes

A writable index pays for writes on every read:

- Live-docs bitmaps, and the live check on every candidate.
- The version map and key filter, kept for replaces and deletes.
- Several segments, each with its own term lookup and scorer setup.
- Block bounds kept conservative, since deletes can invalidate them.
- Formats chosen to be cheap to write and merge, not cheapest to read.

A frozen index is one segment with no deletes and no buffer. None of that survives.

## What freezing adds

Freeze runs once, offline, under the node's merge budget:

1. **Document reordering.** Recursive graph bisection (the BP that Lucene's `-bp` and PISA
   use) sorts documents so similar ones are near each other. Gaps shrink, postings
   compress better, and block-max bounds tighten. strata's merge already writes either
   order (D15, Track P1); freeze always reorders.
2. **Precomputed, quantized scores.** Each posting stores its BM25 contribution quantized to
   8 bits (PISA's approach). Scoring becomes integer additions, and block-max bounds are
   exact. The shard's statistics are fixed, so the quantization is exact for the shard
   (global statistics in a cluster: below).
3. **Tight skip data.** Block-max and impact data are exact for every block, because no
   later delete can invalidate them; MaxScore and block-max WAND prune harder.
4. **Static lookups.** Keys map to docids through a perfect hash (the Kronuz `perfect-hash`
   library) instead of a filter plus a term lookup. The term dictionary drops anything
   only an update path needs.
5. **Denser stored fields.** Larger blocks, compressed with a dictionary trained on the
   shard (zstd dictionaries).
6. **Residency.** The whole index is a candidate for pinning, huge pages, and
   `MADV_RANDOM` or prefetch hints tuned for a file that never changes.

## Freeze and unfreeze

- **Freeze:** stop admitting writes (the group rejects them with a "frozen" verdict),
  commit, force-merge to one segment with the steps above, and commit that segment as the
  index's only content. The frozen segment is a normal format-12 segment with optional
  sections (quantized impacts, the reorder map, the key hash). Any strata reader opens it;
  readers that know the sections use them.
- **Unfreeze:** admit writes again. The frozen segment becomes the base segment of an
  ordinary index: new writes go to new segments, and a delete against it creates a
  live-docs file as usual. Its quantized scores stay valid only while the statistics they
  assumed hold; once the live set drifts past a bound, the next merge drops the optional
  sections and the segment reads like any other. The merge policy treats the frozen
  segment as maximum-sized, so unfreezing never forces a rewrite.
- Both are idempotent and crash-safe through the ordinary commit protocol (D1): a crash
  mid-freeze leaves the writable index or the frozen one.

## In the cluster

A frozen shard's group has nothing to replicate but its one checkpoint:

- **Replicas as copies.** The frozen segment is an immutable, content-addressed artifact.
  Any number of read replicas can install it (snapshot transfer) without joining the
  consensus group, so read capacity scales without growing quorums.
- **Quiet groups.** No writes means no log traffic; the group stays quiescent (Track C,
  D12) and can shrink to a minimal voter set, or to metadata in its placement group only.
- **Object storage.** A frozen segment can be served from object storage with a local
  cache, as Quickwit serves splits (Track D6), so cold frozen indexes cost disk only.
- **Global statistics.** A frozen shard's statistics are fixed, so a query's statistics
  pass (ARCHITECTURE, Distribution) can use cached per-shard figures. Quantized scores are
  exact for the shard's own statistics; with global scoring, the quantized value is
  rescaled per query, or the shard keeps its unquantized impacts for that mode.

## Xapiand

Xapiand's honey idea maps to an index-level setting: `freeze` and `unfreeze` operations,
and a frozen flag in the index's metadata that the HTTP API reports. Time-based indexes
(logs, closed months) freeze as they age, as Elasticsearch's frozen tier and Lucene's
read-only indices do.

## How it is measured

- Query trials on the 5M corpus: the frozen index against the same index unfrozen,
  against Lucene with `-bp`, and against PISA, per query class. PISA is the reference
  for the frozen mode specifically.
- Disk and serving memory, frozen against unfrozen.
- Freeze time and its CPU per GB, budgeted like merges.
- Unfreeze: write throughput and query latency right after, and after the first merge.

## Where it fits

Freezing needs Phase 3's lifecycle (commit protocol, merges, live docs) and Track P1's
reordering. It slots into Track P as P20, with its cluster half after Phase 3R (read
replicas, quiescence) and Track D6 (object storage). The single-node half (freeze,
unfreeze, quantized impacts, the key hash, the query trials) can start once Phase 3's
gate passes.
