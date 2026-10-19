# durability-modes

- Design: `docs/design/durability-modes.md`, Part A (the consistency spectrum and replication),
  revision 1 (revision 0 at `8f8b043`; what changed per finding is the design's "Changes in
  revision 1"); Part B (memory residency and retention) outlined at its end, designed after A's
  re-check. Reviews: `docs/reviews/durability-modes-design.md` (the architect's preliminary
  review, 2026-10-19, `7ffc16b`: REJECT for first steps 1, 3, 4, and 5; DM-R1 to DM-R18, C1 to
  C5) and `docs/reviews/durability-modes-security.md` (`5117745`: APPROVE WITH CONDITIONS; DMS-1
  to DMS-9, C1 to C7).
- Branch / worktree: `design/durability-modes` at `~/code/Kronuz/strata-wt/durability`, from
  origin/main `755a918`; origin/main merged at `c6df449` (`734949e`) and `1365cd1` (`be64292`).
- State: designing (revision 2 in progress)
- Done criteria (set by the lead; contributors tick, never edit): none set yet; the architect's C1
  proposes K1 to K6 (Next 2).
- Done:
  - First session (2026-10-18, `704c2cc`): read REPLICATION, DISTRIBUTION, PLAN (Phase 3R, Track
    C, process rules), ARCHITECTURE, track-c.md, the system review (SYS-4, SYS-8, SYS-10, SYS-11),
    placement-strategies, workload-profiles, rocksdb-survey (K2, Q3, Q7), small-indexes D1 to D3;
    verified the Xapiand claims against MIT own code (no GPL read); outlined D1 to D11.
  - The owner's scope recorded (`321c333`, `5e8f6f0`, `1fa9336`; kept below).
  - Second session (2026-10-18):
    - Merged origin/main (`c6df449`).
    - Wire fields claimed against `design/peer-transport` at `38a6c6f` (D6 to D9): `VoteResponse`
      56 to 64 bytes, `AppendRequest` 104 to 136 without entries, `AppendResponse` 96 to 112 (112
      to 128 compacted), coalesced records 48 to 64; kinds 1, 6, 7 unchanged; strata::group's host
      kinds 256 to 271 and 512 to 527 (design D16).
    - Consensus's repair path (`core.h:597-609` at `61149a2`) checked against a segment-mode
      follower's promotion: five findings, design D8 (the transfer-first refusal at `:600`, the
      base bound at `:602` against a checkpoint bound only by the delivery cursor at `:562`, the
      delivery and persistence gates at `:599`, the epoch at `:606`, mid-entry positions).
    - Found the leader never lowers a follower's matched index (`core.h:957-969`; Track C D15,
      `track-c.md:730-731`), which a voter that lost acknowledged entries needs: design D5 adds
      the `forgetful` reply flag and the rewind.
    - T1 measured with a counting IO over `PosixIO` and an interposed syscall counter (macOS, no
      hold; `bench/durability/results/2026-10-18-t1-macos.txt`, `0b407c9`): create 6 durability
      calls, each commit 4; create plus the first commit 10 `F_FULLFSYNC`s and 7 `fsync`s per tiny
      index, the same over 10 indexes.
    - T2 sizing at 100,000 documents under a light hold (macOS, one thread;
      `bench/durability/results/2026-10-18-t2-macos.txt`, `0b407c9`; dry run on 20,000 first):
      apply, flush, merge, and commit 41.3 to 42.3 µs per document with one flush and 124.9 to
      130.1 µs with a 16 MiB buffer (37 segment files); log append 1.02 to 1.10 µs; receiving and
      verifying the shipped files 0.72 to 0.77 and 1.48 to 1.62 µs. Derived (`bench/durability/
      arith.py`, `results/arith.out`): a segment-mode follower at 0.042× and 0.020× a
      document-mode follower's CPU; a three-voter group at 0.370× and 0.350× (estimates without TLS).
    - Prior art fetched and cited: Raft dissertation §3.8, §6.4.1, §10.2.1, §11.6 (its LaTeX at
      `0c10ea1`); VR Revisited §4.3, §5.1; Kafka's replication design and KIP-966; OpenSearch
      segment replication and issues #2212, #3988, #6369, #9027; CockroachDB follower reads,
      leader leases, and its unsafe Raft-log setting; TiKV's removed `sync-log` (TiDB 5.0 notes);
      Cosmos DB's consistency levels; PostgreSQL, MongoDB, and Elasticsearch durability settings;
      RAMCloud; Scuba and its fast restarts; ScyllaDB and Cassandra TWCS; Dynamo and ScyllaDB 6.0;
      HLC and CRDTs; Linux errseq; boot identities on Linux, macOS, and FreeBSD.
    - Part A written in full: D1 to D16, the threat model, the fault model per preset, the
      relation to small-indexes' D3, changes to other documents, prior art, T1 to T14, Q1 to Q7,
      first steps; Part B outlined (this commit).
  - Third session (2026-10-19), revision 1:
    - Pulled (equal to origin); merged origin/main `be64292` at `1365cd1`; re-verified every
      strata citation the merge moved (`index.hh`'s apply, refresh, commit, install, commit
      protocol, retention, recovery; PLAN's background-work rule and Phase 3R).
    - Read both reviews, the architect's probes (`forgetful_model.py`, `core_probe.cc`,
      `rescore_t2.py` in `strata-wt/review-durability/.scratch/review-durability/`), the owner's
      answers, format 13's reservations on `phase4/format13-on-main` `43ee403` (F13-R41, bit 16,
      F13-R43, F13-R51, F13-R52), and `research/no-stalls` `467b5b3` (its sections 2 and 6).
    - The model (`1544d82`, `8aa4cbb`, `cd34ba1`, `ba4363b`):
      `bench/durability/model/forgetful_model.py` extends the architect's probe (`--compat`
      reproduces it exactly) with failed elections, each voter's `commit_durable` and the floor
      and fence checks, the owner's Q3 rule (`fplus`), and revision 1's recovery election
      (`round`); `forgetful_check.cc` is its C++ port with voter symmetry, 128-bit (or, where
      memory was short, 64-bit) hash compaction, and swarm walks. Python and C++ agree on every
      configuration compared (`results/model-crosscheck.out`).
    - DM-R1's fix as ruled fails inside the model at five voters: the `fplus5` schedule (15 steps)
      loses a `memory` write while at most two voters are forgetful at once, and the checker's
      swarm walks found `fplus5b` (26 steps) on their own; revision 0's every-voter-heard rule
      fails too (`heard5`; a 25-step fence found by swarm walks).
      Revision 1's D6 counts f + 1 forgetful answers at the recovery election's own term, which
      terms make exact, and lets any voter with the count start one (a three-voter stall found
      while checking liveness, `live3`). Results (`results/model-exhaustive.out`,
      `results/model-swarm.out`): at three voters `round` holds within the model to depth 12
      (48,190,011 states; two writes, terms to 4, three OS crashes) and loses no `quorum` write or
      floor beyond it to depth 11 (32,162,295 states; 22,599,017 when every voter must answer), and
      `fplus` holds there to depth 12 (31,776,242). At five voters `round` holds within the model
      to depth 8 (67,817,052 states) and in 20,000,000 kind-first swarm walks to depth 30
      (445,216,259 steps), and beyond it loses no `quorum` write or floor at depth 8 (79,154,748
      states) or in 20,000,000 walks (443,382,315 steps; 82 `memory` losses, as allowed).
      Two-write walks (10,000,000 each, to depth 40) found nothing under `round` and nothing under
      `fplus` either, so the design counts them as no evidence.
    - DM-R12's estimate redone (`arith.py`, `results/arith.out`): SHA-256 through OpenSSL's EVP
      at 0.5 or 1.5 GB/s (the development Mac's i9-9980HK has no SHA extensions, read from
      `sysctl`), TLS and the stack at 1 to 2 ns per byte, and D18's verifier: a three-voter group
      0.38× to 0.43× of document mode's CPU, a five-voter group 0.25× to 0.32×, a follower 0.05×
      to 0.11× (estimates); the cadence bracket 1.135× at rf = 3 and 0.767× at rf = 5; D9's gap
      2,323 ms (`quorum`) and 2,163 ms (`memory`).
    - Revision 1 written (`8aa4cbb`, `cd34ba1`, `ba4363b`, `57f9e9b`, `fc60285`, and this
      commit): every finding answered (the design's "Changes in revision 1"); D17 (file sets on
      the snapshot transfer) and D18 (segment mode's verifier) new; T15 new; Q9 (DMS-4's choice)
      and Q10 (D6's correction to Q3's rule) for the owner; "For Detent" new.
    - Small-indexes' re-check folded in (the lead's message; `perf/small-indexes` `2e9949e`, read
      with `git show`, its C3), this commit: D1 takes SI-R19's entry (a named generation's
      snapshot), SI-R24's thread rule (whoever waits leads; only unwaited checkpoints on the pool),
      SI-R20 as SI-R26 corrects it (a failed checkpoint restores its index at the recorded
      generation through `Repair` from the intact journal, not forgetful; a failed barrier stays
      D5's restart and same-boot rule), and SI-R25's creation replay (strata's `Create` with the
      index's UUID, decided by the log, a foreign UUID refused with nothing removed); D16 gains
      `Create`; T16 new; first step 5 and "How small-indexes' D3 relates" updated. D5's
      failed-barrier wording, the summary, and "For Detent" now say the same-boot rule decides,
      and kind 1's value rule admits a recovery request from any candidate with the count (D6's
      liveness fix). Checked: `index.hh:335-344`, `508-509`, `2255-2261`, `2270-2273`,
      `2311-2314`, `2328-2330`; `files.hh:63`; `pool_executor.hh:1-4`; threadpool
      `task_group.hh:223-228` at `628679a`; `track-c.md:432-450`; the probe outputs
      `owner_waits_on_pool.out` and `stale_incarnation.out` in
      `strata-wt/review-small-indexes/.scratch/si-review/`.
- Doing: revision 2 (fourth session, 2026-10-19), folding in the architect's re-check of
  revision 1 (`4321849`: REJECT for steps 1 and 3; DM-R19 to DM-R23, C6 to C14) and the security
  re-check (`c87385e`: APPROVE WITH CONDITIONS; DMS-10 to DMS-13, C8 to C12). Done so far: origin/main
  `026ee10` merged (`457528b`) and the strata citations re-pinned; the model extended and its
  schedules and searches run (`df75a1e`); C7's calibration row (`e71c810`); the design's text for
  every finding (this commit). Left: the deep runs still going (six voters from `six`'s step 37 at
  depth 6, the mark at three voters to depth 12 and five to depth 8, the walks' calibration), then
  D6's revision-2 evidence table, the ledger's final state, and the push.
- Next:
  1. The lead starts the architect's re-check of revision 1 (design changes) and the security
     reviewer's re-check of DMS-1 to DMS-9; the re-check reads D6 against
     `bench/durability/results/model-*.out` and reruns the model (`bench/durability/model/`).
  2. The lead writes done criteria; the architect's C1 proposes K1 (step 2 with tests on Linux,
     macOS, and FreeBSD and Detent's suite), K2 (step 1 with T9's rows, DM-R2's and DM-R6's
     included), K3 (step 3 with T6's mutants and T12), K4 (ASan, UBSan, and TSan, and coverage at
     TESTING's targets), K5 (CI green on all jobs and Detent's suite at each library commit), K6
     (step 4 only after the full design review).
  3. The owner answers Q9 (segment mode's independent check) and Q10 (D6's correction to Q3's
     rule); the design follows the recommendations meanwhile, and neither blocks steps 2, 6, or 7.
  4. First steps 2, 6, and 7 may start now (the architect's ruling); 1, 3, 4, 5, and 8 after the
     re-check; nothing lands before the full design review.
  5. The lead routes: D17's outgoing slot rule per destination node to Track C's D13 with SR22's
     C16; "For Detent" to Detent's `SHARED_LIBRARY_DECISIONS.md`; "Changes to other documents" on
     `main`; T1 on Linux and FreeBSD; small-indexes re-pins its citations of this design (SI-R26)
     to this commit, since D1's lines moved again.
  6. Design Part B as its own file after A's re-check.
- Blocked on:
  - The re-check (a format, wire, and durability change: steps 1, 3, 4, 5, and 8 wait for it).
  - The owner's Q9 and Q10 (not blocking: the design follows the recommendations).
  - Detent's agreement before step 4.
  - SYS-8 (turn log or per-group journal): not blocking; the per-group journal refuses `acks:
    written` (D3).
- Findings folded in: DM-R1 to DM-R18, DMS-1 to DMS-9, the architect's C1 to C5, and the security
  C1 to C7, per the design's "Changes in revision 1" table; the owner's Q1 to Q6 and Q8;
  small-indexes' C3 (SI-R19, SI-R20, SI-R24, SI-R25, SI-R26). One proposal argued against, with
  evidence: DMS-7's "`forgetful` only with `success`" (D16).
- Last verified (2026-10-19): `quiet check` exited 0, then 2 (light holds: `qexp-e2-m1`,
  `qexp-e2-m9`, `fm4-rss-ab`, `fm4-rss-ab2`), so every model run used `nice -n 19` with at most four
  jobs (a fifth, started by a queue runner's miscount, was stopped within a minute), and the large
  ones were stopped or shrunk when the host neared its memory and swap limits. The model: Python
  and C++ agree on every configuration compared and reproduce the architect's counts
  (`results/model-crosscheck.out`); the forced schedules (`results/model-schedules.out`); the
  exhaustive runs and swarm walks (`results/model-exhaustive.out`, `results/model-swarm.out`).
  `arith.py` regenerated (`results/arith.out`). Every `file:line` the design cites checked to lie
  in its file at the cited commit. No strata, consensus, or journal build or test: this branch
  changes no library code; the model and the probes build apart from strata's targets, so CI does
  not build them. CI: `ba4363b` green on all 8 jobs (run 39255024724: Linux x86_64 GCC and Clang,
  arm64 GCC, ASan and UBSan, TSan, macOS x86_64 and arm64, FreeBSD x86_64); `57f9e9b` and
  `fc60285` are docs-only and skip CI; this commit's run starts at its push, for the lead to
  watch.

## Scope added by the owner (2026-10-18), and where Part A answers it

- "Choose your side": every system on strata lets the user pick availability or consistency per
  index, with per-request overrides, on ONE replication mechanism (consensus per shard), as Azure
  Cosmos DB's five levels on one protocol. Answered by D10 (levels, presets, overrides) and D13.
- Partition-tolerant writes, a later preset designed now so the log and apply contract leave room
  for it: minority-side writes into a side log, merged on healing deterministically (LWW on a
  hybrid logical clock, or CRDTs), opt-in, its conflict window stated, the determinism contract's
  and the digest's needs, and the cost if never used. Answered by D12.
- Leaderless replication (Dynamo/ScyllaDB style) rejected as a second mechanism, with reasons.
  Answered by D13.
- The failover gap, bounded (election timeout, lease handover) per preset. Answered by D9.
- Memory-resident preset (owner, verbatim), for Scuba-like workloads: replicated, but memory
  first. A majority holds the batch in memory (the replicated ack); segments are built in RAM and
  searchable at once; a background tier flushes them to disk for persistence and longer retention.
  State the loss window (a correlated majority loss before the disk tier catches up) and how a
  restart recovers (from peers, from the disk tier, or shared memory). Part B (outlined); A's D5,
  D6, and D7 are built to serve it.
- Retention (owner, verbatim): Scuba drops data past a retention limit; ScyllaDB drops by TTL,
  often by time (its time-window compaction drops whole SSTables). strata needs a model for it.
  Two levels to compare:
  1. Segment level, in strata: time-windowed segments (the `log` profile is time-sorted); a merge
     policy that never merges across windows; dropping a window is deleting whole segments, O(1)
     with no rewrite; per-document TTL as a filter until its window drops.
  2. Shard level, above strata: time-based shards or indexes rolled over by window
     (Elasticsearch's rollover), dropped whole.
  Recommend one, or both with a clear split, without forcing into strata what belongs in a
  separate foundations library. Part B (outlined).
- No stalls: background work judged under load, never write stalls, coordinated with
  `research/no-stalls`. Answered by D14.
- One vocabulary across the stack (strato, shale, the Scuba-like and Kafka-like systems).
  Answered by D10; Q6.
- Live tuning: which bounds change without a restart (Detent) and which do not. Answered by D15.
- Owner's answers (2026-10-19): the recommendations, all of them.
  - Q1: Xapiand's default preset is `replicated`.
  - Q2: the default `sync_interval` is 100 ms.
  - Q3: `balanced`, with the architect's DM-R1 fix: a `forgetful` flag on pre-vote requests
    (kind 6), and D6's grants only after f + 1 forgetful voters are heard.
  - Q4: ship merged segments in segment mode now; revisit with copy merges.
  - Q5: offer `memory` at rf = 1, reversing rocksdb-survey's Q3 ruling (no new durability tier).
  - Q6: one vocabulary: retire `local`, rename `nearest` to `prefix`.
  - Q8: don't wait. The version-2 wire proceeds on its own schedule; this design's fields join
    through the N and N-1 version range (SYS-3) rather than holding version 2.
- Lead's calls on revision 1's questions (2026-10-19), under the owner's standing "use the
  recommendations"; the owner may override:
  - Q9: option 2. D18's sampled rebuild on every segment-mode follower by default; a
    document-mode replica per index (`verify: replica`) and `strict` stay one setting away.
  - Q10: option 1. The ruled f + 1 count starts a recovery election, and the count is taken again
    among that election's answers at its own term (D6, D16), which holds at every replication
    factor in the model. Corrected by revision 2 (DM-R20): it holds at every fixed membership the
    model reached, and in Track C's replacement windows only when the decision counts a majority
    of the candidate's membership, which revision 2 adopts (f + 1 at every odd size, so the call's
    steady-state rule is unchanged).
  - Q9's call (option 2) rested on revision 1's "a rare single corrupt segment, found later";
    revision 2 restates Q9 with each option's measured residual for the owner (DM-R21).
