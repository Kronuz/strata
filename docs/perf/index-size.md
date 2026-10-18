# Index size and merging during indexing

Targets Phase 3's disk row (PLAN, "Disk": the whole index and every section at ≤ 1.0× the
smaller baseline) as T4 measures it, the index each engine leaves after indexing, and the
merging that decides how many segments that index has. Every experiment also reports the
other three axes: indexing time (T4), peak RSS, and write amplification (T3).

## Evidence: the n1017 gate run

5M documents, keyed, at equal memory (`STRATA_BUDGET_MB=2056`, calibrated to Lucene's peak
RSS; raw files on the gate VM, `~/bench/results/index-n1017-t4-{1,8}`, `wamp-t3-*`):

| Engine | Bytes | Segments | Notes |
|---|---:|---:|---|
| strata, 1 thread | 4,850,403,469 | 9 | no merge inside the run |
| strata, 8 threads | 8.02 GB | 14 | |
| Lucene 10.4.0, 1 thread | 2,819,677,322 | 1 | `forceMerge(1)` inside the timed run |
| Tantivy 0.26, 1 thread | 3,066,216,363 | 1 | merges every segment inside the timed run |

T3 at 500k documents, append-only: strata 329.7 MB in one segment (write amplification
1.014), Lucene 320.9 MB (1.498), Tantivy 352.7 MB (2.011). At 20k: strata 1.069, Lucene
1.073, Tantivy 2.166.

What the code says before anything is measured (main at `4c9ebfd`):

1. The engine passes no executor, so merges run inline on the owner thread, and the tiered
   policy (8 per tier, 16 MiB floor, 5 GiB maximum) leaves 9 segments of about 540 MB alone:
   `allowed_segments` gives 9 for 4.85 GB whose smallest is 540 MB.
2. A merged-away segment's file is queued for reclamation only by `retain()` after a commit
   (`index.hh:1869-1925`), and a commit frees at most `reclaim_slice`, 64 MiB
   (`index.hh:395`). The engine commits once, at the end, so every source merged during
   the run stays on disk when the bytes are counted, less 64 MiB.
3. The baselines' bytes are a force-merged single segment; strata's are the segments its
   policy left. The disk row in PLAN is after a full merge, where strata passes at 0.999×
   keyed Lucene `-bp` (`phase-3-ratchets.md`, "Disk after a full merge").

## E1: where the bytes are (measurement, no code change)

Hypotheses, each with what would refute it:

- H1 (per-segment overhead): splitting 100k documents into N segments with merges off costs
  each extra segment a fixed amount (dictionary blocks, partial postings blocks, skip data,
  headers, filters) that, scaled to 5M at N = 9, explains less than a quarter of the
  4.85 − 3.3 GB gap. Refuted if the per-segment cost, scaled, explains more than half.
- H2 (unreclaimed sources): with merges on, the directory holds files no manifest names,
  and at 5M they are most of the gap. Refuted if the directory equals the manifest's files.
- H3 (encoding): one segment of 100k documents is within 5% of Lucene's force-merged 100k
  index in total, but some sections are above their counterparts. Refuted if the whole is
  more than 10% above.

Procedure: `strata_engine` at `4c9ebfd`, Release, macOS x86_64 (bytes do not depend on the
platform: zstd is pinned), the first 100,000 documents of the gate corpus, keyed.

1. One segment: `STRATA_BUDGET_MB=2056`. `sections` on the segment, and every file.
2. N segments, merges off (`STRATA_AUTO_MERGE=0`), budgets that give about 9 and 14
   segments. Per-section totals against step 1.
3. The same budgets with merges on: segments left, the manifest's files against the
   directory.
4. Lucene and Tantivy at 100k on the gate VM (after the T3 run, which counts that disk's
   writes): Lucene by file extension, force-merged and with `NO_MERGE=1`; Tantivy by file
   extension, merged and with `NO_MERGE=1`.

Bytes are exact single measurements; no hold. Kept as the attribution the next experiments
start from; nothing to remove.

### E1 results at 100k and 1M (macOS x86_64, `4c9ebfd`)

One segment at 100k documents (the gate budget never fills), each engine keyed; Lucene's
flushed segment is a compound file, split by its `.cfe` entries:

| Section | strata | Lucene 10.4.0 | Tantivy 0.26 | strata / smaller |
|---|---:|---:|---:|---:|
| Postings (`.doc`, `.idx`) | 15,971,722 | 14,579,635 | 17,102,355 | 1.095× |
| Positions (`.pos`) | 22,446,072 | 22,831,251 | 24,795,243 | 0.983× |
| Terms (`.tim` `.tip` `.tmd`, `.term`) | 4,309,207 | 3,740,646 | 4,414,539 | 1.152× |
| Stored (`.fdt` `.fdx` `.fdm`, `.store`) | 316,696 | 638,182 | 1,005,343 | 0.496× |
| Norms (`.nvd` `.nvm`, `.fieldnorm`) | 100,000 | 100,162 | 200,113 | 0.998× |
| Doc values (`sort_field`) | — | 400,231 | 400,187 | |
| Lengths (excluded, BENCHMARKS) | 187,681 | — | — | |
| `pk_filter` | 118,897 | — | — | |
| Whole directory | 43,493,711 | 42,291,967 | 47,919,012 | 1.028× |

