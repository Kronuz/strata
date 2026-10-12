# strata review: Phase 1

## Phase 1 gate

- Date: 2026-10-11.
- Reviewed:
  - `docs/gates/phase-1.md`.
  - The libraries: `Kronuz/bitpack` `326bfa9`, `docset` `dc5f667`, `fst` `21688fb`,
    `task` `e4ba442`, `residency` `fd584a3`, `hashes` `90ec405`, and `varint` `1744f91`.
  - `include/strata/scorer.hh`.
  - `bench/phase0/` (README, `query.json`, `postings.cc`) at strata `f14c8d4`.
- Checked by:
  - Reading every header.
  - Running every library's test binary; all pass.
  - Re-running the `bitpack` reference, `task`, and CRC-32C benchmarks on the shared
    macOS host.
  - A per-class analysis of `query.json`.
  - Three corrupt-input repros under ASan.

### Verdict

**Not approved yet.** Four items (B1–B4) block the gate and therefore Phase 2. They
are small, and none of them changes a measured result. Once they land, the gate passes
without re-running the other rows. Every other row passes, or carries into Phase 2
under the rulings below.

### Blocking items

1. **B1. The decoders are not memory-safe on corrupt input.** ARCHITECTURE requires
   that decoders be "memory-safe on corrupt input and fuzzed". mmap pages re-read
   after verification are not re-verified, so readers cannot rely on checksums
   alone. Three repros fail under ASan and UBSan (Homebrew LLVM):
   - **PFOR exception position 200.** `bitpack::decode` writes past its 128 outputs
     (`out[positions[i]]`, `bitpack.hh:469`).
   - **Header width 40.** `header & 0x3f` indexes the 33-entry function-pointer
     tables, and the call segfaults (`bitpack.hh:452-454`). An unchecked `high_bits`
     also drives shifts of 32 bits or more.
   - **Root address outside the bytes.** The `fst::map` constructor accepts the
     trailer, and `get` segfaults (`fst.hh:140-144`). A zero delta also loops forever
     in `floor_value` and `max_from` and recurses without bound in `walk` and
     `intersect_from`, and `get_varint` has no length bound.

   The fix, for each library:
   - **`bitpack`** decodes from a bounded span and reports failure. It checks that
     the width is at most 32, that the block length fits the input, that n is at most
     128, that every position is below 128, and that `high_bits` is at most
     32 minus the width. The checks run once per block, not per value, so the SIMD path
     stays as it is.
   - **`fst::map`** validates its bytes at construction, or in a `validate()` call:
     - The root and every node lie in bounds.
     - Every delta lies in [1, address]. Children are written below their parents, so
       this proves the graph is acyclic.
     - Varints take at most 10 bytes.
     - Labels ascend.
     - Field widths are at most 8 bytes.

     The term index is pinned and validated once at open, so lookups stay unchecked
     after that.
2. **B2. No fuzzing.** PLAN requires "at least one CPU-hour of fuzzing per decoder and
   parser added in the phase, with zero findings", and no repository has a fuzz
   target. Add libFuzzer targets for:
   - `bitpack::decode` and `decode_delta`.
   - `fst::map`: validation, then `get`, `floor`, `floor_value`, `range`, and
     `intersect` on arbitrary bytes.
   - `varint::leb128::decode` and `unserialise_length`.

   Seed each target with valid encodings and run it for one CPU-hour under ASan and
   UBSan. B1's repros come out of these targets in seconds.
3. **B3. No format fixtures.** PLAN requires "a fixture for every format change".
   `bitpack`'s tests compare SIMD and scalar bytes with each other, and its benchmark
   compares them with SIMD-BP128, but no test pins the bytes. Add checked-in golden
   bytes for:
   - FOR blocks at every width.
   - A PFOR block.
   - A constant block.
   - A small `FST1` file.
   - LEB128 vectors.

   Without them, the fixed on-disk layout can drift unnoticed.
