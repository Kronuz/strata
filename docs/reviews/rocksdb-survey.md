# strata review: the RocksDB survey

- Date: 2026-10-15.
- Reviewed: `docs/design/rocksdb-survey.md` at `c0d7091` (branch
  `design/rocksdb-survey`, worktree `strata-wt/rocksdb`, not pushed).
- Checked against:
  - PLAN: Goals 1, 2, and 5, Ratchets, Track P, and Phase 3's gate. BENCHMARKS rules
    1–5 and "Equal memory".
  - REPLICATION (the acknowledgement modes, and Xapiand's WAL and RESTORE mappings).
  - `docs/design/phase-3.md` (D11, D12, D17, D18, Merges, Scale); `layers.md`;
    `phase-4.md` (D12, D14, D16); `workload-profiles.md`; `limits.md`.
  - My Phase 3 lifecycle review (condition 7), my Phase 4 and layers reviews.
  - The code at `c0d7091`: `include/strata/{index,merge_policy,format,searcher}.hh`.
- Checked by reading the files above. Nothing was built or run.
- The external citations were not opened: this session had no network. They were
  checked only for internal consistency and against what I already know (R7).

## Verdict

**APPROVE WITH CONDITIONS.** The survey is the right document at the right time.

- It treats RocksDB as a baseline to beat, not a backend to embed (X12).
- It describes RocksDB from its documentation and keeps its code out of strata.
- It applies the one-implementation rule consistently. No rejected item survives as an
  option beside strata's own mechanism.
- X2 and X3 get the replicated core's rule exactly right. Anything that changes logical
  content at merge time makes content depend on merge scheduling, which differs between
  replicas, so the digest (D12) and T2 diverge. Expiry and rule-based deletion must be
  applied operations at a log position.

Three of its descriptions of `main` are wrong or incomplete:

- Snapshots do not exist in code, so the concurrent-read rows cannot run (R1).
- K2's pipelined acknowledgement lets a get see a write that power loss then erases;
  the design must say whether that is allowed (R2).
- K4 mixes a Phase 3 obligation with a new mechanism (R3, and the K4 ruling).

The benchmark plan needs a memory metric that treats mmap fairly (R5), one RocksDB
configuration per value class rather than per row (R6), and limits stated for the
standalone write path (R9).

## Required changes

1. **R1. Snapshots are designed, not built.** The survey marks "Snapshots: Exists, core,
   per-core snapshot handles" (`rocksdb-survey.md:185`).
   - In code, `index.hh` has no snapshot type and no accessor that pins a segment set.
     `segs_` is a vector that `refresh()`, `merge()`, and `commit_files()` mutate on the
     owner thread (`index.hh:294-297, 300ff, 1590`). `searcher.hh`'s `segment_snapshot`
     (`19-29`) is a pair of pointers for one search; it pins nothing.
   - Phase 3's per-core handles published by message (`phase-3.md`, Retention) are
     design text.
   - Consequences:
     - The mapping row changes to "Partial: designed (Phase 3, Retention), not built".
     - `readwhilewriting` and every row with concurrent readers wait for snapshot
       publication.
     - "Checkpoints: Exists" is right (`checkpoint()` exists).
   - This is the same class of finding as K4: Phase 3 design text with no code behind
     it. It joins the K4 ruling's list.

2. **R2. K2's visibility rule** (`rocksdb-survey.md:207`). "Apply proceeds while the
   turn's barrier is in flight; a transaction is acknowledged when its turn's barrier
   returns." Between apply and the barrier, real-time get (D13) reads applied state.
   - So a get can return a write that power loss then erases. It was never
     acknowledged, but a reader saw it.
   - RocksDB with `sync=1` has the same window between memtable insert and fsync, unless
     the reads wait.
   - K2 states which rule applies:
     - **Durable reads:** reads see only positions at or below the last completed
       barrier. Real-time get then needs a "durable position" next to the applied one.
     - **Documented:** the window is documented as REPLICATION's `local` semantics
       applied standalone.
   - The power-loss test (T1) classifies a read of a write that was later lost under
     whichever rule K2 picks.

3. **R3. Split K4.** It has two parts with different owners:
   - **(a) Background merges with a rate limit** are Phase 3's design
     (`phase-3.md:294-297`). They are owed to Phase 3's gate by the K4 ruling below.
   - **(b) Merge-debt backpressure** is new: stage 1 slows when the bytes the policy
     wants merged pass a bound. It is a K item, measured like one.
   - For (b), the survey states three things:
     - **Determinism.** Backpressure changes only timing, never verdicts. Apply must not
       reject for debt, as Phase 3 already forbids for memory (`phase-3.md:251-253`).
     - **Scope.** The rate limit and the debt bound are per node, not per index, at
       1,000 indexes per node (`phase-3.md:371-372`: "never unbounded per index").
     - **What "slows" means for the replicated core.** Under groups, stage 1 cannot
       delay applying committed entries without stalling the log. So backpressure acts
       at admission (the leader's proposal rate), never at apply.

4. **R4. K7's fold order.** Folding delta records at read and at merge keeps digests
   equal across replicas only when the fold is a function of the logical content alone.
   - That holds for commutative operators: integer add, min, max.
   - For a non-commutative operator (append, or replace-if-newer), the fold follows log
     order, never merge or segment order. Each delta carries its log position, and a
     merge folds partial runs only between adjacent positions.
   - K7 states the operator set and this rule, and T2 checks it with merges between
     operands.

5. **R5. Equal memory measures the cgroup, not RSS** (`rocksdb-survey.md:322-330`).
   - strata reads through mmap, and resident file-backed pages count in its RSS. RocksDB
     reads into a block cache (anonymous, also in RSS) and through `pread` into the page
     cache (not in its RSS).
   - Calibrating "strata's peak RSS equals RocksDB's" therefore charges strata's page
     cache and not RocksDB's.
   - The calibration target is the cgroup's `memory.current` (anon plus file), the same
     number the plan already caps with `memory.max`. RSS stays a report column.
   - `bench/calibrate_rss.sh` gains a cgroup mode, or a sibling script.

6. **R6. One RocksDB configuration per value class, not per row**
   (`rocksdb-survey.md:264-268`).
   - A sweep that picks a different compaction style, cache, filter, and block size for
     each row builds a RocksDB no deployment runs. A database takes one configuration
     for its workload mix.
   - Required: one configuration per value-size class (100 B, 400 B, 4 KiB, 64 KiB),
     each chosen by the sweep as fastest over that class's rows by geometric mean. It is
     recorded, and used for every row of the class.
   - strata likewise runs one preset per profile. This matches the rule set for Lucene in
     Phase 4 (one index configuration per engine for all classes).
   - Per-row sweeps are reported, not gated.

7. **R7. Citations are pinned.**
   - Wiki pages change without versions. Each `[wiki]` tag records the date the survey
     read it, or a link to the page revision.
   - Each "Published effect" number names the RocksDB version it was measured on, where
     the source says.
   - Release-note claims cite the `HISTORY.md` entry at a tag, not `main`: HyperClockCache
     as the default block cache from 10.7.0, the automatic variant in 8.7.0, and
     11.12.0's date.
   - 11.12.0 (2026-10-07) is eight days old today, so it is not yet eligible under the
     survey's own two-week rule. The text says so, or cites it only as informational.
   - I could not verify the published numbers offline. Whoever pins the references
     re-reads each against its source.

8. **R8. K1's counters cost nothing when off.** "Compiled in and read by the engine"
   (`rocksdb-survey.md:206`) puts increments on every get and every probe.
   - Counters sit behind a compile-time switch, off in the builds that record ratchet
     rows, as Xapiand's `L_*` categories are.
   - Or, if they stay compiled in, an A/B under the layers review's ≤ 1.03 rule shows
     their cost.
   - Either way, the counter build is the one that explains rows, and the off build is
     the one that records them.

9. **R9. Limits on the standalone write path.** K2 carries transactions in the turn log.
   - The survey states the largest key, value, and batch it accepts, from the mechanism
     that bounds them: the journal's entry and fragment bounds.
   - It compares them with RocksDB's documented maximum key and value sizes in a
     `limits.md` row.
   - The 64 KiB-value rows must fit by construction. A batch of 100 at 64 KiB is 6.4 MB,
     above the 1 MiB command default that `limits.md` records for groups. So either the
     standalone path's bound is stated larger, or the row's batch size follows from the
     bound.

10. **R10. Text.**
    - "Universal compaction: Exists (equivalent)" (`161`). Tiered merging is the same
      family, but strata has no space-amplification trigger;
      `deletes_pct_allowed` (`merge_policy.hh:22`) bounds deleted documents, not total
      bytes over live bytes. The row says "similar family; no space-amplification
      trigger", and M4 decides whether one is needed, as a preset of the one policy.
    - "WAL recovery modes: Exists, one mode" (`135`): correct, and the right outcome.
      It cites D11 by its review commit.

## Rulings

### K4: merges inline, without a rate limit

**Confirmed.**
- Phase 3's design says merges "run in the background, rate-limited, and publish
  through the next refresh and commit" (`phase-3.md:294-297`).
- As built, `refresh()` runs `maybe_merge()` on the caller's thread after the flush
  (`index.hh:294-297`), and so does `commit()` (`385-387`).
- `maybe_merge` loops `run_merges` until the policy asks for none (`372-378`).
- Nothing in `include/strata/` throttles merge I/O; no rate limiter exists.
- `flush_threads` (`170-173`) parallelizes inversion only.

**It is not new, and it blocks Phase 3's gate.** My Phase 3 lifecycle review carries
this as condition 7: "Background merges (D18) … before background merges run in any
build Phase 3R tests, and before the Phase 3 gate". The lifecycle gate report lists
T2's racing-merge schedule and TSan over merge threads as waiting on it
(`docs/gates/phase-3-lifecycle.md`, T2 and T6 rows). Three reasons it cannot simply be
carried past the gate:

1. **Phase 3's indexing rows would measure the wrong writer.** T4's single- and
   multi-thread rows, write amplification (T3), and indexing memory would be recorded
   with merges inline on the writer. Background merges change throughput, peak memory,
   and segment counts. N4 in the Phase 4 review, and the layers refactor's A/B baseline,
   both anchor on the commit where Phase 3's rows are recorded. That commit must hold
   the final writer.
2. **The scale statement depends on it.** Merges are "scheduled across indexes from one
   per-node budget, never unbounded per index" (`phase-3.md:371-372`). Inline merges on
   each index's owner thread cannot be scheduled across indexes.
3. **The lifecycle rows have holes until it lands.** T1 with deletes arriving during a
   merge, T2's merges losing documents while they run, D11's carry-over at install, and
   TSan over merge threads all wait for it.

Where it is carried: Phase 3, before its ratchet rows are recorded. It is the same slot
the layers review gave D17's flush charge (R5 there). It lands with:
- condition 7's tests;
- a per-node rate limit;
- merges that publish through the next refresh, as `phase-3.md:296-297` says.

The alternative is to amend `phase-3.md` to inline merges and drop the scale claim.
That is the owner's call, and I recommend against it, for reason 2.

R1's snapshot publication is the same kind of gap. It is owed before Phase 3R's
read-consistency work, not before Phase 3's ratchet rows, since those rows have no
concurrent readers. It is listed here so both gaps are tracked in one place.

### Q1–Q10

"Owner" marks a question that changes product semantics, a dependency, or PLAN; my
recommendation is attached. The others I rule.

| # | Whose | Recommendation |
|---|---|---|
| Q1 Blind writes | **Owner** | **No.** A version derived from the log position breaks Xapiand's `_version`, which increments by one per document, and adds a second versioning mechanism beside the per-key counter. Measure M2 first. If resolution dominates, make the one mechanism cheaper: Phase 3's create-only flag (I2) for ids the leader knows are new, and a sorted batch probe per transaction against each segment's key range and filter. A profile without conditional writes (`log`) already skips the version map. |
| Q2 Key-ordered merges | Architect | **Yes, as a preset, if measured.** Key order for keyed layouts without text is index sorting (D14) by the key, through the permuted docids kind (P1). It becomes the `document` profile's default only if it wins `readseq`, `seekrandom`, and YCSB E without losing get-by-docid or `readrandom` by more than the noise band; get-by-docid then pays the permuted kind's sorted index. One mechanism, two sort keys. |
| Q3 Process-crash tier | **Owner** | **Use REPLICATION's `none`, not a new tier.** `ack=none` already exists for groups ("acknowledged before persistence", `REPLICATION.md:126`), and Xapiand's async WAL maps to it (`164`). Standalone `none` is the turn log written without waiting for its barrier. That is the same mechanism, so RocksDB's WAL-without-sync rows compare against it once K2 lands. The owner decides whether standalone exposes `none`. |
| Q4 Thread scaling | Architect | **Both, with one must-win row per side's best arrangement.** The must-win row is N cores, each engine in its fastest arrangement: strata's N shards on one node against RocksDB's better of one DB with N threads and N DBs. "N threads into one strata index" is a report row, since stage 1 is sequential per shard by design. |
| Q5 Allocator | Architect; the dependency is the **owner's** | **Each engine with its fastest supported allocator, stated.** RocksDB with jemalloc if that is faster. strata runs with the system allocator and with jemalloc. If strata's fastest needs a non-system allocator, shipping that dependency is the owner's decision, given the minimal-dependencies rule. Until then strata's reference row is the system allocator. |
| Q6 Range deletes independent of size | **Owner** | **Not a requirement now; X4 stands.** The `log` profile expires by whole-segment drops, which already cost O(segments). K8 reports cost per deleted key. Reopen only if a named profile needs ranges too large to enumerate within a transaction's budget. |
| Q7 Bulk ingestion | **Owner** (priority) | **Yes later, as one command, after format 13.** REPLICATION already names "a bulk-import command whose segments the leader builds" for RESTORE (`REPLICATION.md:168`), and Phase 8 migrates through DUMP and RESTORE. It is that command, not a second path. The leader builds the segment and assigns docids. Replicas verify it whole and check its digest contribution, as `install` does. It is a separate design after format 13. |
| Q8 Value separation | Architect, coordinated with Track D7 | **Conditional yes.** If M4 shows merges of large values losing the write-amplification axis, value separation becomes the `stored` plugin's layout for records above a size threshold (a preset), replacing frames for those records. It is not a second store. It overlaps DISTRIBUTION's blob-store decision (D7, Xapiand's volumes), so its design names which of the two holds large values. |
| Q9 macOS barriers | Architect | **Do not patch the baseline.** If RocksDB has no supported setting that issues `F_FULLFSYNC`, its durable macOS rows are not equal-durability rows: they are reported with that note and do not gate. Durable rows gate on Linux and FreeBSD. Find out from RocksDB's documented options first. |
| Q10 Which gate | **Owner** (a PLAN change) | **A separate gate for the `document` profile, after Phase 4's,** once K1–K3, format 13's `keys`, and R1's snapshots exist. Phase 4's gate is already the largest in PLAN. Key-value must-win rows are a new ratchet family, which PLAN's Ratchets has to name. |

## Which work starts when

| Item | Starts | Waits for |
|---|---|---|
| K4(a): background merges, rate limit, condition 7's tests | Now, in Phase 3 | Nothing; it blocks Phase 3's ratchet rows |
| R1: snapshot publication | Now, as Phase 3 design debt | Nothing; before Phase 3R's read-consistency work |
| K1: harness, RocksDB engine (code), generator, counters behind a switch (R8) | Now | Build and runs wait for the shared VM; R5–R7 before the first recorded row |
| M6: filter microbenchmark (reference filters benchmark-only, never linked) | Now | Nothing |
| M1, M2, M3, M5, M9 | After K1 | — |
| K2: standalone turn log | After Track C's C2 journal format freezes | R2 and R9 in its design |
| K3: bounded, ordered key scans | With format 13 | R1 for scans across refresh |
| K4(b): merge-debt backpressure | After K4(a) and M4 | R3's statement |
| K5: key-ordered merges | After format 13 and P1's permuted merges | Q2's measurement |
| K6: frame size, then dictionaries, then the cache | M3 first | Each step kept only if it wins |
| K7: counters and operands | Track P, after field-update generations | R4 |
| K8: key-range delete | After phase-4 D16 | — |
| The `document` profile's gate | After Phase 4's gate | Q10 (owner), K1–K3, R1 |