The same 100k documents in N segments (merges off), against one:

| Section | 1 segment | 15 segments | 22 segments |
|---|---:|---:|---:|
| Postings | 15,971,722 | 16,702,996 (+4.6%) | 16,660,918 (+4.3%) |
| Positions | 22,446,072 | 24,293,607 (+8.2%) | 24,708,313 (+10.1%) |
| Terms and term index | 4,309,207 | 9,737,981 (2.26×) | 11,139,101 (2.58×) |
| Everything else | 766,760 | 786,320 | 799,004 |
| Whole | 43,493,711 | 51,521,335 (+18.5%) | 53,307,906 (+22.6%) |

With merges on at the same budgets the tiered policy leaves 8 segments (47.95 and
47.81 MB, +10%), and the directory holds only the manifest's files: at this size the
commit's 64 MiB slice frees every merged-away source.

H2 at 1M documents (`STRATA_BUDGET_MB=490`, merges on, 21 flushes): the manifest names 5
segments, 637,293,248 bytes; 14 merged-away segment files, 511,283,280 bytes, are still on
disk. The directory is 1.80× the index. At the gate budget (2056 MB) the same 1M
documents flush 5 times (186k to 286k documents each, by the budget's estimate of each
document) and nothing merges, so 5M makes about 21 flushes and the policy merges them to
9 segments: H2 applies there.

Findings so far:

1. H1 holds at 100k: splitting costs mostly duplicated dictionaries (each segment holds
   its own copy of the common vocabulary) and positions (more short lists in VByte tails).
2. H2 holds at 1M; its size at 5M is the next measurement.
3. H3 holds: one segment at 100k is 1.028× Lucene's whole; postings (1.095×) and terms
   (1.152×, mostly keys: D12's section in format 13) are the sections above their
   counterparts.
4. The equal-content table has one asymmetry: both baselines store `sort_field` as doc
   values (400 KB here, 20.1 MB at 5M), strata's engine does not index it.

## E2: reclaim at the end of the run

Change (`tools/strata_engine.cc`): after its one commit, `index` calls `ix.reclaim()`,
which frees every queued obsolete file (the commit frees only `reclaim_slice`, 64 MiB),
and prints the bytes freed and the time it took. Lucene's `IndexWriter` deletes
unreferenced files at commit and close (`IndexFileDeleter`), and Tantivy's builder runs
`garbage_collect_files`, both inside their timed runs, so the three directories then hold
only their indexes.

Hypothesis: the directory equals the manifest's files at every size, and the extra time
is under 1 s at 5M (7 to 130 ms per GiB freed, `reclamation.md`). Keep if the bytes fall to
the manifest's total and the run's time does not grow by more than the reclaim's own
printed time; remove otherwise. Bytes at 1M (`STRATA_BUDGET_MB=490`) and 5M (2056); no
hold for bytes.

Library finding, not changed here: `drain()` frees only queued files, and a merged-away
source is queued by `retain()` after the next commit. A storage-budgeted index that
ingests between commits can refuse a transaction (`no_space`) with merged-away sources on
disk. Queueing a source at `install_merge` when no retained manifest names it would let
admission drain it; `index.hh` is under the layers refactor, so this goes to its owner.

## E3: background merges in the engine

Change (`tools/strata_engine.cc`): `STRATA_MERGE_THREADS=N` gives the index a
`pool_executor` over a `ThreadPool` of N threads (D18 as built), so the policy's merges
run beside indexing instead of on the owner thread, and the engine waits for them
(`wait_merges()`) before its commit, as Lucene's `IndexWriter` close and Tantivy's
`wait_merging_threads()` do. The default, 0, keeps today's inline merges, so A and B are
one binary.

Hypothesis: at one indexing thread the owner stops paying for merges, so wall time falls
by about the inline merge time and CPU time stays the same; bytes and write amplification
stay within a few percent (the same policy, with merges finishing at different moments);
peak RSS rises by the merge's working memory, which now overlaps a filling buffer.
The baselines' one-thread builders run their merges the same way (111% to 112% CPU in
n1017).

Procedure: 100k documents, keyed, `STRATA_BUDGET_MB=41` (15 flushes, merges as at 5M),
A (`STRATA_MERGE_THREADS=0`) and B (1) interleaved with the anchor (Tantivy's one-thread
builder) over five trials, under a light hold: the measured configuration is one
indexing thread, as the baselines' are. Bytes, segments, peak RSS, and the reclaim's
bytes recorded for every run.

Keep if B's wall time over A's has its 95% interval's upper bound below 1.0 with the
median effect at least 3%, its bytes within 2% of A's, and peak RSS within the budget
plus 10% of A's; remove otherwise. The 5M confirmation (one run each) follows only a keep.

## Notes for the format 13 designer (measured, not landed)