4. **B4. Unpinned dependencies.** Fetches follow branches rather than tested commits:
   - strata fetches every library at `GIT_TAG main` (`CMakeLists.txt:10`,
     `bench/phase0/CMakeLists.txt`).
   - `residency` fetches `task` and `hashes` at `main`.
   - The benchmark references (SIMD-BP128, CRoaring, constexpr-phf) come from
     `master`.

   Gate numbers are reproducible only with pinned commits, which the stage-1
   addendum required from Phase 1 on. Pin tested commits, and bump them deliberately.

### Rulings on the six questions

1. **Frozen APIs against stage-1 section 6.3.** They match, with four accepted
   relocations and three additions.
   - **Accepted relocations:**
     - VByte tails moved to `varint::leb128`, per the owner's rule.
     - "No wdf" is a per-term flag in the Phase 2 postings metadata, not a `bitpack`
       header bit.
     - "All ones" is a constant block of value 1, 5 bytes per block.
     - The sparse and roaring sets use out-of-place operators; `bitset`'s operators
       are in place.
   - **Before Phase 2:** B1 changes `bitpack::decode` and `decode_delta` to bounded
     signatures and adds validation to `fst`.
   - **During Phase 2:**
     - The scorer concept (`doc_iterator` plus `score()`, `block_end(t)`, and
       `max_score(upto)`) has the right shallow-advance shape for block-max pruning. It
       gains a way to pass the minimum competitive score into nested scorers.
     - `residency` gains the path by which 64 KiB region checksums reach a backend, and
       the segment format carries the region checksum table from its first version.
     - `docset::bitset::iterator::cost()` returns the cardinality, not the universe, so
       a cached filter can lead a conjunction.
2. **The `fst` miss.** The block-index measurement satisfies the gate's intent.
   - **Why it satisfies it.** Ruling (e) puts no full-vocabulary FST on the lookup
     path: a lookup is a `floor` over the resident block index plus one term-block
     scan. `floor_value` over a 1/32 index runs at 1.63× an open-addressing hash map.
     The full FST takes 10% of the hash map's bytes.
   - **What it leaves out.** The 1.63× covers only the index half of a lookup, and only
     present keys.
   - **What Phase 2 carries.** An end-to-end `termdict` row: exact lookup (index floor
     plus block scan), p50 and p99, present and absent keys. The target is at or below
     Lucene's BlockTree `seekExact` on the same vocabulary, with the hash-map ratio
     reported.
   - **The 2.43× full-vocabulary result** stays as a report row. The 23 MB FST exceeds
     the 16 MB last-level cache, so it measures memory latency.
3. **The `bitpack` size row.** It carries into Phase 2.
   - **The estimate.** Strata's doc deltas and wdf take 934.9 MB with LEB128 tails.
     Lucene's `.doc` is 990.6 MB, including skip data estimated at 26–44 MB, so
     Lucene's deltas and freqs are 946–965 MB and strata is 1.2–3.1% smaller.
   - **Why it stays open.** The estimate's uncertainty, 2.6–4.4% of the file, exceeds
     the 2% tolerance.
   - **What Phase 2 measures.** The whole postings section (deltas, wdf, skip levels,
     and competitive pairs) against Lucene's `.doc` for the same index, at 1.02× or
     less.
   - **The resulting budget.** Strata has about 75 MB for skip and impact data
     (1.02 × 990.6 − 934.9), against Lucene's 26–44 MB. Competitive pairs with exact
     `u32` document lengths need a compact encoding to stay inside it.
   - **A saving to measure.** Blocks store raw wdf while tails store wdf − 1. Storing
     wdf − 1 in blocks too saves a bit per value in blocks of ones and twos.
4. **`task`'s GCC trampoline.** Accepted as the GCC default.
   - **Why.** Constant stack is a correctness property, and GCC 13 provides it only at
     `-O2` without sanitizers.
   - **Cost.** About 7.6 ns per await, from GCC 13 measured both ways on Linux
     (no-op task 21.5 ns with the trampoline, 13.9 ns with symmetric transfer). Strata
     awaits per window, segment, or I/O request, never per document, so the cost stays
     out of the hot loop.
   - **Opting GCC into symmetric transfer** requires a 10^6-deep constant-stack test
     under the exact production flags.
   - **Follow-ups:**
     - Add a regression for a continuation that destroys the awaited task while the
       trampoline loop runs inside `final_awaiter::await_suspend`, under ASan, with GCC
       at `-O0` and at `-O2`.
     - Calibrate `cycles::per_ns()` at startup, because it busy-waits 2 ms on first
       use, which would otherwise land on a reactor.
     - Document or cap frame-pool growth when frames are freed on a thread other than
       the one that allocated them.
   - **My run** reproduces 17.97 ns with zero allocations on macOS, and Asio
     yield-to-resume at p50 123 ns and p99 182 ns.
