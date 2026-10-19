# durability-modes

- Design: `docs/design/durability-modes.md`, Part A (the consistency spectrum and replication),
  revision 2 (revision 0 at `8f8b043`, revision 1 at `1efb815`; what changed per finding is the
  design's "Changes in revision 2", then "Changes in revision 1"); Part B (memory residency and
  retention) outlined at its end, designed after A's re-check. Reviews:
  `docs/reviews/durability-modes-design.md` (the architect's preliminary review, 2026-10-19,
  `7ffc16b`: REJECT for first steps 1, 3, 4, and 5; DM-R1 to DM-R18, C1 to C5; its re-check of
  revision 1, `4321849`: REJECT for steps 1 and 3; DM-R19 to DM-R23, C6 to C14) and
  `docs/reviews/durability-modes-security.md` (`5117745`: APPROVE WITH CONDITIONS; DMS-1 to DMS-9,
  C1 to C7; its re-check of revision 1, `c87385e`: APPROVE WITH CONDITIONS; DMS-10 to DMS-13, C8 to
  C12).
- Branch / worktree: `design/durability-modes` at `~/code/Kronuz/strata-wt/durability`, from
  origin/main `755a918`; origin/main merged at `c6df449` (`734949e`), `1365cd1` (`be64292`), and
  `457528b` (`026ee10`, format 13 on main).
- State: designing (revision 3 in progress: the architect's re-check of revision 2, DM-R24 to
  DM-R28 and C15 to C22)
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
  - Fourth session (2026-10-19), revision 2:
    - Read both re-checks of revision 1 (the architect's at `4321849`, the security reviewer's at
      `c87385e`) and their probes, without editing their worktrees: `forgetful_r1.py`,
      `forgetful_members.py`, `forgetful_check_r1.cc`, `restart3-schedule.out`,
      `members-schedules.out` (`six`, `six_loss`), `rescore_r1.out` in
      `strata-wt/review-durability/.scratch/review-durability/r1/`; `create_replay.py`,
      `d18_residual.py`, `liar_model.py`, `liar_schedules.out` in
      `strata-wt/review-durability-sec/.scratch/durability-sec-r1/`.
    - Merged origin/main `026ee10` (format 13 landed) at `457528b`; re-pinned every strata
      citation that moved (`index.hh`'s install, apply, commit, commit protocol, retention,
      recovery, create, `uncreated`, `clear_uncreated`, load paths, digest; `files.hh`'s UUID and
      count bound) by mapping each old range to the new file's text, and read each new range.
    - The model (`df75a1e`): `forgetful_model.py` and `forgetful_check.cc` gain same-boot restarts
      (`--restart text|persist`), torn tails, retried rounds, membership in the log through Track
      C's replacement window, and the decision's count (`--count f1|major`), each off unless asked
      for; the C++ checker replays schedule files and searches from a schedule's prefix, with
      raw-byte keys when symmetry is off. Every revision-1 configuration keeps its count in both
      checkers (eleven C++ configurations against revision 1's binary, four Python ones against
      `model-crosscheck.out`), and Python and C++ agree on the new features, six voters included
      (`results/r2/crosscheck.out`). The forced schedules reproduce both reviewers' traces:
      `restart3` (and `torn3`, built here, a torn same-boot tail with no OS crash) lose a `memory`
      write under revision 1's restart rule and are blocked by the mark; `six`, `six_loss`, and
      `grow6` (the growth half of rf = 5's window, built here) lose one under f + 1 and are blocked
      by the majority count. The searches and their depths, states, and times are D6's revision-2
      table; walks at six voters were calibrated and found nothing even under f + 1, so they count
      for nothing.
    - `arith.py` (`df75a1e`): the bracket with the verifier (1.165×, 0.803×), T2's margin against
      EVP's rate (the follower's target fails below 0.361 GB/s), D18's detection figures and the
      skip rates (DMS-11), and T4's corrupt-flush counts (439 at rf = 3, 220 at rf = 5).
    - C7's calibration row added to `docs/reviews/calibration.md` at the lead's request
      (`e71c810`), on its own commit so it can be dropped.
    - Revision 2 written (`f48aea4`, `484127b`, `a752460`, `f33e12b`, `316c2e0`, and this commit):
      every finding answered (the design's "Changes in revision 2"). DM-R19 keeps forgetfulness in
      two places: the open record's V (the newest epoch whose recovery found voters forgetful,
      durable before the epoch's first turn by Track C's condition 8) and a mark in every hard state
      a forgetful voter writes; first drafted as marks in the new epoch's first turn, then moved to
      the open record when Track C's D11 showed the open record is synced before any turn. D6's
      argument gains the step that rules out a membership two changes away: starting a round
      persists the candidate's vote, so its membership is its log's newest configuration entry.
