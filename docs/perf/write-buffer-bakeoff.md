# Write-buffer bake-off (step 0 of `docs/design/write-buffer.md`)

Branch `perf/lanebuf` from `design/write-modes` (`acdc908`). Prototype: `bench/kv/proto/lanebuf.hh`,
`lanebuf.cc` (the three candidates from one source), `lanebuf_work.hh` (workloads shared with the
RocksDB adapter `lanebuf_rocksdb.cc`). Benchmark code only; nothing outside `bench/kv/proto` and the
`STRATA_KV_PROTO` block of `CMakeLists.txt` changes.

## Owner's answers (2026-10-21, folded in before the run)

1. Q1: a reader sees a write only once it is acknowledged at the index's durability level. The
   prototype has no log (durability `none`), so a write is acknowledged and visible when its group's
   stage 1 ends; V is stored then.
2. Q2: search reads at a freeze (coalesced); gets and scans read live. The prototype measures gets and
   scans only.
3. Q3 (stricter than the designer's): keep the multi-writer mode for Xapiand only if, on the
   Xapiand-shaped load (T11), it gives **at least 1.5x the fallback's requests per second or at most
   0.5x its request p99**, while costing **at most 3% at one writer** (T1) and **at most 5% more CPU
   per request**.

## Decision rules (the lead, 2026-10-21; review C1 and WB-8)

Set before the first timed run; the results apply them, they don't revise them.

1. **Noise.** Every comparison runs A/A first. A difference counts only when it exceeds that
   row's A/A spread; the 3% single-writer bar (Q3) is met only if C's single-writer cost, minus
   the spread, stays under 3%.
2. **C against A'.** Pick the candidate that wins more of T1 (writers 1/2/4/8), T2/T3 (read and
   scan p99 while writing, including C's main-run merges in the p99), T6 (bytes per record) and
   freeze cost, with no row worse than 1.10x the other. A tie on that count goes to C (less
   memory, simpler freeze).
3. **Xapiand's mode (Q3, on T11a, the journaled path).** Multi-writer only if it reaches at least
   1.5x the requests per second or at most 0.5x the request p99 against one writer with live
   readers, costs at most 3% with one writer and at most 5% more CPU per request; else the
   fallback.
4. **Both finalists miss.** If neither C nor A' beats today's single-writer buffer by more than
   the A/A spread on T1 at 4 writers, record that, ship the fallback for Xapiand, and keep the
   prototype for unjournaled embedders only after a design revision.
5. **What isn't decided here.** T7 (the +1 KiB target) needs the engine build; the prototype's
   segments must be real (not stubs) before T11a's per-segment lookup cost counts.

## Candidates, one source

| Name | Writers | Order for scans |
|---|---|---|
| C | write group (queue, leader runs stage 1), per-writer lanes | shared sorted runs (main + delta + private tail) |
| A' | same | concurrent skip list, tower inside the record, links by the writers, V after the last link |
| F (fallback, D14) | exclusive lease per transaction, one lane | shared sorted runs |

Common: lanes with 32-bit references, one hash version map (8-byte atomic slots, 7/8 full, no
erase, growth with epoch-stamped retirement), leader-filled 24-byte headers, tombstones and `prev`,
the visible position V, freeze, a minimal view with frozen buffers and stub segments.

## Hypotheses and keep/remove rules

H1 (C vs A'): C's single-writer insert is cheaper than A' (no tower links) and A' wins only on scans
beside writers. Rule from the design: a finalist that misses T1, T2, T4, or T7 is out; A' beats C
only if its T3 p99 is more than 20% better at 4 writers while C misses T3, and A' stays within 10%
of C at one writer.

H2 (multi-writer vs fallback): on request-shaped load over many small indexes the leader handoff
costs more than the lease serialization saves. Kept only if Q3's bar above is cleared.

Tuning experiments (at most three, each one hypothesis, run after the candidates): chosen from what
the data shows; recorded below with the numbers whether they win or lose.

## Procedure

Interleaved trials (at least five, spread stated), strict holds in pieces of at most 20 minutes
(`quiet run bakeoff-<n> 20 ...`), builds as light holds at 4 jobs. RocksDB 11.11.1 through the same
workload code (`lanebuf_rocksdb.cc`) with memtables large enough that nothing flushes, WAL off,
`allow_concurrent_memtable_write` at its default, and through the kv harness rows (fits1, fits4,
fits8) for context. Host: Intel i9-9980HK, 8 cores / 16 threads, macOS x86_64 only; the other
platforms are not measured (caveat).

## Status (2026-10-21): paused, nothing timed

The architect rejected the design for now (`docs/reviews/write-buffer-design.md` on
`design/write-modes`); condition C1 lets step 0 continue only with WB-1/3/4/9 built, the hazard
registry instead of the epoch domain, and WB-8's rules set before the first timed run. The lead
holds all timing until the design revision. Functional results only, on the macOS x86_64 host:

| Check | Result |
|---|---|
| Model check, native, 4 writers + 4 readers + freezes/flushes, 60,000 to 80,000 transactions | C, A', F: 0 mismatches against a model and a one-thread replay of the log order |
| Model check under TSan, 3 + 3, 7,500 transactions | C, A', F: 0 warnings (TSan does not model the epoch fences) |
| Mutant 3 (undo without "never"), 4 (V early), 5 (pre-resolution across a generation), 7 (retired table freed at once) | killed: model mismatches (3, 4, 5), crash (7) |
| Mutant 1 (slot before header), 2 (relaxed slot) | not killed natively on x86_64 (TSO); killed by TSan (data races reported), as the review predicted |
| Mutant 8 (read at V, WB-1) | killed by the deterministic case; the random check killed it on C in one run, not on A' or F |
| Case WB-1, torn transfer | passes (`finish_scope` re-reads V when the view's position is past it); fails with mutant 8 (`a=None b=None sum=0`) |
| Case WB-2, delete across a merge install | FAILS on C, A', F: pending delete dropped, 2 live copies, stale value read after the delete |

What the prototype still needs is listed in `docs/status/lanebuf.md`.

## Results

(timed pieces follow the design revision)
