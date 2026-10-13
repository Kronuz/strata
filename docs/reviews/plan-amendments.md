# strata review: PLAN amendments

- Date: 2026-10-13.
- Reviewed: the amendments German set on 2026-10-12, at strata `80eadc2`:
  - `e0d5497`: Goals 1–4, the query-speed ratchet, the "Every gate" additions, Tracks P
    and D, `docs/DISTRIBUTION.md`, `docs/TESTING.md`, and BENCHMARKS' Quickwit and
    scale rows.
  - `eb9452f`: Goal 5, the memory ratchet, ARCHITECTURE's "Memory" section, and
    BENCHMARKS' allocation and reclamation rows.
  - `d87783a`: the "Ratchets" section on all four axes, BENCHMARKS' row classes, and
    Track P's footprint items.
- Checked against:
  - PLAN's phase gates, BENCHMARKS, ARCHITECTURE, REPLICATION, and FEASIBILITY.
  - `docs/design/phase-3.md` at `b2ffe24`.
  - The Phase 2 gate report and the gate review's third re-check
    (`docs/reviews/phase-2-gate.md`), including its coverage and per-stratum
    measurements.
  - Detent's `docs/LIBRARY_BOUNDARIES.md` and retirement runtime at Detent `f857876`,
    and `cluster` at `ca82aad`.
- Checked by reading. The goals and their target values are German's decisions and are
  not reopened here.

## Verdict

The amendments are consistent in direction, and every goal and ratchet value stands.
31 findings need edits, none of which changes a goal or a target:

- A1–A10: places where the documents disagree with each other.
- A11–A18: definitions a ratchet needs before it can be checked.
- A19–A25: conflicts with the Phase 3 design, REPLICATION, and the Detent coordination
  rules.
- A26–A28: baselines, tools, and machines the ratchets need that do not exist yet.
- A29–A31: the order of Phase 3's work against its ratchets.

A19 and A30 land before the Phase 3 segment format is written, and A23's coordination
before split step 5's API is fixed. A31 is German's call. The rest land before the
Phase 3 gate's run plan is written, or at the point their own text names.

## Consistency

1. **A1. The interval's side.** The "Ratchets" pass a ratio by "the 95% bootstrap
   interval, whose upper bound must meet the target" (`PLAN.md:44-46`). For the ≥
   targets (indexing throughput, `62-68`) the upper bound is the lenient side: a row at
   0.97× [0.94–1.01] passes ≥ 1.0×. Disk sizes are exact and take no trials. Edit: "the
   interval's bound on the target's side meets it: the upper bound for ≤ targets, the
   lower bound for ≥ targets; disk rows are single exact measurements."
2. **A2. The pipe floor.** PLAN's Phase 2 gate subtracts "the ~155 µs pipe floor"
   (`PLAN.md:181-182`); the "Ratchets" say "floor-subtracted for queries" without a
   definition (`44-45`); the Phase 2 gate measured 55 µs, the smallest time of any
   engine and query in the session (`bench/phase2/gate.py:39-41, 84`). Edit: BENCHMARKS
   defines the floor as `gate.py` computes it, the report names the trial it came from,
   and PLAN's Phase 2 text drops the constant.
3. **A3. Trial counts.** The "Ratchets" require at least five interleaved trials for
   every ratio (`PLAN.md:44-45`). The Phase 2 indexing row ran three, under the gate
   review's rule (G5). Edit: the five-trial rule applies from the Phase 3 gate.
4. **A4. BENCHMARKS rule 3 against the "Ratchets".** Rule 3 makes a row a win when its
   interval excludes 1.0 and its median effect is at least 3% (`BENCHMARKS.md:14-16`).
   At Phase 3's ≤ 1.0×, a row at 0.99× [0.98–1.00] passes the "Ratchets" and fails
   rule 3. Edit: rule 3 governs before/after claims (Track P) and rows whose target is
   to beat a baseline; ratchet rows pass by the "Ratchets" rule.
5. **A5. The allocation row.** BENCHMARKS makes heap allocations per query "must-win
   from Phase 3" (`BENCHMARKS.md:94`), while PLAN's Phase 3 row is a property, "no
   allocation per matching document on any query path" (`PLAN.md:75`), and malloc counts
   do not compare with a JVM's. Edit: a property row with no baseline. Phase 3:
   allocations per query, regressed on the number of matches over each class's queries,
   have zero slope. Phase 6: zero in steady state on the four named paths (`76`).
