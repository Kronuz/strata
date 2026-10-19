# Query gap: format 13's freeze items (E3, and the freeze set's verdicts)

T4's record (`docs/design/query-gap.md`, "Before the freeze, and after"): each of M16, M17,
and M19 to M23 with its evidence, its bytes against D15's budget, and a keep or drop line;
M18 out. Every experiment ran on the gate VM on 2026-10-19, one thread, light holds, on the
cached reordered merged 5M segment (Q5) and, for D14's guards, its 16-segment index.

## The freeze decisions

| Item | Decision | Evidence | Bytes (D15: postings 11.9 MB over keyed `-bp`; whole index 3.2 MB headroom) |
|---|---|---|---|
| M16 advance entries | **In: A3@128** inside the budget; **A4@128** if the joint selection pays +4.4 MB | E6: A4@128 fastest on every trace (format 12's speed on leapfrogs, 2.8x on scans, 8x on window bounds); A3@128 the fastest inside the budget (72 ns per touched block against A4@256's 92) | A3@128 −1.5 MB; A4@128 +4.4; A1@128 +8.6; @256 −4.3 to −9.8 but half a block decode more per touched block |
| M17 pair order | **Out** | E5: term `TOP_10` 0.976 on S′, above D14's 0.97; jointly with M1 at the run level (`ab-run-level.md`), no named target row wins under either preset and intersections of three or more terms lose beyond 1.03 and the A/A band (1.06–1.15) | A preset record, 26 bytes per segment; frontiers unchanged in size |
| M18 argmax pairs | Out (QG-R4) | | |
| M19 partitions | **Out** (P0, no bytes) | E3: P1 (variable, average 128) adds at most 1.7% over P0's exact bounds on any target row, under 3% at κ's upper end; fixed 32, 40, 64 do not fit and were not simulated | P1 about 0 at 128; fixed 64 +9.2 MB, 40 +14.8, 32 +18.5 (one byte per partition) |
| M20 seeds | **In, two pairs per k** | E4a's oracle passes on every term and union `TOP_k` row (0.44–0.71); E10 ran (M3t: terms in reordered segments, term rows 0.78–0.86 on S′); E4b's dominance seeds pass again on top of it (term 0.81–0.88, union 0.77–0.89 on S′, both indexes), both re-confirmed at the run level with no loss in any row; two stored pairs per k reproduce those seeds (`query-gap-seeds.md`) | 1.2–2.5 MB (two pairs of 2–4 bytes, three k, 103,578 terms) |
| M21 positions codec | **Out** | E7: no codec projects 3% on a phrase row (decode is 8–10% of a phrase row; FOR decodes 9% faster) | FOR +95.7 MB; Elias-Fano −35.3 MB (decode unmeasured); split firsts −5.3 MB |
| M22 Elias-Fano blocks | **In, chosen per block by size** (run containers out: not measured) | E6: an Elias-Fano probe 25–29 ns against FOR's decode and search 64–76 ns; smaller than the stored codec in 16% of the frequent terms' blocks | ≤ 0 (by size); −0.87 MB on 50 terms' 12.5 MB |
| M23 gap − 1 and its check | **In** | E6: the header-width form beats `check_increasing` in every paired repeat (0.85–0.94 per FOR block) | −0.30 MB |
| H22 codec | **No change** | `query-gap-codec.md`: bitpack's FOR block is SIMD-BP128's layout; strata's integrated decode beats PISA's path 2.4x; simdcomp's d1 kernel is 20% faster on the same bytes (format-free) | 0 |

