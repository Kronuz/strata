# hybrid-search: full text plus vectors for AI retrieval (strata and prospect)

- Task: design hybrid search (BM25 plus dense vectors, fused) for many small indexes per host:
  storage in format 13/14 sections, exact search first with SIMD kernels, filtered search, rank
  fusion, approximate later, the write path, the query API, embeddings, gate rows; a fit verdict
  for the owner's go/no-go on the AI-retrieval direction (coordinator, 2026-10-27).
- Design: `docs/design/hybrid-search.md`.
- Branch / worktree: `design/hybrid` from main `98ce743`, at
  `/Users/kronuz/code/Kronuz/strata-wt/hybrid`.
- Maintainer: the designer (hybrid-search workstream).

## State

designing

## Done

- Ledger started.

## Doing

Reading the inputs (landscape D3, format 13 F3/F23, layers L4-L10, write buffer D3-D11, prospect
phase-4 D3-D7); writing the exact-search microbenchmark in `.scratch/vecbench/`.

## Next

1. Run the microbenchmark under a strict hold; record raw lines in `docs/design/hybrid-search/`.
2. Write the design, fit verdict first.
3. Set State to "designing (ready for preliminary review)".

## Blocked on

Nothing.
