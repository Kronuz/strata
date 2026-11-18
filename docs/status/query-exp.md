> Status: HISTORY (2026-10-19) code not on main; its attribution predates the prospect query wins, verify before citing.

# query-exp: the query gap's experiments (E1, the base check, the freeze set, H22, key positions)

- Design: `docs/design/query-gap.md` revision 2 (branch `design/query-gap`, e272ec2); the
  lead's rulings and E0's quick-tier calibration in that branch's `docs/status/query-gap.md`.
- Branch / worktree: `perf/query-exp` from origin/perf/query-counters b7823cd (the re-check
  750ce06 merged), at `~/code/Kronuz/strata-wt/query-exp`.
- Done criteria: the query-gap ledger's K2 to K4 (and K5, K6 if M17 or M23 are kept) are the
  lead's; ticked there, never edited here.

## State

Done. Every freeze item is decided on evidence, and the lead's second ruling is applied: A/Bs
are scored at the run level with an A/A band (`docs/perf/ab-run-level.md`, `ab.py --aa`),
the joint M17 + M1 A/B re-ran under it and stays out on evidence, and M3t and the seeds on it
are re-confirmed with no loss in any row.

## Freeze decisions

| Item | Decision |
|---|---|
| M16 | In: A3@128 inside D15 (−1.5 MB); A4@128 (+4.4 MB, fastest everywhere) if the joint selection pays |
| M17 | Out: 0.976 alone on S′; with M1 at the run level, no target win and 3+-term intersections lose 1.06–1.15 beyond the A/A band |
| M18 | Out (QG-R4) |
| M19 | Out: P1 adds under 3% at κ's upper end; fixed partitions do not fit |
| M20 | In, two pairs per k (1.2–2.5 MB): E4b passes on top of M3t, term 0.81–0.88, union 0.77–0.89 on S′ |
| M21 | Out: no positions codec projects 3% on a phrase row |
| M22 | In: Elias-Fano chosen by size (probe 2.6x FOR's); run containers out (unmeasured) |
| M23 | In: width check beats `check_increasing` in every paired repeat; −0.3 MB |
| H22 | No format change: bitpack's block is SIMD-BP128's layout |
| Key ordinal | Store nothing more: format 13's keys already hold the ordinal |

## Done

- Plan and ledger: 6013d2c. Merged the cleared counters (re-check 750ce06): 5b67776.
- E1 (T1): `docs/perf/query-gap-attribution.md` and its `.sha256`: 9da2532; verdicts updated
  with the freeze set (this commit). Confirmed H1, H2 (term `TOP_10`), H3, H6, H13, H15, H16,
  H20; refuted H4, H7, H9, H10, H14, H22 (as format-dependent); open H5, H8, H11, H12, H17,
  H18, H19, H21, each naming what it waits on. A part no hypothesis names: region verification,
  10–13% of two-phase-critic.
- The base check: `docs/perf/query-gap-base.md`, 72a5588. E0's binary (aec8d11) and the base
  print the same `dump` bytes on the reordered merged, docid-ordered merged, and 16-segment
  indexes over the gate queries and stratified terms.
- E3's bound-source mode and the seed flag (`STRATA_SEED_FILE`, `search()`'s floor): 72a5588;
  E3's table and the freeze record: `docs/perf/query-gap-format13.md` (this commit).
- E2: M1, M1b, M2, M9 measured and removed (`docs/perf/query-gap-e2.md`): 43f7c85.
- E5: M17 by an experiment writer, reader, and in-place rewriter of a format-12 segment
  (`qgbench m17`); removed after its A/B (`docs/perf/query-gap-pair-order.md`): 43f7c85,
  eaaea3c, removal in this commit.
- E6, E7, H22: `qgbench` (`bench/quick/qgbench.cc`, `STRATA_QGBENCH`), records in
  `query-gap-skip-entries.md`, `query-gap-positions.md`, `query-gap-codec.md`: eaaea3c.
- E4a, E4b: `docs/perf/query-gap-seeds.md`, `qgbench seeds` (this commit).
- Key positions: `docs/perf/key-positions.md`, on format 13 at 026ee10: 38b6715.
- The lead's rulings, 2026-10-19: the joint M17 + M1 A/B (`query-gap-pair-order.md`): 7f8affe.
  Coverage (`query-gap-coverage.md`; H17 refuted with the term reference): 6acff45. E10
  (`query-gap-m3.md`): M3 for terms and unions, then M3t, S and S′, with E4b on top and a
  same-binary A/A on S′ for the noise floor (1.074 merged, 1.129 16 segments): 6edd635 and
  this commit. M20 decided (`query-gap-seeds.md`). Format-free leads (simdcomp's d1 kernel,
  FOR wdf blocks): `query-gap-format-free-leads.md`, 7f8affe.
- The lead's second ruling: run-level scoring with an A/A band in `ab.py` (with `32d598a`'s
  per-tag scorer brought over), its tests, and `ab-run-level.md`: 9a25ef5. The joint M17 + M1
  A/B at the run level (both presets, eight runs each after an A/A of eight) and the M3t and
  seeds re-confirmations (six runs each after an A/A of six): this commit. Gate VM cleaned: the
  argmax-first copy, the 16-segment copy, the key-position indexes, and nine dead build
  directories removed; 12 GB free (91% used), from 3.2 GB.

## Next

1. Format 13's contributor: A3@128 or A4@128 per the joint selection; M22's Elias-Fano code;
   M23 with K6's tests; M20 at two pairs per k with the reader's seed path; E7's −35 MB
   Elias-Fano positions as a byte source if its decode passes.
2. M3t as a format-free item: its own landing A/Bs (the guard reread on a quieter host or
   with run-level repeats, since `ab.py`'s query interval misses run drift; D12's FreeBSD and
   macOS runs), and the harness A/B beside Tantivy on the term rows.
3. D4 checks owed for M20 and any landing item: the `STRATA_DELETE_EVERY` index and parity.
4. Format-free leads: simdcomp's d1 kernel, FOR wdf blocks, M10 for two-phase-critic's
   header stepping, region verification on two-phase-critic.
5. Open hypotheses: H5, H11, H12 (counters owed or below level), H18 and H19 (no PMU in the
   guest), H21 (a PISA-linked log-gap count).

## Blocked on

Nothing. The gate VM keeps this work's raw results (small), the base check's references
(341 MB), and five build directories (`build-e2-base`, `build-e2-m3t`, `build-cnt`,
`build-qg`, `build-rel`); the 16-segment index for future guards is recreated by copying
`engines/strata-reordered/idx`'s `manifest-2`, its 16 segments, `meta-1.kv`, and a fresh
`owner.lock` (hard links are refused by the journal).

## Last verified

2026-10-19 on the gate VM: `search_test` passes with `STRATA_M3` and without, and `counters_test` passes on the cleaned tree
(default build and the counting build compile; `qgbench` builds with simdcomp). Every
measurement's raw files and SHA-256s are in the `docs/perf/*.sha256` beside its record.
Light holds for every timed run; another agent's light hold overlapped the first M1b, M2, and
M9 runs (M9 was rerun clean). CI when I stopped: run 39250250346 at 72a5588 green; 43f7c85's run cancelled by the next push;
eaaea3c's (39260668952) in progress; 38b6715 and this commit pushed, not waited on.