5. **`docset` at density 0.01.** It passes.
   - **The numbers.** The rows sit at 1.07–1.17× (Linux VM) and 1.08–1.14× (macOS)
     against a 1.2× target, and most other rows are below 1.0×.
   - **The re-run.** The 3–13% margin is within shared-host noise, so the quiet-host
     `benchkit` re-run (at least five interleaved trials with a 95% interval) runs in
     the same session as the Phase 0 re-runs.
   - **If a row misses.** A row whose interval reaches past 1.2× becomes a Phase 2
     fix, not a gate block. Sparse OR and ANDNOT are off Phase 2's critical path,
     because cached filters and live docs arrive in Phases 3 and 4.
6. **Phase 0 baselines and the Phase 2 targets.** They change the targets in seven
   ways:
   1. **Xapian stops being a binding baseline.** Across classes with more than one
      query, it is 3–28× slower than the faster of Lucene and Tantivy in TOP_10, and
      4–194× slower in COUNT (1,130× on the single-term COUNT). "Must win against
      Xapian" stays as a floor; Lucene and Tantivy set the binding targets.
   2. **No single engine is the reference.** Tantivy leads on intersections and
      phrases, but Lucene leads on the union classes. In TOP_10, Lucene takes
      0.68–0.94× of Tantivy's time; in COUNT, it takes 0.42–0.95×. The read target
      becomes: per class and command, strata's geometric mean is at most 1.5× the
      faster of the two. A class above 1.5× needs a cause and a fix plan before Phase 3;
      a class above 2× blocks the gate.
   3. **Exhaustive counting moves into Phase 2.** Xapiand's exact totals make COUNT and
      TOP_100_COUNT relevant. Tantivy is faster than Lucene on TOP_100_COUNT in every
      class but one, by 1.1–2.2×. Basic exact counting lands in Phase 2, and both
      commands gate. Phase 4 keeps estimate triples and aggregations.
   4. **Equal scoring.** Lucene and Tantivy use Lucene's BM25 (k1 = 1.2, b = 0.75,
      Lucene's IDF). Strata's Xapian formula (k1 = 1, b = 0.5, smoothed IDF) yields a
      different score distribution, and therefore different pruning. BENCHMARKS rule 5
      requires the same function, so strata adds a Lucene-compatible BM25 for the
      TOP_K rows, and the Xapian formula gates in the parity tests.
   5. **Single-thread indexing baselines are missing.** Phase 0 recorded:
      - Tantivy at 4 threads: 14,033 docs/s on 2.49 cores, about 5,600 docs/s per
        core.
      - Lucene at its default: 14,135 docs/s on 5.35 cores, about 2,600 docs/s per
        core.

      Phase 2's indexing row needs both at one thread, into one segment, with
      positions on and equal stored fields. Its targets are at least 1.0× Lucene and
      at least 0.8× Tantivy, ratcheting to at least 1.0× Tantivy at Phase 3.
   6. **The term class has one query** ("the"). Add 200 single-term queries stratified
      by document frequency; Xapiand's keyword lookups make this class common.
   7. **A fixed latency floor.** The single-term COUNT costs 155–168 µs end to end,
      almost all of it pipe and dispatch, so fast classes compress toward 1.0×. Gate
      each class on both end-to-end and floor-subtracted times, and flag classes where
      the floor exceeds half the time.

   All Phase 0 query rows are provisional: they ran on a shared host, sequentially per
   engine. The Phase 2 gate uses the quiet-host interleaved re-run, which includes the
   Xapian negated-query fix.

### Phase 0 open items

