# durability-modes

- Design: `docs/design/durability-modes.md`, Part A (the consistency spectrum and replication),
  revision 6 (revision 0 at `8f8b043`, revision 1 at `1efb815`, revision 2 at `18612db`, revision 3
  at `b56cb46`, revision 4 at `b7c6c78`, revision 5 at `2159dd5`; what changed per finding is the
  design's "Changes in revision 6", then 5, 4, 3, 2, and 1); Part B (memory
  residency and retention) outlined at its end, designed after A's re-check. Reviews:
  `docs/reviews/durability-modes-design.md` (the architect's preliminary review, 2026-10-19,
  `7ffc16b`: REJECT for first steps 1, 3, 4, and 5; DM-R1 to DM-R18, C1 to C5; its re-check of
  revision 1, `4321849`: REJECT for steps 1 and 3; DM-R19 to DM-R23, C6 to C14; its re-check of
  revision 2, `d627895`: REJECT for step 1, APPROVE WITH CONDITIONS for step 3; DM-R24 to DM-R28,
  C15 to C22; its re-check of revision 3, `2e9ad85`: APPROVE WITH CONDITIONS for steps 1 and 3;
  DM-R29 to DM-R32, C23 to C26; its re-check of revision 4, `3cd9f0e`: APPROVE WITH CONDITIONS for
  steps 1, 3, and 8, APPROVE for 9; DM-R33 to DM-R35, C27 to C29; its re-check of revision 5,
  `8b87d1b`: APPROVE WITH CONDITIONS, ready for the full design review once revision 6 folds DM-R36
  and DM-R37; DM-R36 to DM-R38, C30 to C32) and
  `docs/reviews/durability-modes-security.md` (`5117745`: APPROVE WITH CONDITIONS; DMS-1 to DMS-9,
  C1 to C7; its re-check of revision 1, `c87385e`: APPROVE WITH CONDITIONS; DMS-10 to DMS-13, C8 to
  C12; its D18 review of revision 4, `3ad203d`: APPROVE WITH CONDITIONS; DMS-15 to DMS-19, security
  C13 to C17).
- Branch / worktree: `design/durability-modes` at `~/code/Kronuz/strata-wt/durability`, from
  origin/main `755a918`; origin/main merged at `c6df449` (`734949e`), `1365cd1` (`be64292`), and
  `457528b` (`026ee10`, format 13 on main).
- State: designing (revision 6, ready for the full design review)
- Done criteria (set by the lead 2026-10-19, from C1, C6 and C17; contributors tick, never edit):
  - [ ] K1 Step 2 with tests on Linux, macOS and FreeBSD, and Detent's suite green.
  - [ ] K2 Step 1 with T9's rows (DM-R2's and DM-R6's included), DM-R24's T9 rows, and DM-R29's
    two-group case (a kill and a clean close between mark turns; marks across a segment switch),
    with DM-R29's slice-in-a-mark-turn mutant.
  - [ ] K3 Step 3 with T6's mutants (DM-R1's, DM-R5's, DM-R19's, DM-R20's, DM-R24's and DM-R26's
    included), `lead5` killing `fplus`, and T12.
  - [ ] K7 Step 5 with T4's mutants: the forging mutant, DM-R31's structure plants, the planted
    dead entries and the carried-document case, the vouch position schedule and two-member chain,
    planted term values, and the honest control over every part shape with zero check failures.
  - [ ] K8 Step 8's controls (D17, security C15): the file-list bounds, Vouch's echo, and the
    re-fetch routing, each with a test.
  - [ ] K9 Every gate row that depends on the replication mode runs in both document and segment
    mode (C4).
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
- Doing: nothing; stopped at ready for the full design review.
- Next:
  1. The lead starts the full design review of revision 6 (the re-check asked for no further
     re-check before it). The security reviewer confirms DM-R36's choice there: the received
     alphabet on a whole image, or one rule everywhere.
  2. The lead has the done criteria (K1 to K9, per C31) and the calibration row (C32); contributors
     tick, never edit.
  3. The owner answers Q9 (restated on revision 6's checks, C28, C30; recommendation 3, its residual
     in one clause, no mode-switch residual) and Q10 (option 1 recommended); the lead's calls stand
     meanwhile, and neither blocks a first step.
  4. First steps: all may start; step 1 approved (C27); step 5's verifier lands only with C25, C28,
     C30, and security C13, C14, C16, C17; step 8's controls with security C15; nothing lands before
     the full design review.
  5. The lead routes "Changes to other documents" (now with the Byzantine boundary on
     REPLICATION.md's residual), D17's outgoing slot rule, "For Detent", and small-indexes' re-pins.
  6. Design Part B as its own file after A's re-check.
- Blocked on:
  - The full design review of revision 6 (step 5's verifier lands with C30; the security reviewer's
    confirmation of DM-R36's choice).
  - The owner's Q9 and Q10 (not blocking: the lead's calls stand, and step 5 builds what every
    option needs).
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
  "Changes in revision 6" (the revision-6 commit); the
  owner's Q1 to Q6 and Q8; small-indexes' C3 (SI-R19, SI-R20, SI-R24, SI-R25, SI-R26). One proposal
  argued against, with evidence: DMS-7's "`forgetful` only with `success`" (D16).
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
