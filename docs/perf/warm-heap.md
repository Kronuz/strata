> Status: EXPLORATION (2026-11-21) on `explore/warm-heap`; only this note merges. PARTIAL: jobs queued.

# Where a warm index's heap goes

Target: a warm index costs 10.7 KB of heap (d1000s4 through Xapiand's path, eager-open.md), the kv
layout's 5 KB (catalog-tiers.md d1s1: 5,084 B; d100s10: 11,396 B, so about 0.7 KB per segment).
Q1 breakdown by allocation site, Q2 the largest single removal, Q3 `soft_hot` and `demote_idle`.

## Method

`bench/warm_heap/run.sh` (this branch): `catalog_tiers_count` forces a tier on N indexes and prints
live heap per index by allocation site (`heap_count.hh`, requested bytes, not malloc's rounding),
named by `atos`. Fixture `d1000s4`: 1,000 documents in 4 segments, 2,000 clones, strata main
strata:cc30217f. Xapiand's `verify_fields` pools (1.6 KB at 4 segments, eager-open.md) are prospect's
and not in this harness.

## Jobs (queued, results not yet read)

- build + fixtures: 261121-154219-468974
- warm pieces: 261121-154548-fbed57; hot pieces: 261121-154549-b73d45
  (read `$HOME/.scratch/xapiand/warm-heap/sites-{warm,hot}.txt` and the job logs)

## Open

1. The per-site table (Q1) from the two pieces jobs.
2. Experiment per largest site behind a switch, strict A/B plus A/A.
3. Q3 sweep of `soft_hot` and `demote_idle` on `catalog_tiers t3` (needs the table first).
