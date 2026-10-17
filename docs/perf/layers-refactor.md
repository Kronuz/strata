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
