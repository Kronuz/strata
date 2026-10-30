> Status: PARKED (2026-10-20) design approved with conditions at revision 10; implementation follows the speed work, and distributed work parks behind engine speed (owner, 2026-10-20).

# durability-modes

- Design: `docs/design/durability-modes.md`, Part A (the consistency spectrum and replication),
  revision 10 (the owner's Q9 answer, 2026-10-20; revision 0 at `8f8b043`, revision 1 at `1efb815`, revision 2 at `18612db`, revision 3
  at `b56cb46`, revision 4 at `b7c6c78`, revision 5 at `2159dd5`, revision 6 at `4d5acb8`, revision 7
  at `427ca1b`, revision 8 at `d341d60`, revision 9 at `5460e20`; what changed is the design's "Changes in
  revision 10", then, per finding, 9, 8, 7, 6, 5, 4, 3, 2, and 1); Part B (memory
  residency and retention) outlined at its end, designed after A's re-check. Reviews:
  `docs/reviews/durability-modes-design.md` (the architect's preliminary review, 2026-10-19,
  `7ffc16b`: REJECT for first steps 1, 3, 4, and 5; DM-R1 to DM-R18, C1 to C5; its re-check of
  revision 1, `4321849`: REJECT for steps 1 and 3; DM-R19 to DM-R23, C6 to C14; its re-check of
  revision 2, `d627895`: REJECT for step 1, APPROVE WITH CONDITIONS for step 3; DM-R24 to DM-R28,
  C15 to C22; its re-check of revision 3, `2e9ad85`: APPROVE WITH CONDITIONS for steps 1 and 3;
  DM-R29 to DM-R32, C23 to C26; its re-check of revision 4, `3cd9f0e`: APPROVE WITH CONDITIONS for
  steps 1, 3, and 8, APPROVE for 9; DM-R33 to DM-R35, C27 to C29; its re-check of revision 5,
  `8b87d1b`: APPROVE WITH CONDITIONS, ready for the full design review once revision 6 folds DM-R36
  and DM-R37; DM-R36 to DM-R38, C30 to C32; its full design review of revision 6, `1143f75`: APPROVE
  WITH CONDITIONS, step 5's whole-image verifier, mode switch, and `verify` switch rejected; DM-R39
  to DM-R47, C33 to C40; its re-check of revision 7, `5ee34e4`: APPROVE WITH CONDITIONS, the
  whole-image comparison, grounding, and mode switch approved, `Vouch` answering, the `verify`
  switch, and check 5's seed rule held; DM-R48 to DM-R55, C41 to C47; its scoped re-check of
  revision 8, `7473633`: DM-R48 to DM-R55 closed, two notes) and
  `docs/reviews/durability-modes-security.md` (`5117745`: APPROVE WITH CONDITIONS; DMS-1 to DMS-9,
  C1 to C7; its re-check of revision 1, `c87385e`: APPROVE WITH CONDITIONS; DMS-10 to DMS-13, C8 to
  C12; its D18 review of revision 4, `3ad203d`: APPROVE WITH CONDITIONS; DMS-15 to DMS-19, security
  C13 to C17; its full design review of revision 6, `ca70579`: APPROVE WITH CONDITIONS; DMS-20 to
  DMS-24, security C18 to C22; its re-check of revision 8, `134eee8`: APPROVE WITH CONDITIONS for
  `Vouch` answering, REJECT for the `verify` switch as written; DMS-25 to DMS-29, C23 to C27).
- Branch / worktree: `design/durability-modes` at `~/code/Kronuz/strata-wt/durability`, from
  origin/main `755a918`; origin/main merged at `c6df449` (`734949e`), `1365cd1` (`be64292`), and
  `457528b` (`026ee10`, format 13 on main), and `2f444fc` (`3c86484`, DM-R39).
- State: design approved with conditions; revision 10 (2026-10-20) removed the malicious-leader
  machinery by the owner's Q9 ruling, a removal the lead checked (no new mechanism). Conditions
  that lapse with it: C42, C43, C47, security C23 to C28. Revision 10's kept resource bounds
  (loud counters, the wrong-reply row, the follower-side switch clock, the release deferral bound)
  stay as the lead's call
- Lead's calls on revision 8's T2 questions (2026-10-19): T2 gates only on hosts with SHA
  instructions and reports the portable kernel's floor (0.24 GB/s) beside it; option 3's follower
  target is 0.48× (moot since revision 10: option 3 is gone; T2 gates the sampled rebuild's 0.45×,
  0.33×, and 0.12×).
- Lead's calls on revision 9 (2026-10-19): a `verify` switch covering more than one group joins
  D17's two-admin class, and the admin set needs a stated bound for the per-admin sequences
  (`peer-transport.md:1131`, a proposal of 64). Both are moot since revision 10: the signed
  `verify` statement is gone, so neither routing is owed.
- Owner's answer to Q9 (2026-10-20): speed and efficiency over defending against a compromised
  node.
  - Threat model: cluster nodes are trusted, as Raft assumes. A compromised node is out of scope,
    while outsiders are kept out by TLS and the PKI.
  - Segment-mode default: D18's sampled rebuild (about 0.01× CPU). It catches systematic leader
    bugs within tens of flushes; per-file checksums catch bit rot and transit damage. The residual,
    stated honestly: a rare one-off corrupt flush is copied to every replica, and repair means
    re-indexing from the source.
  - Opt-in: a full document-mode replica per index (`verify: replica`), which is ordinary document
    mode and needs no new code.