Together the kept items move the postings section by A3@128's −1.5, M23's −0.3, M20's +1.2
to +2.5, and M22's by-size saving (not projected on the whole segment); with A4@128 instead
of A3@128, +5.9 more. None of them
closes the postings section's 11.9 MB on its own; that is the joint selection's question
(format 13's F13-R8), and E7's 35.3 MB of Elias-Fano positions is the largest byte source
measured here.

Coverage (the lead's ruling): the term rows, re-run on the 200 stratified terms, are above
1.0x (1.28x, 1.17x, 1.17x at `TOP_10`, `TOP_100`, `TOP_1000`; frequent terms 1.39–1.85x
against Tantivy), so M3 and M20 address carried rows; nothing missing on the gate VM changes a
line above (`query-gap-coverage.md`).

## E3: work under substituted bounds

A mode of the real scorers in the counting build (`STRATA_COUNTERS`): `term_scorer`'s
`block_bound`, `group_bound`, `max_score`, and `skip_blocks` read exact partition maxima under
the query's weight (`STRATA_BOUND_SOURCE=exact:N` for fixed partitions of N postings,
`var:N` for Mallia et al.'s variable partitions cut greedily to an average of N), computed
once per term from its decoded postings; within a competitive block, a term's bulk top-k
skips postings whose partition cannot beat θ. Seeds enter through `STRATA_SEED_FILE`. The
scorers, windows, and traversal are strata's own. Counters per query over S at every
command; the projection charges each counter's change at E1's unit cost (the profile's µs in
the part over its count; bound evaluation is not credited, since the simulation prices a
bound at nothing). θ at every query's end equals the stored source's under every
substitution, so no bound was below a score.

| Row | exact:128 (M1's rule): postings scored, blocks, projected | var:128 (P1) | exact:256 |
|---|---|---|---|
| intersection `TOP_10` | 0.724, 0.812, −24.3% | 0.764, 0.807, −23.8% | 0.832, 0.834, −20.5% |
| intersection `TOP_100` | 0.776, 0.875, −17.2% | 0.796, 0.872, −17.0% | 0.834, 0.891, −14.7% |
| intersection `TOP_1000` | 0.818, 0.930, −11.7% | 0.820, 0.926, −11.8% | 0.855, 0.942, −9.9% |
| intersection_union `TOP_10` | 0.983, 0.946, −5.0% | 0.985, 0.938, −5.8% | 1.002, 0.968, −2.1% |
| term `TOP_10` | 0.973, 1.000, −0.5% | 0.870, 1.014, −2.2% | 1.579, 1.000, +9.7% |
| term `TOP_100` | 0.998, 1.000, −0.1% | 0.957, 1.007, −1.5% | 1.414, 1.000, +10.5% |
| term `TOP_1000` | 1.000, 1.000, −0.0% | 0.955, 1.001, −2.0% | 1.239, 1.000, +5.6% |
| union `TOP_10` | 0.958, 0.993, −1.6% | 0.970, 0.997, −1.3% | 0.924, 0.981, −2.3% |
| union `TOP_100` | 0.975, 1.002, −0.7% | 0.992, 1.007, −0.3% | 0.942, 0.993, −1.5% |
| union `TOP_1000` | 0.935, 0.994, −0.8% | 0.941, 0.994, −0.7% | 0.929, 0.993, −1.1% |

With the oracle seed (stored bounds; exact:128 beside it):

| Row | stored + oracle seed | exact:128 + oracle seed |
|---|---|---|
| term `TOP_10`, `TOP_100`, `TOP_1000` | −55.5%, −56.3%, −48.9% | −55.6%, −56.3%, −48.9% |
| union `TOP_10`, `TOP_100`, `TOP_1000` | −14.3%, −19.7%, −24.2% | −15.8%, −20.6%, −23.9% |
| intersection_union `TOP_10`, `TOP_100`, `TOP_1000` | −11.3%, −7.4%, −7.1% | −16.9%, −10.9%, −8.4% |

**Calibration (D5)**, measured over projected reduction:

| Point | Rows | Projected | Measured | κ |
|---|---|---|---|---|
| M1 (E2) | intersection `TOP_10`, `TOP_100`, `TOP_1000` | −24.3%, −17.2%, −11.7% | +20.1%, +12.7%, +3.8% | negative |
| M1 (E2) | union `TOP_k` | −1.6% to −0.7% | −1.1% to −0.9%, inside noise | unusable |
| Oracle seed (E4a) | term `TOP_10`, `TOP_100`, `TOP_1000` | −55.5%, −56.3%, −48.9% | −55.6%, −49.6%, −36.8% | 1.00, 0.88, 0.75 |
| Oracle seed (E4a) | union `TOP_10`, `TOP_100`, `TOP_1000` | −14.3%, −19.7%, −24.2% | −29.5%, −42.9%, −40.9% | 2.06, 2.18, 1.69 |

So E3 decides for terms (κ in [0.75, 1.00]) and, at the edge, for unions (κ in [1.69, 2.18],
partly outside [0.5, 2]); it decides nothing for intersections, where pruning the projection
counts is real (M1 cut postings scored 28% in the counting build) but its bound reads cost
more than they save (E2).

**M19 by D5.** P1's increment over P0 with M1's rule (var:128 against exact:128): term
`TOP_10` −1.7%, `TOP_100` −1.4%, `TOP_1000` −2.0%; union +0.3%, +0.4%, +0.1% (worse);
negated +16.8% at `TOP_10`. At κ's upper end for terms (1.00) every target row stays under 3%,
and unions lose: M19 leaves format 13 with its hooks, P0 stands. Exact:256's postings
scored rise 24–58% on terms, which is why M16's entries stay at 128 postings. **H10 is
refuted** (postings scored under the best variant 0.87 of today's at most, against a level
of 0.8, and no calibrated ceiling reaches 3%). **H1 is confirmed** (the oracle's counters:
a term's `TOP_10` decodes 32% of its unseeded blocks; E4a measured it).

Raw: `~/bench/results/qexp-e3/run1` (the four sources) and `run2` (the seeds), SHA-256s in
`query-gap-format13.sha256`; the per-experiment records are `query-gap-e2.md`,
`query-gap-pair-order.md` (E5), `query-gap-seeds.md` (E4), `query-gap-skip-entries.md` (E6),
`query-gap-positions.md` (E7), and `query-gap-codec.md` (H22).