6. **A6. The query-speed statistic.** BENCHMARKS measures each class's "mean and p99
   latency at concurrency 1, and throughput at saturation" (`BENCHMARKS.md:87`). The
   ratchets apply another statistic: the geometric mean, over a class's queries, of
   per-query ratios of each query's fastest of 10 runs (`bench/phase2/gate.py:47-71`).
   Edit: BENCHMARKS names that statistic for the ratchet rows, and states whether p99 at
   concurrency 1 and saturation throughput are report rows or ratchet rows from a named
   phase.
7. **A7. Phase 7's margin.** "By the same margin" (`PLAN.md:60, 77, 90`) does not say
   which margin. Edit: the numbers per axis against Elasticsearch and Quickwit.
8. **A8. Fuzzing scope.** PLAN fuzzes each "decoder and parser added in the phase"
   (`PLAN.md:95-96`); TESTING fuzzes "one CPU-hour per decoder per gate"
   (`TESTING.md:16`). Edit: one rule in both: an hour at the gate's commit for each
   decoder or parser added or changed in the phase, and every other decoder's corpus
   replayed as a test (A16).
9. **A9. FEASIBILITY.** "A miss is reported with numbers and a cause"
   (`FEASIBILITY.md:17-19`) contradicts must-win ratchets on four axes, and its effort
   table (`25-37`) has no lines for Tracks P and D or test parity. Edit: a miss on a
   ratchet row blocks its gate, and the table gains those lines.
10. **A10. The serving-memory row.** BENCHMARKS makes serving memory must-win against
    Lucene, Tantivy, Xapiand, Elasticsearch, and Quickwit with no phase
    (`BENCHMARKS.md:93`); PLAN sets Lucene and Tantivy from Phase 3, and Elasticsearch,
    Quickwit, and Xapiand at Phase 7 (`PLAN.md:75-77, 394-397`). Edit: the row names each
    baseline's phase.

## Measurability

11. **A11. The reference configuration per axis.** "Each engine in its fastest
    configuration", with "the smaller of their figures" for memory and disk
    (`PLAN.md:41-44`), and Lucene "with and without document reordering"
    (`BENCHMARKS.md:44-46`): when Lucene has two configurations, whose disk and memory
    figures count is undefined, and the JVM's heap setting decides Lucene's memory
    figure (915 MB anonymous in the Phase 2 run, `gates/phase-2.md:115`). Edit:
    - Each gate fixes the baseline set (Lucene, Lucene with reordering, Tantivy), with
      versions pinned.
    - Each axis's reference is the best figure in the set: per class for speed, smallest
      for memory and disk.
    - strata meets every axis in one configuration.
    - Lucene's heap is the smallest `-Xmx` at which its query run stays within the noise
      band of its fastest, stated in the report.
12. **A12. Section counterparts.** PLAN names Lucene's files only, and the term
    dictionary's as `.tim` plus `.tip` (`PLAN.md:81-83`); the Phase 2 report also counts
    `.tmd` (`gates/phase-2.md:123`). Tantivy's counterparts are unnamed. strata places
    some data where Lucene does not: term-level competitive pairs, 3.99 bytes per term,
    sit in the dictionary, while Lucene keeps impacts in `.doc`. Rule 1 excludes term
    lists and exact lengths (`BENCHMARKS.md:9-10`), and says nothing of Phase 3's docids,
    versions, and `pk_filter` sections. Edit: one table of counterparts per section for
    both engines, with Lucene's metadata files (`.tmd`, `.fdm`, `.nvm`) included and
    Tantivy's `.idx`, `.pos`, `.term`, `.store`, and `.fieldnorm` named; sections compare
    as placed; and the table lists the strata sections excluded from the comparison and
    those counted only in the whole index.
13. **A13. Keys and versions.** Phase 3 adds a key term per document (the host's own
    unique term), a `versions` section, `pk_filter`, and a monotone docids section
    (`phase-3.md:64-76, 168-170`). On the 5M corpus the key terms alone add on the order
    of 100 MB to a dictionary whose Lucene counterpart is 40.2 MB (Phase 3 design review,
    D16). An engine run without an id field pays none of it. Edit: the disk, memory, and
    indexing rows state one keyed configuration for every engine: a unique id term per
    document (Lucene `StringField`, Tantivy `STRING`) with versions as doc values or fast
    fields, or no keys anywhere. The replace-heavy row needs keys in every engine.
14. **A14. Memory measurement.** The Phase 2 figures come from `.scratch/serve_mem.py`,
    which samples `RssAnon` and `RssFile` every 200 queries in one run. RSS omits the
    page cache, so Xapian serves its 15.4 GB index at 7 MB (`gates/phase-2.md:107-120`),
    and the indexing budget differs per engine (Lucene 2,047 MB, Tantivy 2 GB, `83-84`).
    Edit: BENCHMARKS defines both rows:
    - Each engine in its own cgroup v2; `memory.current` and `memory.stat` (anon, file,
      file_mapped) sampled at a fixed interval.
    - Serving: idle with the index open, and under the gate query workload; peak and
      steady state; page cache charged alike to every engine.
    - Indexing: one buffer budget for every engine, stated.
    - Five trials.