- Doing: revision 3 (fifth session, 2026-10-19): DM-R24 first (it blocks step 1), then DM-R28's
  vote barrier in the model and the window runs, DM-R26, DM-R25's keyed comparison and Q9, DM-R27's
  twelve items, C15 to C22.
- Next:
  1. The lead starts the architect's re-check of revision 2, scoped to `git log fc30ed3..` (design
     changes; DM-R19 and DM-R20 first, with the model's runs in `bench/durability/results/r2/` and
     `bench/durability/model/runs_r2.sh` to rerun them), and the security reviewer's re-check of
     DMS-10 to DMS-13 and C12.
  2. The lead writes done criteria (C6, the architect's C1): K1 to K6 as the preliminary review
     proposed, with DM-R19's and DM-R20's mutants in K3 (forgetfulness cleared by a restart; the
     mark cleared before the re-learned entries; f + 1 deciding at an even membership).
  3. The owner answers Q9 (restated with each option's residual; the recommendation is option 3 if
     T2 prices it below option 1, else option 1) and confirms Q10 with the majority count; the
     lead's calls stand meanwhile, and neither blocks a first step.
  4. First steps 2 and 4 to 8 may start now under the re-check's conditions (C8 to C12); 1 and 3
     after this revision's re-check; nothing lands before the full design review.
  5. The lead routes: "Changes to other documents" on `main` (Track C's D3 and D11 rows,
     `limits.md`'s two rows, the simulator's schedules and profiles for step 6); D17's outgoing
     slot rule to Track C's D13 with SR22's C16; "For Detent" to Detent's
     `SHARED_LIBRARY_DECISIONS.md`; small-indexes re-pins its citations of this design (SI-R26).
  6. Design Part B as its own file after A's re-check.
- Blocked on:
  - This revision's re-check (steps 1 and 3 wait for it: a format, wire, and durability change).
  - The owner's Q9 (not blocking: the lead's call stands and step 5 builds what every option
    needs) and Q10's confirmation with the majority count (not blocking).
  - Detent's agreement before step 4 lands.
  - SYS-8 (turn log or per-group journal): not blocking; the per-group journal refuses `acks:
    written` (D3).
- Findings folded in: DM-R1 to DM-R18, DMS-1 to DMS-9, the architect's C1 to C5, and the security C1
  to C7, per the design's "Changes in revision 1"; DM-R19 to DM-R23, DMS-10 to DMS-13, the
  architect's C6 to C14 (C6 is the lead's, Next 2), and the security C8 to C12, per "Changes in
  revision 2" (`df75a1e`, `e71c810`, `f48aea4`, `484127b`, `a752460`, `f33e12b`, `316c2e0`, and this
  commit); the owner's Q1 to Q6 and Q8; small-indexes' C3 (SI-R19, SI-R20, SI-R24, SI-R25, SI-R26).
  One proposal argued against, with evidence: DMS-7's "`forgetful` only with `success`" (D16).
- Last verified (2026-10-19, fourth session): `quiet check` exited 0 or 2 (light holds `qexp-m3`,
  `qexp-terms`, `qexp-m3t`, `qexp-m3t-sprime`, `qexp-aa`, `qexp-runlevel`), never 1, so every build and run used
  `nice -n 19` with at most four jobs, and two at a time for the 2.4 GB tables while swap was nearly
  full. Builds: `forgetful_check.cc` with Homebrew clang 23.1.2 at `-O2 -Wall -Wextra` (no warning)
  and at `-O1` with ASan and UBSan, which ran one configuration of each new feature with no report
  and the `-O2` binary's counts (`results/r2/sanitizers.out`). The model: revision 1's counts
  reproduced; Python against C++ on the new features; the forced schedules in both checkers; the
  searches of D6's revision-2 table, each under `run_bounded.sh`'s cap with its output in
  `results/r2/`. `arith.py` regenerated. Every strata `file:line` the design cites re-read at
  `026ee10`. No strata, consensus, or journal build or test: this branch changes no library code,
  and the model builds apart from strata's targets, so CI does not build it; CI runs on this push
  for the lead to watch.

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
