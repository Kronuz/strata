# Indexing gap (T4)

Targets Phase 3's T4 rows (PLAN, "Indexing speed"): one-thread throughput at ≥ 1.0× the
faster of one-thread Lucene and Tantivy, and eight-thread throughput at ≥ 1.0× the faster
of their multi-threaded builders, keyed, at equal memory. Every experiment also reports
peak RSS (T4's memory row: the budget plus 10%), index bytes, and segments.

## Evidence: the n1017 gate run

5M documents, keyed, `STRATA_BUDGET_MB=2056`, strata at main as synced on 2026-10-17
(raw files: the gate VM's `~/bench/results/index-n1017-t4-1` and `index-n1017-t4-8`,
GNU time). Medians of five trials; CPU time is user plus system; written is GNU time's
file-system outputs (512-byte blocks).

| Engine | Wall | CPU | CPU / wall | Peak RSS | Bytes | Segments | Written |
|---|---:|---:|---:|---:|---:|---:|---:|
| strata, 1 thread | 1,091 s | 1,091 s | 99% | 4.61 GB | 4.85 GB | 9 | 6.1 GB |
| Tantivy-st | 512 s | 577 s | 112% | 11.64 GB | 3.07 GB | 1 | 6.2 GB |
| Lucene-st | 890 s | 993 s | 111% | 4.60 GB | 2.82 GB | 1 | 8.5 GB |
| strata, 8 threads | 1,083 s | 1,433 s | 132% | 4.73 GB | 8.02 GB | 14 | 10.9 GB |
| Tantivy, 4 indexing threads | 256 s | 614 s | 241% | 5.10 GB | 3.03 GB | 1 | 8.4 GB |
| Lucene, 8 indexing threads | 260 s | 1,365 s | 522% | 7.20 GB | 2.81 GB | 1 | 9.7 GB |

What the files already say:

1. `STRATA_THREADS` reaches the index: at 8 the CPU rises from 99% to 132% of wall, user
   time from 1,069 to 1,470 s, segments from 9 to 14, bytes from 4.85 to 8.02 GB, and
   written bytes from 6.1 to 10.9 GB. Wall time does not move (1,091 against 1,083 s).
2. The run is serial. If a part P of the one-thread work ran on 8 threads and the rest S
   on one, CPU = S + P and wall = S + P/8: 1,433 = S + P and 1,083 = S + P/8 give P = 400 s
   and S = 1,033 s. Eight threads add about 340 s of CPU (1,433 against 1,091), and the
   serial part is at least as long as the whole one-thread run's non-inversion work.
3. Both one-thread baselines use more than one core (111-112%): their merges run on
   background threads (Lucene's ConcurrentMergeScheduler, Tantivy's merge threads), and
   Lucene reads stdin on a thread of its own. Both force-merge to one segment inside the
   timed run; strata's `index` does not, so its bytes are those of 9 or 14 segments.

## What runs where (code at `45cb70c`)

On the one owner thread, in order, for every document:

- `tools/strata_engine.cc:147-157`: the line read, the `id` and `text` fields found by
  `string_view::find`, and `tokens_document` (`:91-106`): an `unordered_map` per document,
  a heap vector per distinct term, a string per term.
- `index::apply` (`index.hh:290`) and `evaluate` (`:1556-1666`): `valid()` builds a
  `std::set` of the document's terms (`:1544-1554`), `key_of` scans them, `lookup_key`
  probes the version map and then every segment newest first (`:1417-1430`), the
  document is copied into the buffer (`:1655`; `apply` takes the transaction by const
  reference), and `set_key` updates the version map.

At the budget, `flush` (`:1678-1757`) splits the buffer into `flush_threads` docid runs
and inverts them on that many threads (D18 as built: inversion happens at flush, not as
documents arrive); the owner inverts run 0 and joins the rest, then writes every file.
Each run becomes its own segment, so eight threads write eight times as many segments.

Merges: the engine passes no executor (`o.background` stays null), so the index's own
`inline_executor` runs every merge synchronously on the owner thread (`index.hh:729-737`,
`:705-724`), where D18 says merges leave the writer's thread. The one commit at the end
reclaims at most `reclaim_slice` (64 MB) of obsolete files (`:395`, `:400-445`), so the
sources of merges finished during the run may still be in the directory when the bytes
are counted.

## E0: profile (attribution, no code change)

Hypotheses, each with what would refute it:

- H1: the owner thread is busy for the whole wall time at 8 threads, and the inversion
  threads only during flushes. Refuted if the owner thread is idle (blocked) for more than
  a quarter of the wall time.
- H2: at 8 threads the extra serial time is merges and key lookups over more segments.
  Refuted if `run_merge` plus `lookup_key` grow by less than half of the extra CPU.
- H3: the one-thread gap is spread over the engine's document building, `evaluate`, and
  inversion, not one hot spot. Refuted if one function holds more than 40% of the samples.
- H4: the bytes include merged-away segments not yet reclaimed at exit. Refuted if the
  final directory holds only the files the last manifest names.

Procedure, on the gate VM after the targeted T3 run: the engine at `21336b4` built as the
gate builds it (Release, clang, `-march=native`) plus `-g -fno-omit-frame-pointer`;
`perf record -F 999 -g` over `strata_engine index` on the first 100,000 documents of the
corpus (PLAN, "Targeted runs"), keyed, `STRATA_BUDGET_MB=2056`, at `STRATA_THREADS` 1 and
8, page cache dropped first. At 100,000 documents the 2,056 MB budget never fills, so the
run is stage 1 for every document, one flush at the commit, and no merges; a second pair
at 1,000,000 documents (several flushes and merges, as at 5M) checks the mix at scale. Reported: samples per thread, inclusive shares of the phases (read and parse,
`tokens_document`, `evaluate` and its parts, flush inversion, flush writes, merges,
commit), per-second CPU of each thread from `perf script`, and the final directory
against the manifest's segments.

### E0 results (2026-10-17)

Run as above on base `21336b4` (raw: the gate VM's `~/ig/prof-a100k`, `t1.data` and
`t8.data`, `perf script` sample times per thread; the profiled build's wall is 32.3 s at one
thread and 25.9 s at eight).

Eight threads, by second of wall time (perf samples of the owner thread and of all others):

```
s  0-19   owner ~720/s, others 0        stage 1: read, tokenize, evaluate, 100k documents
s 19-24   owner ~450/s, others ~3,000/s the one flush: 8 runs inverted, ~5 CPUs busy
s 24-26   owner ~700/s, others 0        files written, commit, the buffer freed
```

So 75% of the eight-thread wall is stage 1 on the owner, 19% the flush, 5% the tail (H1
holds). The one-thread profile splits the work (inclusive shares of all samples):

| Phase | Share | Where |
|---|---:|---|
| `tokens_document` (engine) | 25% | an `unordered_map` and a vector per term, per document |
| `evaluate` | 28% | `valid()`'s `std::set` 14%, the document copied into the buffer 11%, keys under 1% |
| `flush` | 37% | `segment_writer::add` 19%, encoding 12% |
| the rest | 10% | the read loop, JSON fields, freeing the buffer |

Kernel time is 21% of the one-thread CPU (6.8 of 32.2 s, 322k minor faults: first touch of
the buffer and the writer's tables) and every phase's allocations pay for it; `malloc` and
`free` are 38% inclusive. H3 holds: no function passes 40%.

The flush scales poorly: its inversion takes 12 CPU-seconds in one run and 27 in eight
(user 25.3 to 40.3 s, system 6.8 to 14.0 s for the whole run), so eight threads finish it
only 2.4 times sooner. Each run builds its own term table and dictionary over the same
vocabulary, and eight threads fault and allocate at once.

At 100k no merge runs and no segment exists before the commit, so H2 (merges and key
lookups) cannot show here; it is checked at the larger confirmation.

## Calibration (100k sample)

PLAN, "Calibrate once, then only the fastest": one run of strata, Tantivy, and Lucene at
100,000 documents, keyed, at one thread (`-st` engines) and at eight (the multi-threaded
builders; strata with `STRATA_THREADS=8`), after a 20,000-document dry run. The faster
baseline at each thread count is the anchor every A/B runs beside; a fix that wins clearly
at 100k gets one larger confirmation against the anchor only, the candidate T4 row.

Run 2026-10-17 on the gate VM, base `21336b4` (raw: `~/bench/results/iab-cal100k-t1`,
`iab-cal100k-t8`; one trial each, page cache dropped before each):

| Threads | strata | Tantivy | Lucene | Anchor | 4.5× the anchor |
|---|---:|---:|---:|---|---:|
| 1 | 38.5 s | 17.0 s | 42.3 s | Tantivy-st | 3.8 s |
| 8 | 24.9 s | 9.9 s | 35.0 s | Tantivy, 4 indexing threads | 2.2 s |

Recalibrated on a quiet host (2026-10-17 22:05; host load 1.6 to 3.3 without the VMs; raw:
`~/bench/results/iab-recal100k-t1`, `iab-recal100k-t8`), since Tantivy's eight-thread time
fell from 9.9 s to 3.4 s between runs, far past any spread: the first calibration ran while
the host's load average was 14 to 20.

| Threads | strata (base) | Tantivy | Lucene | Anchor | 4.5× the anchor |
|---|---:|---:|---:|---|---:|
| 1 | 13.3 s | 6.8 s | 15.0 s | Tantivy-st | 1.5 s |
| 8 | 10.8 s | 3.4 s | 10.4 s | Tantivy, 4 indexing threads | 0.76 s |

Every A/B from here waits for the host's load without the VMs to stay under 4 for a minute
(`.scratch/ab_quiet.sh`, which also samples it every 10 s into the run's log).

Harness notes for the lead: the eight-thread baselines (`tantivy-0.26`, `lucene-10.4.0`, the
harness's own engines) index without keys: Tantivy's `id` is `STORED` only and Lucene calls
`addDocument`, so the n1017 eight-thread row compared keyed strata with unkeyed baselines
(BENCHMARKS.md, "Equal content"). `index_trials.sh` times `make index`, which for Lucene
includes a 2 to 3 s Gradle rebuild: 0.3% at 5M, about 7% at 100k. Lucene-st at 20,000
documents spends 71 s in the kernel (79 s wall), so small samples misplace Lucene.

## Experiments

Each is written here before its A/B runs: the hypothesis, the procedure, and the rule
that keeps or removes it.

### E1: parse and tokenize ahead of apply

Hypothesis: at eight threads the owner thread spends most of its time on work that never
touches the index (reading lines, finding the JSON fields, `tokens_document`), and moving
that work to parser threads cuts the eight-thread wall time by about its share of the
owner's samples in E0. A host tokenizes before it calls `apply` (Xapiand does, on its own
threads), so this is the host's side of D18's stage 1, not a change to the index.

Change (`tools/strata_engine.cc`, `ordered_batches`): one thread reads batches of 1,000
lines, `STRATA_PARSE_THREADS` threads (default `STRATA_THREADS` when above 1) turn each
into its transaction, and the owner applies them in input order, so docids, segments, and
bytes are unchanged. At `STRATA_THREADS=1` the engine keeps today's inline path.

Procedure: `bench/index_ab.sh` at 100,000 documents, keyed, `STRATA_BUDGET_MB=2056`,
`STRATA_THREADS=8`, A at the branch's base and B with the change, beside the eight-thread
anchor, five interleaved trials, under a strict hold of at most 30 minutes. Functional
check first: the segment files of A and B are byte-identical at 1 and 8 threads.

Keep if B's throughput over A's has its interval's lower bound above 1.0 with peak RSS
within the budget plus 10%; remove otherwise.

First run (2026-10-17, B at `934811b`; raw: `~/bench/results/iab-e1-t8`, binaries' SHA-256
prefixes in its `setup`), every round inside a window when other builds ran during the strict
hold (20:25 to 20:32, the lead's note), so it is rerun before it counts:

| | Wall, five trials (s) | Median | CPU / wall | Peak RSS | Bytes | Segments |
|---|---|---:|---:|---:|---:|---:|
| A | 23.1, 26.2, 26.9, 26.0, 18.2 | 26.0 | 173% | 1.27 GiB | 49.0 MB | 8 |
| B | 13.3, 18.1, 16.7, 14.1, 11.4 | 14.1 | 277% | 1.42 GiB | 49.0 MB | 8 |
| Tantivy | 8.2, 11.7, 11.2, 8.6, 8.0 | 8.6 | 240% | 0.17 GiB | 47.1 MB | 1 |

B's throughput over A's: 1.607× [1.444, 1.848]. Over Tantivy: A 0.415× [0.331, 0.448],
B 0.647× [0.612, 0.701]; the 4.5× target is 7.0 times further. Segments are byte-identical
(the functional check, and 49.0 MB both). Peak RSS rises 150 MB (the batches in flight),
inside the 2,056 MB budget plus 10%. The host's load average was 14 to 20 during the run
(other agents' builds while the strict hold was live), which spreads all three engines alike
(33 to 48%); the paired ratios hold. Projected magnitude: at the n1017 shape the owner
loses the 25% `tokens_document` share, so the eight-thread 5M run would drop by about a
quarter, still far behind; one-thread time and energy are unchanged (the inline path).

### E2: one segment per flush, terms encoded by hash group on every thread

Hypothesis: today's parallel flush multiplies segments by the thread count (8 at 100k, 14
against 9 at 5M) because each docid run writes its own segment, with its own term table,
dictionary, and per-segment sections; a flush that writes its runs as one segment, its terms
encoded on the same threads, keeps the eight-thread flush's speed at 100k, writes the
one-thread flush's bytes (one segment instead of eight, 43.5 instead of 49.0 MB), and removes
at 5M the segments that eight threads add and the merges and key probes they cost.

This is the research note's term-partitioned flush (docs/research/indexing.md 5.4, 6.3-6.4)
on today's inverter: the encoder and the assembler are the part that carries over to inverter
C, whose partitions replace the runs' term tables as the source of each group's terms.
Neither baseline writes one segment from several indexing threads: Lucene's DWPTs and
Tantivy's indexing threads each write their own.

Change (`writer.hh`, `index.hh`): `flush_from`'s per-term work splits into `encode_term`
(the term's info, postings, and positions bytes, reading only the per-document lengths) and
the placement of those bytes in the sections and the dictionary (`write_segment`), so a flush
and a merge write what they wrote before. `flush_grouped` encodes 256 groups of terms (by the
top byte of the term hash) on T threads and places them in term order through a heap of the
groups' next terms. `flush_runs` writes the runs of a parallel flush as one segment: the
runs' per-document columns appended in order, each group's terms gathered from every run's
table and their postings concatenated with each run's ordinal base. `index::flush` inverts
the runs on their threads as before and writes the one segment.

Procedure: functional first (the segment written by eight threads byte-identical to one
thread's, in unit tests and on the engine), then `bench/index_ab.sh` at 100,000 documents,
keyed, `STRATA_THREADS=8`, A at the base, B at E1, C at E2, D at E1 with perf/inverter's E1
(`d5468e4`, documents as token streams; information for the lead, not a keep decision), beside
the anchor, five interleaved trials under a strict hold. The same run serves as E1's rerun.

Keep if C's segments are byte-identical to one thread's and C's throughput over B's has an
interval whose lower bound is at least 0.97 at 100k (where it removes no merge), with peak RSS
within the budget plus 10%; a lower bound above 1.0 is a win at 100k too. The scale case is
the confirmation run. Remove otherwise.

First run (2026-10-17, raw: `~/bench/results/iab-e2-t8`; host load 2.9 at the start, median
6.1 and at most 9.3 during the run, from `.scratch/e2-load.log`): A at the base, B at E1
(`934811b`), C at E2 (`c4b0202`), D at E1 with perf/inverter's `d5468e4`.

| | Wall, five trials (s) | Median | CPU / wall | Peak RSS | Bytes | Segments |
|---|---|---:|---:|---:|---:|---:|
| A | 15.5, 12.9, 14.2, 13.0, 11.2 | 13.0 | 167% | 1.24 GiB | 49.0 MB | 8 |
| B | 6.3, 6.6, 7.1, 6.4, 5.7 | 6.4 | 296% | 1.38 GiB | 49.0 MB | 8 |
| C | 7.0, 7.3, 7.9, 6.8, 6.0 | 7.0 | 269% | 1.51 GiB | 43.5 MB | 1 |
| D | 3.3, 3.0, 3.3, 3.0, 2.4 | 3.0 | 462% | 0.86 GiB | 49.0 MB | 8 |
| Tantivy | 4.8, 4.9, 5.7, 4.5, 3.6 | 4.8 | 237% | 0.17 GiB | 47.1 MB | 1 |

Over A: B 1.992× [1.967, 2.446], C 1.853× [1.776, 2.209], D 4.343× [4.269, 4.743]. Over
Tantivy: A 0.349×, B 0.751× [0.634, 0.804], C 0.678× [0.598, 0.727], D 1.498× [1.456, 1.752].
E1's rerun confirms it (B over A 1.99×; the first run's 1.61× was depressed by the host's
builds). C writes one segment, byte-identical to one thread's (43.5 MB, the one-thread size),
but its per-round ratio to B is 0.90 (0.899 to 0.950): below the 0.97 rule, so E2 as built is
not kept. A profile of C (`~/ig/prof-c100k`) puts the loss in work the runs did in parallel
and the one segment does on the owner: placing 438k terms and building their dictionary after
every group is encoded (hash groups cannot be placed before all are done), and the eight run
writers freed in turn.

Also from D: with perf/inverter's documents as token streams, the owner's stage 1 shrinks so
far that eight threads reach 1.50× Tantivy at 100k; the 4.5× target is 3.0 times further.

### E2b: key-range groups placed as they finish, run writers freed on their threads

Hypothesis: E2's 10% loss is its serial tail, so groups that are key ranges (split at a sample
of the runs' terms, one in sixteen), assigned to groups on the runs' threads and placed by the
owner as each finishes while the workers encode the next, overlap the placement with the
encoding, and freeing each run's writer on its own thread removes the rest; C2 matches B at
100k with E2's one segment.

Change: `flush_runs` picks up to 255 splitters from the sorted sample and assigns each run's
terms to their group on the runs' threads (`segment_writer::parallel`); `flush_grouped`
places group g once it is encoded, so no heap merge is needed; `index::flush` frees the run
writers on as many threads. Bytes are unchanged (tests and engine check as for E2).

Keep rule: as E2's, C2 over B at least 0.97 by the interval's lower bound at 100k, one
segment, byte-identical to one thread's. Inverter C's partitions are by hash and would need
E2's merge back (about 0.25 s a 100k flush on the owner) or splitters of their own.

Result (2026-10-17, quiet host: load median 1.9 at 100k and 1.6 at 1M; raw:
`~/bench/results/iab-e2b-t8` and `iab-e2b-1m-t8`): **kept, the 100k rule missed by its
lower bound.**

| 100k, 8 threads | Wall, five trials (s) | Median | Bytes | Segments |
|---|---|---:|---:|---:|
| E1 (`934811b`) | 5.7, 5.9, 5.8, 5.7, 5.8 | 5.8 | 49.0 MB | 8 |
| E2 (`c4b0202`) | 6.2, 6.3, 6.2, 6.1, 6.0 | 6.2 | 43.5 MB | 1 |
| E2b (`62d235e`) | 6.1, 5.8, 5.9, 5.9, 6.0 | 5.9 | 43.5 MB | 1 |
| Tantivy | 3.4, 3.5, 3.5, 3.4, 3.4 | 3.4 | 47.1 MB | 1 |

Over E1: E2 0.943× [0.907, 0.967], E2b 0.968× [0.925, 1.019]: E2b's interval holds 1.0, so
it shows no loss at 100k, but its lower bound misses the 0.97 the rule asked for.

| 1M, 8 threads | Wall, five trials (s) | Median | Peak RSS | Directory | Segments |
|---|---|---:|---:|---:|---:|
| E1 | 103.1, 94.4, 95.1, 96.6, 93.6 | 95.1 | 4.55 GiB | 1,191 MB (40 segment files) | 8 |
| E2b | 72.8, 72.1, 72.0, 74.9, 72.4 | 72.4 | 4.85 GiB | 643 MB (5 files) | 5 |
| Tantivy | 45.8, 45.2, 44.9, 45.0, 45.3 | 45.2 | 1.66 GiB | 677 MB | 1 |

At 1M, E2b is 1.310× [1.289, 1.417] E1's throughput and 0.627× [0.600, 0.630] Tantivy's: one
segment per flush leaves the tiered policy nothing to merge (5 segments, under its 8 a tier),
where E1's 48 flushed segments merged on the owner thread down to 8 and left 32 merged-away
files (the reclaim cap, perf/index-size's E2). Peak RSS rises 0.3 GiB (the runs, the one
writer's columns, and the encoded groups live together); both are far over the budget plus
10% (2.26 GB), as n1017's 4.6 GB was. Kept on the 1M confirmation: the 100k sample has no
merge, so the one-segment flush can only tie there, and it does within the interval. The
eight-thread path it serves (docid runs over today's inverter) is the one inverter C replaces
next, so it is not tuned further.

### E3: inverter C at eight threads, its flush on every thread

Hypothesis: with inverter C as the buffer at any thread count (perf/inverter's E3 keeps it to
one thread), the eight-thread run spends its flush in C's stages, each parallel: the tuples
partitioned by hash on all threads, each partition grouped by one, and the terms, split into
key ranges at a sample, sorted, encoded, and placed in order (`flush_grouped`, E2b). The
segment is the one-thread flush's, byte for byte; the eight-thread wall at 100k falls below
E2b's by most of the flush's share.

Change: `tuple_buffer::flush` takes `threads`; above one it partitions on that many threads
(per-thread counts over the tuple chunks, then each scatters its chunks, so each partition
keeps arrival order), groups the 256 partitions on them, samples one term in sixteen for up
to 255 splitters, and hands the ranges to `segment_writer::flush_grouped`. `index` keeps C
as the active buffer at every `flush_threads` and passes it to the flush. One thread keeps
the serial flush. Neither baseline flushes one in-memory segment on several threads.

Procedure: byte-identity first (C's flush at 1, 2, 8 threads in `tuple_buffer_test`, the
engine at 1 and 8 threads), then the 100k A/B at eight threads: A at E2b (`62d235e`), B at
the merge with perf/inverter (`c4d1c06`, its K-run path), C at E3, beside Tantivy.

Keep if C's throughput over B's has its interval above 1.0 at 100k with byte-identical
segments and peak RSS no worse than B's by more than 10%; remove otherwise.
