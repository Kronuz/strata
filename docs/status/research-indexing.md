# research-indexing

- Design: none yet; the research note is `docs/research/indexing.md` (the bound, the survey,
  each idea with its prototype numbers, the ranked recommendations). Prototypes live in
  `research/indexing/`, built with `-DSTRATA_RESEARCH=ON`.
- Branch / worktree: `research/indexing` at `/Users/kronuz/code/Kronuz/strata-wt/research-indexing`,
  from `origin/main` at `4c9ebfd`.
- State: researching. Target (owner, PLAN "Design target"): 4.5x the faster baseline's
  indexing throughput at one thread and at eight; at 5M documents about 1:55 at one thread
  against Tantivy's 8:30 and about 0:57 at eight against roughly 4:10.
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Worktree and branch created; the 100k-document sample copied from the gate VM to
    `.scratch/corpus100k.json` (117,707,867 bytes, sha256 `0a1d5344…3acebd`): 18.3M tokens,
    8.8M postings, 338k distinct terms.
  - `research/indexing/bound.cc`: the per-stage bound benchmark (read, split, hash, probe, append,
    partition, group, encode, write), five interleaved trials.
- Doing: the first build and run of `research_bound`; the bound section of the research note.
- Next:
  1. Post the bound and the first numbers in `docs/research/indexing.md`; commit and push.
  2. Baseline at 100k on this Mac: `strata_engine index` (current writer) and Tantivy's
     builder, one thread, light hold.
  3. `research/indexing/invert.cc`: the inverters A/B (hash-append as today, slices, radix
     partition + counting sort) producing identical postings; five interleaved trials.
  4. Reordering at flush (cheap orderings against docid order and bisection) by segment bytes.
  5. Copy-merge feasibility: bytes in full blocks against tails; the format-13 list.
  6. The design that reaches 4.5x, ranked by distance to it; where it replaces existing code.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-17. Nothing measured yet.
