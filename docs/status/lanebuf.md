# lanebuf (write-buffer bake-off, step 0)

- Branch / worktree: `perf/lanebuf` (on `main` at `88a56e7`), `/Users/kronuz/code/Kronuz/strata-wt/lanebuf`. Not merged.
- State: stopped at the coordinator's quota call. Prototype rebuilt on the hazard registry with the WB-1..4, WB-16..19 fixes;
  T1, T2/T3, T6, freeze, T7 timed; T11a/T11b not run; no tuning experiment run. Nothing is running; no holds.

## Done

- `a4aa2c4` rebuild (hazard registry, position index, fixed chunk directory, scan-triggered builds, component epoch, real segments, applier path).
- `8c39124`, `c1471b4`, `51a4c29`, `a491ede` runner, adapters (RocksDB, main), bytes mode, prebuilt RocksDB clones, JSON fix, E1 behind `LANEBUF_E`.
- Results and verdicts: `docs/perf/write-buffer-bakeoff.md`; raw lines: `docs/perf/write-buffer-bakeoff-data/`.

## Next

1. Measure T11a (a0, a50) and T11b with the yield-wait harness (`lb_plain4`; `bakeoff.py req`; RocksDB clone via `--prebuilt`): then rule 3 decides.
2. Fix C's T3 (E1, E2 in the source, unmeasured), re-run T3 and T1 one-writer; or take A' for scan-heavy indexes.
3. T4, other platforms, WB-6, `install()`/`heal()`.

## Blocked on

Quota. The host is shared with unmanaged QEMU VMs (load 6 to 7): clean T11 rows need a quiet host.
