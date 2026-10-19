# query-exp: the query gap's experiments (E1, the base check, the freeze set, H22, key positions)

- Design: `docs/design/query-gap.md` revision 2 (branch `design/query-gap`, e272ec2); the
  lead's rulings and E0's quick-tier calibration in that branch's `docs/status/query-gap.md`.
- Branch / worktree: `perf/query-exp` from origin/perf/query-counters b7823cd (the re-check
  750ce06 merged), at `~/code/Kronuz/strata-wt/query-exp`.
- Done criteria: the query-gap ledger's K2 to K4 (and K5, K6 if M17 or M23 are kept) are the
  lead's; ticked there, never edited here.

## State

E1, the base check, the freeze set (E3, E6, E7, E2, E5, E4), H22, and the key-position
question are measured and recorded; format 13's freeze table is in
`docs/perf/query-gap-format13.md`. One freeze item, M20, waits on E10 (M3), which E4a calls
for and which is not built.

## Freeze decisions

| Item | Decision |
|---|---|
| M16 | In: A3@128 inside D15 (−1.5 MB); A4@128 (+4.4 MB, fastest everywhere) if the joint selection pays |
| M17 | Out: term `TOP_10` 0.976 on S′, above 0.97 (frequent terms 0.958) |
| M18 | Out (QG-R4) |
| M19 | Out: P1 adds under 3% at κ's upper end; fixed partitions do not fit |
| M20 | Open: E4a and E4b pass; needs E10 before the freeze, or out (1.9–3.7 MB) |
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
- Key positions: `docs/perf/key-positions.md` (this commit), on format 13 at 026ee10.

## Next

1. E10 (M3, bound-ordered traversal), which E4a's pass calls for, and E4b again on top of it,
   before the freeze if M20 is to stay; otherwise M20 leaves format 13.
2. The harness A/Bs beside the fastest engine (D14's second A/B) were not run: no item
   reached them (every format-free item lost its in-process A/B, and the format items are
   decided by their own rules). D17's harness tooling (`bench/ab.sh`'s third engine) is not on
   this base.
3. Format 13's contributor: A3@128 or A4@128 per the joint selection, M22's Elias-Fano code,
   M23 with K6's tests; E7's −35 MB Elias-Fano positions as a byte source if its decode passes.
4. Format-free follow-ups the records name: simdcomp's d1 kernel in `bitpack` (about 20% of
   ordinal decode), FOR for wdf blocks (PFOR's exceptions cost 60% more decode), M10 for
   two-phase-critic's header stepping, region verification's 10–13% on two-phase-critic.
5. Open hypotheses: H5, H11, H12 (counters owed or below level), H17 (the terms file's
   calibration), H18 and H19 (no PMU in the guest), H21 (a PISA-linked log-gap count).
6. CI on this branch's tip (see Last verified).

## Blocked on

- M20's freeze line needs E10 (M3), not in this assignment: the lead decides whether E10 runs
  before the freeze.
- M17 with M1: M17 alone fails D14 by 0.006 on the class row, and M1 on top of M17 read
  0.962 [0.914–1.017] on intersection `TOP_10` in a run whose control moved 11%. Whether the
  pair earns a clean joint A/B before the freeze is the lead's call; by the queue's rules both
  are out.
- For the lead: the quick-tier calibration ran `queries-sample.txt` only (term rows rest on
  one query, so H17 has no term:rare reference); E0's `dump` references and the
  `STRATA_DELETE_EVERY` index are not on the gate VM (the base check made the references with
  E0's binary).

## Last verified

2026-10-19 on the gate VM: `search_test` and `counters_test` pass on the cleaned tree
(default build and the counting build compile; `qgbench` builds with simdcomp). Every
measurement's raw files and SHA-256s are in the `docs/perf/*.sha256` beside its record.
Light holds for every timed run; another agent's light hold overlapped the first M1b, M2, and
M9 runs (M9 was rerun clean). CI when I stopped: run 39250250346 at 72a5588 green; 43f7c85's run cancelled by the next push;
eaaea3c's (39260668952) in progress; 38b6715 and this commit pushed, not waited on.