15. **A15. Indexing measurement.** The rows say "single-thread and multi-thread
    throughput" (`PLAN.md:64`) without the corpus size, merge settings, or end state.
    Phase 2 measured 1M documents in one segment; at 5M strata falls from 9,660 to
    6,510 docs/s (`gates/phase-2.md:98-101`). "Before merging" (`PLAN.md:215-216`) is
    undefined. Edit:
    - The full 5M corpus, with 1M as a report row.
    - The same buffer and merge settings in every engine (Phase 3 design, T3).
    - Throughput to a committed index under each engine's own merging.
    - The N-thread row on the VM's 8 vCPUs, its reference measured in the same session.
    - "Before merging" is the index each engine leaves after that row, with its segment
      count recorded; "after merging" is each force-merged to one segment.
16. **A16. Coverage.** "The phase's code" (`PLAN.md:98-99`) is undefined. TESTING's
    "fuzz corpus replayed as a test" (`TESTING.md:29`) needs corpora in the repository;
    today they live in `.scratch`, and no fuzz target builds from CMake.
    `docs/testing/parity.md` does not exist yet (`TESTING.md:37-41`). Edit:
    - The phase's code is every file and function added or changed in the phase.
    - Coverage runs `llvm-cov` over the unit tests, `parity`, `verify` on a sample index,
      and the checked-in corpora replayed; fuzz targets and replay tests build from
      CMake.
    - The inventory's Phase 2 rows land before the Phase 3 gate, as TESTING states.
17. **A17. Goal 2.** "Every known technique that applies is evaluated"
    (`PLAN.md:16-20`) has no bound. Edit: it is checked against Track P's list, which
    grows by review; each item ends in a `docs/perf/` entry with its four-axis table,
    kept or removed.
18. **A18. Scale statements for single-node components.** Every gate states each new
    component's behavior at the target cluster scale (`PLAN.md:100-101`), but the scale
    contract's rows are cluster-wide (`DISTRIBUTION.md:12-22`); an engine component has
    only "groups per node ≥ 1,000" to answer to. Edit: the contract gains per-node rows
    the core is judged against:
    - Indexes per node: at least 1,000, most of them idle.
    - Memory per idle index.
    - Open files and mappings per node (Linux's default `vm.max_map_count` is 65,530).
    - Commit barriers per node per second.
    - Flush, merge, and reclamation work per node.

## Conflicts

19. **A19. Document reordering against Phase 3's ordinal order.**
    - Goal 1 references Lucene with document reordering (`PLAN.md:12-13`), and P1
      evaluates reordering at merge time (`259-260`). Adding a configuration to the
      baseline set can only lower each reference.
    - The Phase 3 design fixes ordinal order to docid order in every segment
      (`phase-3.md:17-18`), which forbids reordering, and builds on it: the docids
      section is monotone and searched for docid → ordinal (`72-74`), and the tie rule
      skips equal bounds within a segment (`216-220`).
    - Edit, before the Phase 3 format freezes: either the format carries a per-segment
      flag and a second docids kind, a permutation with a sorted docid → ordinal index,
      with the equal-bound skip applying only to docid-ordered segments, so P1 can
      reorder at merge time; or PLAN states that strata does not reorder and where its
      margin comes from instead. Recommended: the flag. Merges are where reordering
      pays, and flushes keep D15's properties.
20. **A20. The cluster's layers.** REPLICATION gives placement and group membership to
    one "meta group" and gossip to Xapiand (`REPLICATION.md:29-30, 44-48`);
    ARCHITECTURE gives placement to Xapiand (`ARCHITECTURE.md:12`); DISTRIBUTION has a
    root authority, partitioned placement groups, and SWIM membership built by Track D
    (`DISTRIBUTION.md:34-50`), and names no home for them, while the core "has no
    network, consensus, schema, or geometry code" (`ARCHITECTURE.md:22`). Edit:
    REPLICATION and ARCHITECTURE adopt DISTRIBUTION's layers, and DISTRIBUTION names
    where each layer's code lives: a strata target beside `strata::group`, Xapiand, or a
    library.
21. **A21. Track C's step 10.** "Step 10 (keep or retire `cluster::Raft`) and strata's
    placement group are one decision" (`PLAN.md:235-236`), but Goal 3 has decided that
    strata's authority is `consensus` (`25-26`; `DISTRIBUTION.md:81`). Edit: strata uses
    `consensus` for every authority (the root, the placement groups, the shard groups);
    step 10 decides only whether `cluster` keeps `cluster::Raft` for its other consumers,
    by LIBRARY_BOUNDARIES' measurements.
22. **A22. Membership's library and transport.** DISTRIBUTION takes membership from
    "the stack built for Detent" (`DISTRIBUTION.md:106-108`), and D2 runs SWIM over
    Detent's peer transport (`PLAN.md:311-313`).
    - That stack has no gossip library. LIBRARY_BOUNDARIES' map holds `metrics`,
      `ledger`, `completion`, `journal`, `consensus` with the peer transport, and
      `cluster` with the bus, the legacy Raft, and `liveness.h`.
    - The peer transport carries Raft over TLS on persistent connections with control
      and bulk lanes. SWIM probes random peers every period, which at 100,000 nodes needs
      a bounded connection cache or a datagram path.
    - Edit: DISTRIBUTION names the membership library and its transport, states
      connections and messages per node per second at 100,000 nodes, and routes any
      change to `consensus`'s transport or to `cluster` through the Detent work
      (ARCHITECTURE, "Libraries", rule 3).
23. **A23. Goal 5 and `ledger`.** Phase 3's memory budget switches to `ledger` at split
    step 5 (`PLAN.md:233-234`), and LIBRARY_BOUNDARIES puts "budgeted retirement" in
    `ledger` and makes `completion` depend on `ledger`.
    - Detent's retirement runtime destroys owners on a dedicated cleanup worker thread
      (Detent `src/detent_distribution/retirement.cc:150-167`). Goal 5 forbids a
      collector thread in strata and in every library it uses (`PLAN.md:30-37`).
    - `consensus`'s retirement runs in budgets on its owner (`cluster`
      `consensus/retirement.h:32-42`) and fits Goal 5.
    - Edit: the retirement runtime is a target of its own that neither `completion` nor
      strata links, and strata takes only `ledger`'s capacity and permit core. This goes
      to the Detent work before step 5's API is fixed, with the coordination D20
      requires. Goal 5's audit of `completion`, `journal`, and `consensus` lands before
      Phase 3R adopts them.
