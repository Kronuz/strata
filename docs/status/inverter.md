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
  - E3, the research note's inverter C as the active buffer (`tuple_buffer.hh`):
    byte-identical; 1.214x E2 (interval 1.156-1.459, short of its 1.3x rule, kept on the
    lead's direction), 1.544x Tantivy at 100,000 documents; 4.5x is 2.91x away. Peak RSS
    1,229 MB against E2's 447.
  - E4, the encoder's per-term waste (kept by BENCHMARKS rule 3): byte-identical; 1.095x E3
    (interval 1.036-1.202), 1.710x Tantivy at 100,000 documents; 4.5x is 2.63x away.
  - E5 (`bb7c9b0`), unresolved: 17 pooled rounds 1.066x E4 (interval 0.985-1.130); the
    profile shows the grouping and the term sort about 180 ms smaller. Kept provisionally;
    a gate-VM rerun is owed.
  - E7 (`f6b06a1`), the engine's front end: 1.043x E5 (interval 1.024-1.075), 1.875x
    Tantivy.
  - E9, C in batches into a dense term store: byte-identical; 1.143x E7 (interval
    1.002-1.586) at 57% less peak RSS (588 MB); 1.975x Tantivy; 4.5x is 2.28x away.
  - E10 (small terms as bytes) and E15 (decode loops, chains fetched ahead) removed, no win.
  - Linux (gate VM, 100,000 documents): E9 is 2.272x Tantivy (interval 2.187-2.331), 4.27x
    the base; 4.5x is 1.98x away.
  - E18 (term-table probes fetched ahead, the version map by view, gathered section
    writes): 1.055x E9 (interval 1.028-1.066), 1.943x Tantivy on macOS.
  - E17 (PFOR blocks in the store) removed: no change on either platform.
  - E19, sorted runs instead of a term table: byte-identical; 1.075x E18 on the gate VM
    (interval 1.030-1.100), 1.044x on macOS; 2.487x Tantivy on Linux, 2.029x on macOS;
    4.5x is 1.81x away on Linux.
  - 1M on the gate VM: E19 2.672x Tantivy (41.8 against 111.8 s), one segment.
  - E11, the flush writes its segment to the file as it goes: peak RSS -21% at 1M
    (3,832 to 3,030 MB) and -20% at 100,000, time unchanged.
- Doing: the key-range interface for perf/indexing-gap's parallel flush (lead's direction,
  2026-10-18), then compact run-entry headers (memory), then the front end.
- Next:
  1. Linux, owed for every kept item: E0's profile (`perf record -g`), the 100,000-document
     calibration, and each A/B in the gate VM; it needs VM time between the indexing gap's
     holds (the lead grants).
  2. The engine's bookkeeping: line reads, `"Q" + id`, `key_of`, `lookup_key`, the undo log,
     the string-keyed version map.
  3. D (the note's refinement): tuples into 256 bucket streams on arrival, so the flush has
     no partition pass and no partition copy (368 MB at 100,000 documents); each partition's
     terms sorted alone and merged.
  4. Memory: a batched C, grouping every few thousand documents into dense per-term
     postings, so the buffer holds postings and the tuples stay cache-sized (E2's density
     with C's per-token cost); the 5M equal-memory row decides it.
- Coordination: perf/indexing-gap merges this branch after each kept item (lead,
  2026-10-18): this branch owns `tuple_buffer.hh` and the one-thread flush, perf/indexing-gap
  the parallel flush and the assembly; boundary changes go under "Interface with
  perf/indexing-gap" in the perf file and in the commit message. `perf/indexing-gap`'s E1 (`934811b`) pipelines the engine's parse at more
  than one thread and calls `tokens_document`, whose body E1 here changed; the two compose.
  E2 moves inversion to stage 2 at one thread; K inversion buffers on threads (D18) are the
  indexing gap's. `phase4/layers-refactor` rewrites `writer.hh` and `index.hh`; these
  changes rebase onto it by the lead.
- Blocked on: nothing.
- Process: E4's build and tests (21:35-21:47) overlapped `perf/indexing-gap`'s strict hold
  `ig-e2`; its eight-thread A/B from that window may need a rerun (the lead decides).
  Every step now runs behind `.scratch/gate.sh`.
- Findings folded in: none yet.
- Last verified: 2026-10-17, `strata_engine` builds at `4c9ebfd` (Apple clang 17, Release,
  `-march=native -g -fno-omit-frame-pointer`).
