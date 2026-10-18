# index-size

- Design: none (a Track P footprint item: index size and merging during indexing);
  experiments in docs/perf/index-size.md, each written before it runs.
- Branch / worktree: perf/index-size at /Users/kronuz/code/Kronuz/strata-wt/index-size,
  from origin/main at 4c9ebfd.
- State: implementing (E1's 5M split running; E2 kept at 1M, its 5M confirmation next;
  E3 written and coded, unbuilt).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Evidence read from the gate VM: strata's T4 bytes are 9 unmerged segments against
    baselines force-merged inside the timed run (30e2beb).
  - E1 at 100k and 1M (e79b9c9): one segment at 100k is 1.028× Lucene; postings 1.095×
    and terms 1.152× are the sections above their counterparts; splitting 100k into 15
    segments costs +18.5% (dictionaries 2.26×, positions +8.2%); at 1M with 21 flushes the
    directory holds 511 MB of merged-away sources beside a 637 MB index (H2).
  - E2, the engine reclaims after its commit (6248c3d): at 1M, 511,283,280 bytes freed in
    0.57 s, the directory equal to the manifest's files.
  - Format 13 notes N1 to N4 (5986033): Rice-coded postings tails (0.959× Lucene's `.doc`
    at 100k, against 1.014× with Lucene's fold), implied positional headers (3% of
    positions), Rice positional tails (1.9%).
- Doing: E1 at 5M (current main, the gate budget, merges on): the live and unreclaimed
  bytes of the T4 configuration, in `.scratch/e1/m5-2056`.
- Next:
  1. E1 at 5M: live against unreclaimed bytes; force-merge a copy in docid order for the
     1-segment comparison at this commit.
  2. Lucene and Tantivy at 5M with `NO_MERGE=1` (bytes, segments), so T4's disk row can be
     like for like; flag the methodology to the lead.
  3. E3's A/B at 100k (light hold): background merges against inline, beside Tantivy.
  4. The postings and positions projections (N1 to N3) on the reordered 5M merge, F11's base.
- Blocked on: nothing.
- Coordination: perf/indexing-gap owns eight-thread scaling and also edits
  `tools/strata_engine.cc` (its E1 parse threads); this branch's engine changes sit after
  the read loop (reclaim, merge executor), so they merge cleanly. Its E0's H4 is this
  item's H2, measured here. perf/inverter owns the one-thread hot loop. research/indexing
  (docs/research/indexing.md) prototypes inverters, reordering at flush, and a copy-merge:
  its compact buffers would cut flushes (segments) at the same budget, and its winners are
  built here as they land. research/codecs has not pushed yet. Format changes go to the
  format 13 designer as notes (docs/perf/index-size.md, "Notes for the format 13 designer").
  Merge-speed changes wait for layers: `merge.hh` and `writer.hh` are rewritten on
  phase4/layers-refactor, whose merge keeps the per-posting vectors.
- Findings folded in: none yet.
- Last verified: 2026-10-17. Release build at 4c9ebfd and 6248c3d (macOS x86_64, Apple
  clang 17); `strata_engine index` runs as listed above. No ctest run yet.