24. **A24. Track D's dependencies.** Track D runs "from Phase 3" (`PLAN.md:303`), but
    D1's simulator is shared with Track C's harness (C5), and D3's placement groups need
    C1 (membership changes) and C2 (multi-group operation), whose code starts after
    split steps 1–2 (`229-232`). Edit, per item: D1 is designed with C5, under one owner;
    D2 follows A22's transport decision; D3 follows C1 and C2; D5 and D6 follow Phase 3's
    manifests and snapshots; D7 lands before Phase 7.
25. **A25. Track gates and "Every gate".** Tracks C, D, and P gate per item
    (`PLAN.md:246, 301, 328-330`) without saying whether "Every gate" applies, and P4 and
    P13 add decoders. Edit: each track item meets "Every gate" for the code it adds.

## Missing pieces

26. **A26. Baselines, tools, and machines that do not exist yet.**
    - Lucene with reordering: the harness has `lucene-10.4.0` and its single-thread
      variant only (`bench/phase2/g5.sh:13, 19`).
    - Quickwit (`BENCHMARKS.md:41`).
    - The allocation-counting build (A5) and the cgroup memory tool (A14).
    - `benchkit`, which PLAN's Phase 0 and BENCHMARKS name as the trial runner and JSON
      store (`PLAN.md:107-108`; `BENCHMARKS.md:107-109`). The gates run
      `bench/phase2/g5.sh` and `gate.py`, which score queries only.
    - A bare-metal Linux machine with NVMe and pinned frequency (BENCHMARKS rule 4).
    - A cluster for Track D's 30-node gates and Phase 7's 300-node rows
      (`PLAN.md:328-330`; `BENCHMARKS.md:102`).

    Edit: PLAN lists each with the phase that first needs it: Lucene with reordering,
    the allocation build, the memory tool, and a runner for all four axes at Phase 3's
    start; the bare-metal machine before Phase 3R; the cluster before Track D's first
    gate.
27. **A27. Simulator fidelity.** D1 simulates at least 100,000 nodes and 10^7 shards
    (`PLAN.md:308-310`) on unnamed hardware, and 10^7 Raft groups at full fidelity do not
    fit one machine. Edit: D1 states its fidelity tiers (membership and placement at full
    scale, shard groups as abstract state machines, full Raft groups at a stated smaller
    scale) and the hardware it runs on.
