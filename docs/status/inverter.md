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
- State: implementing. The kept series is E23, E24, E25, E26, E31, E32 (`c62c902`), each a
  win on one platform without a loss on the other; E27 to E30 are removed with their numbers.
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
  - The key-range interface for perf/indexing-gap (`339acf7`): drain_for_flush, plan,
    add_documents, alphabet, splitters, each_term.
  - E20, compact run entries: peak RSS -15% at 1M (3,031 to 2,580 MB), time unchanged.
  - The digest's scan without a map (`lifecycle_test` 65 to 59 s in Release; the sanitizer
    job's lifecycle runs had doubled since E1).
  - E21, the front end without zeroing or a branch per check: 1.052x on the gate VM
    (interval 1.012-1.144), 2.613x Tantivy there; 4.5x is 1.72x away on Linux.
  - 5M confirmation on the gate VM (one round each, host loaded): strata E21 279.0 s,
    Tantivy 707.2 s: 2.535x; peak RSS 3.70 GB against 11.64 GB; 2.80 GB in 3 segments
    against 3.07 GB; 4.5x is 1.78x away.
  - From E23 on, gate-VM A/Bs read a warm corpus and write indexes to tmpfs (the Mac's disk
    writes at 12 to 22 MB/s; `docs/perf/inverter.md`, "Measurement conditions").
  - E23 (`097d33f`), entries encoded where their tuples are: 1.035x and 1.056x E21
    (1.011-1.076) on the gate VM, 1.023x on macOS; kept.
  - E24 (`a4f1b40`), the positions spill in chunks: peak RSS -8% on macOS at 100,000, -2.3%
    at 1M on the VM, time unchanged; kept (first removed on the VM's numbers alone).
  - E25 (`147be0e`), the merge keyed by 8-byte prefixes: 1.038x on macOS (1.022-1.064),
    1.02x and 1.03x on the VM; kept.
  - E26 (`297464f`), run entries as one stream: 1.041x (1.014-1.115) and 1.035x on the VM,
    1.018x on macOS, -2.1% peak RSS at 1M; kept.
  - E27, E28, E29, E30 removed (`7bd8b67`, `deb2ed2`, `9a384c0`, `b024a86`): no win by rule 3
    on either platform.
  - UBSan's null `memcpy` in E23's scratch fixed (`ff36290`, CI run 39118457889).
  - E31 (`b7dd0f4`), the text split inside the buffer (`document::split_text`): 1.079x on
    macOS (1.069-1.091), peak RSS -7%.
  - E32 (`c62c902`), the version map as one table of buffer indexes: 1.040x on macOS
    (1.016-1.064), peak RSS -9%.
  - At E32 (`c62c902`), on the gate VM: 3.339x Tantivy at 100,000 (3.257-3.603; E31 1.083x,
    E32 1.084x there, both wins) at 209 MB peak RSS against Tantivy's 291, and 3.470x at 1M
    (3.228-4.418) at 1,888 MB against 3,247; on macOS 2.707x at 100,000 (2.660-2.851). 4.5x
    is 1.35x away at 100,000 and 1.30x at 1M on the gate VM. THP through glibc's tunable: 1.064x (0.859-1.126) with
    compaction stalls and +22% RSS, not pursued.
- Doing: profiling E32 (macOS phase timers, a gate-VM perf profile) to size the next item;
  a 5M confirmation when the VM is quiet.
- Next:
  1. `document::split_text` (E31) is an additive public field: the architect's review
     before it lands (layers' L10 puts it in the inverted plugin's part).
  2. The drain's grouping (0.42 s) and encoding (0.52 s) at 100,000 on the VM, the largest
     phases left (phase timers, `docs/perf/inverter.md`).
  3. The flush's long lists hold 81.5% of the postings (E10); their decode and PFOR encode
     is the flush's main cost after E25. Design input for format 13 (through the lead): its
     proposed copy-merge items 1 to 3 (a stored list base, tail-coded blocks anywhere, the
     same for positions; `docs/design/format-13.md` on `phase4/format13`, "Proposed: F13
     candidates from research/indexing") would also make this flush a copy. Each batch's run
     entry could then hold its term's postings as final blocks and a tail block with their
     skip metadata, and the flush would merge runs and append pieces instead of decoding
     LEB128 and re-encoding PFOR (decode, `write_postings`, and `write_positions` were 21% of
     the CPU samples at 100,000 documents on macOS, E24's profile); PFOR pieces are also
     smaller than LEB128 runs. Under format 12 a block spans batches, so the flush re-blocks.
  4. Stage 1's three node-allocating hash maps per document (`buffer_docids_`,
     `versions_`, `docid_keys_`): about 350 MB at the 1.7M documents of a 5M flush, of which
     `map_bytes()` charges 194; an index of ordinals over the buffer (keys, docids, and
     versions read from it) would hold about 110 MB and allocate nothing per document.
  5. Under format 12, the two encodings (LEB128 runs at the drain, PFOR at the flush) are
     the price of having no term table: a block of 128 spans batches. The terms frequent
     in a flush's first batch (most of the positions) could instead carry their partial
     block from batch to batch in a small table and be block-encoded while their tuples are
     in cache, the flush appending their blocks and building skip data from per-block
     metadata; about 15% by the phase timers, at the cost of splitting `write_postings` and
     `write_positions` into a block encoder and an assembler shared with `flush_from`. A
     question for the lead before it is built: format 13's copy-merge items would make the
     same change simpler, and this work would be redone for format 13's blocks.
  6. The buffer's memory across flushes (below, a question for the lead).
- Coordination: perf/indexing-gap merges this branch after each kept item (lead,
  2026-10-18): this branch owns `tuple_buffer.hh` and the one-thread flush, perf/indexing-gap
  the parallel flush and the assembly; boundary changes go under "Interface with
  perf/indexing-gap" in the perf file and in the commit message. `perf/indexing-gap`'s E1 (`934811b`) pipelines the engine's parse at more
  than one thread and calls `tokens_document`, whose body E1 here changed; the two compose.
  E2 moves inversion to stage 2 at one thread; K inversion buffers on threads (D18) are the
  indexing gap's. `phase4/layers-refactor` rewrites `writer.hh` and `index.hh`; these
  changes rebase onto it by the lead.
- Blocked on:
  - Host (for the lead): the Mac's data volume is 98% full (21 GiB free); a 200 MB `dd`
    wrote at 12 MB/s at 02:00, and E24's 100,000-document profile spent 52% of its samples
    in `pwrite` (`runs/e24-prof.sample.txt`), so macOS timings that write are unreliable
    until space returns. The gate VM runs `research_workloads tiny` (pid 268527, about 110%
    CPU since 08:28 VM time) under no hold, beside the small-indexes fixtures. At 03:30
    writes were fast again (875 MB/s) but the volume was 99% full (16 GiB free, 5 GiB less
    than at 02:00).
  - Design question (D17, the lead's or the architect's): at 5M, 54.8 s of 279 s is system
    time, and the 1M profile puts 18.8% of the samples in the kernel's page zeroing, about
    half of it first touches of buffer memory (`fault_callers.py` over `perf-e21-1m.data`:
    `tuple_buffer::add` 5.7%, `malloc` 1.4%, the positions spill 0.9%) and 10.9% the
    harness's cold-cache read of the corpus, which Tantivy pays too. Every flush frees its
    buffer and the next one faults it in again. Keeping the freed pages for the next buffer
    breaks D17's accounting as built (`buffered_bytes()` is the buffer's charge, so kept
    capacity would read as a full buffer and flush at once); a pool of pages owned by the
    budget, charged as idle and handed to whichever buffer grows next (any index's), keeps
    the bound for many indexes. Not built: it changes what D17 counts.
- Process: E4's build and tests (21:35-21:47) overlapped `perf/indexing-gap`'s strict hold
  `ig-e2`; its eight-thread A/B from that window may need a rerun (the lead decides).
  Every step now runs behind `.scratch/gate.sh`. A second slip on 2026-10-18 at about 03:12:
  a command ran `tuple_buffer_test` and `index_test` (old builds, about 15 s on one core at
  nice 19) after `gate.sh` had refused its build, because the tests followed a `;` instead of
  the gate's `&&`, during `perf/index-size`'s strict hold `index-size-e3c-freebsd` (the
  FreeBSD VM's 1M A/B from 03:10:47); its first rounds may need a rerun (the lead decides).
  Every compile, test, and measurement now runs inside one `gate.sh && { ...; }` block.
  An overlap the gate cannot catch: a build checked free at about 04:38:40 (E33 and E34's
  compile, tests, and byte checks at nice 19, four jobs) ran until about 04:42, past the
  start of `small-indexes`' strict hold `si-f5-open2` at 04:39:02, since a hold taken later
  cannot see work already running; its first minutes may need a rerun (the lead decides).
  Likewise my light-hold A/B on macOS (`inverter-mac-e34`, about 04:41:30 to 04:43:30, one
  thread) overlapped the start of `si-f5-mac` (strict, 04:41:48): a strict hold does not
  wait for light holds already running.
- Findings folded in: none yet.
- Last verified: 2026-10-17, `strata_engine` builds at `4c9ebfd` (Apple clang 17, Release,
  `-march=native -g -fno-omit-frame-pointer`).
