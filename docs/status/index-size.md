# index-size

- Design: none (a Track P footprint item: index size and merging during indexing);
  experiments in docs/perf/index-size.md, each written before it runs.
- Branch / worktree: perf/index-size at /Users/kronuz/code/Kronuz/strata-wt/index-size,
  from origin/main at 4c9ebfd.
- State: implementing (E1, the byte breakdown, no code change).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Evidence read from the gate VM (raw files `~/bench/results/index-n1017-t4-{1,8}`,
    `wamp-t3-{20000,500000}-0`): strata 4.85 GB in 9 segments at one thread and 8.02 GB in
    14 at eight; Lucene 2.82 GB and Tantivy 3.07 GB, both force-merged to one segment
    inside the timed run (`BuildIndex.java` `forceMerge(1)`, `build_index.rs` merge of
    every segment, unless `NO_MERGE=1`). At 500k strata's single segment is 329.7 MB
    against Lucene's 320.9 MB and Tantivy's 352.7 MB (T3, append-only).
- Doing: E1. 100k corpus copied from the VM (read-only there; T3 counts the VM disk's
  writes, so nothing here writes to that disk while `t3-targeted.sh` runs).
- Next:
  1. E1: strata at 100k, one segment and N segments with merges off, per section; the
     directory against the manifest's files (the indexing-gap ledger's H4).
  2. Lucene and Tantivy at 100k per file kind, on the VM after T3_DONE.
  3. The biggest cause first, one experiment at a time, each written in the perf file
     before it runs.
- Blocked on: nothing.
- Coordination: perf/indexing-gap owns eight-thread scaling (its E0 lists H4, the bytes
  of unreclaimed sources; this item measures it and shares the result in
  docs/perf/index-size.md); perf/inverter owns the one-thread hot loop. Format changes go
  to the format 13 designer as notes, never landed here. Research notes to read as they
  land: research/codecs (docs/research/codecs.md), research/indexing.
- Findings folded in: none yet.
- Last verified: 2026-10-17. Nothing built or run yet.