1. **Oracle vectors.** They block the Phase 2 analyzer and the Phase 2 parity gate, but
   not segment, postings, and scorer work. They land first in Phase 2.
2. **The Xapiand profile and its companion measurements.** The RESTORE and query
   profile, the offload hop cost, and light-query p99 under heavy load change no
   Phase 2 engine target, so they run in parallel. They close Phase 0's own gate, which
   closes before the Phase 2 gate review, so that the Phase 2 report can state what the
   engine results mean for Xapiand's ingestion problem.
3. **Before the Phase 2 gate:** the quiet-host re-runs, the single-thread indexing
   baselines, the term-query sample, and the Lucene-BM25 setting.

### Before Phase 2 starts

1. B1–B4.
2. PLAN's Phase 2 gate takes the targets in question 6, the end-to-end `termdict` row in
   question 2, and the postings size row in question 3.
3. ARCHITECTURE's segment table says "128-ordinal blocks". It still says "128-docid
   blocks"; the codec rules under the table are already right.

### Carried into Phase 2

These don't block the start:

1. **Interface additions.** The scorer concept's competitive-score hook, the
   `residency` region-checksum path, and `docset`'s cardinality-based `cost()`
   (question 1).
2. **A `SIGBUS` policy and the region checksum table.** The segment format carries the
   region checksum table, and the docs state a `SIGBUS` policy. A media error under
   mmap raises `SIGBUS`, which by default ends the process instead of fencing one
   shard. Either handle it per thread, or state that a media error is a node failure,
   which REPLICATION's fault model already covers.
3. **Tail decode, re-measured with the decoders that ship.**
   - The 2.60 ns LEB128 figure comes from a local unchecked decoder in `postings.cc`.
     The 3.61 ns length-format figure comes from the checked `varint` decoder, and
     StreamVByte decode is not timed.
   - LEB128 stays on size: 1.85 bytes per value against 2.30. Revisit it under the
     format version in Phase 5 if tail decode dominates expansion queries.
4. **Automaton tests.** `fst`'s intersection test reaches 100,000 patterns; it runs
   about 90,000, all prefix automata. Phase 5 repeats the brute-force check with
   Levenshtein and wildcard automata.
5. **Per-width minimums in the `bitpack` report.** My run on the loaded macOS host:

   | Measure | Geometric mean | Minimum |
   |---|---|---|
   | Decode against SIMD-BP128 | 0.99× | 0.89× (2 bits) |
   | Decode with delta against SIMD-BP128/D1 | 1.02× | 0.76× (32 bits) |
   | SIMD over scalar | 7.5× | 2.2–2.3× (16 and 32 bits) |

   At the byte-aligned 16- and 32-bit widths the compiler vectorizes the scalar loop,
   so the report's "4–6× scalar" overstates the minimum. Bytes match SIMD-BP128 at all
   32 widths.

## Phase 1 gate re-check

- Date: 2026-10-11.
- Reviewed:
  - The libraries: `bitpack` `6d14df1`, `docset` `4e4d73f`, `fst` `2cb8927`, `task`
    `c3c30e4`, `residency` `e52bd7b`, and `varint` `de3d05d`.
  - strata `dfd6112`, `bench/phase0/README.md`, and `strata-oracle/vectors`.

### Verdict

**Phase 1 gate: APPROVED.** B1–B4 are resolved. Every test, rebuilt from current
sources with Homebrew LLVM under ASan and UBSan, passes, including the golden fixtures
and the portable `bitpack` build.

### Evidence

1. **B1, corrupt input.**
   - `bitpack::decode` and `decode_delta` take the input size and return 0 for
     malformed or truncated blocks: width above 32, more than 128 exceptions, a
     position of 128 or more, a width sum above 32, or short input.
   - `fst::map` validates at open: bounds, varints of at most 10 bytes, deltas of at
     least 1 that land on an earlier node start, increasing labels, widths of 1–8 bytes,
     the root as the last node, depth of at most 4096, and the key count against the
     trailer.
   - My three crash inputs now decode to 0 or throw `invalid_argument` under ASan and
     UBSan.
