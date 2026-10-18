# Layers refactor: measurements per step

layers.md L14 and L15: every step keeps format 12 byte-identical (`fixtures_test`) and
reports what it changes on the measured axes. Gate VM (Linux x86_64), against the build
before the step.

| Step | What | Bytes | Queries (bench/quick, 16-segment 5M index) | Indexing (1M corpus) |
|---|---|---|---|---|
| 0 | Fixtures | — | — | — |
| 1 | Section I/O moved to `section_io.hh` | identical | not measured (moves only) | not measured |
| 2 | Open path: core and plugin readers | identical; open throws only `corrupt_segment` (29 of 1,089 fuzz inputs changed type, none their verdict) | 0.95–1.02× on every command, 4 rounds; `COUNT` on the stratified terms 1.156× from one round of 1.17 among 0.99–1.01 | — |
| 3 | Flush path: builders and the sink | identical | not on the query path | wall 258.6 and 261.0 s against 279.3 and 261.0 s (alternating, two each); peak RSS 722.6 against 715.9 MB, +0.9%, consistent across runs, not yet explained |
| 5 | Merge path: the core's plan and plugin merges | identical (fixtures; 555 merges per run byte-equal to the old merge, `merge_oracle_test`) | not measured: the merge rows and T3 run on the gate VM (L15) | not measured |
| 6 | Search on plugin readers | identical | not measured: the full L15 plan against the recorded Phase 3 commit | not measured |

## C5 (the lead's runs, 2026-10-18, gate VM on tsc)

- **Step 2's `COUNT` row**, 0707741 (A, step 1) against 97d0ae6 (B, step 2), `bench/quick/ab.sh`
  on the 16-segment 5M index, 10 rounds of best-of-3, sides alternating, one thread, light hold
  (131 s): B/A on the stratified terms 0.990× [0.987–0.993], on the 962 gate queries 0.979×
  [0.977–0.981], 0 mismatches. Step 2 does not slow `COUNT`; the earlier 1.156× was one round's
  outlier.
- **Step 3's peak RSS**, 97d0ae6 (step 2) against 731918d (step 3), 1M documents indexed three
  times each, interleaved, the default 2,056 MB budget, `ab_index.py` with `AB_WARM=1` (2,021 s):
  peak RSS 3,531.5–3,531.6 MB against 3,531.6–3,531.7 MB (equal within 0.01%), wall 1.008×
  [0.926, 1.126], identical bytes (643,074,818) and segments (5). Measured equal at this budget;
  the earlier +0.9% (722.6 against 715.9 MB) was at a smaller budget and is not re-measured here.
