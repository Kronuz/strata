# hybrid-search: full text plus vectors for AI retrieval (strata and prospect)

- Task: design hybrid search (BM25 plus dense vectors, fused) for many small indexes per host:
  storage in format 13's sections, exact search first with SIMD kernels, filtered search, rank
  fusion, approximate later, the write path, the query API, embeddings, gate rows; a fit verdict
  for the owner's go/no-go on AI retrieval, judged against the field's best vector systems
  (coordinator, 2026-10-27).
- Design: `docs/design/hybrid-search.md`, revision 0; microbenchmark source, driver and raw lines
  in `docs/design/hybrid-search/`.
- Branch / worktree: `design/hybrid` from main `98ce743`, at
  `/Users/kronuz/code/Kronuz/strata-wt/hybrid`.
- Maintainer: the designer (hybrid-search workstream).

## State

designing (ready for preliminary review)

Fit verdict: go with conditions C1 to C3. Decisions D1 to D12, gate rows T1 to T10, owner
questions Q1 to Q6, research items R1 to R5, step 1 (`vecdist`) with proposed criteria K1 to K6.
Format change (kinds 23 to 26, bit 17): no review ladder before the first release, but step 2's
decoder gets the safety review (PLAN's tiers).

## Done

- `78393d3`: ledger started.
- `c93f4e7`: partial draft (D1 to D6, D8 to D12, questions, step 1).
- `6364f26`: revision 0 complete: fit verdict, baselines, D7, crossover, gates, research items,
  sources; the microbenchmark (`vecbench-mac.jsonl`, i9-9980HK, one thread, `quiet strict`,
  2026-10-27).

## Doing

Nothing: stopped at ready for preliminary review.

## Next

1. Preliminary review (the lead launches; format and cross-library, so the architect).
2. Owner answers Q1 to Q6; the cheapest experiment (fit verdict) settles C1.
3. Step 1 (`vecdist`) can start now: no question blocks it.

## Blocked on

Nothing for step 1. Step 5 (ANN) waits on Q1 and Q2 and a researcher for R1 (owner's call).

## Last verified

- 2026-10-27: `vecbench.cc` builds for x86_64 (AVX2) and compiles for arm64
  (`-target arm64-apple-macos13`); dry run 48 rows; sweep 204 rows, exit 0.
- Lucene 10.4 claims checked in source at `9983b7c`; Tantivy 0.26.0 source has no vector code.
- Not run: arm64 or Linux timing, graph baselines at small sizes, recall (T1).
