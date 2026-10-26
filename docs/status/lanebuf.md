# lanebuf (write-buffer bake-off, step 0)

- Branch / worktree: `perf/lanebuf` (on `main` at `88a56e7`), `/Users/kronuz/code/Kronuz/strata-wt/lanebuf`. Not merged.
- State (2026-10-25): working C's scan tail (T3) on this branch; experiments in `docs/perf/write-buffer-bakeoff.md`, "Scan tail". Earlier: stopped at the coordinator's quota call. Prototype rebuilt on the hazard registry with the WB-1..4, WB-16..19 fixes;
  T1, T2/T3, T6, freeze, T7 timed; T11a/T11b not run; no tuning experiment run. Nothing is running; no holds.

## Done

- `a4aa2c4` rebuild (hazard registry, position index, fixed chunk directory, scan-triggered builds, component epoch, real segments, applier path).
- `8c39124`, `c1471b4`, `51a4c29`, `a491ede` runner, adapters (RocksDB, main), bytes mode, prebuilt RocksDB clones, JSON fix, E1 behind `LANEBUF_E`.
- Results and verdicts: `docs/perf/write-buffer-bakeoff.md`; raw lines: `docs/perf/write-buffer-bakeoff-data/`.

- Scan tail (2026-10-25/26): plan `docs/perf/lanebuf-scan-tail.md`, results in `write-buffer-bakeoff.md` "Scan tail", raw data in `docs/perf/write-buffer-bakeoff-data/scan-tail-*`.
  Seven experiments: E1 (prefixed entries in a binary counter) wins big; E2 (eager builder) tie, removed; E3 (tail selection) tie on E1, kept after E5; E4 (prefixes in the position index) wins; E5 (fresh and compaction jobs applied as splices) wins by 100x on p99; E6 (prefix merge) and E7 (counter ratio 8) win a little.
  C's scans beside writers at full speed: p99.9 4 ms to 500 ms before, 40 to 63 us now (1.5 to 1.8x RocksDB's; target 1.0x), p99 31 to 41 us (1.3 to 2.1x); at 2 MB/s p99.9 7 us (0.33 to 0.39x RocksDB's): met.
  Cost: T1 -1 to -3% (inside the A/A), T2 unchanged, memory +8.3% accounted per record and +528 B per index after one write, freeze -9%.
  Gates: check/cases native (C, A', F), TSan and ASan clean also with CAP 32 and TAILMAX 16.

## Next

1. Measure T11a (a0, a50) and T11b with the yield-wait harness (`lb_plain4`; `bakeoff.py req`; RocksDB clone via `--prebuilt`): then rule 3 decides.
2. C's remaining T3 gap at full write speed (1.5 to 1.8x on p99.9): a sampled index over each big level, or the visibility position in the 16-byte entry (no record read per source); each costs memory. Or take A' for scan-heavy indexes (0.56 to 0.78x, loses T1 by 4 to 7x). The lead decides whether 1.5 to 1.8x at full write speed (met at 2 MB/s) closes T3.
2a. The position index at 12 bytes per record costs 8.3% memory: a 4-byte prefix tag would halve it; a lead decision on the T6 target (153 B, missed before at 167.8, now 181.7).
2b. Re-run T2/T3/T1 on the other platforms (Linux, FreeBSD, arm64): only macOS x86_64 measured.
3. T4, other platforms, WB-6, `install()`/`heal()`.

## Blocked on

Quota. The host is shared with unmanaged QEMU VMs (load 6 to 7): clean T11 rows need a quiet host.
