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
    partition, group, verify, encode, write), five interleaved trials; measured on the quiet host
    and written into the note's section 2 with the 8-core bandwidth and I/O limits (`56776ab`, then
    this commit).
  - `research/indexing/invert.cc`: inverters A (today), B (slices), C (partitioned) over one token
    stream, byte-identical segments, 340 / 139 / 118 ns per token; `flush_from` found at 1.7 µs a
    term (section 3).
  - `research/indexing/baseline.sh`: today's engine and Tantivy at 100k on this host (18.2 s against
    11.5 s, medians of three).
- Doing: the document-order prototype (`order.cc`, running) and a repeat of the bound run; the
  dictionary-build cost; then the design section and the eight-thread prototype.
- Next:
  1. Reordering at flush: MinHash orders against curid order and bisection, by segment bytes
     (`order.cc`, running).
  2. The dictionary build's share of `flush_from` (a `termdict::builder` stage in `bound.cc`).
  3. Copy-merge: the design and its format-13 list from the merged 5M segment's df histogram
     (96.5% of postings in the 103,578 terms with df >= 128; 1.05% of those in tails).
  4. The eight-thread prototype of C (partitions across threads) under a strict hold of minutes.
  5. The design section, ranked by distance to 4.5x, with where it replaces existing code.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-17, on this Mac (i9-9980HK), one thread; the VM has not run these.
