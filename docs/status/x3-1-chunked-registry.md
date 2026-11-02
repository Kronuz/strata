# x3-1-chunked-registry

- Design: `docs/design/shared-reader.md` revision 4 (R4.1 to R4.8). Ruling: Xapiand
  `docs/reviews/phase7c-x2b-code.md`, "Architect ruling on the reader registry (X2b-B1, N1, N2)",
  item X3-1. Probe: Xapiand-wt/x2b `.scratch/arch-registry/registry_arms.cc`.
- Branch / worktree: `phase7c/x3-1-chunked-registry` from main `71de4a5`, at
  `~/code/Kronuz/strata-wt/x3-1`. Lock-free concurrency: one light architect check of revision 4.
- State: built and proven locally; pushed for CI (jobs not watched).
- Done criteria and evidence (macOS x86_64, Homebrew clang 23):
  - [x] Revision 4 text (R4.1 to R4.8) and the D4, D5, D9, D10 edits it implies.
  - [x] `hazard.hh`: `basic_reader_registry` is a list of chunks (4 records, 320 bytes on 64-byte
    lines, 640 on Apple arm64); empty until the first acquire (registry object 120 bytes on macOS);
    the constructor's argument is now a capacity, so every caller works unchanged (no migration
    needed); `unbounded_records` for none. Lock-free acquire: hinted record (cached pointer), the
    rest, then one compare-exchange on the last link with the new chunk's first record already
    claimed. Link stored and loaded seq_cst (`hazard_orders::link`). A registry destroyed with a
    record claimed keeps its chunks.
  - [x] `index.hh`: with `options::readers` null, `reader()` makes the index's registry
    (`options::reader_records`, default 4096); the old "reader() needs a registry" refusal is gone,
    and `views_basic` now tests the own registry and its cap.
  - [x] Litmus (`hazard_litmus_test`) reruns on the chunked registry (K = 1 so every race crosses
    a link), two new shapes (grown claim, grown retry), 10^7 iterations: 0 free-while-read at
    seq_cst; the weaker-order mutants are still caught on x86_64 in the retry and grown-retry
    shapes (e.g. "reader: record write release only": retry 20, grown retry 20, cap 20).
  - [x] Tests (`hazard_test`): `chunks` (exact capacity, allocation per chunk, stable record
    addresses, scan and claim table across chunks, reuse after release, unbounded), `kept_chunks`,
    `racing_appenders` (8 threads x 500 fresh registries), `growth_stress` (12 threads each holding
    1 to 9 guards while the owner publishes and scans; the list grew to 24 chunks, retired views
    stayed below the records that exist).
  - [x] Sanitizers: `hazard_test`, `hazard_litmus_test`, `shared_view_test`, `shared_read_test`,
    `write_buffer_reads_test`, `install_own_test` under ASan+UBSan (Debug -O1) and under TSan
    (Debug -O1): all exit 0, no reports (`.scratch/x3-1/runsan.out`, rerun after the hint change).
  - [x] Mutants, `tools/mutants/chunked.py` (6 of 6 killed, unmutated survives):
    - `link-relaxed` (publishing a chunk relaxed): killed under TSan, a data race in `hazard_test`
      (the litmus also reports it, so its relaxed-link entry is skipped under TSan; x86_64 cannot
      show this one natively, as for every load or locked-instruction-only weakening).
    - `scan-skips-newest-chunk`: killed by `hazard_test` (`chunks`, and the old
      `claim_and_release`) and by the litmus's grown retry (every iteration).
    - `chunks-freed-with-a-record-claimed` (the destructor frees what a guard will still write):
      killed by `kept_chunks` as an ASan heap-use-after-free.
    - `first-record-not-preclaimed`, `capacity-rounds-to-chunks`: killed by `hazard_test`.
  - [x] Bench, `bench/shared_reader/registry.cc` (`shared_reader_registry`), under `quiet strict`,
    3 runs of 1 s per arm, branch commit, not main: acquire+release on one pointer, ns per op per
    thread, fixed H=64 vs chunked cap 64: 1 thread 8.2 to 8.3 vs 9.2 to 9.6; 8 threads 8.5 to 9.1
    vs 9.8 to 10.5. Owner scan with the stamping pass, ns: 4 claimed 18.4 to 19.0 (fixed H=4) vs
    17.6 to 18.6; 64 claimed 86 to 89 vs 70 to 75; 1,024 claimed 1,372 to 1,417 vs 2,025 to 2,116
    (the pointer chase over 256 chunks); fixed H=1,024 with 4 claimed 1,369 to 1,550. So an
    acquire costs about 1 ns more, a scan is as cheap as the claimed records need, not the
    capacity. The first draft without the cached hint record measured 10.6 to 11.3 ns at 1 thread
    (`.scratch/x3-1/bench-v1.txt`).
  - [x] `cpp-libraries/tools/preflight.sh`: see "Preflight" below.
- Done (commits): see `git log phase7c/x3-1-chunked-registry`.
- Next: CI on the branch; the architect's light check of revision 4; then Xapiand drops
  `readers_per_index` for `reader_records` (X3-2 sets the cap).
- Blocked on: nothing.
- Notes for the reviewer: (1) `reader_records` defaults to 4096, a tuning choice until X3-2's
  admission cap exists. (2) The scan's second (stamping) walk still runs per scan, as before.
  (3) Not run: arm64, Linux, FreeBSD natively (the litmus's must-fail set is x86_64-only by
  design; the arm64 jobs report).