Projections over real segments' bytes by `.scratch/probe/postparts.cc` and `posparts.cc`
(this branch's scratch; each decodes format 12's postings or positions per term and
re-codes the same values). "100k" is the one-segment 100k index; "271k" is the first flush
of the 1M run at the gate budget (271,000 documents). The reference projection on the
reordered 5M merge is owed (F11's `poststats` base).

**N1, F11: tails as Rice codes (a new candidate beside E4).** Tails are 34% of the
postings section at 100k and 23% at 271k; 77% of tail postings have wdf 1, and format 12
spends a byte on each.

| Tail form | 100k | 271k |
|---|---:|---:|
| Format 12: LEB128 ordinal deltas, then LEB128 wdfs | 5,379,417 | 11,613,267 |
| Lucene's fold: LEB128 of `delta << 1 \| (wdf == 1)`, then wdf when not 1 | 4,185,637 | 9,224,681 |
| E4-like: the tail as one FOR block (bit width of the largest delta and wdf), 2 header bytes | 4,132,253 | 9,275,444 |
| Rice-coded gaps with the best k per tail (5 bits), Elias-gamma wdf | 3,385,968 | 7,593,130 |
| The best of the three per tail, plus a codec byte | 3,509,168 | 7,824,644 |

On F11's base, the reordered 5M merge (`seg-200.sx` from the gate VM's
`~/reorder5m/reorder`, 845,553,044 bytes of postings; 24,248,859 tail postings, 17,755,693
with wdf 1):

| Postings part | Bytes | Share |
|---|---:|---:|
| Full blocks (FOR ordinals, PFOR wdfs) | 683,320,995 | 80.8% |
| Level 0 (per-block skips and impacts) | 71,548,163 | 8.5% |
| Tail ordinal deltas, LEB128 | 56,048,569 | 6.6% |
| Tail wdfs, LEB128 | 24,249,272 | 2.9% |
| Level 1, group pairs, headers | 10,386,045 | 1.2% |

| Tail form on the 5M merge | Tail bytes | Postings section | Against keyed `-bp`'s 833.7 MB |
|---|---:|---:|---:|
| Format 12 | 80,297,841 | 845,553,044 | 1.014× |
| Lucene's fold | 65,793,375 | 831,048,578 | 0.997× |
| FOR-packed, one block per tail | 76,049,831 | 841,305,034 | 1.009× |
| Rice gaps, gamma wdf | 62,237,696 | 827,492,899 | 0.993× |

The fold alone closes the carried postings row; Rice tails close it with 6.2 MB to spare.
F11's candidates have E1 (omit all-ones wdf blocks) for full blocks but nothing for the
tails' wdfs, which are 2.9% of the section by themselves.

Rice tails take the 100k postings section from 15,971,722 to about 13,978,000 bytes,
0.959× Lucene's `.doc` (14,579,635), where Lucene's own fold reaches 1.014×. A codec choice
per tail does not pay its byte; Rice alone wins. Decoding is a `countl_zero` per unary
quotient and a k-bit remainder over at most 127 values, so the reader cost sits in the
tail-decode profile F1 asks for (AUT-R13). Gaps follow a geometric law for rare terms,
which is the case Golomb-Rice codes are optimal for (Witten, Moffat, and Bell, *Managing
Gigabytes*, ch. 3).

On the reordered 5M merge, positions are 1,525,049,214 bytes: PFOR blocks 96.2%, tails
49,426,367 (3.2%), headers 8,091,375 (0.5%); Rice tails 45,314,978 (−4.1 MB).

**N2, F14: implied positional headers.** Every term with positions writes two LEB128
lengths (full position blocks, their bytes) that are both zero whenever the term has fewer
than 128 positions, which its cf already says: 686,997 bytes at 100k (3.06% of positions),
1,386,579 at 271k (1.77%). Writing them only when cf ≥ 128 is a reader rule, no flag.

**N3, F14: positional tails as Rice codes.** The tail's LEB128 position deltas
(3,906,970 bytes at 100k, 8,473,191 at 271k) as Rice codes with the best k per term:
3,483,466 and 7,599,936 (−10.8% and −10.3% of the tails; −1.9% and −1.1% of positions).
Elias-gamma alone is worse than LEB128 here (4,517,045 at 100k).

**N4, termdict (F8's K3 and F13-R20).** At 100k the dictionary is 4,309,207 bytes against
Lucene's 3,740,646 (1.152×), with 100,000 keys of 45 raw bytes each (4.5 MB raw) beside
338,367 vocabulary terms (2.6 MB raw); single ordinals are 854,040 bytes (1.95 per
single-posting term). F8 moves keys to their own section; nothing here changes that
plan.

### E1 at 5M (macOS x86_64, `4c9ebfd`, the gate's configuration)

`STRATA_BUDGET_MB=2056`, one thread, merges on, keyed, the full corpus (8,327,453,114
bytes, the VM's file): 21 flushes, two inline merges, 7 segments left (n1017's gate build
left 9; the mechanism is the same).

| Part of the directory | Files | Bytes |
|---|---:|---:|
| Segments the manifest names | 7 | 2,824,094,984 |
| Merged-away sources still on disk | 16 | 2,210,005,216 |
| Manifests, metadata, lock | 4 | 303 |
| Whole directory (what T4 counts) | 27 | 5,034,100,503 |

The 7 segments: two merges of 1,977,000 and 2,020,000 documents (1,089,965,572 and
1,096,692,720 bytes) and five flushes of 118,104 to 284,000.

Attribution of n1017's 4.85 GB against Lucene's 2.82 GB:

1. **Unreclaimed merge sources, 2.21 GB (44% of the directory): the biggest cause.** E2
   removes it: with it, T4's one-thread bytes are 2,824,094,984 against Lucene's
   force-merged 2,819,677,322 (1.0016×) and Tantivy's 3,066,216,363 (0.921×). By
   BENCHMARKS' rules (lengths excluded; the baselines' `sort_field` doc values included)
   strata is below both.