2. **B2, fuzzing.** `fuzz/` targets exist in `bitpack`, `fst`, and `varint`. I built
   each with libFuzzer, ASan, and UBSan and ran it for 45 seconds on macOS, with no
   findings:

   | Target | Executions | Per second |
   |---|---:|---:|
   | `bitpack` | 3.72M | 81k |
   | `varint` | 6.07M | 132k |
   | `fst` | 163k | 3.5k |
3. **B3, fixtures.** `blocks.bin` (99 blocks), `small.fst`, `length.bin`, and
   `leb128.bin` are checked in. The tests compare bytes and fail when a fixture is
   missing.
4. **B4, pins.** strata, `residency`, and the Phase 0 tool pin commits. The references
   are pinned too: SIMD-BP128 at `b809f38`, and CRoaring at v5.2.3.
5. **Also fixed:**
   - `bitset::iterator::cost()` returns the cardinality.
   - `task::cycles::calibrate()` exists.
   - The frame-destruction test passes under ASan with symmetric transfer and with the
     trampoline forced on Clang; I ran both.
   - PLAN carries the Phase 2 targets.
   - ARCHITECTURE says 128-ordinal blocks.
   - The gate report corrects the scalar ratio.

### Follow-ups (non-blocking)

1. **Fuzzing throughput.** The VM's hour-long runs executed far fewer inputs than the
   same targets do natively:

   | Target | VM inputs in one hour | VM per second | macOS per second |
   |---|---:|---:|---:|
   | `bitpack` | 1.74M | ~480 | 81k |
   | `varint` | 1.98M | ~550 | 132k |
   | `fst` | 286k | ~80 | 3.5k |

   That is 45–240× slower, and a 45-second run on the Mac exceeds each VM hour's
   total. Record executions per second and final coverage with every fuzz run, and
   find the cause; an unoptimized build or emulation would explain it.
2. **Transitive pins.** `hashes` fetches `static-string` and `char-classify` at
   `main`, so strata's build pulls them unpinned. `crc32c.hh` includes neither, so this
   affects reproducibility only. Pin them in `hashes` and bump strata's pin.
3. **`fst` recursion depth.** `walk` and `intersect` recurse up to `max_key_length`
   (4096) levels. On macOS secondary threads (512 KiB stacks), a validated adversarial
   FST can still overflow the stack. Strata's terms stay under 246 bytes. Make the
   traversal iterative, or lower the limit, before another consumer relies on it.
4. **`fst::trusted`.** It skips validation. Use it only for pinned copies that passed
   validation once, never for mmap views, whose pages re-read after eviction are
   unverified.
5. **PLAN's Phase 2 gate wording.**
   - The TOP_K classes lack the 2× hard stop.
   - Draw the 200 term queries stratified by document frequency; a uniform vocabulary
     sample is mostly frequency-1 to 10 terms.
   - The `termdict` row needs its threshold: at most 1.0× Lucene's `seekExact`.

### Phase 0 additions

1. **The RESTORE profile resolves the stage-1 hypothesis.**
   - Xapian accounts for 28.2% of RESTORE time, so any engine caps that path's speedup
     at 1.39×.
   - Host synchronization dominates: per-document check-in 25.4%, WAL deletion
     enqueues 14.6%, checkout 7.7%.
   - FEASIBILITY still calls the cause a hypothesis. Record the result there; that
     closes Phase 0's gate.
   - The two host fixes stay on Xapiand's track.
2. **Hop cost and light-query p99 need bare metal.** The offload hop (about 350 µs,
   15%) and the light-query p99 (7.2 ms offloaded, 3.0 ms inline) are absolute latencies
   measured in a VM, where waking a halted vCPU goes through the hypervisor. BENCHMARKS
   rule 4 makes them bare-metal rows. Re-measure them on bare metal before quoting them,
   and take the Phase 6 baseline there.
3. **The oracle vectors serve the BM25 parity gate.** The rank vectors cover 300
   queries over 10,000 documents with 17-digit weights, percents, and bounds at three
   `check_at_least` values. The term-generation vectors cover English stemming
   strategies and CJK n-grams only. The analyzer needs more; the Phase 2 design review
   lists it.
