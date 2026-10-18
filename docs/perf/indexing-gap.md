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

## Calibration (100k sample)

PLAN, "Calibrate once, then only the fastest": one run of strata, Tantivy, and Lucene at
100,000 documents, keyed, at one thread (`-st` engines) and at eight (the multi-threaded
builders; strata with `STRATA_THREADS=8`), after a 20,000-document dry run. The faster
baseline at each thread count is the anchor every A/B runs beside; a fix that wins clearly
at 100k gets one larger confirmation against the anchor only, the candidate T4 row.

## Experiments

Each is written here before its A/B runs: the hypothesis, the procedure, and the rule
that keeps or removes it.
