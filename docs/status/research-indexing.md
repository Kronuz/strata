# research-indexing

- Design: none yet; the research note is `docs/research/indexing.md` (the bound, the survey,
  each idea with its prototype numbers, the ranked recommendations). Prototypes live in
  `research/indexing/`, built with `-DSTRATA_RESEARCH=ON`.
- Branch / worktree: `research/indexing` at `/Users/kronuz/code/Kronuz/strata-wt/research-indexing`,
  from `origin/main` at `4c9ebfd`.
- State: researching; the note's bound, attribution, ideas, design, and ranked recommendations
  are written (sections 2 to 7); cross-platform numbers from the Linux VM and the thread
  scaling rerun are in flight. Target (owner, PLAN "Design target"): 4.5x the faster baseline's
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
  - `research/indexing/baseline.sh`: today's engine and Tantivy at 100k on this host, quiet (10.5 s
    against 6.22 s, medians of three; the 4.5x target at 100k is 1.38 s).
  - `research/indexing/order.cc`: document orders at flush; MinHash k=2 saves 3.4% of postings plus
    positions in 48 ms against bisection's 5.8% in 5.9 s (`0406611`).
  - The dictionary builder measured at 161 ns a term (not the flush's cost); C on 1/2/4/8 threads
    at 115/105/92.5/85 ns a token with the encoder serial, segments identical.
  - The merged 5M segment's df histogram from the gate VM: 96.5% of postings in the 103,578 terms
    with df >= 128, 1.05% of those in tails; the copy-merge design and its format-13 list rest on it.
  - The design (section 6) and recommendations ranked by distance to 4.5x (section 7).
  - Variant D (partition as tokens arrive, hash8 for short tokens, per-partition term sort with a
    256-way merge), byte-identical to A after fixing hash8's length tag, which collided with byte 7
    of 8-byte tokens and was caught by the byte verification (`46a759d`).
  - The gate VM's first numbers beside the Mac's (`9fa1c9b`): the ranking holds, the cache-resident
    path loses least to virtualization; the one-thread row on the VM is the narrow one (`34daa82`).
  - `research/indexing/mergecost.cc`: today's two-segment merge at 497 ns a posting against the
    copy merge's floor at 24.6 (dictionary walk and build, one byte copy), 20x; section 5.3.
- Doing: quiet reruns. The VM's first bound, inverter, and order runs (in the note, marked shared)
  overlapped the index-size engineer's E1 run in the VM; the second local thread run, the
  A/B/C/D run with D, and the VM thread run overlapped each other and E1 (two of my chained jobs
  raced on the hold check) and are discarded as measurements, though every output was identical.
  Queued, each waiting for the previous: the VM bound and inverters (after E1's hold clears at
  21:43), the local A/B/C/D and threads, the VM threads.
- Next:
  1. Fold the VM numbers and the thread rerun into the note (sections 2, 3, 5.4).
  2. Prototype the two refinements of 5.2 (partition as tokens arrive; per-partition term sort
     with a 256-way merge) if time allows; otherwise they stay as projections.
  3. A two-segment copy merge on the sample against `merge_segments`, once format 13's designer
     takes the list in 5.3.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-17, on this Mac (i9-9980HK): bound run 4, inverters at 100k (five trials),
  threads (three trials, strict hold), orders, baseline (three trials); the VM runs are in flight.
