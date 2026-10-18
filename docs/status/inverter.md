# inverter

- Design: none (a Track P item on T4's one-thread row); experiments in
  `docs/perf/inverter.md`, each written before it runs. Owner direction (2026-10-17, through
  the lead): for every cost, try at least one mechanism neither Lucene nor Tantivy uses
  (sort-based inversion, batch SIMD analysis, arena layouts that make flush a copy), and
  build the winners of the research note `docs/research/indexing.md` (branch
  `research/indexing`) as it lands.
- Branch / worktree: `perf/inverter` at `/Users/kronuz/code/Kronuz/strata-wt/inverter`, from
  `origin/main` at `4c9ebfd`.
- Scope: the one-thread document path (the engine's parse and tokenize, stage 1, the
  buffer) and the flush's inversion and encoding. Out of scope: eight-thread scaling
  (`perf/indexing-gap`), index size and merging (`perf/index-size`).
- State: implementing (E3, the tuple-stream inverter, the research note's C).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Worktree, Release build with frame pointers (`.scratch/build-rel`), the first 100,000
    documents of the gate corpus copied from the gate VM (`.scratch/corpus`, sha256
    `0a1d5344…acebd`, equal to the VM's `head -100000 corpus.json`).
  - E0 on macOS (`0d8a98f`): per-document allocation, copying, and grouping hold 80% of the
    one-thread samples at 100,000 documents; calibration Tantivy 12.10 s, strata 21.88 s
    (0.544x).
  - E1, documents as token streams (`d5468e4`, kept): byte-identical segments; 2.12x faster
    than `4c9ebfd`; 1.20x Tantivy (interval 1.15-1.26) at 100,000 documents on macOS; 4.5x
    is 3.74x away. CI run 39093822529 (dispatched).
  - E2, inversion on arrival (kept as E3's scaffold): byte-identical segments; 1.077x E1
    (interval 1.034-1.131) at 45% less peak RSS; 1.26x Tantivy; 4.5x is 3.57x away.
- Doing: E3, the research note's inverter C in the index path (lead's direction,
  2026-10-17): tuples on arrival, the partitioned flush, E2's dead and order handling as a
  relabelling; then the encoder's per-term cost (1.7 us a term to about 0.4).
- Next:
  1. E0 on Linux in the gate VM (`perf record -g`) when the VM is free; calibrate Tantivy and
     strata at 100,000 documents there.
  2. E4, the encoder's per-term cost (`flush_from`, `write_postings`, `competitive`).
  3. The engine's bookkeeping: line reads, `"Q" + id`, `key_of`, `lookup_key`, the undo log,
     the string-keyed version map.
- Coordination: `perf/indexing-gap`'s E1 (`934811b`) pipelines the engine's parse at more
  than one thread and calls `tokens_document`, whose body E1 here changed; the two compose.
  E2 moves inversion to stage 2 at one thread; K inversion buffers on threads (D18) are the
  indexing gap's. `phase4/layers-refactor` rewrites `writer.hh` and `index.hh`; these
  changes rebase onto it by the lead.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-17, `strata_engine` builds at `4c9ebfd` (Apple clang 17, Release,
  `-march=native -g -fno-omit-frame-pointer`).
