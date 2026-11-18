> Status: HISTORY (2026-10-17) research done; its findings fed the inverter in prospect.

# research-indexing

- Design: none yet; the research note is `docs/research/indexing.md` (the bound, the survey,
  each idea with its prototype numbers, the ranked recommendations). Prototypes live in
  `research/indexing/`, built with `-DSTRATA_RESEARCH=ON`.
- Branch / worktree: `research/indexing` at `/Users/kronuz/code/Kronuz/strata-wt/research-indexing`,
  from `origin/main` at `4c9ebfd`.
- State: done. The note (`docs/research/indexing.md`) holds the bound on both machines, today's
  attribution, the survey, seven ideas with prototype numbers (winners and losers), the design
  with where it replaces code, and the recommendations ranked by distance to 4.5x. Every prototype
  output was byte-identical to today's writer's. Target (owner, PLAN "Design target"): 4.5x the faster baseline's
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
    copy merge's floor at 24.6 (dictionary walk and build, one byte copy), 20x; section 5.3
    (`3ba7c9c`).
  - Variant D measured on both machines: about neutral against C (193 against 200 ns a token on the
    VM, 199 against 169 on the shared Mac); killed as a refinement, its per-partition sort kept
    (`d3a1d4a`). Thread scaling rerun with scratch reuse: C 135 to 96 ns a token from one to
    eight threads on the Mac, 158 to 134 on the VM, with the encoder serial (`aae98b4`, `91ae26d`).
  - The quiet VM bound: the cache-resident path at 56 ns a token (72 s over the corpus) against
    the Mac's 49; the one-thread row is reachable on both with 43 s of margin on the VM (this
    commit).
- Doing: nothing; finished. The host was shared from 20:45 on with the index-size and inverter
  engineers' single-thread runs; the quiet numbers are the Mac's bound runs 3 and 4, its first
  inverter and thread runs, and the VM's third bound run (once every other hold had cleared);
  the other reruns are 15 to 40% slower and consistent in ranking. Three runs that overlapped
  each other (my chained jobs raced on the hold check) are discarded.
- Next (for whoever picks the note up; the researcher's work is complete when the two VM runs
  land):
  1. The performance engineers: recommendation 1 (the tuple-stream, term-partitioned inverter,
     section 6, replacing `tokens_document`, `index::evaluate`'s document copy, `segment_writer::add`'s
     table and streams, and `index::flush`'s per-run inversion) and recommendation 2 (`flush_from`'s
     per-term cost from 1.7 µs to about 0.4 µs; `research_invert`'s `flush_from` line measures it).
  2. Format 13's designer: the five items in section 5.3 for copy merges; then the copy merge
     itself, checked byte-for-byte against `merge_segments`.
  3. The architect: MinHash order at flush as a preset (5.1) against the permuted `docids` cost.
  4. Open measurements: why group and verify scale 1.8x at eight threads (2 MiB pages, a
     gather-free layout), and the four stage cuts named under "Is 4.5x reachable?".
- CI: `ci.yml` does not run on `research/**` pushes; dispatched by hand on `2f693d1`
  (https://github.com/Kronuz/strata/actions/runs/39096279166): seven of eight jobs green (Linux
  x86_64 gcc and clang, Linux arm64, macOS x86_64 and arm64, FreeBSD, TSan); `linux-asan-ubsan`
  cancelled at the shared workflow's 60-minute cap before the slow tests, the pre-existing chore
  of `docs/status/README.md` (Queued, item 9), not this branch's doing: it changes no engine code
  (the root CMakeLists gains the `STRATA_RESEARCH` option, off, and `research/indexing/` is built
  only with it).
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-17, on this Mac (i9-9980HK) and the Linux gate VM (same CPU): bound runs 3
  and 4 (Mac, quiet) and 1 and 3 (VM, shared and quiet); inverters at 100k, five trials, on both;
  threads, three trials, strict holds, on both; orders on both; merge cost (Mac); baseline, three
  trials (Mac). Raw outputs in `.scratch/*.txt` of the worktree (untracked).