- Owner (2026-10-20): implementation starts after the current speed work (the query wins, the
  core-vs-RocksDB measurement and its first fixes), first steps 1 and 3 first.
- Done criteria (set by the lead 2026-10-19, from C1, C6 and C17; contributors tick, never edit):
  - [ ] K1 Step 2 with tests on Linux, macOS and FreeBSD, and Detent's suite green.
  - [ ] K2 Step 1 with T9's rows (DM-R2's and DM-R6's included), DM-R24's T9 rows, and DM-R29's
    two-group case (a kill and a clean close between mark turns; marks across a segment switch),
    with DM-R29's slice-in-a-mark-turn mutant.
  - [ ] K3 Step 3 with T6's mutants (DM-R1's, DM-R5's, DM-R19's, DM-R20's, DM-R24's and DM-R26's
    included), `lead5` killing `fplus`, T12, and C38's mutant (an install while forgetful that
    drops the mark).
  - [ ] K7 Step 5 with T4's mutants: DM-R31's structure plants, the planted dead entries and the
    carried-document case, planted term values, and the honest control over every part shape with
    zero check failures. Also: the one alphabet rule's cases; the switch's strict readings and their
    six mutants; a comparison with a > p; a failover before the first `Image`; DMS-22's
    never-written `Image`; DMS-23's floor; plants in main's skip data and threshold seeds; catch-up's
    `--merge-held` mutant; a non-dominating seed and the cross-build honest control (DM-R51); the
    bitset boundary and a wider FOR width (DM-R52); the mark and deferral cases (DM-R53); the
    `replica` digest and repair-source case; and revision 10's four honest-fault questions as tests
    (a claimed-count size bound under writer bugs, via T15's fuzz; a departing `replica` voter runs
    as `sample` and is counted; an unsampled `Image`'s schema and metadata; the repair source's
    preference).
  - [ ] K8 Step 8's controls (D17): the file-list bounds and the second source for an unserved
    `Request`, each with a test, and the refusal of a superseded entry; Kronuz/sha256 pinned by
    commit, with the cross-implementation digest check.
  - [ ] K9 Every gate row that depends on the replication mode runs in both document and segment
    mode (C4).
  - [ ] K10 (C37) completion's unknown boot identity, `TurnRecovery`'s unclean reason, and the
    journal directory's non-local warning, each reported as state with its T9 case.
  - [ ] K11 (C34, C35) T16's crash window with its mutant; T11's expired token and the term
    history's bound; gauges counted per node. (T14's 10,000-group run is the lead's gate row.)
  - [ ] K4 ASan, UBSan and TSan over every touched test; coverage at TESTING's targets.
  - [ ] K5 CI green on all jobs, and Detent's suite, at each library commit.
  - [ ] K6 Step 4 lands only after the full design review and Q8.
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
  - Fifth session (2026-10-19), revision 3:
    - Read the architect's re-check of revision 2 (`d627895`, "Re-check (revision 2)": DM-R24 to
      DM-R28, C15 to C22) and its probes in `strata-wt/review-durability/.scratch/review-durability/
      r2/` without editing them: `mark_mechanism.py` and its output, `digest/digest_forge.py` with
      `records.txt` and `digest_check`, `forgetful_model_vb.py`, `replay_vb.py`, `grow67.py`,
      `six_heard.py`, `members_prefixes.py`, `runs_arch_r2.sh`, the `sec/` liar and create-replay
      probes and outputs; journal `cb66f3d`'s `turn_log.h` (the open record at `:261`, reclamation
      at `:375-391`, forward slices at `:355-374`, recovery's inferred opens at `:468-471`);
      `index.hh`'s digest (`:644-700`), install (`:751-812`), and `flush_active` (`:1931-1945`);
      `files.hh`'s manifest encoding (`:256-264`); Kronuz/hashes and Kronuz/random (no keyed hash, a
      seeded `mt19937_64`); the SipHash paper (Aumasson and Bernstein, 2012).
    - DM-R28 (`abe2500`): D4's vote barrier in both checkers (`--barrier candidate`, the default;
      `--barrier all` for every term change and grant; `--barrier none` reproduces revision 2,
      which `runs_r2.sh` now passes); the C++ checker at seven nodes and four changes; the
      architect's `grow67` and prefixes, `grow67_loss`, `six_heard`, `grow67_heard`, and the
      relabeled five-voter schedules. Every forced schedule under each barrier mode in both
      checkers (`results/r3/schedules.out`): `six` and `grow67` and their losses hold under every
      barrier, `grow6` and the shrink fence are artifacts, and revision 1's `fplus5` and `heard5`
      (and `fplus5b` under `--barrier all`) do not replay, which D6 and Q10 now say. Python and C++
      agree under each barrier mode (`crosscheck.out`). The window and restart searches redone
      under the barrier (D6's revision-3 table, `runs_r3.sh`).
    - DM-R24 (`660f9fc`): `mark_model.py`, built on the architect's probe with segments,
      reclamation, the open record's lifetime, and a precision check; revision 3's rules hold to
      depth 11 (5,111,474 states), the controls reproduce the 6-step loss and the reclaimed open
      record's two failures; the architect's own probe reproduced under `firstturn` to depth 11
      (949,562 states).
    - DM-R25 (`9e8115b`, `1151ad5`): the digest records of T2's corpus counted (92.0 per document, 35.8 bytes
      each); SipHash-2-4 checked against the reference vectors and the architect's forged segment
      shown to sum differently under a key while its F21 digest matches; Q9's options priced in
      `arith.py` (estimates; `1151ad5` takes SipHash's lower bound from the paper's three-unit
      arithmetic, 2.01 µs per pass, after a first four-unit figure the paper's own text rules out).
    - C20's two calibration rows (`4afbf67`), on their own commit as asked.
    - The searches finished (`84372f3`, `6dcdabd`, `25adb3b`, `a32db18`): the window shapes, the
      restarts, `manual`'s admission, and the growth half under the barrier.
    - Revision 3 written (`f6c3a45`, `b2c97c0`, and this commit): every finding answered (the
      design's "Changes in revision 3"). The format-13 answer for DM-R25: no change.
  - Sixth session (2026-10-19), revision 4:
    - Read the architect's re-check of revision 3 (`2e9ad85`, "Re-check (revision 3)": APPROVE WITH
      CONDITIONS for steps 1 and 3; DM-R29 to DM-R32, C23 to C26) and its probes in
      `strata-wt/review-durability/.scratch/review-durability/r3/` without editing them:
      `mark_groups.py` and `runs_arch_r3.sh` with their outputs, `lead5.py` and `schedules/`,
      `plant/term_index_plant.cc` and its output, `q9_cross.py`, `records_stated.py`; the lead's
      calibration correction (`4a6403d`). Re-read journal `cb66f3d`'s `turn_log.h` (`:297`,
      `forward_slice()` `:357-374`, `reclaim()` `:378-391`), strata's `reader.hh`, `keys.hh`,
      `format.hh`'s section table, and Kronuz/termdict at strata's pin `eaf83b3` (the builder, the
      cursor from offset 0, `scan` from the index's floor) and Kronuz/fusefilter `28f4241` (its
      build's fixed seed sequence).
    - DM-R29 (`caa1b45`): the architect's `mark_groups.py` ported with `--switch-marking`;
      `runs_r4.sh`; `none` holds to depth 9 with two groups and depth 8 with three, with and
      without segment switches between mark turns, and `any` loses in 7, 6, and 6 steps; every
      count equal to the architect's. D3 rule 2 takes `none`, with the reason for not `own`.
    - DM-R30 (`fc42074`): `lead5`, `lead5_loss`, and `heard5l` in both checkers' schedules
      (`forgetful_model.py`, `schedules/`), replayed under each barrier mode
      (`results/r4/schedules-lead5.out`) and searched from step 9 (`cc-lead5-9-*`), equal to the
      architect's; `forgetful_model.py`'s comment on the relabeled schedules corrected.
    - DM-R32 items 4 to 6 (`e83bf23`): `records.py`'s block count (5.27 µs, not 5.73), `arith.py`'s
      49-byte manifest entry, option 3 with DM-R31's checks and its crossover with option 1.
    - Revision 4 written (this commit): every finding answered (the design's "Changes in revision
      4"). DM-R31: D18's checks 4 and 5 (metadata; both dictionaries re-encoded from a cursor walk
      and compared byte for byte, the key range and filter, norms, statistics, skip entries and
      competitive pairs, no unchecked section), whole images (checked against the follower's own
      image on a mode switch and a catch-up reinstall; fetched from or vouched for by a
      segment-mode follower that is not the leader otherwise, `Vouch` in D16 and D17, the residual
      counted as `install_unvouched`); Q9 restated with the checks and the crossover.
  - Seventh session (2026-10-19), revision 5:
    - Read the architect's re-check of revision 4 (`3cd9f0e`: DM-R33 to DM-R35, C27 to C29) and the
      security reviewer's D18 review (`3ad203d`: DMS-15 to DMS-19, security C13 to C17), and their
      probes without editing them: `r4/plant/dead_stats_plant.cc` and its output,
      `own_reclaim.py`, `own_reclaim_forced.py` and its output in
      `strata-wt/review-durability/.scratch/review-durability/`; `impacts_roundtrip.cc`,
      `vouch_position.py`, `verifier_bounds.py` and their outputs in
      `strata-wt/review-durability-sec/.scratch/durability-sec-r4/`. Re-read strata on this branch:
      `index.hh:640-680` (F21's digest), `1380-1416` (`install_merge`'s carried deletions),
      `1931-1945`; `searcher.hh:92-121`; `search.hh:72-96`, `310-332`, `707-724`, `1222-1232`;
      `format.hh:425-663` (term info, its codec, `competitive`); `reader.hh:1135-1300` (the
      postings iterator, both skip levels), `1489-1528`, `1726-1737`, `1888-1920`; `writer.hh:20-40`,
      `232-250`, `295-334`, `510-673`; `merge.hh:85-175`; `keys.hh:60-125`;
      `tuple_buffer.hh:390-406`.
    - Found beside the reviews: an honest merge output's term alphabet is the union of its inputs'
      (`merge.hh:92-99`), not its walked terms', so DMS-16's "alphabet from the walked keys" would
      refuse honest merges; D18 takes the writer's derivation per dictionary and segment kind.
    - Models and probe (`results/r5/`, `runs_r5.sh`): `check3_model.py` (every output of up to four
      ordinals from two input documents, 4,743,860 outputs, and of up to three from three,
      12,904,310: revision 4's check 3 passes phantom or altered records that raise a statistic,
      revision 5's passes none and refuses no honest output); `vouch_model.py` (revision 5's rule
      holds over every reachable state with two entries, 74,225 states, and to depth 13 with three,
      917,782; revision 4's rule by position loses in 4 steps, DMS-15's fix alone in 6, DM-R34's fix
      alone in 6); `term_value_check.cc` against this branch's `format.hh` (every planted field of a
      term value refused by the rebuilt value and passed by the round trip).
    - Revision 5 written (this commit): every finding answered (the design's "Changes in revision
      5").
  - Eighth session (2026-10-19), revision 6 (text only):
    - Read the architect's re-check of revision 5 (`8b87d1b`, "Re-check (revision 5)": DM-R36 to
      DM-R38, C30 to C32), D8 and D18, and the architect's `r5/plant/check35_real.out` (part A: M1
      and M2) and `r4/plant/term_index_check5.out` (a widened alphabet reads the same), without
      editing them. Re-read on this branch: `merge.hh:86-100`, `keys.hh:72-100`,
      `tuple_buffer.hh:388-406`, `reader.hh:1133-1160`, `1736-1752`.
    - DM-R36 (the lead's call): for a whole image (a new member, a repair, a catch-up or
      mode-switch reinstall), check 5 accepts the received term alphabet when it is strictly
      ascending and contains every walked term's byte, re-encoding byte for byte under it;
      everywhere else revision 5's rule, worded "stored alphabets" (the union of the inputs' stored
      alphabets for a merge). **The security reviewer confirms this choice at the full design
      review** (it gives up security C14's "a function of the walk alone" for whole images; one
      rule everywhere is the sound alternative). T4: whole-image honest control, widened, narrowed,
      and unordered alphabets.
    - DM-R37 (the architect's preferred fix): from the switch command the follower merges none of
      the segments it then holds and keeps its log from the command; the leader's first `Image`
      after it carries no dead ordinal of a document deleted before the command (it expunges them
      first, one merge per segment holding a dead ordinal); every dead ordinal matches, an
      unmatched one fails like any mismatch. The "counted, residual stated" text and
      `reinstall_dead_unmatched` are removed from D18, Q9, and T4; the summary, threat row, and
      fault table state the switch as matched; T4 gains the mode-switch cases and mutants. Argued
      from the text; no model was run, and the expunge's cost is not measured (D15 marks the switch
      heavy).
    - DM-R38: D8's catch-up bullet names the checkpoint ceiling (item 1); the decoders report where
      they ended, with step 5's accessors and a T4 plant in a segment (item 2); "for the terms of
      the deleted documents it keeps" (item 3).
    - Revision 6 written (this commit): every finding answered (the design's "Changes in revision
      6").
  - Ninth session (2026-10-19), revision 7:
    - Read the architect's full design review of revision 6 (`1143f75`: DM-R39 to DM-R47, C33 to
      C40) and the security reviewer's (`ca70579`: DMS-20 to DMS-24, security C18 to C22), the
      design, and this ledger; the architect's `r6/model/switch_model.py`, `vouch_model_r6.py`,
      `plant/alphabet_whole.out`, `cite/map_citations.py`, and `run_capped.sh`, and the security
      reviewer's `durability-sec-r6/switch_model.out` and `alphabet_rules.out`, without editing them.
    - Merged origin/main `3c86484` at `2f444fc` (clean). Re-pinned every strata citation from
      `026ee10` (`bench/durability/repin_citations.py`: 29 unchanged, 56 moved, 7 by hand), and read
      the seven at `3c86484`: `create()` and `create_deferred` (`index.hh:372-376`, `2291-2306`),
      `lock_directory` and `clear_uncreated` (`2329-2374`), `test/directory_test.cc:650-667` (a
      create is 4 durability calls), the skip data's new layout (`reader.hh:1277-1328`, `1475-1570`;
      `writer.hh:533-687`; `format.hh:427-454`), and the threshold seeds `main` added
      (`format.hh:527-539`, `writer.hh:751-823`, `check.hh:45-54`, `searcher.hh:198-215`), which
      check 5 now covers. Completion `7734544`'s `posix.h` re-read for T1's Linux mapping.
    - Models (`results/r7/`, `runs_r7.sh` under `run_capped.sh`'s wall-clock and memory caps, two at
      once at most): `vouch_model.py` widened as the architect's (every member may catch up, plants
      carried): revision 7 holds over every reachable state with two entries (354,323) and to depth
      11 with three (1,025,427; the 1.5-million-state cap stops depth 12, 1.4 GB, not cited);
      revision 6's catch-up rule loses in the architect's 5 steps; revision 5's controls lose in 4,
      6, and 6. DM-R41: option (a) holds P1 to P3 with a missed plant (176,827 states with two
      entries; 1,297,001 to depth 12 with three), option (b) reaches no grounded member (24,090
      states, with and without the plant), and (a) under revision 6's catch-up rule loses in 5. The
      architect's `vouch_model_r6.py` and its two switch readings rerun unedited, matching its
      review's figures. `switch_model.py` ported (the computation at p checked, the straddling
      merge added as `--expunge-held-only`): every needed document present under the strict
      readings at the architect's counts (16,241,982; 11,083,575; 1,769,706; 6,403,536;
      4,319,922 cases), and each of the eight mutant runs loses within a log's first 3 operations.
    - Revision 7 written (`2037cfd`, this commit): every finding answered (the design's "Changes in
      revision 7").
  - Tenth session (2026-10-19), revision 8:
    - Read the architect's re-check of revision 7 (`5ee34e4`: DM-R48 to DM-R55, C41 to C47), the
      design, and this ledger; the architect's `r7/model/vouch_model_rewind.py`,
      `vouch_model_cycle.py`, `switch_model_atp_{nosub,noback}.py`, `probes/seed_fp.py` and every
      output in `r7/probes/`, without editing them. Read strata at `3c86484` for every new citation
      (`check.hh:35-56`, `writer.hh:533-826`, `search.hh:72-98`, `searcher.hh:200-215`,
      `format.hh:427-454`, `527-539`, `aggregate.hh:28-36`, `CMakeLists.txt:110`); `main` `af37f69`'s
      `docs/status/format13.md:192-199` (`include/` equal to `3c86484`'s); `design/peer-transport`
      `38a6c6f`'s D17 (`peer-transport.md:1126-1190`); Kronuz/sha256 `45b7123`'s README.
    - Models (`52a4517`; `results/r8/`, `runs_r8.sh` under `run_capped.sh`, two at once at most):
      both architect probes ported and every one of their configurations reproduced (rewind: kept
      loses in 6, clipped holds 507,095 and 970,016 states; cycle: 5, 5, and 6 steps, and 443,964
      and 2,686,382 states). Beside them: with `clean` entries, the interval cut back but the
      grounding kept loses in 9 steps, and both cut back hold (859,430 states to depth 11 with two
      entries, 654,736 to depth 9 with three); in a group born under `sample`, the rules hold over
      every state with two entries (922,928), to depth 9 with three (770,749) and 8 with four
      (904,879; depth 9 passed the 1.5-million-state cap, not cited), and each rule removed loses
      (4, 8, 8 steps). `switch_model.py` relabeled, its five strict runs equal to `results/r7/`
      case for case, its eight mutants and the two `--atp` mutants losing as before.
    - The lead's message (CL-10): D17's hash rule on Kronuz/sha256; `arith.py` re-priced with its
      README's rates (not measured here). Found while re-pricing: T2's option-3 follower target
      (0.40×) counted the verifier alone; with the follower's hashing and transport it is 0.439× at
      the slow end on hosts with SHA instructions (0.451× at revision 7's rates), so T2 now states
      0.48×; and on hosts without SHA instructions option 2's targets fail at 2 ns per byte, so T2
      gates on hosts with them and reports the floor.
    - Revision 8 written (this commit): every finding answered (the design's "Changes in revision
      8").
  - Eleventh session (2026-10-19), revision 9:
    - Read the security reviewer's re-check of revision 8 ("Re-check (revision 8)", `134eee8`:
      DMS-25 to DMS-29, C23 to C27) and the architect's scoped re-check's two notes (`7473633`,
      "New findings"), the design (D15 to D18, the verify switch, Q9, T4, first steps 5 and 8), and
      this ledger; the reviewer's `switch_consumed.py`, `vouch_model_cycle_deferred.py` with its
      diff, and `runs-deferred/`, read only. `design/peer-transport` `d8384c8`'s D17
      (`peer-transport.md:1136-1189`; `38a6c6f`'s paragraph on time is at the same lines), strata
      `3c86484`'s `CMakeLists.txt:9-32`, consensus `61149a2`'s `CMakeLists.txt:7`, `23`, and
      Kronuz/sha256 `45b7123c295f8c416eb6b7a852b0b0b2c47d1286`'s `README.md:41-45` and test files
      read for the citations.
    - Models (`4766094`; `results/r9/`, `runs_r9.sh` under `run_capped.sh`, `quiet check` exit 2 at
      all 13 launches, `nice -n 19`, two at once at most, peak 903 MB): the reviewer's change ported
      into `vouch_model_cycle.py` (`--switch-ground deferred`, `deferred_any`, and the new mutant
      `deferred_lw`; the default stays revision 8's `eligible` so `results/r8/` reproduces) and
      `switch_consumed.py` ported. The deferral holds P1 to P3 over every reachable state with two
      entries (1,096,346; 530,777 with a grounded segment-mode member), to depth 9 with three
      (828,817) and 8 with four (933,528), and `deferred_any` to depth 9 with three (835,565), all
      equal to the reviewer's; the `all` mutant loses in 4 steps, `deferred_lw` in 5, and with the
      deferral the up-written and yes-written mutants still lose in 8 each; revision 8's rule
      reproduces (922,928 states with two entries, 770,749 to depth 9, and the group born under
      `install` 443,964 to depth 8). `switch_consumed.py`: under `eligible`, no grounded member in
      87,753 states to depth 10 after both followers enter catch-up before the `up`; under
      `deferred`, a grounded member in one step, and with one follower out the same.
    - Revision 9 written: DMS-25 to DMS-29 and the architect's two notes answered (the design's
      "Changes in revision 9"). Decision for the reviewers to overturn: DMS-27's document-mode
      member is named by the signed `replica` statement (a body field), not computed by a rule from
      the leader, because a replica cannot recover the leader of a past term from its log; a
      `sample` to `replica` group is refused with a named reason rather than grounded by a new rule,
      so no grounding path was added beyond DM-R41's.
  - Twelfth session (2026-10-20), revision 10 (text and archive only):
    - Pulled (up to date at `637e14d`). Read the lead's message with the owner's Q9 ruling, the
      design (D6, D8, D10, D14 to D18, the threat model, the fault table, T2, T4, T13, Q9, the first
      steps, "For Detent", "Changes to other documents") and this ledger. Read the strata code the
      kept checks cite at this branch's pin (`index.hh:659-715`, `1401-1429`, `2546`;
      `searcher.hh:92-121`; `search.hh:76-82`; `reader.hh:491`, `1147-1149`, `1828`, `1986`;
      `writer.hh:170`, `642`, `751`; `format.hh:689`; `check.hh:45-54`; `aggregate.hh:33-35`;
      `CMakeLists.txt:110`) and `REPLICATION.md:8-24`, `61-67`, PLAN's recovery tooling.
    - Removed, by decision: D18's keyed install checks (option 3), the keyed sums, the keys' hygiene,
      the keyed sampling decisions, and check 4; `Vouch` and its control kind (D16, D17, D18, step
      8); grounding, the standing attribute, `grounded_by_switch`, and the release mark's grounding
      rules (D6, D8, D10, D15, D16, D18); the signed `DTADMS01` `verify` statements with the
      history bits, per-admin sequences, and owed grounding (D15, D16, threat model, the
      `peer-transport.md` row); `install_unvouched` (D18, Q9, threat, fault table); step 9 and the
      SipHash rows (For Detent, prior art, Kronuz/hashes); T2's option 3, and T4's forging, vouch,
      standing, cycle, replay, re-grounding, first-switch, and bounds cases and mutants.
    - Kept: the recovery rule, forgetful marks, checkpoints, `Create` replay (D3 to D6); the mode
      switch's strict readings and expunge, the comparison at p, the release's mark and deferrals;
      the file-set transfer's bounds and per-file checksums; the leader-set resend floor and
      `switch_min_interval` as resource bounds; the sampled rebuild with checks 2, 3, and 5; the
      Kronuz/sha256 pin. Where a mechanism served both (the manifest discard for the term history,
      the second source for an unserved `Request`, the fetch-again rule, the one alphabet rule and
      seeds by dominance, the named document-mode voter), the honest part stays; "Revision 10" item
      3 says why for each.
    - Archived with `git mv` under `bench/durability/archive/` (README note): `vouch_model.py`,
      `vouch_model_cycle.py`, `vouch_model_rewind.py`, `switch_consumed.py`, `keyed_digest.py`,
      `runs_r9.sh`, and their results (`results/r3/keyed_digest.out`, `r5/vouch-*`,
      `r7/v7-*` and `arch-vr6-*`, `r8/cy-*`, `cyb-*`, `rw-*`, all of `r9/`). Left in place:
      `check3_model.py`, `term_value_check.cc`, `switch_model.py` and their results (the kept
      checks'), `arith.py` (its revision 3, 4, and 8 sections still price option 3, so
      `arith.out` reproduces), and `runs_r5.sh`, `runs_r7.sh`, `runs_r8.sh`, which still name the
      moved models.
    - Numbers redone as arithmetic (labeled estimates): per idle group 66 bytes, about 72 with
      padding, 3.7% of the idle index (was 82, about 88, 4.5%); the unkeyed digest saves at most
      2 × 5.27 µs ÷ 64 = 0.16 µs per document of the sampled rebuild (0.4% of a document-mode
      follower's 42.8 µs). The detection figures (3.1% and 6.1% per corrupt flush; 13.3% and 1.8%
      unseen after 64; 1.8% and 0.03% after 128; the 439 and 220 corrupt flushes of T4 missing every
      one with probability 9.9e-7 and 9.6e-7) recomputed and unchanged.
    - Revision 10 written (this commit): the design's "Revision 10" and "Changes in revision 10".
      Design file 3,615 lines before, 3,020 after (D18, heading to the next section: 716 before, 290 after).
- Doing: nothing; stopped at ready for preliminary review of revision 10.
- Next:
  1. The lead starts a preliminary review of revision 10 (the design's "Status" lists what to look at
     first). The removals are text and the archive; no model reruns (the kept models' results are
     the ones already in `results/`).
  2. Done criteria, the lead's to change (contributors tick, never edit). Drop, because they exist
     only for the removed machinery:
     - K7, the first list: "the forging mutant" (T4's forging mutant leader and its unkeyed
       control); "the vouch position schedule and two-member chain".
     - K7 "Also (C39)": DM-R40's catch-up schedule; DM-R41's missed-plant switch with its two
       mutants; DMS-21's bounds (they become the first step's question 1, not a gate).
     - K7 "Also (C46, DM-R55)": both rewind schedules (DM-R48); the cycle, its two mutants, and the
       `yes`-after-unchecked case (DM-R49); the re-grounding and the leader-alone image (DM-R50).
     - K7 "Also (revision 9)": all of it: DMS-25's schedules with mutants, DMS-26's replay cases,
       DMS-27's re-grounding cases and each `regrounding_refused` reason, DMS-29's wide-interval
       source with the keep-the-shipped-list mutant.
     - K8: "Vouch's echo" and "the re-fetch routing" (the `no` routing), and "(C39) DMS-17's
       refusal by an unvouched member".
     - K10: `verifier_unavailable`, with C37's timing (the keys it reported on are gone).
     Keep, unchanged: K1 to K6, K9, K11; K2 and K3 in full; K7's planted dead entries and the
     carried-document case, planted term values, DM-R31's structure plants (as seeded faults at
     rate 1), the honest control over every part shape, the alphabet rule, the switch's strict
     readings and their six mutants, a comparison with a > p, a failover before the first `Image`,
     DMS-22's never-written `Image`, DMS-23's floor, plants in main's skip data and seeds,
     catch-up's `--merge-held`, a non-dominating seed with the cross-build honest control, the
     bitset boundary and a wider FOR width, and the mark and deferral cases (DM-R53); K8's
     file-list bounds, the second source for an unserved `Request`, the refusal of a superseded
     entry, and DMS-28's pin and cross-implementation digest. Add (revision 10): `verify:
     replica`'s digest and repair-source case (T4), and the four questions after "First steps" as
     step 5's test items (the size bound, the departing document-mode voter, the image's
     metadata, the repair source).
  3. Conditions and approvals that bound the removed parts lapse with them: the architect's C42
     (`Vouch` answering), C43 (the `verify` switch), C47 (their re-check), the security reviewer's
     C23 to C27 (DMS-25 to DMS-29; DMS-29's manifest discard survives in D17 for the term history
     alone), and security C28 (DMS-30, binding the verify switch's code). Security C15 still binds
     step 8's controls (the list bounds, the second source, the superseded entry); the lead
     restates it. Nothing in the design asks the reviewers to reopen the kept parts.
  4. Routing withdrawn: `peer-transport.md` D17's row (the `verify` statement on an index group's
     log, the bound on the admin set, the two-admin-class call) and the Kronuz/hashes row
     (SipHash); the Kronuz/sha256 row stays. Step 9 is withdrawn.
  5. Left in the design because the ruling did not name them, for the lead's call (each is a
     malicious-voter or malicious-leader guard that also bounds a resource): D5's rewind floor and
     allowance (DMS-13, kept as the resend floor), D6's loud-not-covert counters and the
     `SIX_LIAR` outcome counted outside the model, the threat model's wrong-reply voter row (kept,
     retitled), DMS-23's follower-side clock on whole comparisons (kept as a resource bound on
     them), and D8's release mark and deferral bound (kept: a released follower cannot tell a
     dropped document from a leader bug). If the owner's ruling is meant to reach them, say so and
     the next revision trims them.
  6. First steps: all may start. Step 5 is smaller (no keyed sums, no `Vouch`, no standing, no
     statement verification); steps 1 to 4 and 6 to 8 are unchanged but for step 8's two control
     kinds. The lead still routes "Changes to other documents" (`format-13.md`'s seed note;
     `limits.md`'s rows; D17's outgoing slot rule; Kronuz/sha256's pin by hash), "For Detent"
     (without the SipHash item), small-indexes' re-pins, and C45's two calibration rows.
  7. The owner answers Q10 (option 1 recommended); it blocks nothing. Design Part B as its own file.
- Blocked on:
  - Nothing for the design. The preliminary review of revision 10.
  - The owner's Q10: not blocking; the lead's call stands.
  - Detent's agreement before step 4 lands.
  - SYS-8 (turn log or per-group journal): not blocking; the per-group journal refuses `acks:
    written` (D3).
- Findings folded in: DM-R1 to DM-R18, DMS-1 to DMS-9, the architect's C1 to C5, and the security C1
  to C7, per the design's "Changes in revision 1"; DM-R19 to DM-R23, DMS-10 to DMS-13, the
  architect's C6 to C14, and the security C8 to C12, per "Changes in revision 2"; DM-R24 to DM-R28
  and the architect's C15 to C22, per "Changes in revision 3"; DM-R29 to DM-R32 and the architect's
  C23 to C26, per "Changes in revision 4" (`caa1b45`, `fc42074`, `e83bf23`, and the revision-4 design commit); DM-R33
  to DM-R35, the architect's C27 to C29, DMS-15 to DMS-19, and the security C13 to C17, per "Changes
  in revision 5" (the revision-5 commit); DM-R36 to DM-R38 and the architect's C30 to C32, per
  "Changes in revision 6" (the revision-6 commit); DM-R39 to DM-R47, the architect's C33 to C38
  (C39 and C40 are the lead's), DMS-20 to DMS-24, and the security C18 to C22, per "Changes in
  revision 7" (`2037cfd` and the revision-7 commits); DM-R48 to DM-R55 (C41 to C47 are routed
  or the lead's), per "Changes in revision 8" (`52a4517` and the revision-8 commit); DMS-25 to DMS-29 (C23 to C27) and
  the architect's scoped re-check's two notes, per "Changes in revision 9" (`4766094` and the
  revision-9 commits); the owner's answer to Q9 (2026-10-20), per "Changes in revision 10" (this
  commit); the
  owner's Q1 to Q6 and Q8; small-indexes' C3 (SI-R19, SI-R20, SI-R24, SI-R25, SI-R26). One proposal
  argued against, with evidence: DMS-7's "`forgetful` only with `success`" (D16).
- Last verified (2026-10-20, twelfth session): text and archive only; no compile, test, model, or
  measurement ran, so no `quiet check` was needed (it gates a model run, and none ran). Every
  `file:line` the design newly cites or keeps was read at this branch's pin: `index.hh:659-715`,
  `1401-1429`, `2546`; `searcher.hh:92-121`; `search.hh:76-82`; `reader.hh:491`, `1147-1149`,
  `1828`, `1986`; `writer.hh:170`, `642`, `751`; `format.hh:689`; `check.hh:45-54`;
  `aggregate.hh:33-35`; `CMakeLists.txt:110`; `REPLICATION.md:8-24`, `61-67`; PLAN's recovery
  tooling. Not re-read: Kronuz/termdict's lines at its pin `eaf83b3` (`termdict.hh:102`, `125`, `141`,
  `172`, `197-212`, `498-520`), kept from revision 9 as cited, not in this checkout. The archived
  models were moved, not rerun. Arithmetic recomputed in a throwaway Python run (the detection
  figures, the 439 and 220 flushes' 9.9e-7 and 9.6e-7, 66 against 82 bytes, 3.7%, 0.16 µs).
- Last verified (2026-10-19, eleventh session): `quiet check` before every launch, gated on its exit
  (`results/r9/quiet.log`): 2 at all 13 launches (a light hold, `f13g-sessions-m`), never 0 or 1, so
  every run went at `nice -n 19` under `run_capped.sh`, at most two at once. Peak resident size in
  each `.time` file and run's last line, the largest 903 MB (the deferral over every state with two
  entries). Ruff: `vouch_model_cycle.py` and `switch_consumed.py` formatted and clean. No compile,
  strata, consensus, or journal test: the branch changes no library code. Every new `file:line` the
  design cites was read (`peer-transport.md:1131`, `1144-1155`, `1158-1178`, `1188-1189` at
  `d8384c8`; strata `CMakeLists.txt:9-32` at `3c86484`; consensus `CMakeLists.txt:7`, `23` at
  `61149a2`; Kronuz/sha256 `README.md:41-45` at `45b7123`). Not run: DMS-27's re-grounding from
  `replica` and DMS-26's replay (argued from the text; the cycle model has `down` as `sample`
  only); the deferred grounding on three entries past depth 9 and four past 8.
- Last verified (2026-10-19, tenth session): `quiet check` before every launch, gated on its exit
  (`results/r8/quiet.log`): 0 twice, 2 every other time (`f13g-harness-r`, light), never 1, so every
  run went at `nice -n 19` under `run_capped.sh`, at most two at once. Peak resident sizes in each
  `.time` file and run's last line, the largest 2,049 MB (the architect's cycle over every state
  with two entries); swap stood near 15.3 of 16 GB. Ruff: `vouch_model_rewind.py`,
  `vouch_model_cycle.py`, `switch_model.py`, and `arith.py` formatted and clean (`arith.py`'s
  output byte for byte as before but for its new section). No compile, strata, consensus, or
  journal test: the branch changes no library code. Every new `file:line` the design cites was read
  at `3c86484` (or the named commit). Not run: DM-R50's re-grounding from a document-mode member
  (argued from rules 1, 3, and 4, not modeled); DM-R53's mark (argued); DM-R51 on two builds.
- Last verified (2026-10-19, ninth session): `quiet check` before every launch, gated on its exit
  (`results/r7/quiet.log`): exit 1 (`f13g-reorder-merge`, strict) held every launch until it
  cleared at 17:52 PDT; afterwards 0 or 2, every run at `nice -n 19`, at most two at once. Peak
  resident sizes in each `.time` file and run's last line, the largest 1.4 GB (the capped depth-12
  attempt); swap stood near 19 of 20 GB. Ruff: `vouch_model.py`, `switch_model.py`, and
  `repin_citations.py` formatted and clean. No compile, strata, consensus, or journal test: the
  branch changes no library code. Every new `file:line` the design cites was read at `3c86484`.
- Last verified (2026-10-19, eighth session): text only; no compile, test, model, or measurement
  ran, so no `quiet check` was needed. Every new `file:line` (`merge.hh:92-99`, `keys.hh:74-97`,
  `tuple_buffer.hh:392-405`, `reader.hh:1145-1147`, `1750`) was re-read on this branch. The
  architect's `check35_real.out` and `term_index_check5.out` were read, not rerun.
- Last verified (2026-10-19, seventh session): `quiet check` before each launch exited 0, then 2 (a
  light hold, `f13q-noef`), so every run went at `nice -n 19`, one at a time (`runs_r5.sh` checks
  before each launch). One slip: a 1.5 s, 14 MB dry run of `check3_model.py` started while `quiet
  check` exited 1 (a strict hold, `f13q-noef-idx`), because the command printed the exit instead of
  gating on it; nothing from it is kept, and `results/r5/` is `runs_r5.sh`'s, after the hold ended
  and after `ruff format`. Swap was 20.9 to 21.0 of 22.5 GB used, so model runs were capped at 1.5
  million states (one attempt to exhaust `vouch_model.py` with three entries stopped at that cap,
  1.38 GB resident, and is not cited). Build: `term_value_check.cc` with Homebrew clang 23.1.2 at
  `-O1 -Wall -Wextra`, no warning, against this branch's `include/` and termdict `eaf83b3`, fst
  `24da59a`, varint `21af82a`, hashes `7ab0c23` (strata's pins, in `.scratch/deps/`). Every new
  `file:line` the design cites was re-read on this branch. Ruff: `check3_model.py` and
  `vouch_model.py` formatted and clean. No strata, consensus, or journal build or test: this branch
  changes no library code.

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
- Owner's answer (2026-10-20), Q9: the threat model is trusted nodes (a compromised node is out of
  scope; TLS and the PKI keep outsiders out); the segment-mode default is D18's sampled rebuild,
  with its residual accepted (a rare one-off corrupt flush is copied to every replica; repair is
  re-indexing from the source); a full document-mode replica per index (`verify: replica`) is the
  opt-in. This supersedes the lead's call on Q9 (option 2, below), which it confirms, and
  revisions 3 to 9's restatements of Q9.
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
    revision 2 restates Q9 with each option's residual, computed (`arith.out`), for the owner (DM-R21).
  - Revision 3 restates Q9 again with the keyed comparison (DM-R25, C21: the lead's option 2 did
    not defend against a compromised leader while the comparison was unkeyed) and Q10 with the
    vote barrier's effect on revision 1's five-voter schedules (DM-R28) and rf = 3's exception
    (DM-R27 item 12, option 4).