2. **Segment count**, against the same documents in one segment: owed (the docid-order
   force merge of this index is running).
3. **Encoding**, per section: E1's 100k table; postings and terms are the sections above
   Lucene's.

Merge cost at this size, for E3 and a final merge: a docid-order force merge of the 1M
index's 5 segments (637 MB out) takes 55 s on this host (one thread, `nice 19` under other
light holds). Sampled with `sample` over 45 s: about a quarter of the samples in
`malloc` and `free` (a `std::vector` per posting's positions in `merge_segments`), about
7% sorting postings that each source already holds in ordinal order, and a quarter in the
gather loop. A merge that streams each source's postings through one positions buffer
and a k-way merge by ordinal writes the same bytes; it belongs in `merge.hh`, which
phase4/layers-refactor rewrites, so it is a note for that branch, not a change here.

E3's dry run (20k documents, `STRATA_BUDGET_MB=8`, one round): A 2.52 s, B 7.40 s, the
same 9,174,087 bytes in 2 segments. B's extra 5 s is `merge_memory_wait`: `run_merge`
sleeps while `budget->pressure(estimate)` holds, `estimate` being the sources' file bytes
(`index.hh:1095-1100`), and a buffer filling the budget keeps that true, so a background
merge starts 5 s late; the engine then waits for it at the end. At 5M a merge runs about
100 s beside indexing and the delay is hidden; at 100k the merges take tenths of a second
and the delay is the whole difference. The A/B therefore adds B0: background merges with
`STRATA_MERGE_MEMORY_WAIT_MS=0` (an engine knob for the experiment), which starts a merge
at once and lets it draw past the budget, as the index already does after the wait.
Same rules; B0's peak RSS is the axis to watch.

### E3 results at 100k (macOS x86_64, `2f54077` plus the wait knob)

Five interleaved rounds of A (inline merges), B (one merge thread), C (one merge thread,
`STRATA_MERGE_MEMORY_WAIT_MS=0`) and T (Tantivy's one-thread builder), 100k documents,
keyed, `STRATA_BUDGET_MB=41`, under a light hold (raw: `.scratch/e3/r100k/results.tsv`):

| Unit | Wall (five rounds) | Median | Peak RSS | Bytes | Segments |
|---|---|---:|---:|---:|---:|
| A | 10.10, 10.57, 11.40, 10.93, 10.24 | 10.57 s | 251 MiB | 47,950,319 | 8 |
| B | 9.82, 10.02, 11.28, 10.44, 9.86 | 10.02 s | 284 MiB | 45,356,349 | 3 |
| C | 10.43, 10.18, 10.90, 10.14, 9.67 | 10.18 s | 274 MiB | 45,356,349 | 3 |
| T | 6.33, 6.03, 6.18, 5.96, 5.36 | 6.03 s | 397 MiB | 47,919,012 | 1 |

B/A 0.948× [0.879–1.067], C/A 0.963× [0.889–1.032]: neither interval excludes 1.0, so at
100k background merges do not change the time; the merges here are tenths of a second.
B and C end with 3 segments (5.4% fewer bytes) where A ends with 8: with merges in flight
the policy sees other candidates, and the end-of-run loop merges what it asks for. Peak RSS
rises 13%. By its rule E3 is not kept as a change of default. The knob stays, default off,
as the mechanism E5's scheduling runs on. At 5M the two inline merges cost about 190 s of
the owner's time on this host (a 637 MB merge takes 55 s here), which is the effect the
100k sample cannot show.

## E4: MinHash order at flush, for size (the lead's request)

research/indexing's 5.1 measures MinHash k = 2 over every term at 0.966× postings plus
positions, writing each order with docids renumbered. A flush assigns docids at arrival, so
a flush in MinHash order also writes the permuted `docids` section (format 12's kind 2),
and its stored records lose curid order. E4 measures the whole segment, which is what the
architect rules on as a preset.

Hypothesis: the whole segment shrinks by less than postings plus positions do, because of
the `docids` section and the stored records; natural merges in docid order give the gain
back. Kept as evidence for the architect's ruling; nothing lands here (D15 says flushes keep
docid order).

Procedure (`.scratch/probe/minhash.cc`): a flushed segment's documents get research's
signature (`wordwise::hash` of each non-key term, the same mix and salts, k = 2,
lexicographic, ties by ordinal) from a pass over the segment's postings, and the segment is
rewritten in that order by `merge_segments` with docids kept, which writes what a flush in
that order writes (`merge_test`). P1's bisection, as the reference.

| Segment | Docid order | MinHash k = 2 | Ratio | Bisection (P1) | Ratio |
|---|---:|---:|---:|---:|---:|
| 100k, one segment: whole file | 43,493,556 | 42,619,808 | 0.980× | 41,861,564 | 0.962× |
| postings | 15,971,722 | 15,117,622 | 0.947× | 14,799,435 | 0.927× |
| positions | 22,446,072 | 21,955,115 | 0.978× | 21,408,316 | 0.954× |
| terms and term index | 4,309,207 | 4,253,385 | 0.987× | 4,334,449 | 1.006× |
| `docids` | 9 | 431,283 | | 431,283 | |
| stored | 316,696 | 413,447 | 1.305× | 439,843 | 1.389× |
| 271k flush (the 1M run's first): whole file | 140,707,252 | 138,128,220 | 0.982× | | |
| postings and positions | 129,169,008 | 125,047,993 | 0.968× | | |
| `docids`, stored | 9, 842,314 | 1,304,221, 1,143,957 | | | |

The order costs 280 ms per 100k documents from the segment's postings here (654 ms at
271k; research measures 48 ms from the tokens). The permuted `docids` section and the
stored records take 39% of the postings-and-positions gain at 100k and 36% at 271k.

## E5: merge scheduling for the copy merge (the lead's request)

research/indexing 5.3 (`mergecost.cc`): a two-segment merge costs 497 ns per posting today
and 24.6 ns at the copy floor (skip rewrites not modeled), which takes the 5M force merge
from 404 s on the gate VM to about 15 s. The copy merge needs format 13's items (a base
ordinal per term, tail-coded blocks anywhere, aligned positions; the lead forwards them to
the format 13 designer), so under format 12 every merge after a source's tail re-blocks.
What the schedule decides does not depend on the merge's speed: how many segments the run
ends with, the bytes, the write amplification, and which merges sit on the run's critical
path. So E5 builds the schedule now on today's merges, measures those, and projects the
time with the copy merge's cost.

The schedule, for T4 and for a host that commits rarely:

1. Natural merges by the tiered policy run in the background (E3's executor), so no merge
   sits on the owner thread.
2. The run ends as both baselines' runs end: one `force_merge(1)` in docid order
   (`STRATA_FINAL_MERGE=1`; Lucene's `forceMerge(1)` without BP, Tantivy's merge of every
   segment), then the commit and E2's reclaim. With the copy merge the final merge is about
   15 s at 5M, the only merge on the critical path.
3. Whether natural merges are worth their write amplification once the final merge exists
   is a second question: without them, T3's write amplification is 2.0 (each byte flushed
   once and merged once); with them it is higher, and the segment count during indexing
   (key lookups probe every segment's filter; refreshes search every segment) is what they
   buy. A schedule simulator over the run's real flush sizes answers it before a measured
   run: segments over time, write amplification, merge work today and with the copy merge.

Hypothesis: the run ends with one segment of the docid-order force merge's bytes
(2,770,003,548 at 5M, 0.982× Lucene's force-merged 2,819,677,322 and 0.903× Tantivy's
3,066,216,363); the final merge adds today's merge time (233 s at 5M on this host) and,
with the copy merge, about 15 s; peak RSS during the final merge is the risk (5.80 GB at
5M today, against T4's calibrated 4.6 GB).

Procedure: at 1M documents (`STRATA_BUDGET_MB=490`, 21 flushes), one run each of
(a) today's engine with E2, (b) background merges and the final merge, (c) no natural
merges (`STRATA_AUTO_MERGE=0`) and the final merge: segments, bytes, written bytes (this
host's file-system writes from `/usr/bin/time -l` block counts), peak RSS, and the final
merge's own time. Kept when (b) or (c) ends at one segment within 0.5% of the docid-order
force merge's bytes; the time is reported against the copy-merge projection, and becomes a
T4 row only once the copy merge lands.

## Findings for others (not changed on this branch)

For the layers refactor's owner (`index.hh`, `merge.hh`, rewritten on
phase4/layers-refactor):

- **IS-1, sources wait for a commit to be reclaimable.** `install_merge` drops merged-away
  sources from the index, but their files are queued only by `retain()` after the next
  commit (`index.hh:1869-1925`), and `drain()` frees only queued files
  (`index.hh:1366-1378`). A storage-budgeted index that ingests between commits can refuse
  a transaction with `no_space` while merged-away sources sit on disk (2.21 GB of them at 5M
  in E1). Fix: at install, queue each source that no retained manifest names.
- **IS-2, a merge whose estimate exceeds the budget waits for nothing.** `run_merge` sleeps
  while `budget->pressure(estimate)` holds, up to `merge_memory_wait` (5 s), with `estimate`
  the sources' file bytes (`index.hh:1095-1100`). When the estimate is above the budget's
  capacity (the final force merge at 5M: 2.8 GB against 2,056 MB), no flush can make room,
  so every such merge sleeps the full 5 s: 2% of today's 233 s final merge, a third of the
  copy merge's 15 s. Fix: skip the wait when the estimate exceeds the capacity, or when the
  index holds no buffer that a flush would release.
- **IS-3, the merge's own cost.** `merge_segments` builds a `std::vector` per posting for
  its positions and sorts each term's postings, which each source already holds in ordinal
  order (sampled on a 1M merge: about a quarter of the samples in `malloc` and `free`, 7% in
  the sort). It also holds a whole term's postings with positions, so its peak memory is
  the largest term's (5.80 GB peak RSS for the 5M force merge on this host, against T4's
  calibrated 4.6 GB). A streaming k-way merge writes the same bytes; the copy merge
  (research/indexing 5.3) replaces both for append-only sources.

For the lead (T4 and T3 methodology):

- **IS-4, T4 compares different semantics.** n1017's baselines force-merge to one segment
  inside the timed run (`BuildIndex.java`'s `forceMerge(1)`, `build_index.rs`'s merge of
  every segment; `NO_MERGE` unset), while strata's `index` leaves the policy's segments:
  T4's bytes compare strata's "before merging" with the baselines' "after merging"
  (BENCHMARKS.md defines both), and T4's times include the baselines' force merges only.
  Either every engine runs `NO_MERGE=1`, or strata runs with `STRATA_FINAL_MERGE=1` (E5).
- **IS-6, for perf/indexing-gap: inline merges are about a third of the one-thread run at
  5M.** E5's 1M runs on this host: inline natural merges took about 51 s of a 189 s run
  (27%; (a) against (c)'s 138 s of indexing before its 44 s final merge). At 5M the two
  natural merges write about 2.2 GB (E1), about 208 s here, and the gate VM merges 1.73×
  slower (404 s against 233 s for the same force merge), so about 360 s of n1017's 1,091 s.
  indexing-gap's E0 ran at 100k, where no merge runs, so its profile cannot show this share.
  Background merges (E3) take it off the owner thread; the copy merge takes most of it away.
- **IS-5, strata's T3 denominator held garbage.** `write_amp.sh` divides the device's
  written bytes by the directory's final bytes, which for strata included the unreclaimed
  sources (IS-1, H2): at 5M n1017's run wrote 6.09 GB for an index of about 2.82 GB (2.16)
  but its directory said 4.85 GB (1.26). E2 makes the directory the index.

E4 at 5M: the docid-order force merge of E1's index (one segment, 5,032,104 documents)
rewritten in MinHash order, against P1's bisection of the same corpus on the gate VM
(`phase-3-ratchets.md`, at `4594cce`):

| Section | Docid order | MinHash k = 2 | Ratio | Bisection (P1) | Ratio |
|---|---:|---:|---:|---:|---:|
| Whole file | 2,770,003,548 | 2,697,465,708 | 0.974× | 2,539,274,828 | 0.917× |
| Postings | 1,007,889,746 | 937,979,415 | 0.931× | 845,553,044 | 0.839× |
| Positions | 1,631,157,618 | 1,594,028,517 | 0.977× | 1,525,049,214 | 0.935× |
| Terms and term index | 91,997,951 | 91,300,852 | 0.992× | 91,243,936 | 0.992× |
| `docids` | 9 | 29,249,137 | | 29,249,137 | |
| Stored | 15,489,384 | 21,509,721 | 1.389× | 24,935,785 | 1.610× |

The order took 15.9 s from the postings (one thread, `nice 19`; research's token-side
signature is cheaper); bisection takes 846 s on 8 threads and 2,678 s on one (T3's row). The
gain grows with the segment: 2.0% of the whole at 100k, 1.8% at a 271k flush, 2.6% at 5M.
Against the baselines at 5M one segment: MinHash 0.957× keyed Lucene's force merge,
1.061× keyed `-bp`.

For the architect's ruling, what the numbers say about a flush-time preset:

1. The whole-segment gain is about 60% of the postings-and-positions gain research reports;
   the permuted `docids` section (about 5.8 bytes a document) and the stored records'
   compression (+30% to +39%) take the rest.
2. A natural merge in docid order (`merge_segments` without an order) puts the documents
   back in docid order and gives the gain up: at T4's 5M run 80% of the documents sit in
   merged segments. A flush-time order is worth its `docids` bytes only if merges keep
   their sources' orders (research 5.3's item 4: the copy merge concatenates them) or order
   their outputs again.
3. Every MinHash-ordered segment is permuted, so the docid-ordered tie skip (D5) stops
   applying to it, as for P1's reordered segments.

### E5 results at 1M (macOS x86_64, `40f41df`, one run each)

1,000,000 documents, keyed, `STRATA_BUDGET_MB=490` (21 flushes), `nice 19` under other light
holds. Written bytes are the final index plus what the engine reclaimed plus the commit's
slice (about 64 MiB), so write amplification is approximate; the directory always equals
the manifest's files.

| Schedule | Segments | Bytes | Reclaimed | Write amplification | Final merge | Wall | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| (a) Inline tiered merges (E2 only) | 5 | 637,293,500 | 511,283,280 | about 1.91 | | 189 s | 1.88 GB |
| (b) Background tiered + final merge | 1 | 620,281,976 | 1,138,985,320 | about 2.94 | 36.5 s | 197 s | 2.38 GB |
| (c) No natural merges + final merge | 1 | 620,281,976 | 603,341,508 | about 2.08 | 44.2 s | 182 s | 2.47 GB |
| (d) Inline tiered + final merge | 1 | 620,281,976 | 1,148,576,528 | about 2.96 | 28.3 s | 195 s | 2.36 GB |

1. Every schedule with the final merge ends at one segment of the docid-order force
   merge's bytes, 2.7% under (a)'s five segments (1.95% at 5M, E1). E5 is kept as the
   schedule; its default in the T4 run is the lead's call (IS-4).
2. (c) does the least merge work: each byte is flushed once and merged once (write
   amplification about 2.08, Tantivy's T4 2.0, Lucene's 3.0), and today it is not slower
   than (a): one final merge costs about what the natural merges cost (a's two merges wrote
   about 540 MB). Single runs on a shared host: a direction, not a timed row.
3. The final merge is the run's critical path: 44 s here today, about 2 to 3 s at the copy
   merge's floor (research 5.3's 24.6 ns per posting over 1M documents' 117M postings), and
   about 15 s at 5M against today's 233 s on this host and 404 s on the gate VM.
4. The cost today is memory: the final merge raises peak RSS by 31% at 1M, and to 5.80 GB
   at 5M against T4's calibrated 4.6 GB, so with today's merge (c) would fail T4's memory row.
   That is IS-3's whole-term buffering; the copy merge's working set is a block.
5. Natural merges buy a bounded segment count while indexing (refreshes, key lookups), at
   the write amplification in the table. With the copy merge they are cheap enough for a
   lower merge factor; E6 measures the segment count over the run against write
   amplification for segments per tier of 8, 4, and 2.

The recommendation for the schedule, once the copy merge lands: batch builds (T4) run (c),
no natural merges and one final copy merge; indexes with readers during ingestion run the
tiered policy in the background (E3) at the merge factor E6 picks, and a host's explicit
`force_merge` takes the copy path. Until then, (c) costs T4's memory row and (a) is today's.

## E6: the merge factor for natural merges

Change (`tools/strata_engine.cc`): `STRATA_SEGMENTS_PER_TIER` sets the tiered policy's
segments per tier (the merge factor follows it), and `STRATA_LOG_SEGMENTS=1` adds the
segment count to each 100,000-document progress line.

Hypothesis: halving the merge factor roughly halves the segments held during the run and
adds about one pass of write amplification per halving; the final bytes fall with the
segment count. The copy merge makes the extra passes cheap in time; their bytes written
remain (T3's row).

Procedure: 1M documents, keyed, `STRATA_BUDGET_MB=490`, inline merges, no final merge, one
run each at 8 (today), 4, and 2: the segment count after every 100k documents, the final
segments and bytes, write amplification as in E5, wall time and peak RSS (single runs, a
direction). Kept as the curve E5's recommendation reads; nothing changes a default.

## E7: merges of adjacent segments, the copy merge's schedule

A copy merge concatenates its sources' ordinals, so its output is in docid order only when
its sources are adjacent: their docid ranges disjoint and consecutive (research 5.3, item 4).
The tiered policy picks by size, not adjacency. At 5M (E1) it merged segments 1, 3, 4, and 6
to 10, leaving 2 and 5 between them, so a copy of that merge would need the permuted
`docids` section (about 5.8 bytes a document, E4) or a re-encode, and every later merge with
2 or 5 interleaves too. Lucene's `LogMergePolicy` merges only adjacent segments for this
reason (its doc ids are positional); its `TieredMergePolicy` gives that up.

Change: `merge_policy::adjacent` (off by default) makes `find_merges` and
`find_forced_merges` consider only runs of segments consecutive in docid order, scored as
today (skew, size, live ratio, the balance rule); candidates carry their segment's
`min_docid` and `max_docid` (one line in `index.hh`'s `run_merges`), and the engine sets it
with `STRATA_ADJACENT_MERGES=1`. Every merge then keeps docid order, so the copy merge never
needs a permutation, and a flush-time order (E4) concatenates through it.

Hypothesis: with adjacency the segment count and write amplification stay within 10% of
the tiered policy's on append-only and replace workloads (`merge_policy_test`'s simulations
with the adjacency invariant checked after every merge), and the 1M run's bytes within 1%.
Kept as the copy merge's schedule if so; removed if adjacency costs more than 10% of
either.

### E7 results: the policy simulations (`merge_policy_test`, `e35acaa`)

The test's workloads with the default policy (8 per tier, 16 MiB floor, 5 GiB maximum),
both modes over the same flushes; every adjacent merge checked consecutive by docid, and
the budget checked among docid neighbors:

| Workload | Tiered: segments (most), write amplification | Adjacent: segments (most), write amplification |
|---|---|---|
| Gate-shaped: 21 flushes of 70 to 210 MB, 5,032,104 documents | 9 (10), 1.69 | 9 (10), 1.67 |
| Append-only, small flushes (12 to 38 MB), 20M documents | 40 (48), 3.74 | 48 (50), 3.70 |
| Updates: 90% replaces by the end, 1M documents | 15 (20), 4.20 | 20 (20), 4.16 |

At T4's shape adjacency costs nothing. With many small flushes it ends with 20% more
segments (and 4% more at the peak) for 1% less writing; with replaces it ends with 33%
more, the peak unchanged. The tiered path's own output is identical to main's (the test's
five tiered lines diffed against main's build). The full suite passes (33 tests, Release,
macOS x86_64).

### E6 and E7 at 1M (macOS x86_64, `e35acaa`, one run each)

1,000,000 documents, keyed, `STRATA_BUDGET_MB=490` (21 flushes), inline merges, no final
merge, `STRATA_LOG_SEGMENTS=1`. Write amplification as in E5.

| Policy | Segments after each 100k | Final | Bytes | Reclaimed | Write amplification | Wall |
|---|---|---:|---:|---:|---:|---:|
| Tiered, 8 per tier (E5's a) | not logged | 5 | 637,293,500 | 511,283,280 | about 1.91 | 189 s |
| Adjacent, 8 | 1 2 5 6 9 4 6 7 8 5 | 6 | 639,380,726 | 474,887,916 | about 1.85 | 159 s |
| Tiered, 4 | 1 2 5 3 6 5 7 5 6 6 | 7 | 647,206,936 | 510,194,788 | about 1.90 | 172 s |
| Adjacent, 4 | 1 2 5 3 6 5 7 5 6 6 | 7 | 646,406,924 | 515,904,296 | about 1.90 | 150 s |
| Tiered, 2 | 1 2 4 5 5 6 6 7 6 7 | 8 | 650,320,111 | 931,794,700 | about 2.54 | 174 s |
| Adjacent, 2 | 1 2 4 5 5 6 6 7 7 7 | 8 | 649,393,594 | 940,648,932 | about 2.55 | 170 s |

**E6 is refuted.** A lower merge factor does not cut the segments held: the tier budget
allows about as many segments at 2, 4, or 8 per tier (more tiers of fewer each), so the
peak falls only from 9 to 7 and the final count rises from 5 to 8; 2 per tier adds 0.6 of
write amplification for nothing. The default, 8, stays; the knob stays for the record.

**E7, against its rule:** at 1M adjacency stays within 0.33% of the tiered bytes and 3% of
its write amplification at every merge factor, and its segment counts match at 4 and 2 (one
more at 8). In the simulations it matches at T4's shape, and ends with 20% (small flushes)
and 33% (updates) more segments with the peak within 4%. The rule asked for segment counts
within 10%, so on final counts it misses on two workloads of three; on peaks it passes.
What adjacency buys is the copy merge everywhere: without it a copy of a non-adjacent merge
writes the permuted `docids` section (about 5.8 bytes a document) and loses the docid-ordered
tie skip, or re-encodes. That trade belongs to the copy merge's review. The mode stays
opt-in (off by default), and the recommendation is to make it the default together with the
copy merge, in batch schedules where E5's final merge makes the final count 1 anyway.

## E3b: background merges at 1M, where merges are a quarter of the run

E3's 100k sample holds little merge work (floor-sized merges of 3.5 MB flushes), while at
1M inline natural merges are 27% of the run (IS-6) and at 5M about a third. The question E3
asked, whether background merges take that time off the owner thread, is answered only
where the merges are that share, so E3b runs at 1M: a deviation from the 100k rule for
timing A/Bs, stated here and in the ledger, as the one larger run the rule allows.

Procedure: 1,000,000 documents, keyed, `STRATA_BUDGET_MB=490` (21 flushes), A inline
(`STRATA_MERGE_THREADS=0`) and B background (1 thread), with Tantivy's one-thread builder as
the anchor, five interleaved rounds under a light hold; wall, peak RSS, bytes, segments.

Keep if B/A's 95% interval's upper bound is below 0.97 (a 3% effect), B's bytes within 2%
of A's, and B's peak RSS within 10% of A's; otherwise remove the knob, and the finding goes
to the copy merge (which removes most of the merge work instead of moving it).

### IS-4 measured: the baselines before merging (5M, macOS x86_64)

Each baseline's T4 builder with `KEYED=1` and `NO_MERGE=1` (its own background merges, no
force merge), at its T4 settings (Lucene's 2,047 MB buffer, Tantivy's 2 GB heap), the full
corpus, `nice 19` under a light hold. The force-merged figures are n1017's.

| Engine | Before merging: segments, bytes | Force-merged: bytes | strata (E2) before merging against it | strata force-merged (docid order) against it |
|---|---|---:|---:|---:|
| strata | 7, 2,824,094,984 | 2,770,003,548 | | |
| Lucene 10.4.0 | 3, 2,829,753,418 | 2,819,677,322 | 0.998× | 0.982× |
| Tantivy 0.26 | 3, 3,095,480,261 | 3,066,216,363 | 0.912× | 0.903× |

With E2, strata's T4 bytes are below both baselines like for like, before merging and after.
Without it they were 1.78× Lucene's before-merging bytes. The baselines end with 3 segments
because their buffers hold inverted postings (about 1.5M documents in Lucene's 2 GB here)
where strata's holds un-inverted documents (21 flushes of 186k to 286k), research/indexing's
compact-buffer finding seen from the disk side: fewer flushes would cut strata's segment
overhead (1.95% at 5M) without any merge. Informational times on this host: Lucene 540 s,
Tantivy 320 s, peak RSS 4.26 and 9.59 GB.

## E8: the eight-thread bytes with E2 (measurement)

n1017's eight-thread run left 8.02 GB in 14 segments. A flush at `STRATA_THREADS=8` splits
the buffer into eight docid runs, one segment each (D18 as built), so eight threads flush
eight times as many segments and the policy merges more of them; with one commit, more
merged-away sources stay on disk. Hypothesis: with E2 the eight-thread bytes fall to the
manifest's segments, within a few percent of the one-thread index's (more segments, more
duplicated dictionaries), and the unreclaimed share is larger than at one thread.

Procedure: 1M documents, keyed, `STRATA_BUDGET_MB=490` and 2056, `STRATA_THREADS=8`, one run
each with E2's engine: segments, bytes, reclaimed bytes; against the one-thread runs at the
same budgets (E5's a and E1's 1M). Informational bytes under a light hold, after E3b's
timing. Segment count per flush at eight threads is perf/indexing-gap's (its E2, one segment
per flush); this records only what it costs on disk.