28. **A28. TESTING's tools.** Mutation testing names no tool or score
    (`TESTING.md:21`). The cross-engine differential compares counts with Tantivy's and
    Lucene's (`15`), while `gate.py` checks Tantivy's only
    (`bench/phase2/gate.py:52`). Edit: TESTING names the mutation tool and the score
    reported per gate, and `gate.py` checks Lucene's counts too.

## Ordering

29. **A29. Phase 3's work against its ratchets.** The Phase 3 gate holds every
    ratchet's Phase 3 row, on multi-segment indexes before and after merging
    (`PLAN.md:215-216`). The Phase 2 gate report's "Open" and its third re-check give
    the gaps (medians; query ratios floor-subtracted):

    | Row | Phase 2 | Phase 3 target | Work | Lands before |
    |---|---|---|---|---|
    | Query classes | 34 of 40 command-class rows have an upper bound above 1.0×; intersection 1.21–1.45×, negated up to 1.43×, phrase 1.35–1.39×, intersection_union up to 1.40×, frequent terms at `TOP_100` 1.47× | ≤ 1.0× every row, against a reference that gains Lucene with reordering | Intersection and negated: P2, P7, P9. Phrase: P8, and P5 with its disk cost. intersection_union: C7. Frequent terms: the frontier cost (C15), P3. Every class: P6, since per-segment term lookups (650 ns each in `termdict`) and scorer setup multiply with segments | The gap table re-measured with Lucene with reordering, at Phase 3's start; each item behind C14's `verify` |
    | Indexing, one thread | 0.84× Tantivy at 1M | ≥ 1.0× the faster | A profile of the Phase 2 writer; D18; Phase 3's own costs (key lookups, versions, the docid sort, merges, commits) on the same rows | D18's measurement, before buffer code |
    | Indexing, N threads | Not measured | ≥ 1.0× the faster | D18's K buffers | Same |
    | Serving memory | 1.33× Tantivy's resident index pages | ≤ 1.0× | P15: region verification reads whole 64 KiB regions on first touch | The region size, in A30's format |
    | Indexing memory | 0.90× at 1M; 5.89 GB in one segment at 5M | Within the budget plus 10%, and ≤ 1.0× | D17's budget | Budget code |
    | Allocations | Not recorded (C15) | None per matching document | The allocation-counting build; ARCHITECTURE's per-query arenas | Query-path changes |
    | Term dictionary | 2.53× | ≤ 1.0× | P12, with key terms per A13 | A30's format |
    | Postings | 1.017× a `.doc` without reordering | ≤ 1.0× the reference's | An encoding change, or P1 (A19) | A30's format |
    | Whole index | 0.99× | ≤ 1.0× with keys, versions, `pk_filter`, and docids | All of the above | A30's format |

    Edit: PLAN's Phase 3 section lists the Track P items committed to Phase 3 with the
    rows each targets, and the gap table is re-measured, with Lucene with reordering,
    before the first of them lands.
30. **A30. One format version for Phase 3.** Phase 3's format work: `pk_filter`,
    `versions`, and the monotone docids section (`phase-3.md:64-76`); the stored
    section's largest frame (C9); A19's docids kind; the dictionary's redesign (P12);
    the postings change the disk row needs; and P15's region size, which the table of
    contents records. Merges re-encode postings, and C1 puts format decisions before
    merge code. The Phase 3 design still calls this format 10 (`phase-3.md:29, 64`),
    while Phase 2 ends at 11. Edit: PLAN orders all of them into format 12, frozen before
    merge code, with its fixture and a test that format 11 is rejected.
31. **A31. Phase 3R's dependency.** Phase 3R starts after C1–C4 and builds on Phase 3's
    deterministic core (`PLAN.md:332-336`); the Phase 3 gate now also holds every
    ratchet row, and PLAN does not say whether Phase 3R waits for all of it. Edit: PLAN
    states it. Recommended: Phase 3R starts once Phase 3's lifecycle rows pass (power
    loss, digest, disk full, fuzzing, sanitizers, coverage), and the ratchet rows gate
    Phase 4. This is German's call, since it decides which work runs in parallel.

## Unaffected

These parts stand as written:

- Goals 1–5 and every target value in the "Ratchets".
- The rule that every technique reports all four axes and may not push another axis
  past its ratchet.
- Track P's list, the footprint items P12–P15, and Track D's items, subject to A24.
- DISTRIBUTION's scale contract, its "From Xapiand" decisions, and its open decisions.
- TESTING's layers, coverage targets, and clean-room rule for Xapian's tests.
- ARCHITECTURE's "Memory" section and the reclamation rule.
