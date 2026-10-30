> Status: HISTORY (2026-10-19) research done; the prototype code stays on research/no-stalls.

# research-no-stalls

- Design: none; the research note is `docs/research/no-stalls.md` (the bound, the survey, each
  idea with its prototype's numbers, the recommendation). Prototypes live in
  `research/no-stalls/`, built with `-DSTRATA_RESEARCH=ON`; the RocksDB driver against a pinned
  release (`STRATA_ROCKSDB_SRC`, `STRATA_ROCKSDB_BUILD`).
- Branch / worktree: `research/no-stalls` at `/Users/kronuz/code/Kronuz/strata-wt/no-stalls`,
  from `origin/main` at `734949e`.
- Brief (owner, 2026-10-18): RocksDB's write stalls are the failure mode strata must not inherit;
  slow merges slow everything at the worst possible time, like garbage collectors. Deliver the
  bound, the survey, at least three ideas with one that breaks a common assumption, each with a
  prototype measured on strata at sustained ingest against RocksDB under the same load, write
  and query latency over time (p50/p99/p999 per window).
- State: done. The note holds the bound (section 2), the survey from pinned sources (3), the
  harness and calibration (4.0, 4.1), the core matrix over three rounds (4.2), the pause pairs
  and the extras (4.3), the many-index matrix (4.4), the losers (5a), the recommendation (6),
  findings for others (7), and how to reproduce (8). Raw outputs stay in the worktree's
  `.scratch/runs/` (untracked).
- Done criteria (set by the lead; contributors tick, never edit): none set.
- Done (commits on `research/no-stalls`, pushed; CI green at `c72f0f1`, run 39206012974, all
  eight jobs; later commits change only the research drivers, docs, and one additive hook):
  - `31dd085` the harness: `research_ingest` (strata: one writer at an offered rate into N
    indexes on one pool, a reader over owned snapshots, per-window p50/p99/p999 of write
    sojourn, write service, and query time beside segments, allowance, excess, pending merges,
    merged bytes, flush time) and `research_rocks` (the same loop over RocksDB v11.8.1 with L0
    files, pending compaction bytes, stall time); additive `index.hh` hooks (`segment_view::bytes`,
    `take_snapshot()`, `schedule_merge()`, `policy()`).
  - `cb14936` the note's bound and survey; `core.sh`, `many.sh`, `summarize.py`.
  - `85c2050` the harness method and the calibration findings (4.0, 4.1).
  - `b9dddfd` the ideas (5) and the first pause pair.
  - `4613018` feed-forward admission and the adaptive refresh (`control=feedforward|refresh`).
  - `a9448fd` the core matrix results (4.2).
  - `5f1e594` `publish_finished()` (finished merges installed without a flush), the host
    scheduler's own 250 ms tick; the first many-index rounds kept as `runs/many-v1`.
  - This commit: 4.3, 4.4, 5a, 6, 7, 8 and this ledger.
- Results, in one place (5,000 docs/s, a merge budget of 1.5 bytes per flushed byte, 180 s,
  rotated rounds; raw in `.scratch/runs/`):
  - RocksDB leveled: stalled 120 of 180 s (writes of 1.3 s, admission cliffs); universal:
    19 to 39 s with a 12.6 s stop; triggers off: L0 to 64, gets unchanged.
  - strata today: no stall, segments 5 → 90, query p50 0.1 → 1.5 ms and p99 0.2 → 3.6 ms,
    a 150 to 167 s drain of queued merges at the end; its real stall is the inline flush
    (80 µs/doc on the writer, sojourn p99 80 to 530 ms by host speed).
  - I1 count ramp: no cliff but locked at its floor (52 to 60% admitted); feed-forward form:
    service p99 under 8 ms, no lock-in, 56% admitted, count still climbs (a refresh a second
    makes a segment a second).
  - Adaptive refresh: all admitted, 42 segments against 90, query p99 1.2 against 3.6 ms,
    45% less flush CPU per document; visibility 3.5 s instead of 1.
  - Cheapest-first host scheduling: 35% fewer segments for the same bytes (one index), 23%
    fewer across 32, no backlog (0.8 to 12 s drains against 150 to 248 s).
  - I2 width 8 → 30: 30% fewer merge bytes for the same end state.
  - I3: a merge parks at a checkpoint 35 to 69 ms (p50) to 225 ms (max) after a request;
    today 95% of merge time sits in gaps of 64 ms or more (max 1.7 s).
  - `hot` (merges charged to the reader): better through 100 s, worse after; its fixed cap
    fired for every index at once and overrode heat. Inconclusive; the next experiment named.
  - The rate limiter's bucket banks nothing: 68 to 74% of the budget used (RocksDB 85 to 91%).
- Next (for whoever picks the note up):
  1. The designer: section 6's items 1 to 3 as one design (the background flush, the host's
     merge scheduler, the refresh-then-admission debt response with the backlog unit and the
     named limits), reviewed by the architect; `publish_finished()`, `schedule_merge()`,
     `take_snapshot()` and `policy()` are the engine surface it needs and can land with it.
  2. The performance engineer: the per-block checkpoint (item 4), the limiter (item 7), the
     width preset (item 5) once 1 to 3 exist; the inverter's owner: the batch drain and the
     version map's growth (section 7).
  3. The format 13 designer: copy merges (item 6) with the checkpoint in the same loops.
  4. A second many-index experiment for `hot`: a heat-weighted cap, per-index counts and query
     times in the output, skewed ingest.
- Blocked on: nothing.
- Findings folded in: none (no reviews yet).
- Last verified: 2026-10-19 01:45 on this Mac (i9-9980HK, macOS 15); every run in the note
  under a light hold; after 00:27 every run behind `quiet wait --light`. One pause pair before
  that overlapped another agent's strict hold (`scale-threads-vm`, 00:21 to 00:36) and was
  redone. The host's speed drifted about 2.7× over the night; A/Bs are within rotated rounds.
