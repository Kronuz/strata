# lanebuf (write-buffer bake-off, step 0)

- Task: build and run the bake-off prototype of `docs/design/write-buffer.md`; record in
  `docs/perf/write-buffer-bakeoff.md`.
- Branch / worktree: `perf/lanebuf` from `design/write-modes` at `acdc908`,
  `/Users/kronuz/code/Kronuz/strata-wt/lanebuf`. Not merged, not pushed.
- State: paused at the coordinator's instruction. The architect rejected the design for now
  (`docs/reviews/write-buffer-design.md` in `strata-wt/write-modes`); condition C1 lets step 0
  continue only once the prototype builds the fixes below. Nothing is timed; no strict hold taken.
  A design revision is coming; resume from it.
- Owner's answers folded in: Q1 (visible once acknowledged; no log here, so at group end), Q2,
  Q3 (1.5x requests/s or 0.5x p99, at most 3% at one writer, at most 5% CPU per request).

## Done

- `7a6d1b9` plan and ledger.
- `523e677` the prototype: `bench/kv/proto/lanebuf.hh` (C, A', F from one source), `lanebuf.cc`
  (driver, freeze sweep, model check, the architect's two cases), `lanebuf_work.hh` (T1, T2/T3,
  T11 loops, engine-agnostic). Compiles with Apple clang and passes the model check natively at
  4 writers and 4 readers with freezes and flushes running, for C, A', and F (80,000
  transactions each, 0 mismatches; verdicts, docids, versions equal a model and a one-thread
  replay of the log order; scans and held-view reads equal the model at their position).
- The architect's cases (`lanebuf cases --impl c|a|f`):
  - WB-1 torn transfer: passes (the prototype's `finish_scope` re-reads V when the view's
    freeze position is past it, which is the `max(V, view.position)` rule); fails with
    `-DLANEBUF_MUTANT=8` (read at V): `a=None b=None sum=0`, the architect's number.
  - WB-2 delete resurrected across a merge install: FAILS on all three, as intended: the
    pending delete naming a merged-away source is dropped at the freeze (`lost_pending_deletes`
    1), two live copies of the key exist after the flush, and the deleted key reads back as its
    stale value.

## What the prototype still needs (from the review)

1. WB-2: a merge's install (and heal/install) must carry or apply the pending deletes naming its
   sources. Not built; the case above is the test.
2. WB-3: the prototype copies a stale pre-staged record into the leader's lane at link time
   (`restage`, the review's "simpler" fix) and marks the old copy "never"; the random check
   exercises it (hundreds to thousands of restages per run) but has no schedule that freezes
   exactly while a writer pre-stages, and T8's TSan run is still owed (build queued behind other
   agents' light holds).
3. WB-4: no per-buffer position index. Runs, tails, and the flush walk each lane up to a
   published end mark and filter by the atomic position. Needs the index (and a retire path for
   run pairs a reader installs: the prototype pushes them to a shared lock-free list).
4. WB-9: record position is `std::atomic<u32>` (done); the "superseded" flag does not exist. T10's
   relaxed-slot mutant cannot fail natively on x86_64; TSan and arm64 are its killers.
5. WB-8 / C1: readers are protected by a process-wide epoch domain, not the hazard registry
   (`hazard.hh` with SR-D18's fix): must change before T2 is timed. Segments are stubs (sorted
   `std::vector<std::string>`), not the real key filter and dictionary. The lead must set, before
   the first timed run: the outcome when both finalists miss, an A/A spread for the 3% and 5%
   bars, the T3 clause (p99.9, maximum, run-build distribution), and T1's one-writer column as
   informational. T7 and T4's A' target also need a decision.
6. WB-6: V is stored at group end (durability none); the contiguous acknowledged prefix with one
   owner is not modeled.
7. Version semantics: a delete keeps its version and a re-add continues it (segments here keep
   tombstones so flush timing does not change versions). The engine's segments hold none; the
   design must say which rule it means (the `cases` WB-2 run drops tombstones on flush to match).

## Next

1. TSan model check (build queued as `lb-tsan-build`), then the read-at-V and the other mutants
   (1, 2, 3, 4, 5, 7) against the check.
2. Resume from the design revision: hazard registry, position index, real segments, RocksDB
   adapter (`lanebuf_rocksdb.cc`), `run.py` entries, CMake target, then the timed pieces under
   strict holds once the lead has set WB-8's rules.

## Blocked on

The design revision (architect's C1); the lead's WB-8 rules before any timed run; the owner's
decision on tombstone/version semantics (item 7).
