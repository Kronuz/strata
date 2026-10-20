# strata review: durability modes, part A (preliminary)

- Date: 2026-10-19, read from 23:30 on 2026-10-18 to 01:40 PDT.
- Reviewed: `docs/design/durability-modes.md` (Part A, D1 to D16, the threat model, the fault
  model, T1 to T14, Q1 to Q7, first steps; Part B's outline) and `docs/status/durability-modes.md`
  (the owner's scope, the corrections, the lead's notes) at `8f8b043` on `design/durability-modes`,
  equal to `origin`; the probes `bench/durability/` and the raw files
  `results/2026-10-18-t1-macos.txt`, `results/2026-10-18-t2-macos.txt`, and `results/arith.out`
  at `0b407c9`.
- Checked against:
  - `docs/REPLICATION.md`, `docs/design/track-c.md`, `docs/ARCHITECTURE.md` (write path),
    `docs/PLAN.md` (Phase 3R, "Background work is measured under load"), `docs/DISTRIBUTION.md`,
    and `docs/reviews/system-2026-10-17.md` (SYS-8) at `8f8b043`, whose `main` content is `734949e`.
  - `design/peer-transport` at `38a6c6f` (D5, D6, D7, D9, D11, "Library changes", "For Detent").
  - `design/simulator` at `27cfb30` (SIM-R2, D5, D6, D9); `perf/small-indexes` at `7c23560` and its
    review (SI-R6 to SI-R11); `research/no-stalls` (I1, I4, I5).
  - `phase4/format13` at `0954761`: the ledger's freeze-readiness list (`format13.md:107-117`)
    and the fourth re-check's freeze table and F13-R40 to F13-R43 (`format-13-design.md`),
    written four minutes after this design's commit.
  - Code: `consensus` `61149a2` (`core.h`, `types.h`, `storage.h`, `checkpoint.h`,
    `retained_state.h`, `transport/codec.h`); `journal` `cb66f3d` (`turn_log.h`, `journal.h`),
    which `consensus` pins; strata `8f8b043` (`index.hh`, `reader.hh`, `bench/durability/`);
    `Kronuz/sha256` `2bb34f0`; Xapiand's MIT own code at `7e8cccb7d` (two spot checks below).
  - Detent `310e59c`, read only: `SHARED_LIBRARY_DECISIONS.md`, `LIBRARY_BOUNDARIES.md`.
  - `Kronuz/cpp-libraries` `docs/research/foundations-vision.md` at `8814481`.
  - `docs/reviews/calibration.md` at `8f8b043` (equal to `main`): all ten rows, applied below.
- Checked by:
  - Reading every file:line the design cites at the commit its header names. All match but the
    ones the findings name.
  - `quiet check`: exit 1 at 23:36 (`scale-threads-mac2`, strict), so nothing compiled or ran
    until it ended; exit 2 (`nostalls-core-r3`, light) from 23:47, so every run below used
    `nice -n 19` on one thread.
  - Probes in `strata-wt/review-durability/.scratch/review-durability/`, each with its `.out`:
    - `forgetful_model.py`: an explicit-state model of D2, D5, D6, and D7 at 3 voters (writes,
      replication with Raft's conflict rule, both commit cursors, barriers, OS crashes that keep
      only the synced prefix, re-learning, D6's three grant rules, and its timeout as an event).
      At every election it checks that the new leader holds every write acknowledged at `memory`
      and at `quorum`. `model-d10.out`: depth 10, one write, terms up to 2; `model-d12.out`: two
      writes, terms up to 3, to 6 million states. Atomic messages, so a pass is bounded evidence
      and a counterexample is real.
    - `core_probe.cc` against `consensus` `61149a2` with Homebrew clang 23.1.2 under ASan and
      UBSan (`core_probe.out`, no report): a restarted voter's grants, and the leader's handling
      of a second reply to one append.
    - `rescore_t2.py` (`rescore_t2.out`): T2's medians and ratios from the raw file, then the
      figures the estimate leaves out, under assumptions it labels.
  - T1's raw file against the design's table: create is 6 durability calls (6 `F_FULLFSYNC`,
    4 `fsync`), each commit 4 (4 and 3), 14 per index over create and two commits, 140 over 10
    indexes. All as the design states.
  - `HEAD` against `origin` for every repository read (calibration row 5): equal for the strata
    branches, `consensus`, `journal` (its untracked build directories aside), `sha256`,
    `cpp-libraries`, and Detent; `research/no-stalls` moved to `a9448fd` during the review (the
    design cites §5 at `b9dddfd`); Xapiand's local `master` is 26 commits ahead of its upstream and
    was read at `7e8cccb7d`, the design's commit.
  - Not run: the last line of this review.

Citations: `DM:N` is `docs/design/durability-modes.md` at `8f8b043`; `TC:N` is `track-c.md` there;
`PT:N` is `peer-transport.md` at `38a6c6f`. `core.h`, `storage.h`, `checkpoint.h`, and `codec.h`
are `consensus` `61149a2`; `turn_log.h` and `journal.h` are `journal` `cb66f3d`; `index.hh` and
`sizing.cc` are strata `8f8b043`.

## Verdict

**REJECT for first steps 1, 3, 4, and 5; steps 2, 6, and 7 may start. D5 holds; D6's timeout
loses `memory` writes inside the model; and the journal format, the core, and the wire would
freeze without four mechanisms they need (DM-R2, DM-R3, DM-R4, DM-R6).**

What holds, and reproduces:

- **D5 alone holds in the model.** With forgetful voters kept out of elections until they
  re-learn, the model finds no lost `memory` or `quorum` acknowledgement in 10,696,714 states (two
  writes, terms up to 3, depth 11), nor with only D6's every-voter-heard grants in 6,586,795
  (`model-d12.out`).
  The argument at DM:261-271 is the right one: majorities intersect in a voter that holds the
  entry or re-learned it, and terms survive restarts (D4).
- **D6's `quorum` floor holds in the model.** Beyond the model (every voter forgetful), neither
  grant rule produced a `quorum` loss at depth 10 (2,305,718 and 5,194,973 states), as the
  intersection argument at DM:330-332 predicts; a `heard` recovery loses only a write no voter
  held, as DM:333-336 says.
- **The rewind is needed, as the design found.** A leader never moves `next` below
  `matched + 1` (`core.h:967-969`) and never lowers `matched` (`core.h:945, 957-958`), so a voter
  that lost acknowledged entries would be resent nothing it lacks.
- **The promotion's five checks** against `Repair` are each right: the transfer refusal
  (`core.h:600`), the base bound (`:602`) against a checkpoint bounded by `applied_` (`:562`),
  the busy and delivery gate (`:599`), the epoch (`:606`), and skipping applied transactions by
  position (`index.hh:416-427`).
- **The measurements reproduce.** T1's counts as above; T2's medians and every derived figure
  (0.370× and 0.350× at rf = 3, 0.240× and 0.218× at rf = 5, 1.81 and 2.61 µs per follower,
  23,958 and 7,798 documents of replay) recompute from the raw file (`rescore_t2.out`).
- **The wire arithmetic** matches the peer transport's D9: 56 → 64 (PT:612), 104 → 136 and
  `control_frame_bound` 104 + 32·e + b → 136 + 32·e + b (PT:602-605), 96 → 112 and 112 → 128
  (PT:614), coalesced records 48 → 64 (PT:618). At 1,000 groups per node, a third led at rf = 3,
  the 16 bytes per record cost about 107 KB/s per node (6,660 records a second, arithmetic).
- **The Xapiand corrections** spot-check: a failed asynchronous sync is logged and dropped
  (`src/xapiand_fsyncher.h:39-47`), and the debouncer defaults are `opts.cc:114-136`'s. The new
  record kind `0x12` is free (`turn_log.h:87`), and `kern.bootsessionuuid` exists on the
  development Mac.
- **D11 and D13** are sound rejections, and the vocabulary (D10) is the right shape.

What blocks:

- DM-R1: D6's `recovery_timeout` lets a forgetful voter grant and campaign inside the model.
- DM-R2: a same-boot restart after a failed barrier, and on the per-group backend after any
  process crash, keeps a voter electable over a tail it lost or never synced.
- DM-R3: with apply at `commit_memory`, no strata checkpoint can satisfy D1, and one that
  violates it makes D6's repair diverge silently.
- DM-R4: `synced` has no path back to the leader, and a quiescent group never advances
  `commit_durable`.
- DM-R5: the core reads `durable_` at 34 sites; the design assigns a cursor to some, and
  membership changes, promotions, and witness truncation need the durable one.
- DM-R6: the turn log supersedes and reclaims at completion, which D3 moves from the barrier to
  the write.
- DM-R7: three changes reach Detent's groups without an opt-in.
- DM-R8: segment files get a second byte mover and a second flow control beside `RFTXFR01`.
- DM-R9: the version-2 fields are incomplete and freeze before the semantics are reviewed.
- DM-R15: Part B can reopen D1, D5, and the wire after they freeze.

## What freezes

| Element | Where | State | Findings |
|---|---|---|---|
| `RFTW0002`: the end-of-turn synced turn, the open record's boot identity, the clean-close record, recovery after S | D3, DM:195-207 | Changes required: same-boot recovery, supersession at the barrier, activation, S's definition | DM-R2, DM-R6, DM-R18 |
| The per-group journal's boot identity and clean-close marker | D3, DM:210-216 | Incomplete: its recovery stops at the frontier; Detent's backend | DM-R2, DM-R7 |
| Version 2 bodies of kinds 2, 3, 4, 9, 10 | D16, DM:628-631 | Sizes sound; the set incomplete | DM-R4, DM-R5, DM-R9 |
| Kinds 6 and 7 "unchanged" | DM:628 | Wrong: kind 6 needs `forgetful` | DM-R1 |
| Host kinds 256 to 271 and 512 to 527 for the segment fetch | DM:632 | A second mover; prefer `RFTXFR01` or a group-level kind before version 2 | DM-R8 |
| `Membership` + optional `acks` | DM:633 | Sound; its rule needs one statement | DM-R18 |
| `EntryKind::Recovery` | DM:634 | Storage and checkpoint readers call it corruption, not unsupported | DM-R7 |
| `commit_durable` in hard state | Absent | A row owed, or the "only grows" claim withdrawn | DM-R5 |
| Format 13: segment identity, per-version HLC, time bounds and tier mark | DM:404-408, 559-560, 639, 839-840 | Identity per F13-R41; the HLC's place undecided; F13-R43's attribute list not asked for | DM-R15, DM-R16 |

## Rulings on D1 to D16

| Decision | Ruling | Changes |
|---|---|---|
| D1 Commit in three steps | Sound with changes | Checkpoints of a past generation (DM-R3); supersession at the barrier (DM-R6) |
| D2 `memory` | Sound with changes | The window's cap and one model statement (DM-R1, DM-R13) |
| D3 The bounded barrier | Sound with changes | Same-boot recovery, the per-group backend, supersession, S (DM-R2, DM-R6, DM-R18) |
| D4 Hard state first | Sound | None |
| D5 Forgetful voters | Sound as an election rule (`model-d12.out`) | Failed barriers and same-boot restarts (DM-R2); the cursors (DM-R5); opt-in for Detent (DM-R7); the medium (DM-R15) |
| D6 Recovery after a correlated loss | Wrong as written; its `quorum` floor holds | DM-R1 |
| D7 Two cursors | Sound with changes | Checkpoints (DM-R3), the `synced` path (DM-R4), the site table (DM-R5) |
| D8 Segment mode | Sound with changes | One mover (DM-R8), a load-bearing bound (DM-R10), residuals (DM-R11), the estimate (DM-R12), identity (DM-R16) |
| D9 The failover gap | Sound with changes | DM-R10, DM-R14, DM-R17 |
| D10 One vocabulary | Sound with changes | Exact windows and read bounds (DM-R13) |
| D11 No multicast | Sound | None |
| D12 Partition-tolerant writes | Sound as a deferred shape | The HLC's place in format 13, before it freezes (DM-R16) |
| D13 No leaderless replication | Sound | None |
| D14 No stalls | Sound with changes | The memory window's cap (DM-R13); copy-forward's row (DM-R18) |
| D15 Live tuning | Sound with changes | The mode switch against D9's bound (DM-R10) |
| D16 Formats and wire | Changes required | DM-R5, DM-R7, DM-R8, DM-R9, DM-R15, DM-R16 |

## Findings

### Safety

1. **DM-R1. D6's timeout makes a forgetful voter grant and campaign inside the model** (D2, D6,
   T3, T5, the fault model).
   - `model-d10.out`, mode `timeout`, at most one voter forgetful at once, 7 steps: A leads term 1
     and writes v0; B writes it; A acknowledges at `memory`; A loses power before its barrier and
     restarts forgetful with nothing; B is unreachable (a partition, or its process down) until
     A's `recovery_timeout` passes with no leader; A's own log is as up to date as every log it
     has heard, so A campaigns (DM:304-307) and C grants; A leads term 2 without v0, writes its
     `Recovery` entry, and B truncates v0 at the heal (by Raft's rule, or by DM:321-324 if B
     learned the commit). A second trace has the follower lose power instead: it campaigns after
     its timeout, wins with C's grant, and asks A to replace an entry at or below A's commit,
     which today's core refuses (`core.h:868-871`) and D6's rule accepts. Modes `none` and
     `heard` find nothing at the same bound.
   - D6 says as much: after the timeout the winner holds only "every committed entry the
     reachable voters hold" (DM:307-308). The trace shows the timeout firing on one OS crash and
     one partition: inside D2 ("the OS crash or power loss of up to f of 2f + 1 voters",
     DM:172-174), T3 (DM:738), the simulator change ("a forgetful voter counts against f until it
     re-learns", DM:700), and T5 (DM:740). The fault table says `replicated` loses nothing there
     (DM:661), and Q3 recommends exactly this policy (DM:775-777). D6 is introduced for "more
     than f voters ... forgetful at once" (DM:295), but a voter cannot tell that from "f
     forgetful and the rest unreachable" with the signals D16 gives it: kinds 1, 6, and 7 stay
     unchanged (DM:628).
   - The four model statements also differ. D2 says "at once", T3 "in any `sync_interval` plus a
     barrier", the simulator and T5 "forgetful at once". The windowed one admits two forgetful
     voters at once (a slow re-learner, then a second power loss minutes later), which is outside
     D5's argument (DM:261).
   - Fix:
     1. Kind 6 carries `forgetful`. A forgetful voter applies D6's conditions only once it has
        heard, since its restart, f + 1 distinct forgetful voters of the membership in force,
        itself included. Inside the model no voter reaches f + 1, so none grants or campaigns
        before it re-learns, and `aggressive` loses only beyond the model too.
     2. The fault table states the availability this costs: at rf = 3, one forgetful voter and
        one unreachable voter stop elections until either returns, where `strict` would elect.
     3. One model statement, "at most f voters lose their OS or power or are forgetful at once,
        a forgetful voter counting until it re-learns", in D2, the summary, T3, T5, and the
        simulator change.
     4. T3 gains this trace as a forced schedule; T6 gains the mutant "D6 without the f + 1
        count", which the model above kills.

2. **DM-R2. A same-boot restart can keep a voter electable over a tail it lost or never synced**
   (D3, D5, T7, T9).
   - D5 makes a voter forgetful "after any failed barrier ... and never retries the sync as
     success" (DM:241-242), but a failed barrier fails the turn log (`turn_log.h:270-272`,
     `330-331`) and nothing durable records it. The restart sees the same boot identity and no
     clean close, which DM:234 does not call unclean, so the same-boot rule applies: "Recovery
     syncs each segment holding turns after S ... and the voter resumes, not forgetful"
     (DM:238-240). That retry is what SI-R8 rules out: "a reopen in the same boot reads it from
     the page cache ... After a failed `fsync`, Linux may report success to a retry over lost
     data" (`small-indexes-design.md` at `7c23560`).
   - Two outcomes, both inside the model. If the kernel dropped the pages, recovery keeps a
     shorter prefix and the voter, not forgetful, can elect a leader without a `memory` write
     after one more voter's power loss. If it kept them clean, the voter reports `synced` over a
     tail that is not on disk, `commit_durable` counts it, and a `quorum` write rests on one
     durable copy, which a disk loss removes.
   - The per-group backend is worse. Its recovery replays only to the manifest's frontier
     (`journal.h:333-358`); D3 says the variant "needs no format change" (DM:214). Every batch
     acknowledged from memory past the frontier is dropped by a plain process crash, which D2
     says the guarantee survives in any number, and the voter stays electable.
   - Fix:
     1. Same-boot recovery rewrites every turn after S from the bytes it read, then syncs, so a
        page left clean after a failed writeback is written again; a torn turn after S other than
        the last means pages were lost, and the voter recovers forgetful.
     2. A voter reports `synced` only for turns this process synced.
     3. On the per-group backend, same-boot recovery replays framed batches past the frontier to
        the first invalid one, rewrites and syncs them, and publishes the frontier; or that
        backend refuses `acks: written`.
     4. T9 rows: a failed barrier with the pages kept clean and with them dropped, each followed
        by a same-boot reopen; a process crash with batches past the per-group frontier.

3. **DM-R3. Apply at `commit_memory` leaves no checkpoint D1 allows** (D1, D6, D7).
   - D1: no checkpoint records an applied position above `commit_durable` (DM:155-158). D7: apply
     follows `commit_memory` (DM:357). strata's apply sets the index's applied position
     (`index.hh:456`), and `commit()` "makes everything applied durable" (`index.hh:519-527`), so
     a checkpoint always records the current position. Under sustained ingest
     `applied ≥ commit_memory > commit_durable` at every instant, and today's gate refuses the
     capture once apply runs ahead of the persisted commit index (`core.h:562`:
     `applied_ > durable_.commit_index`), which D4 lets lag.
   - A checkpoint taken at the current position breaks D6. Repair restores it with lost writes
     inside, and the redelivered replacements at those positions are skipped as already applied
     (`index.hh:424`): the replica diverges silently, against "Replicas never diverge"
     (DM:328), and T4's digest would catch it only after the fact.
   - Fix: strata checkpoints the newest refresh generation whose position is at or below
     `commit_durable`, the same generation positions an `Image` names (DM:383-391), keeping that
     generation's files, merge inputs included, until a later checkpoint covers them; the core's
     gate compares the capture's `through` with the durable cursor; D1 states the retained disk
     (one checkpoint interval of superseded files); T4 runs a D6 recovery after checkpoints
     taken under sustained ingest. Pausing apply until `commit_durable` catches up is the
     rejected alternative: it stalls every verdict for up to `sync_interval` and a round trip at
     each checkpoint, which D14 forbids.

4. **DM-R4. `synced` has no path back to the leader** (D7, D16, Track C's D12).
   - D7 sends the append reply at `Written` with `matched` as the written index and `synced`
     beside it (DM:366-374). A later barrier has nothing to ride: the leader drops a reply whose
     RPC has no flight (`core.h:939-940`) and one whose `matched` differs from the flight's end
     (`core.h:944`); `core_probe.out` shows both. `sync_through` forces the barrier
     (DM:515-518) but not the report.
   - Quiescence needs only "every member ... matched its last index and commit" (TC:635-637).
     A group quiesces with `commit_durable` behind; with no heartbeats, `commit_durable` never
     advances, its checkpoints (capped there, D1) stop, and its log never truncates. A `quorum`
     write whose entry commits at `commit_memory` can see its group quiesce before
     `commit_durable` reaches it, and its acknowledgement then waits forever.
   - Fix: a progress report the core accepts without a flight, keyed by term and incarnation,
     sent when a barrier moves `synced` past the leader's last `sync_through` and at most once a
     heartbeat otherwise (or a reply to an append that carries `sync_through` waits for the
     barrier; state which); quiescence requires `commit_durable` at the last index and known to
     every member; the `quorum` latency in a `replicated` index stated against `strict`'s (two
     barriers and a round trip, plus at most one heartbeat); T10 measures that latency, and T14
     checks that an idle group's `commit_durable` reaches its last index before it quiesces.

5. **DM-R5. Two cursors over 34 sites, and three of them need the durable one** (D7, D16;
   Track C's D3 and D9).
   - The core reads `durable_` at 34 places (`core.h:46-1185`). The design names a cursor for
     apply, verdicts, refresh, linearizable reads, `quorum` and `all`, the checkpoint ceiling,
     D6's floor, and the append's commit field (DM:348-351, 370). It names none for:
     - membership changes (`core.h:626-627`: an entry of the term committed, none above the
       commit index). On `commit_memory`, a second change can be admitted on a first that a
       correlated loss later removes, which Track C's single-step argument (its D3) never
       allowed for: beyond the model the membership itself, not only a write, would rest on
       page caches;
     - the promotion check (`core.h:650`) and the witness's bound (`data_matched`,
       `core.h:1136-1145`, from `matched`, which D7 makes the written index). A witness that
       truncates entries the data voters hold only in page cache turns a recoverable correlated
       loss into a lost write;
     - `deliver` (`core.h:1163-1165`), which reads the persisted commit index: with D4's
       commit index riding the bounded barrier, apply would wait for the barrier, against D7;
     - `Read` (`:739-747`), the uncommitted cap (`:722`), install's caught-up check (`:484`),
       and `Repair`'s bound (`:602`).
   - `commit_durable` must be clamped to the verified prefix in appends and coalesced
     heartbeats, as the commit is (`core.h:914`; TC:631-633). Otherwise an honest follower with
     a stale suffix learns a `commit_durable` above entries it must still replace, and fails
     closed against D6's floor (DM:321-324; calibration row 8's check).
   - The threat model's floor "only grows" (DM:652), but nothing persists `commit_durable`, and
     D16 has no hard-state row.
   - Fix: a site table like Track C's R2 (TC:227-251), each `durable_` use with its cursor and
     reason; membership admission, promotion, and witness truncation on the durable cursor;
     `commit_durable` clamped in kinds 3 and 9; `commit_durable` in hard state with its D16 row,
     or the claim withdrawn.

6. **DM-R6. The turn log supersedes and reclaims at completion, which D3 moves to the write**
   (D1, D3, T9).
   - `completed()` runs once "the staged turn's barrier returned" (`turn_log.h:315-329`): it lets
     a forwarded copy replace its original and a checkpoint record supersede its group's earlier
     records (`:343-350`), then reclaims every segment left with no live record (`:378-392`).
     D3 releases turn k + 1 at turn k's write (DM:193-194) and says nothing of these updates.
   - Applied at the write, a copy forwarded into an unsynced turn retires its synced original,
     the original's segment is recycled, and a power loss loses both: a synced record, which a
     `quorum` write may rest on, is gone. A checkpoint record would release its group's records
     before its own barrier.
   - Fix: D3 states that copy replacement, supersession, and reclamation take effect at the
     barrier covering the turn, never at its write; T9 adds power loss after a copy-forward
     turn's write whose source segment would otherwise be reclaimed.

7. **DM-R7. Three changes reach Detent's groups without an opt-in** (D3, D5, D6, D16).
   - "After any failed barrier" (DM:232) applies to every group, so a Detent voter whose barrier
     fails would restart unable to vote, and a recovery could write `EntryKind::Recovery` into
     its log. "Older readers refuse the kind" (DM:634) cites the wire codec (`codec.h:78, 170`);
     the storage reader accepts kinds up to `Configuration` (`storage.h:242, 272`), and the
     checkpoint reader calls a later kind corruption (`checkpoint.h:107`), which F13-R42's lesson
     says must read as unsupported.
   - D3's per-group variant (DM:210-216) changes the backend Detent runs (SYS-8): a boot identity,
     a clean-close marker, and DM-R2's recovery.
   - The version-2 bodies add 32 bytes to every append and 16 to every reply of groups that never
     use them. Detent agreed to restart into "finalized version 2"
     (`SHARED_LIBRARY_DECISIONS.md:131`), the peer transport's, which holds none of these fields.
     Its rule: "nothing Detent calls today changes behavior unless Detent opts in" (`:10-11`).
     The design has no "For Detent" list (the peer transport's is PT:1329-1372).
   - Fix: forgetfulness after a failed barrier, the `Recovery` entry, and the per-group journal's
     new records and rule apply only to groups whose configuration allows memory
     acknowledgements, or to hosts that opt in (offered to Detent: it closes fsyncgate for it
     too); `Recovery` under a new batch magic, read as unsupported by older storage and
     checkpoint readers; a "For Detent" list with each change, its bytes, and its opt-in, which
     the lead adds to Detent's file; Detent's agreement before step 4.

### Mechanism and wire

8. **DM-R8. Segment files get a second byte mover and a second flow control** (D8, D16).
   - D8 moves files through host frames and keeps consensus's snapshot transfer for "the image
     itself (for strata, the manifest)" (DM:392-400). Host frames are node-level, authorized by
     the session's node with the group in the body (PT:444-446), and spend group zero's credit
     (PT:730). Snapshot chunks are group-level kinds 16 and 17 (PT:623-624) on the group's own
     bulk window, "so one mechanism paces snapshots" (PT:840-843; TC:796-797).
   - So strata's installs use two movers and two flow controls, and the host re-implements D5's
     binding. Its rule differs: a committed membership only (DM:649), where D5's rule 2.2 binds an
     uncommitted one, so an honest learner whose addition is uncommitted cannot fetch
     (calibration row 8). One group's multi-gigabyte catch-up shares group zero's credit with
     every other group's per-refresh fetches to that node, which Track C's R12 exists to prevent.
   - Fix, before version 2 freezes: one mover. Extend `RFTXFR01` (kinds 16 and 17) with a file-set
     transfer addressed by physical identity, each file with its size, CRC-32C, and SHA-256,
     skipping what the receiver holds, at group level with the group's bulk credit and D5's
     binding; segment shipping, D18's retry, repair, Q7, and Detent's one-file image all ride it.
     Or define the fetch as a group-level kind now, since 19 to 255 are refused at version 2
     (PT:626). Either way one implementation, in `consensus`, which strata drives.

9. **DM-R9. The version-2 fields are incomplete and freeze before their semantics are reviewed**
   (D16, first steps).
   - The fields land in version 2 "before the coordinated `DTC2` commit" or wait for version 3
     with the feature off (DM:620-624). Step 4 waits only for the peer transport's re-check and
     Detent's agreement, while step 3, the semantics, waits for the full review (DM:821-822).
   - Missing: `forgetful` on kind 6 (DM-R1); the `synced` report (DM-R4); `commit_durable`'s
     clamp and hard state (DM-R5); the mover's kinds (DM-R8); `Recovery`'s storage magic
     (DM-R7); Part B's possible cursor (DM-R15).
   - Fix: the coordinated commit waits for this design's full review of D5 to D8, or these fields
     leave version 2 and memory acknowledgements wait for version 3 under D7's feature rule
     (PT:523) (Q8); D16 lists every field above with its kind and size.

### Segment mode

10. **DM-R10. The replay bound is not load-bearing** (D9, D14, D15, T8).
    - "Replay: at most `election_min`" (DM:459) holds only while each follower's fetch keeps up.
      With the leader's uplink or a follower's disk saturated, every install lag passes the
      bound and the promoted follower replays all of it. D14's remedy, "the leader flushes
      earlier" (DM:596), makes more, smaller segments and more merge output to ship (+103% bytes
      at the 16 MiB configuration, `arith.out`). D15's switch from document to segment mode
      takes "the leader's next image whole" (DM:614), during which a follower's lag is the whole
      index.
    - Fix: a follower whose install lag passes the bound applies the tail itself through the
      document-mode path it already keeps for mixed versions (DM:431-432) until an `Image`
      covers it, so the replay stays within `election_min` at that follower's CPU, one
      mechanism; T8 runs with the leader's uplink saturated and during a mode switch.

11. **DM-R11. Segment mode spreads a leader's faults and delays followers' reads** (D8, D10, T4,
    T11).
    - In document mode each voter builds its own segments, and the digest comparison catches the
      one that diverges (REPLICATION.md:64-66). In segment mode followers install the leader's
      bytes, so a leader-side apply bug, nondeterminism, or bit flip before checksumming
      (REPLICATION.md:19) reaches every replica under valid CRC-32C and SHA-256, and only the
      log, which checkpoints truncate, remains to rebuild from. T4's equality (DM:739) holds by
      construction between a leader and the followers that installed its bytes. D13 rejects
      leaderless replication partly for leaving "no digest to compare replicas by"
      (DM:578-579).
    - Followers' visible state trails the leader by a refresh plus the `Image`'s commit, the
      fetch, and the install. `prefix` (the `replicated` default, DM:510), `bounded`, and
      `session` reads on followers see that lag, which D10 does not state; OpenSearch's caveat is
      in the prior art (DM:715).
    - Fix: the fault model and REPLICATION state the blast radius; a verifier applies a sampled
      range in document mode on one replica per group on a schedule and compares its digest with
      the installed image at that position, a mismatch reported as corruption; T4 seeds a
      leader-side fault in segment mode that the verifier must catch; D10 states the follower
      lag, and T11 tests `session` and `bounded` against it.

12. **DM-R12. The 0.35 to 0.37 estimate leaves out the hash, the transport, refresh, and D9's
    cadence** (D8, Q1, Q4, T2).
    - SHA-256: `Kronuz/sha256` is "portable C++, with no SHA instructions" (its README:68), and
      the install probe checks CRC-32C only (`sizing.cc:230`). At an assumed 200 to 400 MB/s, a
      pass costs 1.09 to 2.17 µs/doc at one flush and 3.04 to 6.07 at 16 MiB (`rescore_t2.out`),
      as much as the follower's whole measured cost or more. The leader hashes every shipped
      byte once, and a follower again when it fetches from a non-leader (DM:650): rf = 3 becomes
      0.379× to 0.387× (leader-sourced) and 0.395× to 0.420× (peer-sourced) at one flush, 0.358×
      to 0.365× and 0.373× to 0.397× at 16 MiB. A peer-sourced follower at one flush is 0.068× to
      0.093× a document-mode follower: under T2's 0.1× (DM:737), over Q1's "a twentieth or less"
      (DM:759-760).
    - Transport: the install probe reads the leader's files from the local page cache and writes
      without a sync (`sizing.cc:219-238`). No network, no TLS, no leader serving rf − 1 copies,
      no follower `fsync` is in the figures; T2's target includes them (DM:737), the summary and
      Q1 do not say so. OpenSearch's own result includes "a loss at 9 replicas" (DM:715).
    - Refresh and cadence: neither configuration refreshes, but the default 1 s refresh flushes
      (ARCHITECTURE.md:79-82, 105-106), which sets segment sizes, merge output, and the shipped
      bytes. The one-flush run holds 100,000 documents unshipped, 4.17× D9's replay bound of
      23,958: a cadence document mode may run when refresh is relaxed for bulk ingest (as
      Elasticsearch users set `refresh_interval: -1`) and segment mode may not. Segment mode at
      the 16 MiB cadence against document mode at one flush is 1.047× (136.3 against
      130.2 µs/doc): more CPU, not 0.37. For one large index the estimate brackets
      [0.37, 1.05]; where many indexes share the node's buffer budget (ARCHITECTURE.md:102-103)
      both modes flush small, and the 0.35 pairing applies.
    - Fix: D8, the summary, and Q1 state the figures as leader-sourced without hash or transport,
      with the bracket; T2's procedure measures at the default refresh, over loopback TLS with
      SHA-256 (or a hardware-accelerated hash, by extending `Kronuz/sha256` with SHA-NI and
      ARMv8 SHA2), from both sources, and for one large index at the cadence D9 allows against
      document mode at its own best cadence.

### Exactness

13. **DM-R13. Several loss windows and read bounds are not exact** (D2, D10, D14, the fault
    model, T7, T11).
    1. **`memory`.** The window is `sync_interval` plus the barrier's actual latency
       (DM:36, 175-178), and D14 lets a slow device widen it with no cap: `max_unsynced`
       "refuses nothing" (DM:594). A device that stalls for 30 s leaves 30 s of acknowledged
       writes unsynced on its voter, and on a majority when the stall is shared (one storage
       backend under several voters). Fix: past `max_unsynced`, or k · `sync_interval` since a
       core's oldest unsynced write, that core's memory acknowledgements wait for its barrier:
       the acknowledgement degrades to `quorum`'s latency, no write is refused, and D2 states the
       window as the smaller of the cap and the interval plus a barrier. Lucene, Tantivy, and
       Xapian have no counterpart (they acknowledge after a commit's syncs), so the cap is named
       by its mechanism.
    2. **`replicated` under an OS crash of at most f voters** (DM:661) is not "nothing lost"
       while DM-R1 stands.
    3. **`leader`.** "Survives ... a leader process crash it returns from within an election
       timeout" (DM:492) and "if another voter is elected before it returns" (DM:660) both miss
       the case: the restarted node comes back a follower whose timer starts at its restart
       (`core.h:432`), its peers' timers started at their last heartbeat and fire first, and the
       first to campaign wins with the other follower's grant, the ex-leader refusing, and
       without the tail (`core.h:765, 803, 1002-1010`). The condition is "if another voter wins
       the next election". T7 then contradicts D10: it requires every write acknowledged at
       `leader` or stronger to survive 1,000 `SIGKILL`s (DM:742). Its level must be `memory`.
    4. **`relaxed` under a partition** (DM:664). A quiescent leader's CheckQuorum starts afresh
       when the first write wakes it (TC:357-360), so the window runs `election_max` from that
       write, not from the partition.
    5. **`bounded(K, T)`** (DM:500). A deposed leader's messages carry a stale commit index for up
       to `election_max` on a minority side, so the real-time staleness is T plus `election_max`,
       not Cosmos DB's bound against the true state the table names.
    6. **`session`** (DM:501). "Whose log holds the token's term at its index" cannot be checked
       once a checkpoint truncates below the index. Fix: a term history per group (the first
       index of each term, as Kafka's leader-epoch cache keeps), retained across checkpoints.
    - Fix for 2 to 6: each restated in D10 and the fault table; T7's level `memory`; T11 for a
      minority side's `bounded` reads and for tokens below the base.

14. **DM-R14. Leases assume restarted voters wait, and they do not** (D9, T5).
    - `core_probe.out`: a voter restarted at term 5 grants a pre-vote at term 6 one millisecond
      after its restart, and a vote at the next step. The guard reads leader contact held only in
      memory (`core.h:765, 770, 803`). "The old lease ends before any follower's timer can fire"
      (DM:473) does not cover a voter that grants on request, so a candidate can win inside the
      old lease and lease reads go stale.
    - Fix: with leases on, a voter that starts grants no vote or pre-vote for the lease duration
      after its start, as if it had heard a leader then (the assumption a lease rests on, made
      explicit); a new leader serves lease reads only after a quorum answered a heartbeat of
      its term and it committed an entry of its term; T5 adds restarts with leases on. Leases are
      off by default, so this binds before they are built.

15. **DM-R15. Part B can reopen what A freezes** (D1, D5, D16, Q7).
    - Of Part B's open choices (DM:829-840), four reach A's frozen surfaces:
      1. "Whether the acknowledgement is A's `memory` ... or process memory only": with process
         memory, a process crash loses acknowledged writes, and D5's same-boot rule ("resumes, not
         forgetful", DM:238-240) is wrong for such groups.
      2. "What `commit_durable` means without a synced log": a group that never syncs never
         advances `commit_durable`, so D1's ceiling and D6's floor stop its checkpoints; a
         disk-tier position as a third cursor would be a wire field version 2 freezes without.
      3. Retention needs "each segment's time bounds readable without opening it (a format-13
         reservation)" (DM:839-840), which D16 does not ask for (DM:639). Format 13's fourth
         re-check places it in the manifest's attribute list (F13-R43), with a memory tier's "not
         yet on disk" mark and HLC bounds as later tags.
      4. A retention drop "stamped with the leader's time" (DM:843-844) is a second clock beside
         D12's HLC unless it is the HLC.
    - Fix: A decides B's acknowledgement medium now (recommended: A's `memory`, written to the
      kernel, so D3, D5, and the wire stand), or D5 defines forgetfulness per medium and D16
      reserves the cursor; D1 and D7 say what bounds checkpoints in a group with no synced log;
      D16 asks format 13 for F13-R43's attribute list with the time-bound and tier tags;
      retention stamps use the HLC.

16. **DM-R16. Format 13's identity and the HLC need this design's choice before the freeze** (D8,
    D12, D16).
    - "The producing term in the identity" (DM:404-405). F13-R41 shows today's ids collide across
      replicas (A's and B's segment 2 share `0200000000000000 4141414141414141` at 732 and 740
      bytes) and that a producing term repeats across replicas that flush in one term, as
      document mode does; and the footer's CRC does not bind the 16-byte prefix (CRC-32C over any
      valid prefix is `0x48674bc7`). So "files it lacks" (DM:387) must compare identity, size,
      and the whole file's checksums, and the identity is F13-R41's (128 random bits, or a
      per-node incarnation with a counter).
    - "A per-version HLC, reserved in format 13" (DM:559-560). Format 13's table answers "the
      durability design chooses": the version field (F9's u64) or a `keys` sub-structure under an
      unlisted bit, and "LWW also needs a delete's HLC until the conflict window closes ...
      tombstones as keyed documents". D12 chooses neither and says nothing of deletes. The HLC is
      also a foundations need with no library yet (`foundations-vision.md`, "Foundations the
      vision is missing").
    - Fix: D8 adopts F13-R41's identity and the whole-file check; D12 chooses the HLC's place and
      tombstones before format 13 freezes, and names the HLC as a foundations library, not strata
      code.

17. **DM-R17. D9's components are misattributed, and the client's part is missing** (D9, T8).
    - A pre-vote persists nothing (`core.h:764-767, 1007-1010`), so the pre-vote round is a round
      trip, not "two barriers and a round trip" (DM:457; `arith.py`'s `rounds = 2`). The core
      persists the no-op before it sends (`core.h:1088-1090`; appends leave from `drive` after
      `Persisted`, `:470-473`), so a `quorum` first commit is two barriers and a round trip, not
      one barrier (DM:458). The total is 2,323 ms, not 2,403 (−3%); about 2,164 ms for `memory`.
    - The gap ends at the leader's first acknowledgement, but a client also waits out its request
      to the dead leader and is redirected, which T8's "next acknowledged write" includes.
    - Fix: the components corrected; the client's detection and redirect stated (strato's
      timeout and retry), or T8 measured at the leader.

18. **DM-R18. Precision** (D3, D5, D7, D14, D16, T14, the threat model).
    1. "The journal holds no clean-close record" (DM:234): the newest epoch must end with one,
       or a clean close, a same-boot reopen, and then an OS crash would read clean.
    2. "So turns land in file order" (DM:194) holds in the page cache, not on disk, where
       writeback reorders; the recovery rule (DM:197-200) already handles disk order and T9's
       sector subsets test it. The sentence should say so.
    3. `acks` "takes effect when committed" (DM:355), "applies by position, as limits do"
       (DM:633), a membership is "in force from the moment it is durable" (TC:216-218), and D5
       reads the newest durable entry (DM:231-232). One rule: a node's own replies and
       forgetfulness follow the newest durable entry, the leader's client acknowledgements the
       committed one.
    4. `RFTW0002`'s activation: a node rolled back to N − 1 cannot read its own `RFTW0002`
       journal, so the driver writes it only after memory acknowledgements activate (PT:524).
    5. Limits: "a fixed number of fetches per requesting node at once" (DM:653) names no number
       or mechanism (calibration row 6; Elasticsearch's recovery chunk concurrency is the
       counterpart, and Lucene, Tantivy, and Xapian have none).
    6. T14's target is "the bound `research/no-stalls` sets" (DM:749), not yet set.
    7. D14's copy-forward row is "Unchanged" (DM:597), but DM-R6 changes when it releases
       segments.
    8. S, "the turn its writer last saw synced" (DM:195-196), must be the last turn whose write
       returned before a barrier that has since returned started, since a write concurrent with
       a barrier may or may not be covered.

## The open questions

- **Q1, Xapiand's default: `replicated`,** on DM-R1 (its loss row) and DM-R12 (its CPU claims
  restated). It still loses less than today in every row: today's process crash costs seconds.
- **Q2, `sync_interval`: 100 ms,** with DM-R13's cap.
- **Q3, the recovery policy: `balanced`,** with DM-R1's f + 1 count; `aggressive` then loses only
  beyond the model.
- **Q4, merges: shipped,** with their hashing and transport counted (DM-R12).
- **Q5, `memory` at rf = 1: option 1.** At rf = 1 it is process-crash safety, and D6 never runs.
- **Q6, one vocabulary: option 1.**
- **Q7, the split: option 1 only with DM-R15,** or Part B can reopen frozen formats and the wire.
- **Q8 (new), the coordinated version-2 commit:** it waits for this design's full review of D5 to
  D8 (recommended), or version 2 ships without these fields and memory acknowledgements wait for
  version 3. The owner and Detent decide, since Detent restarts once into version 2.

## Which work starts when

| Step | Starts | Waits for |
|---|---|---|
| 1. `RFTW0002` | After the re-check | DM-R2, DM-R6, DM-R7's gating, DM-R18 items 1, 2, 4, and 8 |
| 2. Boot identity and the cross-IO directory barrier | Now | Additive; Detent's suite at its commit |
| 3. `consensus`: cursors, forgetful voters, `Recovery`, `Repair{promotion}`, the ceiling | After the re-check | DM-R1, DM-R3's core gate, DM-R4, DM-R5, DM-R7 |
| 4. Version-2 fields | After the re-check and Q8 | DM-R1, DM-R4, DM-R5, DM-R8, DM-R9, DM-R15 |
| 5. `strata::group` | Phase 3R | DM-R3, DM-R8, DM-R10, DM-R11, DM-R16 |
| 6. The simulator's fault classes and checker modes | Now | DM-R1's trace as a forced schedule and its single model statement |
| 7. T1 on Linux and FreeBSD | Now | Nothing |

## Blocking items / Required changes

Before steps 1, 3, 4, and 5, and before the re-check:

1. DM-R1: kind 6 carries `forgetful`; D6's conditions only after f + 1 forgetful voters heard;
   one model statement; T3's forced schedule and T6's mutant.
2. DM-R2: same-boot recovery rewrites the tail; a torn turn before the last makes the voter
   forgetful; `synced` only for this process's barriers; the per-group backend's same-boot rule
   or its refusal of memory acknowledgements; T9's rows.
3. DM-R3: checkpoints of the newest generation at or below `commit_durable`, with retention and
   the core's gate; T4's row.
4. DM-R4: the `synced` report's path and latency; quiescence on `commit_durable`.
5. DM-R5: the site table; membership, promotion, and witness truncation on the durable cursor;
   the clamp; `commit_durable`'s persistence.
6. DM-R6: supersession, copy replacement, and reclamation at the barrier; T9's row.
7. DM-R7: opt-ins for Detent; `Recovery`'s magic and unsupported classification; the "For
   Detent" list.
8. DM-R8: one mover for segment files, at group level, decided before version 2.
9. DM-R9: the complete field list and Q8's order.
10. DM-R15: B's acknowledgement medium decided, or A's rules stated per medium; F13-R43 asked for.

In the revision, not blocking: DM-R10 to DM-R14 and DM-R16 to DM-R18.

## Conditions

1. **C1. Done criteria.** The ledger has none (`status/durability-modes.md:9`). The lead writes
   them before step 2. Proposed: K1, step 2 with tests on Linux, macOS, and FreeBSD and Detent's
   suite; K2, step 1 with T9's rows, DM-R2's and DM-R6's included; K3, step 3 with T6's mutants,
   DM-R1's and DM-R5's included, and T12; K4, ASan, UBSan, and TSan over every touched test and
   coverage at TESTING's targets; K5, CI green on all jobs and Detent's suite at each library
   commit; K6, step 4 only after the full design review and Q8.
2. **C2. For the security reviewer,** through the lead: the fetch's binding by committed
   membership against D5's rule 2.2 (DM-R8); which fetches need SHA-256 and what it costs
   (DM-R12); a member that sends `forgetful` probes falsely can bring D6's count to f + 1 early,
   which is the compromised-voter row (PT:213), but the row should say so.
3. **C3. Library commits** record Detent's build and its `LIBRARY_BOUNDARIES.md` entry (Track C's
   condition 1).
4. **C4. Both replication modes.** T3 to T5, T8, T11, and T13 run in document and segment mode,
   and the gate rows state that matrix's cost.
5. **C5. The changes to other documents** (DM:679-704) gain DM-R11's residual for REPLICATION and
   DM-R13's restatements.

## Verdict and required changes, in one line each

**REJECT for first steps 1, 3, 4, and 5; steps 2, 6, and 7 proceed under C1 and C3.**

1. DM-R1: D6's timeout grants inside the model; count f + 1 forgetful voters first.
2. DM-R2: same-boot restarts trust a tail a failed barrier or the per-group frontier lost.
3. DM-R3: apply at `commit_memory` leaves no checkpoint D1 allows; checkpoint a past generation.
4. DM-R4: `synced` cannot reach the leader, and quiescent groups stop at `commit_durable`.
5. DM-R5: 34 `durable_` sites; configuration, promotion, and witness on the durable cursor.
6. DM-R6: turn-log supersession and reclamation must wait for the barrier.
7. DM-R7: failed-barrier forgetfulness, `Recovery`, and the per-group change reach Detent.
8. DM-R8: a second byte mover and flow control for segment files.
9. DM-R9: version 2's fields are incomplete and freeze before review.
10. DM-R10: the replay bound depends on fetch bandwidth; followers apply past it.
11. DM-R11: segment mode spreads leader faults and delays follower reads; a verifier.
12. DM-R12: the CPU estimate omits the hash, transport, refresh, and D9's cadence.
13. DM-R13: the `memory`, `leader`, `relaxed`, `bounded`, and `session` statements, and T7.
14. DM-R14: restarted voters grant at once, which leases must forbid.
15. DM-R15: Part B's medium, cursor, and format needs reach A's frozen surfaces.
16. DM-R16: F13-R41's identity and the HLC's place in format 13.
17. DM-R17: D9's components and the client's redirect.
18. DM-R18: clean close per epoch, disk order, the `acks` rule, activation, limits, T14, S.

Not run: no benchmark, timing, or A/B, and no SHA-256, network, or TLS throughput measured
(DM-R12's sensitivities are arithmetic over labeled assumptions); no strata build or suite (the
branch changes no library code, and CI skips docs); no `consensus` or `journal` suite, only
`core_probe.cc` against their headers; no simulator (unbuilt); no Linux, FreeBSD, or arm64; no
fuzzing; the model only to depth 10 or 11 at 3 voters, two writes, and terms up to 3, with
atomic messages; the security reviewer's review of the threat model not read (it runs
separately); the prior art's quotations (OpenSearch, KIP-966, Cosmos DB, PostgreSQL, MongoDB)
not re-fetched; no Xapian or Xapiand GPL source (`src/xapian`, `sortable_serialise`) opened.

## Re-check (revision 1)

- Date: 2026-10-19, read and run from 06:39 to 07:25 PDT.
- Reviewed: `design/durability-modes` at `fc30ed3` (revision 1 at `1efb815`, the lead's calls on Q9
  and Q10 at `fc30ed3`; `git log --first-parent 8f8b043..fc30ed3`, 12 commits, `main` merged at
  `1365cd1`): `docs/design/durability-modes.md` (its "Changes in revision 1" first),
  `docs/status/durability-modes.md` (the owner's answers to Q1 to Q6 and Q8, the lead's calls on
  Q9 and Q10), the model sources `bench/durability/model/forgetful_model.py` and
  `forgetful_check.cc`, `bench/durability/arith.py`, and the raw files
  `results/model-crosscheck.out`, `model-exhaustive.out`, `model-swarm.out`,
  `model-schedules.out`, `arith.out`, and `2026-10-18-t2-macos.txt`. The small-indexes design at
  `perf/small-indexes` `45985dc` (`git show`, D3 and D3-C). The security re-check (`c87385e`,
  pushed to this branch after `fc30ed3`) read for overlap; its findings are its own.
- Checked against: the preliminary review above (DM-R1 to DM-R18, C1 to C5) and the security
  review (DMS-1 to DMS-9); Track C at `fc30ed3` (D1, D3 and its replacement-window table at
  `track-c.md:270-275`, D4, D9); consensus `61149a2` (`core.h`: all 34 lines that read `durable_`,
  and `934-972`, `1145-1172`); journal `cb66f3d` (`turn_log.h:84-88`, `268-335`); strata
  `index.hh` at `fc30ed3` (`335-344`, `405-441`, `498-511`, `2165-2196`, `2253-2276`,
  `2300-2336`); the peer transport at `38a6c6f` (`624`, D9's sizes); format 13 at `43ee403`
  (F13-R41 `:470-482`, F13-R51 `:1116-1123`, bit 16 `:358`, F21 `:1152-1170`); PLAN's recovery
  tooling (`PLAN.md:663-680`); `calibration.md` (ten rows, unchanged since the preliminary
  review, applied below).
- Checked by:
  - `quiet check`: exit 2 (light holds `qexp-e4a`, `qexp-e4b`, `qexp-joint`, `qexp-m3`) or 0 at
    every check from 06:39 to 07:22, never 1; every build and run at `nice -n 19`, at most four
    jobs, each long run under a wall-clock cap (`run_bounded.sh`).
  - Builds: the designer's `forgetful_check.cc` with Homebrew clang 23.1.2, `-O2`, and `-O1` with
    ASan and UBSan (a depth-8 run: 1,432,552 states, the Python model's count, no report); my
    variant `forgetful_check_r1.cc` (the designer's file plus same-boot process restarts and
    round retries), `-O2` and with ASan and UBSan (1,489,228 states at depth 8, its Python
    counterpart's count, no report).
  - Reproduced: the cross-check counts, 9,963,394 states (three voters, depth 10, 29 s) and
    12,039,941 (five voters, depth 5, 47 s); every forced schedule under five modes
    (`schedules-rerun.out`): `dmr1` loses under `timeout`, `fplus5` under `timeout` and `fplus`,
    `heard5` under `timeout`, `heard`, and `fplus`, `fplus5b` fences under `timeout` and `fplus`,
    and `round` blocks all four; `live3` decides under `round`.
  - Deeper, wider, and new: the run table after the status table, each with its `.out`.
  - Probes, all in `strata-wt/review-durability/.scratch/review-durability/r1/`, kept with
    their outputs: `forgetful_r1.py` (restarts and retries on the designer's Python model, and
    the `restart3` schedules), `forgetful_check_r1.cc` (the same in C++), `forgetful_members.py`
    (the model with Track C's membership in the log and a replacement window, the `six` and
    `six_loss` schedules), `members_final_state.py`, `members_prefix_walks.py`, `rescore_r1.py`
    (revision 1's CPU figures from the raw T2 file), `rerun_schedules.sh`, `run_bounded.sh`, the
    runs' `cc-*.out`, `py-*.out`, `schedules-rerun.out`, `restart3-schedule.out`,
    `members-schedules.out`, `members-six_loss-final.out`, `rescore_r1.out`, and copies of
    `core.h`, `turn_log.h`, and the small-indexes and peer-transport designs at the commits
    above. The preliminary review's probes are one directory up, unchanged.
  - Not run: the last line of this section.

Citations: `DM:N` is `docs/design/durability-modes.md` at `fc30ed3`; `ST:N` is
`docs/status/durability-modes.md` there; `SI:N` is `small-indexes.md` at `45985dc`; `core.h` is
consensus `61149a2`; `r1/` is `.scratch/review-durability/r1/`.

### Verdict

**REJECT for first steps 1 and 3; steps 4, 5, and 8 may start under C9, C11, and C12; steps 2, 6,
and 7 continue, step 6 under C10. Revision 1 answers every preliminary finding, and its D6 is a real
correction: my DM-R1 fix (f + 1 forgetful voters heard since the restart) was unsound at five
voters, as the designer found (`fplus5`, reproduced), and counting again at the round's own term
holds for one fixed membership at every depth I reached. Two new safety findings put `memory`
writes at risk inside the model: forgetfulness does not survive a same-boot restart (DM-R19, 7
steps, three voters, one OS crash), and the exactness argument assumes one membership, which
Track C's replacement window at rf = 5 breaks (DM-R20, six voters, at most two forgetful at once,
a `memory` write gone from every replica). Both fixes are small, a persisted mark per group and a
majority count, each with its proof and its schedules.**

What holds, and reproduces:

- **D6 at a fixed membership.** The exactness argument (DM:521-533) is right when one membership
  is in force: a normal leader above U needs a quorum that avoids every forgetful answerer and the
  candidate, which 2f + 1 voters cannot supply. The designer's cross-check counts reproduce
  exactly, and my runs go deeper and add process restarts and retried rounds (the run table): no
  violation at three voters to depth 13 with two writes, at five voters to depth 8 with two
  writes, and with restarts and retries at three voters to depth 12 and at five voters to depth 8.
- **My DM-R1 fix was wrong**, and revision 1 says so with the trace: `fplus5` loses `v0` in 15
  steps with at most two voters forgetful at once (`schedules-rerun.out`), because the count since
  the restart gathers episodes that never overlap. The check that would have caught it is the one
  DM-R20 applies below: build the schedule in which forgetful episodes run one after another.
- **The journal and the cursors.** Same-boot recovery (DM:370-378), effects at the barrier
  (DM:353-359, against `turn_log.h:315-329`, where `completed()` applies notes and reclaims), S
  (DM:360-366), the site table (DM:673-699: all 34 `durable_` lines of `core.h` are listed, each
  with a cursor that matches the code it guards), the `synced` path and quiescence (DM:649-657),
  `commit_durable` as the hard state's commit index (DM:407-411, 1025).
- **The mover, the wire, and Detent.** D17's sizes recompute (`Begin` 6,480 + 52, `Request`
  168 + 16, `Missing` 4 + 795 × 8 within 6,532); the fields are version 3's (DM:997-1001); "For
  Detent" (DM:1216-1242) gates every change on `acks: written`.
- **The CPU figures recompute** from the raw T2 file (`rescore_r1.out`): follower 0.051× to
  0.112×, three voters 0.376× to 0.434×, five voters 0.250× to 0.318× of document mode's CPU,
  matching DM:131-132, 775-776, and T2's estimate (DM:1281); each is labeled an estimate
  (DM:771). Item 5 of the request is answered under DM-R12.
- **The small-indexes rules** match `45985dc` in substance: SI-R19's entry (DM:273-277 against
  SI:308-314), SI-R20 and SI-R26's restore (DM:287-297 against SI:365-376), SI-R25's replay
  (DM:298-309 against SI:401-421). SI-R24's claim sentence is DM-R22 item 7.

What blocks:

- DM-R19: a forgetful voter that restarts in the same boot, or closes cleanly and reboots, before
  it re-learns becomes electable without the entries it lost (blocks steps 1 and 3).
- DM-R20: in a six-voter replacement window, a recovery election wins with three forgetful
  answers from episodes that never overlap, and its leader later wins a normal election and
  replaces a `memory` write everywhere (blocks step 3; binds step 6's schedules).

### Status

| Item | Status | Where, at `fc30ed3` |
|---|---|---|
| DM-R1 D6's timeout | **Partially closed.** The kind 6 flag (DM:1011), the round at its own term (DM:504-533), the flags on kinds 1 and 2 (DM:1007-1008), one model statement (DM:325-327, 118-120, 1156-1157, T3 and T5 at DM:1282, 1284, the simulator change DM:1210), the availability price (DM:579-580, 1164), T3's forced schedules and T6's mutants (DM:1282, 1285) are all in. At a fixed membership the rule holds in every run below. It does not hold across a membership change (DM-R20), and it rests on forgetfulness that a restart can clear (DM-R19) | DM-R19, DM-R20 |
| DM-R2 Same-boot restarts | **Closed.** Rewrite and sync after S before the open record, forgetful on any torn turn after S (DM:370-378); `synced` only for this process's barriers (DM:377-378); the per-group backend refuses `acks: written` (DM:393-398); T9's rows (DM:1288). DM-R19 is a separate hole beside it | |
| DM-R3 Checkpoints | **Partially closed.** The rule is right: the newest refresh generation at or below `commit_durable`, its files kept until a later checkpoint covers them, the core's gate on `through` (DM:257-272, 686), T4's row (DM:1283). Its claim that today's retention keeps those files (DM:264-266) does not hold (DM-R23) | DM-R23 |
| DM-R4 The `synced` path | **Closed.** Every reply carries `synced`; a reply to an append whose `sync_through` reaches its entries waits for the barrier; coalesced records without a flight; quiescence on `commit_durable` (DM:649-661); T10, T14 (DM:1289, 1293). Reply order around a waiting reply is DM-R22 item 3 | |
| DM-R5 The cursors | **Closed.** The site table covers all 34 lines (DM:673-699; `grep` of `core.h`); membership, promotion, and witness on `commit_durable` (DM:682-683, 695-697); both commits clamped (DM:665-667, 1009, 1013); the hard state's commit index is `commit_durable` (DM:407-411, 1025). The leader's own match at `core.h:1151` is DM-R22 item 2 | |
| DM-R6 Supersession | **Closed.** `written()` and `synced(turn)` (DM:353-359); D14's row (DM:971); T9 (DM:1288) | |
| DM-R7 Detent | **Closed.** `acks: written` gates forgetfulness, `Recovery`, and the records (DM:415-416, 553-556, 1221-1229); new magics and `Unsupported` (DM:1024, 1027); "For Detent" (DM:1216-1242); Detent's agreement before step 4 (DM:1241-1242, 1344) | |
| DM-R8 One mover | **Closed.** D17 (DM:1048-1105); host kinds withdrawn (DM:1015) | |
| DM-R9 The field list | **Closed** for the wire (DM:1003-1046; Q8 moved them to version 3, DM:997-1001). DM-R19's fix adds one storage element (a persisted forgetful mark), which D16's lists must carry | DM-R19 |
| DM-R10 The replay bound | **Closed.** Followers apply past the bound (DM:747-754); no early flush (DM:811-814); the mode switch (DM:988); T8 (DM:1287) | |
| DM-R11 Blast radius | **Closed** as a statement (DM:1109-1114, 1168, 1193-1194, 762-764, T4 and T11 at DM:1283, 1290). The residual Q9's option 2 leaves is DM-R21 | DM-R21 |
| DM-R12 The estimate | **Closed.** SHA-256 through EVP, TLS and the stack, the verifier, the bracket (DM:765-787), the summary (DM:129-135), T2 at the default refresh over loopback TLS from both sources (DM:1281); every figure recomputes (`rescore_r1.out`). Item 5 of the request: every CPU ratio states its base and direction ("of document mode's CPU", "segment against document", "+0.01× of a group's document-mode CPU") except Q9's "(about 0.01× more)" (DM:1311), and each rests on T2's measured parts plus three labeled, unmeasured assumptions (EVP's rate, the transport's ns per byte, the verifier's 1 in 64); none is a measurement, and T2 is the gate. The preliminary review's bracket [0.37, 1.05] was arithmetic over T2's parts, not a measurement either; revision 1's is 1.135× at rf = 3 and 0.767× at rf = 5, larger because both modes now pay the transport and segment mode the hash. Two precision items, DM-R22 items 4 and 5 | DM-R22 |
| DM-R13 Exactness | **Partially closed.** Items 3 to 6 closed (`leader` DM:848, T7 at `memory` DM:1286, `relaxed` DM:1166, `bounded` DM:856, `session` and the term history DM:857, T11 DM:1290). Item 1: the cap is stated (DM:330-334, 967) but reads as the leader's client acknowledgements, while DM-R13's own case is a stall under the followers (DM-R22 item 1). Item 2: "Nothing lost (D5, D6: no recovery election wins within the model)" (DM:1162) is false under DM-R19 and DM-R20 | DM-R19, DM-R20, DM-R22 |
| DM-R14 Leases | **Closed** (DM:824-831; T5 DM:1284) | |
| DM-R15 Part B | **Closed.** `resident` at A's `memory` (DM:1352-1361); a synced journal in every preset (DM:316-318); the attribute lists (DM:1041-1046); the HLC (DM:1371-1373) | |
| DM-R16 Identity and HLC | **Closed.** F13-R41's identity and the whole-file check (DM:714-719, against `format-13.md:470-482`); bit 16 and tombstones as keyed documents (DM:917-928); the HLC as a foundations library (DM:929-933) | |
| DM-R17 D9's parts | **Closed.** 2,323 ms and 2,163 ms recompute (`arith.out:38`); the client's part (DM:804-806); T8 (DM:1287) | |
| DM-R18 Precision | **Closed**, all eight items (DM:384-386, 350-352, 632-635, 387-390, 1083-1094, 1293, 971, 360-362) | |
| DMS-1 to DMS-9 | **Closed** at the lines the security re-check lists (`c87385e`, its C1 to C7 table), which I checked against the same lines and agree with. Its new DMS-10 to DMS-13 are its findings; DMS-11 overlaps DM-R21 and is cited there | |
| SI-R19 | **Closed** (DM:273-277; SI:308-314) | |
| SI-R24 | **Closed in substance**: the caller leads, only unwaited checkpoints go to the pool, retention on the owner (DM:278-286). The claim rule is stated less exactly than SI:329-333 (DM-R22 item 7) | DM-R22 |
| SI-R20, SI-R26 | **Closed.** A record names only completed checkpoints; restore at the recorded generation through `Repair`, a leader transferring first, not forgetful; a failed journal barrier is D5's (DM:287-297, 428-429), as SI:365-376 says. (SI:365's heading still reads "a failed checkpoint is a failed barrier" above a body that says it is not; small-indexes' to fix) | |
| SI-R25, T16 | **Closed** as small-indexes states it (DM:298-309, 1036-1037, 1295; SI:401-421); the duplicate-`Create` case is the security re-check's DMS-10 | |
| C1 Done criteria | **Open, the lead's.** The ledger still has none (ST:13-14) | |
| C2 Security points | **Closed** by DMS-1 to DMS-3 and the threat model (DM:1145-1149) | |
| C3 Library commits | **Closed** as a rule ("For Detent" item 6, DM:1239-1242); checked at each commit | |
| C4 Both modes | **Closed** (DM:1273-1276) | |
| C5 Other documents | **Closed** (DM:1193-1196) | |

The model runs, mine against the designer's deepest (each one thread, `nice -n 19`, the times
the runs printed):

| Model | Voters, scope | Depth | States | Time | Result | File |
|---|---|---:|---:|---:|---|---|
| Designer's, `round` | 3; two writes, terms to 4, three OS crashes, symmetry | 12 (designer) | 48,190,011 | 337 s | holds | `model-exhaustive.out` |
| Designer's, `round` | the same | 13 | 119,235,974 | 975 s | holds | `r1/cc-round-n3-p2-d13.out` |
| Designer's, `round` | 5; one write, terms to 4, three OS crashes, symmetry | 8 (designer) | 67,817,052 | 1,128 s | holds | `model-exhaustive.out` |
| Designer's, `round` | 5; two writes, terms to 4, three OS crashes, symmetry | 8 | 72,045,304 | 1,186 s | holds | `r1/cc-round-n5-p2-d8.out` |
| Mine, restarts kept forgetful (the fix) and retried rounds | 3; two writes, terms to 4, three OS crashes, two restarts, symmetry | 12 | 149,268,279 | 1,245 s | holds | `r1/cc-r1-round-n3-p2-persist-retry-d12.out` |
| Mine, the same | 5; one write, terms to 4, three OS crashes, one restart, symmetry | 7 | 14,475,565 | 192 s | holds | `r1/gauge-r1-persist-retry.out` |
| Mine, the same | 5; one write, terms to 4, three OS crashes, one restart, symmetry | 8 | 109,734,286 | 1,755 s | holds | `r1/cc-r1-round-n5-persist-retry-d8.out` |
| Mine, restarts as D3 reads them | 3; one write, terms to 2, two OS crashes, one restart | 8 | 1,489,228 | 11 s (C++), 38 s (Python) | `memory` and `fence` at 7 steps | `r1/py-restart-text-n3-d8.out` |
| Mine, membership in the log | 6, a replacement window | forced, 43 and 49 steps | | | `memory` lost; blocked by DM-R20's majority count | `r1/members-schedules.out` |

### New findings

1. **DM-R19. Forgetfulness does not survive a restart, so a voter that lost entries becomes
   electable without re-learning** (D3, D5, D16, T7, T9).
   - D5 makes a voter forgetful after an unclean restart or a torn same-boot tail (DM:418-427)
     and keeps it so "until it re-learns" (DM:433-437). Nothing records that state. The open
     record stores only the boot identity (DM:382-386), the hard state gains "no new bytes"
     (DM:1025), and the rules for the next restart look only at the boot identity, the clean-close
     record, and torn turns: "a same-boot restart without one rewrites and syncs the tail and
     resumes, not forgetful" (DM:422-423), and T7 asserts it: "a voter restarts forgetful only
     when its tail was torn" (DM:1286).
   - So: A leads, `v0` is written on A and B and acknowledged at `memory`; A loses power and
     restarts forgetful without `v0`; A's process crashes (or the host restarts it after a failed
     barrier) in the new boot; the open record names this boot, no turn is torn, and A resumes
     not forgetful. A campaigns at term 2 and wins with C's vote, and its log lacks `v0`, which B
     then replaces. One OS crash at three voters, inside the model, which counts A until it
     re-learns (DM:325-327). `r1/restart3-schedule.out` replays it (7 steps); the
     exhaustive search with D3's restart rule finds it and a fence at depth 7
     (`r1/py-restart-text-n3-d8.out`: 1,489,228 states; the C++ variant agrees). A clean close
     followed by a clean reboot does the same: the newest epoch ends with a clean-close record,
     so the restart is not unclean (DM:367-369).
   - It also breaks D6's argument, whose step "an answerer that had answered was still forgetful
     then" (DM:528-529) assumes the flag lasts until re-learning.
   - Fix: forgetfulness is durable per group. A forgetful voter writes a mark for the group (in
     the turn log beside the open record, or one byte in the group's hard state) before it
     answers anything, and clears it with a re-learned mark written after the re-learned entries;
     neither needs its own sync, since a power loss makes the voter forgetful again and a
     same-boot restart reads both from the page cache. Every restart, same-boot, clean, or
     unclean, carries the mark forward; a clean close does not clear it. D16 lists the element
     and D3 or D4 states which (the choice decides whether step 1 or step 3 carries it). T7 reads
     "forgetful when its tail was torn or it was forgetful when it stopped"; T9 gains a forgetful
     voter's same-boot restart and its clean close and reboot, each before re-learning, each still
     forgetful; T6 gains the mutant "forgetfulness cleared by a restart", which `restart3` kills.
     With the mark kept, my variant holds where the designer's model holds (the table above).

2. **DM-R20. D6's exactness assumes one membership; in Track C's replacement window at rf = 5 a
   recovery election wins inside the model and a `memory` write is lost** (D2, D5, D6, Q10, T3,
   T6).
   - The argument bounds a later leader's quorum by "2f + 1 − (f + 1) = f voters" (DM:530-531),
     with the later leader counting the same 2f + 1 voters. Track C changes membership one voter
     at a time, a node using its newest durable configuration entry (`track-c.md:216-218`), and
     its replacement passes through six voters at rf = 5, "quorum 4, two failures"
     (`track-c.md:274`). A candidate X in force with six voters needs f + 1 = 3 forgetful answers;
     a leader W with the five-voter membership that removes the leaving voter Q needs 3 of 5, and
     with X and one forgetful answerer P excluded, 3 remain. So W can be elected between the
     answers, P re-learns from W, and Q, the leaving voter, loses power after that and answers
     X: the three forgetful answers come from episodes that never overlap, the same shape that
     broke my DM-R1 fix, now through a membership change.
   - `r1/forgetful_members.py` is the designer's model with the membership in the log (Track C's
     D1 and D3, admission on `commit_durable` as D7 puts it). The `six` schedule (43 steps): S
     leads at term 1, `v0` is written on S, T, X, and P and acknowledged at `memory`, S appends
     Q's removal and T syncs it; X, then Q, then P lose power, Q re-learning from S in between, so
     X has heard three forgetful voters; X starts a round at term 2 and P (forgetful) and R grant;
     T is elected at term 3 by R, S, and T under the five-voter membership; P re-learns from T; Q
     loses power again and grants X, flagged; after `recovery_timeout` X decides with 4 grants,
     3 flagged, lacking `v0`. At most two voters are forgetful at any instant, f for both
     memberships. `six_loss` (49 steps) continues: Q re-learns from X, X learns a higher term
     without T's log, wins a normal election at term 5 with Q, which holds X's log, and R and S,
     whose logs end below term 2, and its appends replace `v0` at S, T, and P under `recovery_term` 2 with no fence
     (D6's rule, DM:557-561). `members-six_loss-final.out`: no replica holds `v0`, which was
     acknowledged at `memory`. The loss needs the recovery winner to stop being forgetful once it
     leads, which the designer's model assumes (`forgetful_model.py:328` in
     `bench/durability/model/`) and D5's re-learning rule reads as ("a leader of its durable
     term", DM:437); the win alone already breaks D6's claim and the model's oracle.
   - The failure needs a forgetful candidate and an even membership with f of 2 or more (rf = 5's
     replacement, growth from 5 to 7): with a candidate that is not forgetful, the adjacent
     memberships leave no quorum between the answers, and at rf = 3's four-voter window (f = 1) a
     forgetful candidate overlaps its one other flagged answerer, two at once. The schedule uses the
     model's abstractions, atomic messages and no pre-vote; with pre-vote, T reaches term 2 through
     X's own request, whose refusal reaches X after it decides, which D6 allows (it counts the
     answers received, DM:517-520).
   - Fix: the decision counts a majority of the candidate's membership flagged forgetful,
     ⌊n/2⌋ + 1 of n voters, the candidate's own counted; that is f + 1 for odd n, so steady-state
     groups are unchanged, and f + 2 for even n. The argument, restated for the memberships Track
     C admits: a normal leader above U elected before the decision has the candidate's membership
     or one a single change away (the next change waits for the previous one at
     `commit_durable`), and must avoid the candidate and every flagged answerer; with n + 1
     voters that leaves at most ⌈n/2⌉ < ⌊(n + 1)/2⌋ + 1, and with one flagged answerer removed at
     most n − 1 − ⌊n/2⌋ < ⌊(n − 1)/2⌋ + 1, so none exists, no answerer re-learns, and more than
     f are forgetful at once. With that count, `six` stops at the decision (`members-schedules.out`,
     `--count major`). The start condition may stay f + 1. The model statement says what f is
     for an even membership (⌊(n − 1)/2⌋, Track C's table) and that it holds for every
     membership in force. D5 and D6 say whether a recovery winner is forgetful once it leads.
     The fault table gains the price: in a six-voter window, three forgetful voters stall until
     one returns or `manual`. Q10's "holds at every replication factor" (DM:1330-1331; ST:202-204)
     becomes "at every fixed membership, and in replacement windows with the majority count";
     option 1 stands with that count, and options 2 and 3 avoid DM-R20 too (rf = 3's window is
     safe), at the costs Q10 states. T3 gains `six` and `six_loss` as forced schedules; T6 gains
     the mutant "f + 1 at an even membership"; the checker gains membership changes (my Python
     model is a start; the designer's C++ port needs six voters).

3. **DM-R21. Q9's option 2 states a residual that nothing in the design produces** (D18, Q9, the
   fault model).
   - Q9's recommendation: "a rare single corrupt segment, found later, is rebuilt from the source
     or a backup" (DM:1316-1318), which the lead's call rests on (ST:200-201). Nothing finds it
     later. D18 samples each new flush and merge once, at 1 in 64 per follower (DM:1115-1124); a
     missed flush is merged, and the merge check re-runs the merge over the inputs the follower
     holds, the corrupt one among them, so it passes (the security re-check's DMS-11 makes the
     same point for an attacker; for an honest fault it means a miss is permanent). The digest
     comparison is equal by construction in an all-segment-mode group (DM:1112-1113), and T4's
     end-of-run rebuild is a test oracle, not a production check.
   - The per-event figure is in D18 (3.1% detected at rf = 3) but not in Q9, where the choice is
     made: a single corrupt flush reaches every replica undetected with probability 96.9% at
     rf = 3 and 93.9% at rf = 5 (`rescore_r1.out`), where in document mode the fault stays on the
     leader's own copy, which the digest comparison finds and another replica repairs. "Rare" has
     no rate.
   - The sample covers new segments and merges, not the `Image`'s live-docs generations
     (DM:711-712): a leader-side fault in deletions (an old version left live after a replace) is
     outside it.
   - "A backup" names nothing strata plans: PLAN's recovery tooling rebuilds derived structures
     from stored records and restores "from elsewhere: a replica, the journal, or a checkpoint"
     (`PLAN.md:675-678`), each of which holds the same bytes in an all-segment-mode group.
   - Fix, before the full design review: Q9 and D18 state the residual as it performs (detected
     with probability 1 − (63/64)^(rf − 1) per corrupt flush, permanent once missed, deletions
     unsampled, repair by re-indexing from the client's source or PLAN's stored-record rebuild
     when the stored records are intact, and no backup); or D18 samples the live-docs delta of
     each sampled range and keeps its own record of unsampled flushes for a later scrub. The
     lead's call on Q9 may stand; the owner sees this residual before the full review clears it.

4. **DM-R22. Precision** (D1, D2, D7, D8, D14, Q9).
   1. The memory window's cap (DM:330-334, 967) is stated for a core's `memory`
      acknowledgements, which only a leader gives; D2 and D14 say it binds every voter's written
      replies (a follower past it replies after its barrier), since DM-R13's case was a stall
      under the followers.
   2. The leader's own match in `advance_commit`, `last_index()` at `core.h:1151`, must be its
      written index for `commit_memory` and its synced index for `commit_durable`; the "Beyond
      `durable_`" list (DM:695-699) leaves it out.
   3. A reply that waits for its barrier (DM:650-652): a later reply retires every older flight
      (`core.h:946-948`), and a reply with no flight drops (`core.h:940`), so a follower that
      answers a later append at `Written` first loses the `synced` report the `quorum` write waits
      for, until the next reply. D7 states that replies stay in append order, or that the leader
      takes `synced` from any reply of the term as it does from coalesced records.
   4. The cadence bracket (DM:779-780) leaves out the verifier the rest of D8 includes: 1.165× at
      rf = 3 and 0.803× at rf = 5 with it (`rescore_r1.out`).
   5. T2's targets sit 1.6 to 4 points above the estimate's top (DM:775-776, 1281), and the
      follower's fails if EVP runs at about 0.35 GB/s or less without SHA extensions
      (`rescore_r1.out`: 0.121×); D8 says the gate's margin rests on that unmeasured rate.
   6. Q9's "(about 0.01× more)" (DM:1311) names no base; D18's figure does (DM:1126).
   7. SI-R24: D1's "it leads instead" (DM:285) becomes small-indexes' claim rule (SI:329-333),
      so the two designs state one rule: a caller that starts waiting while a posted batch is
      queued claims it and runs it on its own thread, the posted task finds it claimed and
      returns, and a batch running on a pool worker needs no other worker.

5. **DM-R23. A checkpoint of a past generation needs retention keyed on that checkpoint's own
   manifest; today's keys the head on the in-memory state** (D1, T16).
   - D1: files of later states stay unreferenced until a later checkpoint, and "merge inputs that
     a later merge replaced stay referenced by the checkpoint's manifest, so retention keeps them
     (`index.hh:2167-2195`)" (DM:262-266). Today's `retain()` keeps the head `m_.generation`
     (`index.hh:2170`) with the names of the in-memory manifest, `names_of(m_)`
     (`index.hh:2227-2231`), and the current segments `segs_` (`index.hh:2195`); `commit_files()`
     sets `m_` to what it wrote (`index.hh:2145`). Under D1 what the checkpoint writes is
     generation g's snapshot, while `m_` keeps the current state, whose applied position decides
     what apply skips (`index.hh:407-410`). If `m_` became the snapshot, apply would re-run the
     positions after g; if it stays current, the head's names are the current state's, so the
     merge inputs only g names, and the live-docs and metadata files encoded for g (DM:262-263),
     are obsolete to `retain()` and queued for `reclaim()` (`index.hh:2198-2220`). A crash before
     the next checkpoint then opens a manifest naming missing files (`index.hh:2317-2318`:
     "every file the manifest names must exist").
   - Fix: D1 states that the durable head is the checkpoint's manifest, its names taken from the
     batch entry's snapshot and kept beside the in-memory state's files, and that `retain()` and
     `manifest_files()` read the head's names from it, not from `m_`; T16 gains "a checkpoint of
     a past generation whose inputs a later merge replaced and whose live-docs files it wrote,
     then the owner's retention and reclaim, then a crash and an open: every file the manifest
     names exists and the digest equals that generation's". It binds step 5's checkpoint code.

### New blocking items

1. DM-R19: forgetfulness persisted per group, carried across every restart; D16's element; T6,
   T7, and T9 as above. Blocks step 1 (if the mark is the journal's) and step 3.
2. DM-R20: the majority count at the decision; the argument restated for adjacent memberships;
   the model statement for even memberships; the recovery winner's state; the fault table's
   price; Q10's wording; T3's `six` and `six_loss`, T6's mutant, and membership changes in the
   checker. Blocks step 3.

### What approval needs

The two items above in the design, with the checker's runs (the designer's C++ port at six voters
with membership changes, at least the forced schedules and a bounded search near them) under
the fixed count; then a re-check scoped to `git log fc30ed3..`. DM-R21 to DM-R23 go into the same
revision: DM-R23 binds step 5's checkpoint code, DM-R21 and DM-R22 the full design review; none of
the three blocks the re-check.

### Conditions

1. **C6. Done criteria** (C1, still open): the lead writes K1 to K6 into the ledger before any
   step lands, with DM-R19's and DM-R20's mutants in K3.
2. **C7. Calibration**, for the lead: "2026-10-19 | DM-R1's fix in the preliminary review | f + 1
   forgetful voters counted since the restart loses a `memory` write at five voters | revision
   1's `fplus5` | Build the schedule in which the counted episodes run one after another, at
   five voters as well as three". DM-R20 is the same check applied to membership.
3. **C8. Step 2** (completion's boot identity and directory barrier) continues as ruled; nothing
   here changes it.
4. **C9. Step 4** (version-3 fields) may start: DM-R19 and DM-R20 add no wire field. It lands
   only after the full design review (DM:1001, 1344) and the security re-check's conditions.
5. **C10. Step 6** (the simulator) adds `restart3`, `six`, and `six_loss` to T3's forced
   schedules, process restarts that keep forgetfulness, and membership changes in the
   replacement window, before its gate.
6. **C11. Step 5** (strata::group) may start; its checkpoint code waits for DM-R23's statement,
   its `Create` replay for DMS-10's rule, and its batcher states SI-R24's claim (DM-R22 item 7).
7. **C12. Step 8** (D17's file sets) may start under the security re-check's C10 (DMS-12).
8. **C13. DM-R21, DM-R22, and DM-R23** in the next revision; DM-R23 before step 5's checkpoint
   code, the others before the full design review.
9. **C14. The model's limits stated** where D6 cites it (DM:582-612): atomic messages, fixed
   membership (until DM-R20's extension), no process restarts or retried rounds in the
   designer's model (my variants add both: with forgetfulness kept across restarts they change
   no verdict at the depths run, and with D3's reading they find DM-R19), and the depths and
   scopes as in the table above.

Not run: no benchmark, timing, or A/B (the CPU figures are arithmetic over the design's labeled
assumptions; no EVP, TLS, or network rate measured); no strata, consensus, or journal build or
suite (the branch changes no library code); no simulator (unbuilt); no Linux, FreeBSD, or arm64;
no fuzzing; the designer's checker and my variants only to the depths and scopes in the run
table, with atomic messages and hash compaction (64-bit keys: a state omitted with probability
about n² / 2⁶⁵); the membership model only as forced schedules, 200 walks from the start to depth
45, and 20,000 walks per count from `six`'s step 33 to depth 14 (`members-prefix-walks.out`:
nothing under either count, so the walks are no evidence either way), no exhaustive search at six
voters; the security re-check's probes not rerun; T1 and T2 not re-measured; the prior art not
re-fetched; no Xapian or Xapiand GPL source opened.

## Re-check (revision 2)

- Date: 2026-10-19, read and run from 09:22 to 10:35 PDT.
- Reviewed: `design/durability-modes` at `18612db`, `git log 4321849..18612db` (nine commits of this
  design, `52c31e3` to `18612db`, beside the merge of `main` `026ee10` at `457528b`):
  `docs/design/durability-modes.md` (its "Revision 2" and "Changes in revision 2" first),
  `docs/status/durability-modes.md`, `docs/reviews/calibration.md` (`e71c810`), the model in
  `bench/durability/model/` (`forgetful_model.py`, `forgetful_check.cc`, `runs_r2.sh`,
  `run_bounded.sh`, the 13 schedule files) and all 26 raw files in `bench/durability/results/r2/`,
  and `arith.py` with `results/arith.out`.
- Checked against: my re-check of revision 1 above (DM-R19 to DM-R23, C6 to C14); the security
  re-check of revision 1 (`durability-modes-security.md`: DMS-10 to DMS-13, its C8 to C12) and its
  probes in `strata-wt/review-durability-sec/.scratch/durability-sec-r1/`; Track C at `18612db`
  (D3's windows `track-c.md:253-286`; D11's open record `:476-486`, recovery `:510-517`,
  copy-forward `:523-531`, checkpoint record `:582-587`); journal `cb66f3d` (`turn_log.h:376-388`
  reclamation, `:468-489` inferred opens; the copy in `r1/`); strata at `18612db` (`index.hh`
  `415-418`, `506-518`, `644-700`, `751-812`, `2160-2168`, `2186-2253`, `2336-2360`; `reader.hh`
  `74-87`, `625-641`; `files.hh:170-176`); Kronuz/hashes `7ab0c23`, strata's pin (`hashes.hh`,
  `xxh64`); `small-indexes.md` at `45985dc` (`:325-335`, the `r1/` copy); `limits.md`;
  `calibration.md`, eleven rows, applied below (the newest, C7's, is the check the window shapes
  below apply).
- Checked by:
  - `quiet check`: exit 0 at every check from 09:28 to 10:35, never 1 or 2. Every build and run
    at `nice -n 19`, one thread each, at most three at once and two for most of the time, since
    swap was nearly full (21.4 of 22.5 GB at the start), each long run under `run_bounded.sh`'s cap.
  - Builds, Homebrew clang 23.1.2 at `-O2 -Wall -Wextra`, no warning: the designer's
    `forgetful_check.cc` at `18612db`; two copies with wider limits, `forgetful_check_r2x` (four
    configuration changes, `MAXSCRIPT` 4) and `forgetful_check_r2x7` (also seven nodes, with
    `canonical()`'s identity permutation widened to match), each reproducing the designer's
    `cc-grow43-major-d5` exactly (13,519,082 states); `digest_check.cc` against Kronuz/hashes at
    `7ab0c23`.
  - Reproduced byte for byte: the forced schedules in both checkers (`runs/schedules-cc.out` and
    `runs/schedules-py.out` against `results/r2/`), `arith.out`, and the security reviewer's
    `create_replay.out` and `d18_residual.out`. Reproduced by count: `cc-persist-torn-retry-n5-d7`
    (18,393,574 states, holds; 208 s here, 249 s there) and `cc-torn-persist-n3-os0-d10`
    (15,605,421 states, holds; 41 s here, 43 s there).
  - New probes and runs: the run table after the status table.
  - Probes, all in `strata-wt/review-durability/.scratch/review-durability/r2/`, kept with their
    outputs: `mark_mechanism.py` and `run_mark.sh` (`mark_mechanism.out`); `digest/digest_forge.py`,
    `digest/digest_check.cc`, and `digest/include/` with its provenance (`digest_forge.out`,
    `digest_check.out`, `records.txt`); in `sec/`, the security reviewer's three probes copied and
    rerun, `create_replay_r2.py` (`create_replay_r2.out`), `residue_oracle.py`
    (`residue_oracle.out`), `liar_model_r2.py` (the security re-check's 44-line liar patch, ported
    onto the designer's revision-2 model; `liar_baseline_r2.out`) and `liar_schedules_r2.py`
    (`liar_schedules_r2.out`); `members_prefixes.py` (`members_prefixes.out`), `grow67.py`
    (`grow67.out`), `six_heard.py` (`six_heard.out`), `forgetful_model_vb.py` (the designer's model
    with D4's vote barrier) and `replay_vb.py` (`replay_vb.out`), `schedules/` (`shrink`, `grow`,
    `replace2`, `grow67`), `runs_arch_r2.sh`, the three checkers' sources
    (`forgetful_check-18612db.cc`, `forgetful_check_r2x.cc`, `forgetful_check_r2x7.cc`),
    `arith-18612db.out`, and every run's output in `runs/`. The revision-1 probes are in `r1/`,
    unchanged.
  - Not run: the last line of this section.

Citations: `DM:N` is `docs/design/durability-modes.md` at `18612db`; `ST:N` is the ledger there;
`TC:N` is `track-c.md` there; `turn_log.h` is journal `cb66f3d`; `index.hh` and `reader.hh` are
strata `18612db`; `r2/` is `.scratch/review-durability/r2/`.

### Verdict

**REJECT for first step 1; APPROVE WITH CONDITIONS for first step 3 under C15; steps 5 and 8
continue under C11 (with C18) and C12; steps 2, 4, 6, and 7 as ruled, step 6 under C10 and C19.
Revision 2 closes DM-R20 and the policy half of DM-R19: the majority count holds wherever f + 1
loses, in the designer's `six` and in a seven-voter growth window built here, and
forgetfulness now survives every restart in the model. The bytes meant to carry it do not. The
open record's V decides by the epoch of a group's newest hard state, and Track C's copy-forward
rewrites an idle forgetful group's pre-verdict hard state into a later epoch, so a failed barrier, a
copy-forward, and a clean close bring a voter back electable without a write it acknowledged, with
no OS crash (DM-R24, 6 steps); the open record itself is reclaimable within its epoch. Two new
findings bind later steps: F21's digest is unkeyed xxHash64 summed modulo 2^64, so a leader gives
a planted segment the honest digest in O(1), and neither the sampled rebuild nor digest anchoring
catches it (DM-R25, Q9 and step 5's verifier); and `manual`'s f + 1 admission decides on
`balanced`'s timeout, which readmits `six` (DM-R26, step 3's recovery election). The model's
elections skip D4's vote barrier, which makes the designer's `grow6` an artifact; the majority
count stands on `six` (DM-R28).**

What holds, and reproduces:

- **DM-R20's majority count.** `six`, `six_loss`, `grow6`, and `grow6_loss` stop at the decision,
  and the designer's calibrated pairs from their prefixes hold under it where f + 1 loses
  (`results/r2/cc-six37-*`, `cc-six38-*`, `cc-grow43-*`); `six` stays a real counterexample to f + 1
  under D4's vote barrier, which the model omits, while `grow6` does not (DM-R28). Its arithmetic
  (DM:701-704) checks for every n with C' in {C, C ± 1}. A shape the designer's checker cannot run
  (it stops at six nodes), growth from rf = 5 to 7 with the candidate counting under six voters and
  W elected under seven (C' = C plus a voter), loses under f + 1 at 70 steps with at most two voters
  forgetful at once and stops under the majority (`grow67.out`, both checkers); from its step 65,
  f + 1 finds the loss at 5 steps and the majority holds in 109,641,449 states
  (`runs/cc-grow67-65-*-d5.out`).
- **DM-R19's policy.** Forgetful until re-learned, across every restart: `restart3` and `torn3`
  are blocked, and the designer's searches with restarts and torn tails hold to depth 12 at three
  voters and 8 at five (DM:821-829; the depth-7 five-voter run reproduced here). The orderings the
  designer did not run hold in the journal-level probe below with copy-forward off (268,246 states
  to depth 11): a stop before the new open record, between its write and its sync, and between
  its sync and the first turn; a clean close and reboot in the middle of re-learning; and the
  clearing batch torn by a power loss or a dropped writeback. Each finds the voter forgetful.
- **DMS-10.** Over every log of up to 7 entries with up to two checkpoint records at any past
  generation, restarting after every prefix (330,828 replays), revision 2's rule never differs from
  the live state; revision 1's differs in 58,377, even with no checkpoint at all: `[C, w2, C]`
  replays to an empty index, since revision 1's reading takes the newest `Create` as the creation
  record (`sec/create_replay_r2.out`).
- **DMS-12 and DMS-13** are stated as asked (DM:1324-1334; DM:598-616), each limit naming its
  mechanism; **DM-R22's seven items** are in; **DM-R23's statement** matches `index.hh` at every
  line it cites.
- **The figures recompute.** `arith.out` byte for byte: 1.165× and 0.803× of document mode's CPU
  with the verifier (1.135× and 0.767× without it); the follower's target of 0.12× of a
  document-mode follower's CPU failing below an EVP rate of 0.361 GB/s (0.121× at 0.35 GB/s); one
  corrupt flush detected with probability 3.1% at rf = 3 and 6.1% at rf = 5; T4's 439 and 220
  corrupt flushes. Each is an estimate or arithmetic, labeled so.
- **C7's row** (`calibration.md:18`) says what I asked, with DM-R20's membership twin folded into
  the check column.

What blocks:

- DM-R24: rule (c) of the open record's V fails under copy-forward, and the record holding V and
  the boot identity is reclaimable within its epoch (blocks step 1).

### Status

| Item | Status | Where, at `18612db` |
|---|---|---|
| DM-R19 Forgetfulness across restarts | **Partially closed.** The policy is right and stated wherever it must be: forgetful until re-learned across every restart (DM:567-577); the mark set in every hard state written while forgetful and cleared in the batch that completes re-learning, after its entries (DM:533-544, 586-588); V in the open record (DM:486-497); the recovery winner's state (DM:588-590); D16's rows (DM:1286-1288); T6's two mutants, T7's corrected claim, T9's restart rows (DM:1612, 1613, 1615); the model's `persist` restarts; the restart orderings above hold at the journal level. The mechanism's rule (c), "its newest hard state was written in an epoch older than V" (DM:493-495), fails under copy-forward, and the open record that holds V is reclaimable in a long epoch | DM-R24 |
| DM-R20 The decision's count | **Closed.** A majority of the candidate's membership (DM:677-682); the argument for a membership and its successor, with the step that pins C to the candidate's log (DM:683-707); why f + 1 fails (DM:708-721); the model statement for even memberships (DM:426-429); the stall priced (DM:769-777, 1488); Q10 corrected (DM:1670-1689; ST:246-251); T3's four schedules and T6's mutant (DM:1609, 1612); Track C's and the simulator's rows (DM:1526, 1535); the model with membership in the log and its searches (DM:806-840), reproduced. The stall is acceptable as an availability price beyond the model, has no time bound (the window's removal needs a leader), and its `manual` way out is unsafe as written: DM-R26. Precision: DM-R27 items 1 to 3, 10, and 12; the growth half's evidence: DM-R28 | DM-R26, DM-R27, DM-R28 |
| DM-R21 Q9's residual | **Partially closed.** For honest faults it is stated as it performs, in D18, Q9, and the fault table (DM:1422-1436, 1642-1650, 1492): per corrupt flush 3.1% and 6.1% detected, missed for good once unsampled and merged, systematic faults within tens to a hundred flushes, no backup, repair from the client's source; deletions sampled (DM:1402-1406); option 3 offered and T2 to price it (DM:1437-1452, 1608). For a planted flush, the figures the row and Q9 state (DM:1423-1424, 1470, 1651-1653) do not hold: DM-R25. "Deferral and the seed hold it at the honest figure" (DM:1431) holds only while the queue does: DM-R27 item 7 | DM-R25, DM-R27 |
| DM-R22 Precision | **Closed**, all seven: item 1 DM:431-440, 1228; item 2 DM:952-954; item 3 DM:897-906; item 4 DM:1033-1036 (`arith.out:42-43`); item 5 DM:1036-1041, 1608 (`arith.out:44-47`); item 6 DM:1639-1645; item 7 DM:366-371 against SI:329-333, the same rule ("word for word" but for two words) | |
| DM-R23 Retention on the durable head | **Closed** as a statement (DM:335-352; T16 DM:1622), each cited line matching `index.hh`: `commit_files()` sets `m_` (`2166`), `retain()` keeps `m_.generation` and its names (`2188-2216`, `2248-2252`) and queues the rest (`2219-2244`), open requires every named file (`2338-2340`) and reclaims orphans (`2353-2358`). `install()` writes the head from the current state too: DM-R27 item 5. Binds step 5 | |
| DMS-10 Duplicate `Create` | **Closed.** Only a UUID's first `Create` creates; a duplicate is refused at proposal for an index the leader holds and applies as `exists` in the log; replay opens at the newest checkpoint record of the UUID and treats a later `Create` as a no-op (DM:389-403; D16 DM:1299-1300); T16's retry case after and before a checkpoint record, same boot and power loss (DM:1622). `create_replay_r2.py` above | |
| DMS-11 The sample against a leader that chooses | **Partially closed.** Follower-secret samples (DM:1398-1401), deletions with their flush (DM:1402-1406), deferral with `verify_dropped` and `verify_lost` (DM:1407-1411, 1231), merges stated as merge-fault-only (DM:1412-1417), the leader's row re-priced (DM:1470), T4's choosing mutant (DM:1610). The comparison those samples feed is forgeable by the attacker DMS-11 is about; the generator's kind is DM-R27 item 11 | DM-R25, DM-R27 |
| DMS-12 The file list's bounds | **Closed.** Size, count, and total checked from `Begin` before buffering, against `transfer_list_bytes` (8 MiB, its mechanism named) and the committed `Image`; the sum overflow-checked (DM:1324-1334); T15's case (DM:1621); `limits.md`'s row owed (DM:1536): DM-R27 item 6 | |
| DMS-13 The rewind floor | **Closed.** The larger of the reported `synced` and `matched` less one `max_unsynced` window by the leader's own count; once per restart and per `recovery_timeout` per peer (DM:598-616); the row (DM:1471); T6's mutant (DM:1612). The window bounds an honest loss only if the cap counts the batch it answers: DM-R27 item 4 | |
| Security C12 (DMS-3's cost) | **Closed** (DM:1471). Its "`manual`, or no `recovery_timeout`, resists a compromised voter" holds only once `manual`'s election cannot decide on the timeout | DM-R26 |
| C6 Done criteria | **Open, the lead's** (ST:17-18; ST:160-162 lists K3's three mutants) | |
| C7 Calibration | **Closed** (`calibration.md:18`, `e71c810`) | |
| C8 Step 2 | Standing | |
| C9 Step 4 | Standing. Revision 2 adds no wire field: the mark is storage (DM:1286-1287) | |
| C10 Step 6 | **Closed** in the design (DM:1535, 1609, 1705); C19 adds this re-check's schedules | |
| C11 Step 5 | **Closed** for its three items (DM:335-352, 389-407, 366-371); C18 adds DM-R25 before D18's verifier | |
| C12 Step 8 | **Closed** with DMS-12 | |
| C13 DM-R21 to DM-R23 | **Closed** as revision items, DM-R21 partially as above | |
| C14 The model's limits | **Closed** (DM:842-860); DM-R28 adds the vote barrier the model omits | DM-R28 |

The runs, mine (each one thread at `nice -n 19`, the times the runs printed):

| Run | Scope | Depth | States | Time | Result | File |
|---|---|---:|---:|---:|---|---|
| Designer's forced schedules, both checkers | `restart3`, `restart3p`, `torn3`, `six`, `six_loss`, `grow6`, `grow6_loss`, each count | forced | | 1 s | byte for byte the designer's | `runs/schedules-*.out` |
| Designer's five-voter restart search, repeated | 5 voters, one write, terms to 4, three OS crashes, one restart, torn tails, retried rounds; symmetry, 64-bit keys | 7 | 18,393,574 | 208 s | holds; the designer's count | `runs/repro-cc-persist-torn-retry-n5-d7.out` |
| Designer's torn-tail search under the mark, repeated | 3 voters, one write, terms to 3, no OS crash, two restarts, torn tails | 10 | 15,605,421 | 41 s | holds; the designer's count | `runs/repro-cc-torn-persist-n3-os0-d10.out` |
| The two wider checkers against `cc-grow43-major-d5` | 6 voters, `grow6` from step 43 | 5 | 13,519,082 each | 45 s, 56 s | holds; the designer's count | `runs/repro-r2x*-grow43-major-d5.out` |
| The journal's bytes, rule (c) as written | One core, one group: epochs, open records with boot identity and V, S, torn turns that keep their bytes, batch, hard-state, and checkpoint records with the mark, copy-forward, clean close; two client entries | 10 | 1,853 | <1 s | G recovers not forgetful without `v1` at 6 steps, no OS crash | `mark_mechanism.out` |
| The same, copy-forward off | | 11 | 268,246 | 12 s | holds | `mark_mechanism.out` |
| Records keep their first epoch (`orig`) | | 11 | 702,363 | 34 s | holds | `mark_mechanism.out` |
| A marked hard state in the epoch's first turn (`firstturn`) | | 11 | 949,562 | 50 s | holds | `mark_mechanism.out` |
| `orig`, `firstturn`, three client entries | | 9 | 85,636; 103,134 | 4 s each | hold | `mark_mechanism.out` |
| `firstturn`, "newest" by journal order | | 10 | 2,027 | <1 s | loses at 6 steps | `mark_mechanism.out` |
| `grow67` forced, both checkers | 7 voters, rf = 5 to 7 (`add:5,add:6`), FMAX 2, the least f in force | 70 steps | | | f + 1: `memory` lost, at most two forgetful at once; majority: stops at the decision | `grow67.out`, `runs/grow67-schedules-cc.out` |
| From `grow67`'s step 65, f + 1 / majority | The round's first two answers in, before W's election | 5 | 109,641,450 / 109,641,449 | 781 s / 779 s | f + 1: `memory` at 5 steps; majority: holds | `runs/cc-grow67-65-*-d5.out` |
| `six` under `manual`'s f + 1 | 6 voters, rf = 5's window | forced | | | With the timeout: `memory` lost. Every voter answering: no decision (4 and 5 hold `v0` and refuse) | `six_heard.out` |
| From `six`'s step 37, every voter answering, f + 1 | The designer's calibrated point, where `round` with f + 1 loses `v0` at 6 steps (`results/r2/cc-six37-f1-d6.out`) | 6 | 132,328,669 | 676 s | holds | `runs/cc-six37-heard-f1-d6.out` |
| From `six`'s step 34, every voter answering, f + 1 | The round just started | 6 | stopped at 241,591,911 | 1,077 s | no result: a 2^28-slot table filled | `runs/cc-six34-heard-f1-d6.out` |
| Two replacements at rf = 3, f + 1 / majority | 5 nodes, four changes; from the second window's prefix with C2 and C3 in force at once and `v0` at `memory`; FMAX 1 | 8 | 60,570,784 each | 161 s / 157 s | both hold with equal counts: no decision is reachable that shallow, so evidence for four changes, not for the count | `runs/cc-replace2-64-*-d8.out` |
| rf = 5 to 3, f + 1 / majority | 5 nodes, `remove:4,remove:3`; the four-voter window between C5 (f = 2) and C3 (f = 1) with `v0` at `memory`; FMAX 1 | 7 | 76,799,839 each | 249 s / 243 s | `fence` at 7 steps under both counts, no forgetful voter: the model's artifact (DM-R28) | `runs/cc-shrink31-*-fm1-d7.out`; depth 8 filled a 2^27-slot table at 120,795,956 states (`cc-shrink31-f1-fm1-d8.out`) |
| The four schedules with D4's vote barrier | `six`, `grow6`, `grow67`, the shrink fence; the designer's model with the candidate synced at campaign | forced | | <1 s | `six` and `grow67` lose `v0` as before; `grow6` stops at W's election (step 43); the shrink fence stops at node 4's (step 37) | `replay_vb.out` |
| The liar on revision 2's model | Baseline without `--liar`: the designer's 641,290 states at depth 8 | forced | | | `LIAR` (rf = 3): `memory`, as at revision 1; every voter answering blocks it. `LIAR5`: `memory` with two honest OS crashes at once. `SIX_LIAR_LOSS`: `memory`, at most two honest voters forgetful at once, `v0` on no replica | `sec/liar_*_r2.out` |
| `create_replay_r2.py` | Logs of up to 7 entries, up to two checkpoint records at any past generation, a restart after every prefix | | 330,828 replays per rule | 1.9 s | revision 2: none differs from the live state; revision 1: 58,377 | `sec/create_replay_r2.out` |
| `digest_forge.py`, `digest_check.cc` | F21's record encodings; Kronuz/hashes `7ab0c23` | | | 0.13 s | the planted segment's digest equals the honest one; keyed sums differ | `digest/*.out` |

### New findings

1. **DM-R24. Copy-forward re-dates a forgetful group's hard state past V, and the open record that
   holds V is reclaimable within its epoch** (D3, D4, D16, T6, T7, T9).
   - Rule (c): a group recovers forgetful "when its newest hard state was written in an epoch
     older than V", which is what keeps forgetful "a group that has written nothing since the
     verdict, idle or not yet replayed" (DM:493-496). Track C's reclamation rewrites exactly such a
     group's live records forward, and "a record copied forward keeps its group and per-group
     sequence number (condition 9), and takes the turn it is written in" (TC:526-528); D14 keeps
     that (DM:1232). `RFTW0002`'s rows add no field that keeps a record's first epoch (DM:1288), so
     the epoch recovery can see is the copy's.
   - `r2/mark_mechanism.py` models the journal's bytes rather than the policy: epochs, the open
     record with its boot identity and V, turns with S, torn turns whose bytes stay (TC:510-516),
     G's batch, hard-state, and checkpoint records with the mark, copy-forward that retires its
     original at its barrier (DM:458-464), the clean close, and recovery as D3 states it, with a
     ghost of what G acknowledged. Read as written it loses in 6 steps with no OS crash: G
     acknowledges `v1` at `Written`; the barrier fails and the turn's pages are dropped; the
     same-boot restart finds the torn turn and recovers G forgetful (V = 2); the open record;
     copy-forward moves G's pre-verdict record (mark 0) into epoch 2; a clean close and a restart
     find V = 2, G's newest hard state "written" in epoch 2, no mark: G recovers not forgetful
     without `v1` (`mark_mechanism.out`). That is `restart3`'s shape at one forgetful voter, inside
     the model: G's voter can then elect a log without `v1` while `v1`'s other holder is
     unreachable. With copy-forward off the same probe holds to depth 11 (268,246 states).
   - The open record itself. `reclaim()` frees the oldest segments once no live group record remains
     in them (`turn_log.h:376-388`), and recovery already infers an epoch whose open record went with
     them (`:468-471`). Nothing pins the newest epoch's open record, so on a node that has run long
     enough to reclaim its epoch's first segment, the boot identity and V are gone at the next
     recovery. DM:496-497 makes V conservative then, and an unknown boot identity reads as unclean
     (DM:506-507), so nothing is lost; but then every `written` group on the core recovers
     forgetful after any process crash. The same-boot path (DM:475-485) works only for young
     epochs, and T7's "exactly when its tail was torn after S or it was forgetful when it stopped"
     (DM:1613) is false on a node that ran long, which T7's kill loop, restarting every few
     seconds, rarely reaches. Read the other way, a missing boot identity taken as the same boot,
     a power loss in a long epoch loses writes.
   - Fix, in D3, D4, and D16, before step 1's recovery is built:
     1. "Newest hard state" is the one with the highest per-group sequence, never the latest in
        journal order (`firstturn_jorder` loses in 6 steps).
     2. Either a recovery that leaves a group forgetful writes that group's hard state with the
        mark in the new epoch's first turn, so V covers only the window before that turn
        (`firstturn`: none to depth 11, 949,562 states, and to depth 9 with three entries); or
        copied records keep the epoch they were first written in, and rule (c) reads it (`orig`:
        none to depth 11, 702,363 states). The first changes no format and costs one hard-state
        record per forgetful group per verdict (34 bytes each, by TC:550's record size); the
        second adds bytes to every copied record for the journal's life. I recommend the first.
     3. The newest epoch's open record stays readable for the epoch's life: the turn log counts it
        live (its segment is not reclaimed while its epoch is current), or rewrites its boot
        identity and V into the head before its segment goes. D3 states that an open record
        recovery cannot read reads as unclean, never as the same boot.
     4. T9: an idle forgetful group's records copied forward, then a clean close and a restart,
        and then a same-boot crash, forgetful at each; a long epoch whose first segment was
        reclaimed, then a same-boot crash, not forgetful where nothing was lost. T6: the mutants
        "rule (c) on the copy's epoch" and "newest hard state by journal order", which
        `mark_mechanism.py` kills.

2. **DM-R25. A leader gives a planted segment the honest F21 digest in O(1), so neither the sampled
   rebuild nor digest anchoring detects it** (D18, Q9, the threat model, T2, T4; DMS-11).
   - Both checks compare F21 digests: the rebuild "compares the two segments' logical digests ...
     (F21, `index.hh:644-652`)" (DM:1392-1394), and anchoring compares a sum kept from the log
     with the installed segments' (DM:1438-1445). F21's digest is "the sum modulo 2^64 of xxHash64
     over records" (`index.hh:644-645`), unkeyed (`index.hh:652`, Kronuz/hashes' `xxh64::hash`,
     seed 0), over records whose bytes a segment's author writes (`reader.hh:74-87`, `625-641`).
   - xxHash64 is not preimage-resistant: after the last full 32-byte stripe, each step of the tail
     and the final avalanche is a bijection of the 64-bit state for known input, and an 8-byte chunk
     enters as `h ^ rotl(x·P2, 31)·P1`, which solves for x. `r2/digest/digest_forge.py` drops
     document 7's posting for "secret" and adds one posting of document 7 for a junk term whose 8
     free bytes it solves for. The planted segment's digest equals the honest one,
     `be6a7adb380275bc`, and so does document 7's own contribution; a search of at most 2^20 tries
     finds a printable junk term too, 0.13 s for both. `digest_check.cc` recomputes the three sums
     with Kronuz/hashes at strata's pin and agrees (`digest_check.out`). Under a keyed hash
     (BLAKE2b with a secret key) the same records' sums differ.
   - So a compromised leader that forges is detected with probability 0 under options 2 and 3, not
     the "3.1% at rf = 3 and 6.1% at rf = 5" the row states, nor "under Q9's option 3 at its first
     install" (DM:1470; D18 DM:1423-1424: "with the secret seed the attacker's figure is the honest
     one"). The seed decides which flushes are checked, and every planted flush passes the check.
     T4's choosing mutant corrupts without forging (DM:1610), so T4 would pass and miss this. Honest
     faults are unaffected: a random change matches with probability about 2^-64.
   - Fix, in D18, Q9, the threat row, T2, and T4, before step 5's verifier and before the owner
     answers Q9: every comparison the verifier makes, the sampled rebuild's and digest
     anchoring's, sums a keyed PRF over the same record encodings under a key each follower draws at
     process start and never sends. HMAC-SHA-256 through the EVP the transport already links adds no
     dependency and is the slowest; SipHash or keyed BLAKE3 cost less per byte, which matters for
     option 3, where every follower hashes every record it installs. F21's public digest stays the
     cross-replica comparison where no member writes another's bytes. T2 prices the keyed hash for
     option 3 (every installed segment and every applied batch, on every follower) and option 2; T4
     gains a forging mutant, `digest_forge.py`'s compensating posting, which the keyed comparison
     catches and F21's misses; the row and Q9 state option 2's planted figure as 3.1% and 6.1% with
     the key and 0 without, option 3's as "at first install" with the key. Q9's recommendation of
     option 3 (DM:1659) holds only with the key.

3. **DM-R26. `manual`'s f + 1 admission decides on `balanced`'s timeout, which readmits `six`** (D6,
   the fault model, the threat model, T3, T6).
   - In a window stall, the operator's statement "lets one recovery election run under
     `balanced`'s rule. The same statement may admit a count of f + 1 for that one election"
     (DM:735-739), and `balanced` decides when "every voter of that membership answered or
     `recovery_timeout` passed with no leader" (DM:680-681). f + 1 at an even membership through
     the timeout is `six` and `grow67` exactly: three flagged answers from episodes that never
     overlap, a `memory` write lost with at most f forgetful at once, after the operator admitted a
     loss beyond the model that had not happened (`six_heard.out`, first case).
   - With every voter's answer required, the same schedule cannot decide: the voters still holding
     `v0` (4 and 5) refuse the candidate, whose log is behind theirs (`six_heard.out`), and a
     decision loses only what no voter holds, D6's own figure for `balanced` with every voter
     answering (DM:765-766). The calibrated pair agrees: from `six`'s step 37, where `round` with
     f + 1 loses `v0` within 6 steps (`results/r2/cc-six37-f1-d6.out`), the same search with every
     voter's answer required holds (132,328,669 states, `runs/cc-six37-heard-f1-d6.out`).
   - The threat row's "so `manual`, or no `recovery_timeout`, resists a compromised voter"
     (DM:1471) depends on the same rule: `LIAR` decides through the timeout and is blocked when every
     voter must answer (`sec/liar_schedules_r2.out`).
   - Fix: the f + 1 admission, and preferably every `manual` election, decides only when every
     voter of the membership answered, never on the timeout. D6's availability and the fault table
     (DM:769-777, 1488) say the stall then lasts until every voter is reachable and an operator
     acts, with no time bound, since the window's removal needs a leader; a counter or alert names
     the stall. T3 gains `six` under the admission with every voter answering, which must decide
     nothing; T6 gains the mutant "the f + 1 admission decides on the timeout", which `six` kills.

4. **DM-R27. Precision** (D2, D5, D6, D8, D17, D18, T16).
   1. The liar in a window. D6 has a lying voter completing the count "with f honest forgetful
      voters at an odd membership (f + 1 in a window)" (DM:759-761). In a window the liar also
      votes in the normal election that lets an earlier honest answerer re-learn, so f honest
      voters at once suffice there too: `SIX_LIAR_LOSS`, the architect's `six` with voter 3 answering
      flagged falsely, decides with four flagged answers, the majority of six, with at most two
      honest voters forgetful at once, and its continuation leaves `v0` on no replica
      (`sec/liar_schedules_r2.out`); `LIAR5` confirms the odd case at rf = 5. The sentence reads
      "with f honest forgetful voters at once, at any membership".
   2. The model statement (DM:426-429; the fault model DM:1478-1480) gives a window's two adjacent
      memberships "the same f", which holds for replacement windows only. Growth and shrink pass
      through an even membership whose neighbors differ (5, 6, 7 voters: f = 2, 2, 3), where the
      bound is the least f in force; `grow67` is inside it and loses under f + 1, and the majority
      blocks it. D2 says "the least f of the memberships in force".
   3. The argument (DM:693-701) rules out a W two changes after C. The other direction, a stale W
      two changes before C, needs one sentence: W's quorum, of voters that are not forgetful,
      meets the intermediate change's durable majority in a voter that holds the change and
      refuses a log without it.
   4. DMS-13's floor (DM:598-606) is "one unsynced window" only if the cap (DM:433-437) counts the
      batch it answers: a batch that would take a core's unsynced bytes past `max_unsynced` is
      answered after its barrier. Otherwise the crossing batch is acknowledged at `Written` past the
      window, an honest forgetful voter's rewind stops short of its loss, and it waits a
      `recovery_timeout` for the next one (DM:607-616).
   5. `install()` commits the installed image as the head (`commit_files()`, `index.hh:808-811`).
      Under D8 an `Image` is durable as a checkpoint only at or below `commit_durable`
      (DM:976-978), so the incremental install updates the current state and leaves the head to
      the next checkpoint: a third reader of DM-R23's split beside `retain()` and
      `manifest_files()`, for step 5.
   6. `limits.md` has no manifest or segment-count row, so its `transfer_list_bytes` row (DM:1536)
      states the largest manifest and file count 8 MiB admits; an index past it could not be
      transferred, repaired, or shipped.
   7. "Deferral and the seed hold it at the honest figure" (DM:1431) holds while the queue holds
      and the range is retained. Past `verify_pinned_bytes`, or after a checkpoint truncates the
      range, samples drop, counted (DM:1407-1411), as the row says (DM:1470); D18's sentence says so
      too.
   8. Digest anchoring's "16 bytes per live document" (DM:1448-1449): in memory or on disk, and how
      it is rebuilt after a restart. With millions of indexes per host this is the option's memory
      cost, which T2's CPU figure does not show.
   9. T16 takes "`create_replay.py`'s fixed arm" as its oracle (DM:1622). That arm calls a `Create`
      a duplicate whenever a manifest with its UUID is on disk, so over a first checkpoint's residue
      (a manifest written, its record never landed) it opens the residue instead of replaying from
      the creation (`sec/residue_oracle.out`). D1 and SI-R25 replay from the creation there, since
      the residue may name files a failed sync left behind, and T16's own first case tests that. The
      oracle is the live state under D1's rule, as `sec/create_replay_r2.py` uses.
   10. Joint consensus. D6's count and argument assume Track C's one voter per change (TC:255-256),
      but Track C keeps joint consensus as a build its zone-outage simulation may call for
      (TC:282-284). D6 says so, and that under a joint configuration the decision would need a
      majority of each of C_old and C_new flagged, as every joint quorum does, with the argument
      and the model redone before such a change lands.
   11. The samples' secret is "seeded per process from the operating system's generator"
      (DM:1399-1400). A seeded non-cryptographic generator is predictable from enough of its
      outputs, and a follower's load while it rebuilds is visible to its peers. Each decision is
      drawn from a cryptographic generator (the OS's, or one it seeds), one draw per flush.
   12. rf = 3's window stall buys uniformity, not safety. At four voters (f = 1) f + 1 is safe: a
      forgetful candidate's two flagged answers overlap, two forgetful at once, beyond the model
      (DM:720-721), and for a candidate that is not forgetful no leader fits between its answers
      (outside the candidate and its two flagged answerers, four voters leave one, three leave at
      most one, and five leave two, each short of its majority). So the stall the fault table names
      "four at rf = 3" (DM:1488) is the price of one rule for every n. Q10 says so, so the owner
      weighs it at the replication factor most groups run.

5. **DM-R28. The model counts a candidate's election under its synced prefix's membership,
   without D4's vote barrier, so `grow6` and a shrink-window fence are its artifacts** (D4, D6's
   model evidence, T3, C14).
   - The model's membership in force is "the newest configuration entry in the synced prefix"
     (`forgetful_model.py:134-141`), and a normal election is one atomic step that does not sync the
     candidate (`:203-235`); the limits paragraph says so for rounds only (DM:845-846). In the
     design a candidate syncs its term and vote before any request leaves (DM:523-524), a barrier
     that covers every turn it has written (DM:453-457), so by Track C's rule (TC:216-217) it counts
     under the newest configuration entry in its log.
   - The gap produces elections the design does not admit. `grow6`'s W, node 3, holds the
     promotion written but unsynced and wins under the founding five (DM:715-718); with the
     barrier it counts six voters, and that election is not enabled (`replay_vb.out`). My shrink
     search from rf = 5 to 3 finds a `fence` at 7 steps under both counts with no forgetful voter:
     node 4, removed, holds its removal unsynced and wins under the founding five with 2 and 3
     while leader 0 commits under the three-voter membership with 1; with the barrier node 4 is
     not a voter, and that election is not enabled either. `six` and `grow67` replay unchanged
     with the barrier (`r2/forgetful_model_vb.py`, `replay_vb.py`).
   - So "f + 1 loses a `memory` write at rf = 5 in both halves of the window" (DM:55-58, 714-718)
     rests on `six` alone. By the arithmetic the growth half at rf = 5 may not fail at all: a W
     counting the founding five must lack the promotion, and the old leader that holds the
     promotion and `v0` must vote for it. `grow67` shows the growth failure where it occurs, from
     six voters to seven. The majority count stands on `six`. The model can also miss elections
     the design allows, since a candidate whose unsynced newest entry removes a voter counts the
     larger membership.
   - Fix: the model syncs a candidate at campaign, normal elections and rounds alike, as
     `forgetful_model_vb.py` does for normal elections, and the membership searches are rerun
     with it; `grow6` and `grow6_loss` leave T3's forced schedules (DM:1609) and the summary, and
     `grow67` joins them; the limits paragraph (DM:842-855) states the barrier.

### New blocking items

1. DM-R24: "newest" by per-group sequence; the first-turn mark (or each copied record's first
   epoch); the newest open record kept readable for its epoch's life, and an unreadable one read
   as unclean; T6's and T9's rows. Blocks step 1.

### What approval needs

DM-R24's three rules in D3, D4, and D16 with its T6 and T9 rows, and `r2/mark_mechanism.py`
rerun under the chosen variant (`--variant firstturn` or `orig`) to depth 11 with copy-forward on,
plus a run whose open record is reclaimed if the turn log's lifetime rule is modeled; then a
re-check scoped to `git log 18612db..`, which the rechecker can run from these probes. DM-R25 to
DM-R28 go into the same revision; none of them blocks that re-check.

### Conditions

1. **C15. Step 3** (`consensus`) may start. Before its restart, recovery-election, and rewind code
   lands: DM-R24's rule (the core writes the first-turn mark, if that is the choice), DM-R26's
   every-voter rule for `manual`, and DM-R27 item 4's cap. Nothing lands before the full design
   review (DM:1693-1696).
2. **C16. Step 1** (`journal`) waits for DM-R24's re-check. Only the open record's lifetime and
   rule (c) change; one write in flight, effects at the barrier, S, the clean-close record, and
   same-boot recovery stand as written.
3. **C17. Done criteria** (C6, still open): K2 (step 1) carries DM-R24's T9 rows; K3 (step 3)
   carries DM-R19's and DM-R20's mutants (ST:160-162) and DM-R24's and DM-R26's.
4. **C18. Step 5**: D18's verifier and digest anchoring wait for DM-R25's keyed comparison, and
   step 5's tests carry T4's forging mutant; its install path takes DM-R27 item 5.
5. **C19. Step 6** (the simulator) adds `grow67` in place of `grow6` and `grow6_loss` (DM-R28),
   `six` under `manual` with every voter answering, `SIX_LIAR` as a compromised-voter outcome
   counted outside the model, and DM-R24's restart after copy-forward where its journal fault
   model reaches it.
6. **C20. Calibration**, for the lead, two rows on my own reviews:
   - "2026-10-19 | DM-R2's fix in the durability-modes preliminary review | Same-boot recovery that
     'resumes, not forgetful' clears forgetfulness a power loss set (DM-R19) | The re-check of
     revision 1, `restart3` | Model every restart path the fix adds against every flag the safety
     argument relies on".
   - "2026-10-19 | DM-R19's fix in the re-check of revision 1 | A mark carried by 'the newest hard
     state', not checked against Track C's copy-forward and reclamation (DM-R24) | The re-check of
     revision 2, `mark_mechanism.py` | Model the bytes that carry a safety flag through every
     lifecycle event of the records that hold them (copy-forward, supersession, reclamation, torn
     turns, clean close), not only the policy".
7. **C21. Q9** goes to the owner with DM-R25's restatement. The lead's call, option 2, does not
   defend against a compromised leader until the verifier's comparison is keyed.
8. **C22. Joint consensus.** If Track C builds it (TC:282-284), D6's decision counts a majority of
   each configuration flagged, and its argument and the model are redone before that change
   lands (DM-R27 item 10). The lead routes this to Track C's D3 with "Changes to other documents"
   (DM:1526).

Not run: no benchmark, timing, or A/B (the CPU figures are arithmetic over the design's labeled
assumptions; no EVP, keyed-hash, TLS, or network rate measured); no strata, consensus, or journal
build or suite (the branch changes no library code; the model builds apart); no simulator (unbuilt);
no Linux, FreeBSD, or arm64; no fuzzing; the designer's deepest searches (three voters to depth 12,
five to depth 8, six from `six`'s step 37 to depth 6) not repeated, only the five-voter depth-7 one;
no exhaustive search at six or seven voters from a founding state, only from the forced prefixes to
the depths in the run table, with atomic messages and 64-bit keys (a state omitted with probability
about n² / 2⁶⁵); the shape searches at rf ≤ 5 (shrink to depth 7, two replacements to depth 8) reach
no decision the two counts treat differently (the shrink search's fence is DM-R28's artifact), and
the rf = 3 to 5 search was stopped unrun (the designer's `rf3win` covers a four-voter window at f =
1); joint consensus not modeled (Track C admits one voter per change today; DM-R27 item 10), and no
witness in the model or my runs; `mark_mechanism.py` models one core and one group, not the turn
log's segments or its open record's reclamation (DM-R24's second part rests on `turn_log.h`'s code,
not on a run), and its variants to depth 11 only; the liar only as forced schedules, no liar search;
the vote-barrier model only replayed, not searched; the every-voter search from `six`'s step 34
filled its table with no result; no forgery of a real segment file through strata's writer
(`digest_forge.py` builds F21's record encodings and hashes them with strata's `xxh64`, which is
what the digest sums); the prior art not re-fetched; no Xapian or Xapiand GPL source opened.

## Re-check (revision 3)

- Date: 2026-10-19, read and run from 12:42 to 13:36 PDT.
- Reviewed: `design/durability-modes` at `b56cb46`, `git log d627895..b56cb46` (17 commits,
  `1031222` to `b56cb46`): `docs/design/durability-modes.md` (its "Revision 3" and "Changes in
  revision 3" first), `docs/status/durability-modes.md`, `docs/reviews/calibration.md` (`4afbf67`,
  `b56cb46`), the models in `bench/durability/model/` (`mark_model.py`, `mark_reach.py`,
  `runs_mark_r3.sh`, `runs_r3.sh`, `forgetful_model.py` and `forgetful_check.cc` with `--barrier`,
  the schedule files), `bench/durability/keyed_digest.py`, `records.py`, `arith.py`, and the raw
  files in `results/r3/` and `results/r3/mark/`.
- Checked against: my re-check of revision 2 above (DM-R24 to DM-R28, C15 to C22); journal
  `cb66f3d`'s `turn_log.h` (the copy in `r1/`: staging with its forward slice `:282-297`,
  `forward_slice()` `:355-374`, `reclaim()` `:375-391`); `track-c.md` D11 at `b56cb46` (`:460-625`);
  strata at `b56cb46` (`index.hh:644-700`; `reader.hh:74-87`, `376-500`, `618-640`, `669-760`,
  `895-912`, `1893-1918`; `keys.hh:163-173`; `format.hh:120-145`; `CMakeLists.txt:12-16`, the pins);
  Kronuz/termdict `eaf83b3`, Kronuz/fst `24da59a`, Kronuz/varint `21af82a` (strata's pins:
  `termdict.hh`'s `reader::find_run` and `reader::scan`); `calibration.md`, fourteen rows, applied
  below (the newest, C20's on DM-R19's fix, asks for the bytes through every lifecycle event; DM-R29
  is that check with more than one group).
- Checked by:
  - `quiet check` before every build and run: exit 0 or 2 (light holds, `f13q-final` and
    `f13q-thresholds`), so every build and run used `nice -n 19` and at most three jobs at once. One
    probe build and its run (`plant/term_index_plant`, about 1 s of compile and a run under 0.1 s)
    started at 13:13 PDT while `quiet check` reported a strict hold (`f13q-ef-idx`, from 13:12, held
    until 13:27), because my command did not stop on its exit 1; nothing else ran until that hold
    ended. Swap was nearly full (23.2 of 24.6 GB used), so the Python searches stayed under about
    four million states each.
  - Builds, Homebrew clang 23.1.2: the designer's `forgetful_check.cc` at `b56cb46`
    (`r3/forgetful_check`, `-O2 -Wall -Wextra`, no warning); `r3/plant/term_index_plant.cc` against
    termdict, fst, and varint at strata's pins (`-O1 -Wall -Wextra`, no warning).
  - Reproduced byte for byte: every forced schedule under each barrier mode in both checkers
    (`runs_r3.sh schedules` with my build: `runs/repro/schedules.out`, no line differs from
    `results/r3/schedules.out`), `results/arith.out`, and `results/r3/records.out` over T2's corpus
    (sha256 `0a1d5344…`, the ledger's); `mark_model.py`'s three fast controls but for their times;
    `keyed_digest.out`'s verdicts under a fresh key (`runs/repro/keyed_digest.out`). Reproduced by
    count: the designer's depth-10 journal run (1,458,689 states, 69 s) and its `none` control
    (598,427 states, precision lost in 4 steps; `runs/repro/mark-firstturn-*-d10.out`); with my
    build, `cc-six38-f1-d5` and `-major-d5` (6,288,672 and 6,288,671 states, `memory` at 5 steps and
    holds) and `cc-torn-persist-n3-os0-d10` (9,793,683 states, holds; `runs/repro-cc-*.out`).
  - Probes, all in `strata-wt/review-durability/.scratch/review-durability/r3/`, kept with their
    outputs: `mark_groups.py` (the designer's journal model, per group, with the first-turn marks
    spread over turns and slices riding in them) and `runs_arch_r3.sh`, outputs in `runs/mg-*.out`
    and `runs/mg3-*.out`; `lead5.py` (`lead5.out`), `schedules/` (`lead5.txt`, `heard5l.txt`,
    `lead5_loss.txt`), `lead5-cc.out`, and `runs/cc-lead5-*.out`; `plant/term_index_plant.cc` with
    `plant/include/` and its `PROVENANCE` (`plant/term_index_plant.out`);
    `forgetful_check-b56cb46.cc`, the checker's source as built; `q9_cross.py` (`q9_cross.out`) and
    `records_stated.py` (`records_stated.out`), the arithmetic of DM-R32 items 4 to 6;
    `post_hold.sh` and its log, and the reproductions in `runs/repro/`. The revision-1 and
    revision-2 probes are in `r1/` and `r2/`, unchanged.
  - Not run: the last line of this section.

Citations: `DM:N` is `docs/design/durability-modes.md` at `b56cb46`; `ST:N` is the ledger there;
`TC:N` is `track-c.md` there; `turn_log.h` is journal `cb66f3d`; `index.hh`, `reader.hh`, `keys.hh`
are strata `b56cb46`; `FM:N` is `bench/durability/model/forgetful_model.py` and `MM:N` is
`mark_model.py` there; `r3/` is `.scratch/review-durability/r3/`.

### Verdict

**APPROVE WITH CONDITIONS for first step 1 under C23; first step 3 stays APPROVE WITH CONDITIONS
under C15, with C23's order added; steps 5 and 8 continue under C11, C12, and C18, which C25 widens;
steps 2, 4, 6, and 7 as ruled; step 9, SipHash-2-4 in Kronuz/hashes against its reference vectors,
may start. Revision 3 closes DM-R24's first and third rules: a group's newest hard state is the one
with the highest per-group sequence, and the newest epoch's open record lives through `0x13` copies
and reads as unclean when nothing of it can be read; the designer's journal model reproduces the
6-step loss here, and both it and mine hold under the new rules for one group. Its second rule holds
for one group and fails for two. The marks go "in the epoch's first turns", and every turn the
journal stages takes a reclamation slice once it holds more than `retain_segments` segments
(`turn_log.h:297`, `359`), so the first mark turn can copy a later-marked group's unmarked hard
state into epoch V; a process crash or a clean close before that group's own mark brings it back not
forgetful, without a write it acknowledged, in 7 steps or 6 and with no OS crash (DM-R29). Keeping
slices out of the epoch until every mark is written, or letting a slice copy only groups already
marked, holds to depth 9 with two groups and depth 8 with three. DM-R26 is closed. DM-R28's barrier
is faithful to D4, but the conclusion the revision draws from it is not: the owner's rule loses a
`memory` write at five voters under every barrier D4 imposes when the leader that wrote it is the
third forgetful voter (`lead5`, 13 steps, both checkers), so the fixed-membership loss that D6, Q10,
and the calibration row now call open is shown (DM-R30). DM-R25's keyed sums are sound for what they
sum, F21's records as the scan path reads them; lookups, ranking, and option 3's own live-docs check
read other structures the leader writes, and a planted term index hides 20 of 200 terms from lookups
while the scan F21's postings come from, and so every keyed sum, is unchanged (DM-R31).**

What holds, and reproduces:

- **DM-R24's rules 1 and 3.** "Newest" by per-group sequence (DM:560-565); the open record a live
  record, copied forward as `0x13` by reclamation's slices, its segment kept until a copy's turn is
  synced, and an open record read neither whole nor through a copy taken as unclean (DM:589-603,
  1534). `mark_model.py`'s controls reproduce here byte for byte but for their times
  (`runs/mark-controls-repro.out`): revision 2's rule loses in 6 steps, journal order in 7, the
  unreadable boot identity read as this boot's in 5. My two-group model, run with one group and the
  designer's marking rule, reproduces the design runs' state counts exactly (122,267 at depth 8;
  1,458,689 at depth 10, `mark-firstturn-copy-d10.out`'s count) and holds.
- **The orderings beyond the designer's, but one.** An open-record copy racing a segment switch: the
  copy pins its segment while it is the newest synced copy and the original's segment leaves only
  after one is synced (DM:595-597), and both models take switches, copies, barriers, and stops in
  every order (`mark_model.py`, `mark_groups.py`), holding. A torn `0x13`: it drops with its turn
  while the original is still pinned; a synced copy damaged after the original is gone falls back to
  an older whole copy, or reads as unclean, which loses nothing. Deferred replays across two
  restarts: rule 2 decides every `written` group at recovery (DM:571-574), so a group whose replay
  is deferred carries its mark from the verdict's epoch on, and T6's mutant tests the deferral
  (DM:1963). The ordering that fails is the marks' own: DM-R29.
- **DM-R25's keyed sum.** SipHash-2-4 matches its reference vectors in Python and in my C++ probe;
  the forged and printable segments sum differently under a key while their F21 digests equal the
  honest one (`keyed_digest.out`, reproduced under a fresh key). F21's record encodings are
  injective (a tag, fixed-width fields, and u32-length-prefixed bytes, `reader.hh:74-87`, `625-640`,
  `908-912`), and the prime modulus keeps every multiplicity: a planted multiset matches with
  probability about 2⁻⁶⁴ per comparison, and a mismatch stops that follower. No keyed value is
  stored, so format 13 needs no change (DM:1558-1562, 1669-1671); the covered log range an `Image`
  names is strata's command (DM:1543-1546).
- **DM-R26.** `manual` and its f + 1 admission decide only with every voter's answer (DM:809-811,
  895-905), the stall is stated without a time bound and named by `recovery_stalled` and an error
  log (DM:938-953, 1836), and the threat row (DM:1818) and D15 (DM:1492) agree. `six_heard` and
  `grow67_heard` decide nothing (reproduced in `runs/repro/schedules.out`); the designer's search
  from `six`'s step 37 with every voter required holds (65,219,423 states,
  `cc-six37-heard-f1-d6.out`).
- **DM-R28's barrier.** Both checkers sync a candidate when it campaigns and count it under its
  whole log's membership (FM:203-206, 238-246, 437-441); `--barrier all` syncs every term change and
  grant (FM:208-210, 355-358); a campaign's hard-state record forces a barrier on its core that
  covers every turn written (DM:515-517, 641-645), so the model is the design. `grow6` and the
  shrink fence are not enabled under it, and `six`, `six_loss`, `grow67`, `grow67_loss` lose under
  f + 1 and stop at the decision under the majority, under every barrier mode (reproduced).
- **Q10's option 4 is safe by D6's argument.** At a four-voter membership, where the least f in
  force is 1, a forgetful candidate and its one flagged answerer are forgetful at once, beyond the
  model. For a candidate that is not forgetful, both answerers are forgetful at the second answer
  unless the first re-learned from a leader W elected between them, and W's quorum avoids the
  candidate and both answerers (the later one had not voted above the round's term), which leaves
  one voter of four, at most one of three, and two of five, each short of a majority. A lying voter
  then needs one honest forgetful voter, as at rf = 3's steady state. The model does not separate
  the two counts there (no decision within the designer's depths, DM:1043-1044), so option 4 rests
  on the argument, as Q10 says.
- **DM-R27's twelve items** are at the lines in the status table, and Q9's figures recompute from
  `arith.py` byte for byte (two slips and a swing factor: DM-R32 items 4 to 6).

What blocks (landing, not starting):

- DM-R29: rule 2's slices in the mark turns (C23, before step 1's recovery and driver code lands).
- DM-R31: the keyed checks' coverage (C25, before step 5's verifier lands, and before the owner
  answers Q9 on option 3's claim).

### Status

| Item | Status | Where, at `b56cb46` |
|---|---|---|
| DM-R24 Copy-forward and the open record's lifetime | **Partially closed.** Rule 1, newest by per-group sequence (DM:560-565; D4 DM:651-656), and rule 3, the open record live for its epoch through `0x13` copies and an unreadable one read as unclean (DM:589-603; D16 DM:1534, 1537-1541; the threat row DM:1821; T7 DM:1964; T9 DM:1966), are closed, with `mark_model.py` and its controls (DM:1049-1078, reproduced). Rule 2, the marks before anything else of the epoch (DM:566-588), is closed for one group and open for several: its slices may ride in any mark turn (DM:569-571), which loses a write when the marks take more than one turn. T6's mutants and T9's cases (DM:1963, 1966) are in, short of DM-R29's | DM-R29 |
| DM-R25 The planted segment's digest | **Partially closed.** Every comparison is a keyed sum (DM:1645-1679): SipHash-2-4 under a per-process follower key from `getentropy`, never sent or stored, modulo 2⁶⁴ − 59; sampling decisions are the same PRF of the segment's place in the log (DM:1680-1689); option 3 reworked as keyed install checks with no per-document state (DM:1690-1748); format 13 unchanged (DM:1558-1562); T2 and T4 (DM:1959, 1961). The sums cover F21's records as the scan path reads them; queries, and option 3's own live-docs check, read structures they do not cover, and a whole image installed at once has no log range to check (DM-R31); the sampling input lacks the entry's term (DM-R32 item 3) | DM-R31, DM-R32 |
| DM-R26 `manual`'s admission | **Closed** (DM:809-811, 895-905, 938-953; D15 DM:1492; the threat row DM:1818; the fault table DM:1836; T3 and T6 DM:1960, 1963). `six_heard` and `grow67_heard` decide nothing, and the search from `six`'s step 37 with every voter required holds | |
| DM-R27 Precision | **Closed**, all twelve: 1 DM:925-929, 1818; 2 DM:486-491, 1825-1828; 3 DM:831-836; 4 DM:499-502, 721-730, 1474, T6 DM:1963; 5 DM:1218-1225, T16 DM:1973, step 5 DM:2078; 6 DM:1587-1590, 1884 (its typical entry is miscounted, DM-R32 item 5); 7 DM:1787-1789; 8 DM:1717-1724; 9 DM:1973; 10 DM:954-958, 1874; 11 DM:1680-1689; 12 DM:867-872, 1836, 2048-2051 | DM-R32 |
| DM-R28 The model's vote barrier | **Closed** for the model: the candidate syncs at campaign in both checkers (FM:203-206, 238-246, 437-441; `--barrier all` FM:208-210, 355-358), the window schedules and searches redone under it (DM:1016-1047), `grow6` and the shrink fence gone from T3 (DM:1960), `grow67` and `grow67_loss` in (DM:852-856), the limits paragraph states it (DM:1085-1087). The barrier is D4's. The revision's reading of revision 1's five-voter schedules under it is wrong: DM-R30 | DM-R30 |
| DM-R19 Forgetfulness across restarts | **Partially closed**, with DM-R24: what remains is rule 2's mark turns | DM-R29 |
| DM-R21 Q9's residual | Closed for honest faults, as at revision 2; for planted ones, keyed (DM-R25) but not covering the query path | DM-R31 |
| DMS-11 The sample against a leader that chooses | The comparison is keyed and each decision a keyed PRF (DM:1680-1689, 1756-1758), as the security re-check asked; the security reviewer rules | DM-R32 item 3 |
| C15 Step 3 | **Closed** in the design: step 3's restart, recovery-election, and rewind code lands only with DM-R24's mark, DM-R26's rule, and the cap (DM:2065-2070, 2076). C23 adds the marks' order to what its recovery hands the turn driver | C23 |
| C16 Step 1 | Superseded by this verdict | C23 |
| C17 Done criteria | **Open, the lead's**: "none set yet" (ST:19-20); the ledger proposes K2's and K3's rows (ST:200-207) | C23, C24 |
| C18 Step 5 | **Closed** for its items (the keyed comparison DM:1645-1679; T4's forging mutant DM:1961; the install path DM:1218-1225, 2078); C25 widens it | C25 |
| C19 Step 6 | **Closed** in the design (DM:1883, 1960, 2079): `grow67` for `grow6`, `six` under `manual` with every voter answering, `SIX_LIAR` outside the model, restarts after copy-forward and a reclaimed open record; `lead5` joins (C24) | C24 |
| C20 Calibration | **Closed** (`calibration.md:19-20`, `4afbf67`). `b56cb46` amends C7's row (`calibration.md:18`) to call the fixed-membership loss open, which `lead5` contradicts | DM-R30 |
| C21 Q9 | **Closed** as asked (DM:1985-2031); option 3's claim to detect a planted segment needs DM-R31, and its cost lead at rf = 3 rests on an unmeasured walk (DM-R32 item 6) | C25 |
| C22 Joint consensus | **Closed** (DM:954-958, 1874) | |

The runs, mine (each one thread at `nice -n 19`; times as the runs printed):

| Run | Scope | Depth | States | Time | Result | File |
|---|---|---:|---:|---:|---|---|
| `mark_model.py`'s controls, repeated | `literal`, `firstturn_jorder`, `same` | 10 | 10,133; 34,543; 1,668 | <1 s each | identical to the designer's but for times: safety lost in 6, 7, and 5 steps | `runs/mark-controls-repro.out` |
| `mark_groups.py` at one group, `--mark-always` | The designer's rule, to calibrate | 8; 10 | 122,267; 1,458,689 | 5 s; 74 s | holds; `mark_model.py`'s counts exactly | `runs/mg-calib-g1-d8.out`, `-d10.out` |
| Two groups, slices in any mark turn (D3's text) | One mark per turn, two entries per group, two switches | 7 | 146,563 | 7 s | safety lost in 7 steps, no OS crash | `runs/mg-any-mpt1-d9.out` |
| The same, a clean close allowed between mark turns | | 6 | 32,286 | 1 s | safety lost in 6 steps | `runs/mg-any-mpt1-close-d9.out` |
| Two groups, no slice before the last mark (`none`) | Clean close allowed between mark turns | 9 | 2,716,835 | 156 s | holds | `runs/mg-none-mpt1-d9.out` |
| Two groups, slices only of marked groups (`own`) | | 9 | 2,807,989 | 161 s | holds | `runs/mg-own-mpt1-d9.out` |
| Two groups, every mark in one turn, slices ride | | 9 | 3,012,423 | 170 s | holds | `runs/mg-any-mpt0-d9.out` |
| Two groups, copies keep their first epoch (`orig`), slices ride | No marks | 9 | 3,853,303 | 194 s | holds | `runs/mg-orig-any-d9.out` |
| Three groups: `any`; `own`; `none` | One mark per turn | 6; 8; 8 | 83,203; 2,249,291; 2,233,453 | 4 s; 127 s; 127 s | lost in 6 steps; holds; holds | `runs/mg3-*.out` |
| The copy read through its newest turn (`--copy-reading newest`): one group, the designer's rule; two groups, `none`; `own` | | 10; 9; 9 | 1,034,466; 2,072,008; 2,158,688 | 52 s; 113 s; 117 s | all hold | `runs/mg-*-newest-*.out` |
| `lead5`, `heard5l`, `lead5_loss`, forced, both checkers | 5 voters, `fplus`, `timeout`, `heard`, `fplus_heard`, `round`, `round_heard`; `--barrier none`, `candidate`, `all` | 13 to 14 | | <1 s | the owner's and revision 0's rules lose `v0` under every barrier, at most two forgetful at once; `round` and `round_heard` not enabled at the election | `lead5.out`, `lead5-cc.out` |
| From `lead5`'s step 9, `fplus` / `round`, candidate barrier | MAXOS 3, terms to 4 | 6 | 16,377,089 / 29,609,357 | 59 s / 102 s | `memory` at 4 steps / holds | `runs/cc-lead5-9-*-candidate-d6.out` |
| The same, every barrier | | 6 | 14,429,542 / 26,856,533 | 54 s / 92 s | `memory` at 4 steps / holds | `runs/cc-lead5-9-*-all-d6.out` |
| `term_index_plant` | 200 terms, four blocks, termdict at strata's pin | | | <1 s | scan: 200 and 200 entries, keyed sums equal; find: 200 and 180 | `plant/term_index_plant.out` |

### New findings

1. **DM-R29. A reclamation slice in an early mark turn copies a later-marked group's unmarked hard
   state into epoch V, and a crash before that group's mark recovers it not forgetful** (D3 rule 2,
   D4, D16's driver API, T6, T9).
   - Rule 2 writes each forgetful group's marked hard state "in the epoch's first turns, before any
     other turn", and lets "reclamation's copies ... ride in the same turns, `turn_log.h:297`, since
     a copy keeps its old sequence and survives or tears with the marks beside it" (DM:566-571).
     That holds for a copy of a group whose mark is in the same turn or an earlier one. The turns
     are plural, and every turn the journal stages takes a forward slice once it holds more than
     `retain_segments` segments (`turn_log.h:297`, `359`), chosen by segment and group id
     (`turn_log.h:357-371`), not by which groups are marked yet. A slice in the first mark turn can
     therefore copy group H's newest hard state, unmarked and from before the verdict, into epoch V
     while H's mark waits for a later turn. If the process stops between those turns without a
     verdict, a same-boot crash with no torn turn or a clean close, recovery reads V from the open
     record, H's newest hard state by sequence is the copy, and the copy's turn is in epoch V, not
     older: rule (c) leaves H not forgetful (DM:605-608), though H lost a write it acknowledged.
   - `r3/mark_groups.py` is the designer's `mark_model.py` per group (its state, recovery, barrier,
     reclamation, open copies, checks, and the oldest-copy reading of rule (c)), with the marks one
     group per turn and slices allowed to ride in them. With one group and the designer's marking
     rule it reproduces `mark_model.py`'s counts exactly (122,267 at depth 8, 1,458,689 at depth
     10). With two groups and slices in any mark turn, D3's text, it loses safety in 7 steps with no
     OS crash: H writes `v1` at `Written`; a failed writeback and restart (V = 2, G and H
     forgetful); the open record and its sync; G's mark turn carrying a slice of segment 1, which
     copies H's founding record; the barrier, which retires the originals and reclaims segment 1; a
     process crash: no verdict, V = 2, H's newest hard state seq 1 in epoch 2, H not forgetful
     without `v1` (`runs/mg-any-mpt1-d9.out`). With a clean close allowed between mark turns, 6
     steps (`runs/mg-any-mpt1-close-d9.out`); with three groups, 6 (`runs/mg3-any-mpt1-d8.out`).
   - The marks take more than one turn whenever a core holds more forgetful groups than one turn
     fits: a 64 MiB segment less the quarter a slice may take holds about half a million marks at
     the 2.92 bytes written per byte that Track C measured for 34-byte records
     (`track-c.md:550-552`; arithmetic), and the host may stage them over several turns well before
     that, as groups finish their recoveries.
   - Fix, in D3 rule 2, D16's driver API, T6, and T9: the turn driver takes no reclamation slice in
     an epoch until every first-turn mark is written (`--ride none`: holds to depth 9 with two
     groups, 2,716,835 states, and to depth 8 with three, 2,233,453), or a mark turn's slice copies
     only groups whose marks are in that turn or an earlier one (`--ride own`: 2,807,989 and
     2,249,291 states, holding). Every mark in one turn (`--mpt 0`, 3,012,423 states) and copies
     that keep their first epoch (`orig`, 3,853,303 states) hold too, but the first cannot bound the
     turn and the second is the format byte revision 3 declined. I recommend the first: one flag in
     the driver, and reclamation pauses for a few turns per recovery. T6 gains the mutant "a slice
     in a mark turn before its group's mark" and T9 the case "two groups whose marks take two turns,
     a slice in the first copying the second's newest hard state, a kill and a clean close between
     the turns: the second recovers forgetful at both".

2. **DM-R30. The owner's rule loses a `memory` write at five voters under every barrier D4 imposes,
   so the five-voter loss D6, Q10, and the calibration row call open is shown** (D6, Q10, T3, T6,
   `calibration.md:18`).
   - Revision 3 finds that `fplus5` and `heard5` do not replay under the candidate's barrier, since
     their third forgetful voter, the leader that let voter 0 re-learn, campaigned while holding the
     write and so synced it (DM:880-886), and concludes that "whether the ruled rule loses at five
     voters is open, and it has no argument either way" (DM:2036-2038; T6 DM:1963; T3 drops the
     schedules, DM:1960). The barrier is right; the conclusion rests on replaying revision 1's two
     schedules, not on a search for the shape the barrier leaves. The leader that wrote the write
     campaigned before the write existed, so no campaign's barrier covers it there.
   - `lead5` (13 steps, `r3/lead5.py`, `r3/schedules/lead5.txt`): leader 0 writes `v0` and
     replicates it to 1 and 2 (`memory`); 1 and 2 lose power; 2 probes 1 flagged; 2 re-learns `v0`
     from 0, still leader; 0 loses power, never having synced `v0`; 0 probes 1 flagged, so 1 has
     heard three forgetful voters, itself included, while at most two were forgetful at any instant;
     1's `recovery_timeout` passes; 1 wins term 2 with 3 and 4, without `v0`, which 2 holds. It
     loses under the owner's rule (`fplus`) and revision 0's (`timeout`) under `--barrier none`,
     `candidate`, and `all`, in both checkers (`lead5.out`, `lead5-cc.out`), and `heard5l`, the same
     with probes from 3 and 4 in place of the timeout, loses under `heard` and `fplus_heard` (14
     steps). Revision 3's rule blocks both: 1 cannot campaign normally, and a round has at most two
     flagged answers at its term. From `lead5`'s step 9 the C++ search finds the loss in 4 steps
     under `fplus` (16,377,089 states with the candidate's barrier, 14,429,542 with every barrier)
     and holds under `round` (29,609,357 and 26,856,533 states, depth 6;
     `runs/cc-lead5-9-*-d6.out`).
   - Fix: D6's "Why the count since the restart is not" (DM:873-890) cites `lead5` and `heard5l` and
     drops "open"; Q10 (DM:2033-2038, 2060) says the ruled rule loses at five voters under every
     barrier, so options 2 and 3 rest on a rule shown to fail at rf ≥ 5; T3 runs `lead5` and
     `heard5l` among the forced schedules with no loss under D6 as revised, and T6's `fplus` mutant
     is killed by `lead5` (DM:1960, 1963); `FM:849-851`, which says the owner's rule still loses
     `fplus5_vb` when `schedules.out` shows its step 15 not enabled, is corrected; the lead amends
     `calibration.md:18`'s parenthetical to cite `lead5` (C24).

3. **DM-R31. The keyed sums cover F21's records as the scan path reads them; queries, and option 3's
   own live-docs check, read structures the leader writes and no check reads** (D18, Q9, the threat
   model, the fault table, T4).
   - F21's postings come from `scan_terms()`, which walks the term dictionary's blocks in order from
     `floor_value("")` (`reader.hh:491-500`; `termdict.hh` `reader::scan` at `eaf83b3`), and its
     lengths from the `lengths` section (`reader.hh:443`, `1895`). A query finds a term through
     `find_term()`, one FST floor over each block's first key (`reader.hh:470-484`;
     `reader::find_run`), scores with the `norms` section and the statistics at the head of
     `term_index` (`reader.hh:394`, `425-439`), and prunes with impacts read from skip data
     (`reader.hh:461-467`). A key lookup, which check 2 uses for every delete and replace, consults
     the key filter and the key range before the keys' own FST (`keys.hh:165-173`), while the D
     records come from the cursor walk (`reader.hh:743-750`). The FST is validated as an automaton
     at open, not against the blocks it indexes. None of these is in F21's records, so no keyed sum
     covers them.
   - `r3/plant/term_index_plant.cc`, against termdict, fst, and varint at strata's pins: an honest
     dictionary of 200 terms in four blocks, and the same blocks under a planted index that moves
     the second block's first key 20 terms earlier. The scan sees all 200 entries under both and
     their SipHash-2-4 sums modulo 2⁶⁴ − 59 are equal; `find` finds 200 of 200 under the honest
     index and 180 under the planted one, missing `t044` to `t063` (`plant/term_index_plant.out`).
     In a segment, a leader hides a document from every query on a term it chooses, and every keyed
     check passes; the same holds for ranking through `norms`, the statistics, or impacts. A planted
     key filter makes check 2's lookup miss an old version on its replace: the follower expects
     nothing deleted, the leader's bitmap deletes nothing, they agree, and two live versions of the
     key remain, which only F21's whole-index `digest()` would refuse (`index.hh:657`), and nothing
     runs it in production.
   - A whole image installed at once, a new member's, a repair's, or a reinstall after D8's
     catch-up, has no log range to check against: its base is whatever the serving member sent. In
     an all-segment-mode group every member holds the leader's bytes, so a compromised leader
     serving the base plants it there.
   - So D18's "a dropped, altered, or planted posting, document, or stored value changes one sum and
     not the other" (DM:1700-1701) holds, while "What it catches: any fault ... honest or planted,
     at the first install" (DM:1726-1731), the threat row's "a planted flush, merge, or deletion is
     detected at its first install" (DM:1817), the fault table (DM:1840), the summary (DM:259-260),
     and Q9's "the only option that detects a planted segment" (DM:2022-2024) overstate it. The
     uncovered list D18 names, "skip data, block maxima" (DM:1731, 2015-2016), lacks the term index,
     the key index and filter, norms, and the statistics, and option 1 does not detect faults there
     either: it only holds its own.
   - Fix, in D18, Q9, the threat row, the fault table, and T4, before step 5's verifier: the keyed
     install check walks each structure from its start (the dictionary's blocks from offset 0 to the
     section's end, since `scan("")` starts at the index's floor of the empty key, which a planted
     index also sets) and checks every lookup structure against that walk: the term index holds
     exactly each block's first key at that block's offset; every walked key is found at its ordinal
     through the key range, filter, and index, which also makes check 2's misses sound; `norms`
     equal the encoded lengths; the statistics equal the walked sums; the impacts and skip data
     equal what the walked postings and norms give. Verifying keeps every file byte for byte the
     `Image`'s, as D17's identity rule needs (DM:1213-1214); a follower that built those structures
     itself would hold files no other member could serve or match. A whole image is fetched from a
     member that is not the leader whenever one is reachable, with its residual stated. T4 gains a
     mutant leader that plants a term index hiding one term and one that plants a key filter hiding
     one key across a replace, each caught at its first install; T2 prices the added lookups. Or, if
     the owner prefers, the design states the residual in each of those places: under every option a
     compromised leader can hide documents from lookups, add them to lookups, or misrank them,
     undetected.

4. **DM-R32. Precision** (D3, D6, D18, Q9, T7, the model).
   1. Rule 2 decides every `written` group before the epoch's first turn (DM:571-574), so a core
      writes nothing until every group's newest hard state and configuration are read, and the marks
      of many forgetful groups take turns and segments before reclamation resumes (DM-R29's fix
      makes that explicit). With thousands to millions of indexes per host this is the open's
      latency and its head room: D3 states it, and T7 or T8 measures the time from open to the first
      admitted turn at 1,000 and 100,000 groups per core.
   2. `mark_model.py` reads two surviving copies of a record through the oldest's turn, "the reading
      least favorable to finding a hole" (MM:89-91); it is the reading most favorable to safety,
      since it gives rule (c) the older epoch. D3's "the epoch of the turn holding its surviving
      copy" (DM:607) leaves the choice open, and `TurnRecovery` must report one. Read through the
      newest copy's turn, the fixed rules hold too (one group with the designer's marking at depth
      10, 1,034,466 states; two groups at depth 9 with `none` and `own`, 2,072,008 and 2,158,688;
      `runs/mg-*-newest-*.out`), so once DM-R29 is fixed either reading is safe; D3 names the one
      `TurnRecovery` reports, and the model's comment says what its reading is.
   3. The sampling decision's input, the group, the `Image` entry's index, and the segment's
      position (DM:1680-1684), repeats when D6 replaces an `Image` entry above `commit_durable` at
      the same index, so the replacement draws the same decision; the entry's term in the input
      makes every decision fresh, as "a log index never repeats" (DM:1683) intends.
   4. `records.py`'s 2012 bracket counts one block too many: its docstring takes 124 cycles at 8
      bytes, its formula gives 139.7 there (`124 + 15.7 × (blocks − 1)` with the length block
      counted), so every record under 64 bytes, 95.5% of them, costs 15.7 cycles too many, and the
      5.73 µs top is 5.27 µs as the docstring states it (`r3/records_stated.out`); the lower bound
      is unaffected.
   5. `arith.py`'s typical manifest entry is 46 bytes, but its own breakdown (identity 16, id 2,
      size 5, crc 5, counts 3 + 3 + 4, live ref 2 + 3 + 4, values 1, attributes 1) sums to 49, which
      admits about 76,900 and 49,600 segments, not 79,100 and 50,500 (DM:1587-1588, 1884;
      `r3/q9_cross.out`).
   6. Q9's "at rf = 3 it costs less than 1 across the whole estimate" (DM:2025-2026) rests on the
      walk's decode, assumed 0.5 to 2 µs per document and pass (DM:1743-1744), which nothing here
      measured; at one flush's slow end option 3 reaches option 1 at 4.1 µs per document and pass,
      twice the assumed top, and at 16 MiB at 7.8 (`r3/q9_cross.out`, over `arith.py`'s own inputs),
      so T2 measures the walk, record building included (`digest_record` allocates a string per
      record, `reader.hh:74-87`), and DM-R31's added lookups, before Q9 is final.
   7. A flush check "holds its range's documents while it runs" (DM:1721): the sum is additive, so
      it can stream the range and keep 8 bytes per document to subtract a document deleted or
      replaced later in the range; D18 states the bound.

### New blocking items

None blocks a start. Two block landings:

1. DM-R29: no reclamation slice in an epoch before its last first-turn mark (or slices only of
   groups already marked), in D3 rule 2, D16's driver API, T6, and T9. Blocks step 1's recovery and
   turn driver from landing, and step 3's recovery from handing marks to a driver without it (C23).
2. DM-R31: the keyed install checks verify the query-path structures, or the residual is stated
   everywhere option 3's claim is made. Blocks step 5's verifier from landing (C25).

### Conditions

1. **C23. Step 1** (`journal`) may start. DM-R29 pauses only the part it touches, the turn driver's
   first-turn marks and its slices around them, until D3 rule 2 carries the order (no slice in an
   epoch before its last mark, my recommendation, or a slice only of groups already marked), D16's
   API says the driver takes the marks before it admits any other record and holds its slices until
   they are written, and T6 and T9 carry DM-R29's mutant and case. The rechecker confirms the text
   against `r3/mark_groups.py` (`--ride none` or `own` holds, `any` loses). Step 3's recovery hands
   every mark to the driver before the epoch's first turn, as rule 2 already requires (C15).
2. **C24. DM-R30** goes into D6, Q10, T3, T6, and `FM:849-851` in the next revision, with `lead5`
   and `heard5l` among T3's forced schedules and C19's simulator schedules; the lead amends
   `calibration.md:18` to cite `lead5` in place of "the fixed-membership loss is now open". Q10
   returns to the owner with options 2 and 3 resting on a rule shown to lose at rf ≥ 5; the lead's
   call (option 1) stands. Before a counterexample is retired as a model's artifact, the same loss
   is searched for from the schedule's prefix under the corrected model, with every holder of the
   write that has not synced it since as the copy that goes.
3. **C25. Step 5** (widens C18): D18's keyed install checks take DM-R31's verification of the
   query-path structures, whole images come from a member that is not the leader when one is
   reachable, and T4 carries the planted term-index and key-filter mutants, or D18, the threat row,
   the fault table, and Q9 state the residual. Q9's recommendation is restated on that basis before
   the owner answers it, with DM-R32 items 4 to 6. D18's keyed comparison and DM-R31 are the threat
   model's leader row, so the lead routes them to the security reviewer's next re-check, as the
   ledger suggests (ST:197-199).
4. **C26. Calibration**, for the lead, a row on my own fix: "2026-10-19 | DM-R24's fix in the
   re-check of revision 2 | A first-turn mark per forgetful group, argued and modeled with one
   group; marks over several turns with reclamation slices riding in them lose a write (DM-R29) |
   The re-check of revision 3, `mark_groups.py` | Model every per-group rule with at least two
   groups whose records share segments and turns, and split every 'first turn' into more than one
   turn".
5. **C17** stays open (the lead's): K2 carries DM-R24's T9 rows and DM-R29's case; K3 carries the
   mutants the ledger lists (ST:200-207), DM-R29's, and `fplus` killed by `lead5`.

Not run: no benchmark, timing, or A/B (Q9's and D18's CPU figures are arithmetic over the design's
labeled assumptions; no SipHash, EVP, segment-walk, TLS, or network rate measured); no strata,
consensus, or journal build or suite (the branch changes no library code; the models and probes
build apart); no simulator (unbuilt); no Linux, FreeBSD, or arm64; no fuzzing; the designer's
deepest searches not repeated (the journal model at depth 11 with 64-bit keys, the window searches
from `six`'s step 37 and `grow67`'s step 65, the five- and three-voter restart searches to depths 7
and 11), only those in the run table; `mark_groups.py` to depth 9 with two groups and 8 with three,
two entries per group, one or every mark per turn, segment switches only outside the mark turns,
reclamation of whole segments, no sector subsets within a turn, and Python's full-state tables (no
hash compaction); `lead5` and `heard5l` as forced schedules plus one depth-6 search from step 9 per
mode and barrier, no exhaustive five-voter search to depth 13; the planted structure shown for the
term index alone, against termdict at strata's pin with no strata segment written, the key filter,
norms, statistics, and impacts argued from the code at the cited lines; no forgery through strata's
writer; the SipHash paper not re-fetched (the reference vectors were checked in Python and C++);
joint consensus not modeled; no Xapian or Xapiand GPL source opened.

## Re-check (revision 4)

- Date: 2026-10-19, read and run from 13:50 to 14:14 PDT.
- Reviewed: `design/durability-modes` at `b7c6c78`, `git log 2e9ad85..b7c6c78` (5 commits, `4a6403d`
  to `b7c6c78`): `docs/design/durability-modes.md` ("Revision 4" and "Changes in revision 4" first),
  `docs/status/durability-modes.md`, `docs/reviews/calibration.md` (`4a6403d`), the models in
  `bench/durability/model/` (`mark_groups.py`, `runs_r4.sh`, `forgetful_model.py`'s `LEAD5`,
  `HEARD5L`, `LEAD5_LOSS`, `mark_model.py`'s comment, the three schedule files), `arith.py`,
  `records.py`, and the raw files in `results/r4/` and `results/arith.out`.
- Checked against: my re-check of revision 3 above (DM-R29 to DM-R32, C23 to C26); journal
  `cb66f3d`'s `turn_log.h` (the copy in `r1/turn_log-cb66f3d.h`: `stage()` `:281-314`,
  `forward_slice()` `:355-374`, `reclaim()` `:375-391`); strata at `b7c6c78` (`index.hh:644-705`,
  `1371-1414`, `1931-1945`; `reader.hh:383-502`, `1892-1905`; `searcher.hh:92-121`;
  `search.hh:72-96`, `310-318`, `1224-1232`; `format.hh:442-455`); Kronuz/termdict `eaf83b3` and
  Kronuz/fst `24da59a` at strata's pins (`termdict.hh:78-190`, `443-545`); `calibration.md`, fourteen
  rows, applied below (C20's row asks for the bytes through every lifecycle event, which DM-R33 is
  for a merge's dead ordinals).
- Checked by:
  - `quiet check` before every build and run: exit 2 throughout (a light hold, `f13q-thresholds`),
    so every build and run used `nice -n 19`, the model runs one at a time (each depth-9 Python
    search peaked at 3.9 to 4.2 GB resident, `runs/*.time`, with swap at 21.5 of 22.5 GB used), and
    the one strata build three jobs.
  - Builds, Homebrew clang 23.1.2: `r4/plant/term_index_check5.cc` against termdict, fst, and
    varint at strata's pins (`-O1 -Wall -Wextra`, no warning); `r4/plant/dead_stats_plant.cc`
    against strata at `b7c6c78` and its pinned dependencies (`r4/plant/CMakeLists.txt`, the
    fetched sources of `strata/build/_deps`, each at the pin `r4/pins.txt` lists and clean;
    RelWithDebInfo, 60 s).
  - Reproduced byte for byte: `results/r4/schedules-lead5.out` with my revision-3 build of the
    unchanged `forgetful_check.cc` (`runs/repro/schedules-lead5.out`); `results/arith.out`
    (`runs/repro/arith.out`); `results/r4/records.out` over T2's corpus (sha256 `0a1d5344…`,
    `runs/repro/records.out`). Reproduced by count: every `mark_groups.py` row of revision 4's
    table, the `none` and `any` rows rerun here at my revision-3 depths (below) and the `own`,
    `newest`, and calibration rows against my revision-3 files (`r3/runs/mg-own-mpt1-d9.out`,
    `mg-none-newest-d9.out`, `mg3-own-mpt1-d8.out`, `mg-calib-g1-d8.out`), and the four searches
    from `lead5`'s step 9 against my revision-3 files (`r3/runs/cc-lead5-9-*-d6.out`: identical
    counts, expansions, and transitions).
    `bench/durability/model/mark_groups.py` differs from my `r3/mark_groups.py` only in
    `--switch-marking` and its header (diff read; the snapshot I ran is
    `r4/mark_groups-b7c6c78.py`, sha256 `6d0b4fb7…`).
  - Probes, all in `strata-wt/review-durability/.scratch/review-durability/r4/`, kept with their
    outputs: `runs_arch_r4.sh` (outputs and `/usr/bin/time -l` files in `runs/`);
    `own_reclaim.py` and `own_reclaim_forced.py` (`runs/own_reclaim_forced.out`);
    `plant/term_index_check5.cc` (`plant/term_index_check5.out`); `plant/dead_stats_plant.cc`
    with `plant/CMakeLists.txt` (`plant/dead_stats_plant.out`); `pins.txt`; the reproductions in
    `runs/repro/`. The revision-1 to revision-3 probes are in `r1/` to `r3/`, unchanged.
  - Not run: the last line of this section.

Citations: `DM:N` is `docs/design/durability-modes.md` at `b7c6c78`; `ST:N` is the ledger there;
`FM:N` is `bench/durability/model/forgetful_model.py` there; `turn_log.h` is journal `cb66f3d`;
`index.hh`, `reader.hh`, `searcher.hh`, `search.hh`, `format.hh` are strata `b7c6c78`'s
`include/strata/`; `termdict.hh` is Kronuz/termdict `eaf83b3`; `r4/` is
`.scratch/review-durability/r4/`.

### Verdict

**First step 1: APPROVE WITH CONDITIONS (C27). First step 3: APPROVE WITH CONDITIONS under C15 and
C23, as ruled. First step 5 may continue; its verifier lands only with DM-R33's and DM-R34's fixes
(C28, widening C25). First step 8: APPROVE WITH CONDITIONS under C12, with `Vouch` as a control kind
whose answering rule is step 5's (DM-R34). First step 9: APPROVE. Revision 4 closes DM-R29, DM-R30,
and DM-R32, and closes DM-R31 for everything it named: no forward slice before the last mark turn's
write returns holds in my multi-group model at every depth I ran in revision 3 and in two new
shapes, while slices in any mark turn lose; `lead5` and `heard5l` are cited at every place the
five-voter loss is stated, both checkers reproduce them, and Q10 now says the ruled rule loses at
rf ≥ 5; and D18's check 5 kills my planted term index, a planted extra index key, and a re-cut
dictionary, each failing the byte-for-byte re-encoding while the honest one passes. Two gaps remain
in what the install checks cover, both for step 5's verifier. A merge output carries documents
deleted while it ran as dead ordinals, ranking statistics count dead ordinals, and no check covers
their content: with three dead ordinals, strata at this commit gives an index the same F21 digest
(the live records every keyed sum covers) and moves the best match for "alpha OR beta" out of the
top three (DM-R33). And a member whose own whole image was installed unvouched still vouches for
later entries, so two such members can vouch for each other's planted content (DM-R34).**

What holds, and reproduces:

- **DM-R29's order.** D3 rule 2 (DM:618-643), D4's mark (DM:738-742), D16's `hold_forward`
  (DM:1658-1662), T6's mutant and T9's case (DM:2205, 2208), and step 1's row (DM:2347) say one
  thing: the driver admits no other record until the marks are written, and `forward_slice()`
  returns nothing until the last mark turn's write returns. The model's `--ride none` is that rule
  (a mark turn holds marks only; slices resume in the first turn after), and it holds: two groups to
  depth 9 (2,716,835 states), with segment switches between mark turns (2,751,883), three groups to
  depth 8 (2,233,453; with switches 2,244,878), and three groups with two marks in the first mark
  turn and one in the second (2,237,109 states, depth 8); `--ride any`, revision 3's text, loses in
  7 steps, 6 with a clean close, 6 with three groups, 6 with segment switches allowed between mark
  turns, and 6 with three groups two marks to a turn. Every count I share with revision 4's table
  equals it (DM:1180-1189).
- **The five-voter loss under every barrier (DM-R30).** `LEAD5`, `HEARD5L`, `LEAD5_LOSS` (FM:858-873)
  are my schedules; under `--barrier none`, `candidate`, and `all` both checkers lose `v0` under
  `fplus` and `timeout` (`lead5`), `heard` and `fplus_heard` (`heard5l`), and fence it under `fplus`
  (`lead5_loss`), with at most two voters forgetful at once, and `round` and `round_heard` cannot
  hold the election (`schedules-lead5.out`, reproduced byte for byte). The searches from step 9
  match mine state for state.
- **Check 5 on the dictionaries.** The builder is a function of its entries and alphabet: it cuts a
  block at 64 entries, or at 32 once the block passes 1,024 bytes (`termdict.hh:125`), shares the
  exact common prefix inside a restart run (`:101-102`), and indexes each block's first key at its
  offset (`:172`). The cursor walks from offset 0 by block lengths, never through the index
  (`termdict.hh:498-523`). `r4/plant/term_index_check5.out`: the honest dictionary passes; r3's
  planted index (180 of 200 found) and a planted extra index key (172 found) fail on the index; a
  dictionary re-cut at 40 entries, which reads every term, fails on the blocks, so a change to the
  cut rule needs a format version, as D18 and the termdict row say (DM:1884-1886, 2128); a widened
  alphabet re-encodes equal and reads the same. Once the received bytes equal the builder's output
  for the walked entries, every lookup behaves as on an honest writer's bytes, as D18 argues
  (DM:1874-1883).
- **DM-R32's arithmetic.** `arith.out` and `records.out` regenerate byte for byte; the 2012 bracket
  is 5.27 µs per pass (`records.py:38-41`), the typical manifest entry 49 bytes (`arith.py:204-208`),
  and Q9's crossover at rf = 3 4.54 µs per document and pass at one flush's slow end (`arith.out`,
  "Q9 r4 crossover"), as DM:2276-2283 states.

What blocks (landing, not starting):

- DM-R33: a merge output's dead ordinals, outside every check (C28, before step 5's verifier
  lands, and before the owner answers Q9 on option 3's claim).
- DM-R34: `Vouch` from a member whose own image is unvouched (C28, before step 5's whole-image rule
  lands).

### Status

| Item | Status | Where, at `b7c6c78` |
|---|---|---|
| DM-R29 Slices in the mark turns | **Closed.** D3 rule 2: no reclamation slice in the epoch until the last mark turn's write returns (DM:618-643); D4 (DM:738-742); D16's `hold_forward`, additive, `forward_slice()` returning nothing while set (DM:1658-1662); T6's mutant "a reclamation slice in a mark turn before every group's mark is written" (DM:2205); T9's case with two groups, a kill and a clean close between the mark turns, and marks across a segment switch (DM:2208); step 1 (DM:2340-2347). Models reproduced at my depths (the run table below). The "why no slice" paragraph's reason is wrong though its choice stands (DM-R35 item 1) | DM-R35 |
| DM-R30 The five-voter loss | **Closed.** D6 cites `lead5`, `lead5_loss`, and `heard5l` and drops "open" (DM:961-981); the model table (DM:1066); revision 4's runs (DM:1190-1192); Q10 says the ruled rule loses at five voters under every barrier and that options 2 and 3 keep a rule shown to lose at rf ≥ 5 (DM:2300-2333); T3 runs the three schedules (DM:2202); T6's `fplus` mutant is killed by `lead5` (DM:2205); FM:849-852 corrected; the retiring rule (DM:1212-1215); the simulator rows (DM:2124, 2352); `calibration.md:18` amended (`4a6403d`). One slip in the summary (DM-R35 item 2) | DM-R35 |
| DM-R31 What queries read | **Closed for what it named; DM-R33 and DM-R34 are new.** What a query reads beyond F21's records (DM:1823-1839); the cursor walk from offset 0 (DM:1836-1839); check 4, metadata against the log (DM:1868-1871); check 5, both dictionaries re-encoded byte for byte, the key range and filter, norms, statistics, document frequencies, skip entries and pairs, no unchecked section (DM:1872-1911); check 2's lookups sound through check 5 (DM:1855-1860); whole images (DM:1957-1984); `Vouch` (DM:1642, 1737-1742); option 2 runs check 5 on its samples (DM:1995-1999); option 1's residual names the structures (DM:2034-2040); the threat row (DM:2058), the fault table (DM:2081), the summary (DM:308-313); T2 (DM:2201), T4 (DM:2203); Q9 (DM:2228-2290). `term_index_check5.out` | DM-R33, DM-R34, DM-R35 item 3 |
| DM-R32 Precision | **Closed**, all seven: 1 DM:655-667, T8 DM:2207 (1,000 and 100,000 groups per core, at most 1 s, the journal's growth beside); 2 DM:668-674, 693, `mark_model.py:89-92`; 3 DM:1810-1816; 4 `records.py:38-41`, DM:1946-1948; 5 `arith.py:204-208`, DM:89, 1711, 2125; 6 DM:2276-2283, T2 DM:2201; 7 DM:1917-1923 (16 bytes per document of one flush's range, keyed slots) | |
| DM-R25, DM-R21, DMS-11 | As at revision 3; the keyed comparison and the keyed sampling decisions now take the entry's term (DM:1810-1816); the security reviewer rules on D18 | DM-R33 |
| C23 Step 1 | **Closed** in the design: the marks' order is D3's, D16's, T6's, and T9's (above), confirmed against `mark_groups.py` (`none` holds, `any` loses) | C27 |
| C24 DM-R30 | **Closed** (above), `calibration.md:18` included | |
| C25 Step 5 | **Closed** for DM-R31 as named, with T4's planted term index, key filter, structure, metadata, and whole-image cases (DM:2203); widened by C28 | C28 |
| C26 Calibration | **Closed**: the row added to `calibration.md` in this re-check | |
| C17 Done criteria | **Open, the lead's**: "none set yet" (ST:20-21); the ledger proposes K2 and K3 with DM-R29's case and `lead5` (ST:233-234) | C17 |

The runs, mine (each one thread at `nice -n 19`, one at a time; times as the runs printed):

| Run | Scope | Depth | States | Time | Result | File |
|---|---|---:|---:|---:|---|---|
| `mark_groups.py`, two groups, `any` | One mark per turn | 7 | 146,563 | 7 s | safety lost in 7 steps, no OS crash | `runs/mg-any-mpt1-d9.out` |
| The same, a clean close between mark turns | | 6 | 32,286 | 1 s | lost in 6 steps | `runs/mg-any-mpt1-close-d9.out` |
| Two groups, `none` (D3 rule 2), a clean close between mark turns | | 9 | 2,716,835 | 146 s | holds | `runs/mg-none-mpt1-close-d9.out` |
| The same, segment switches between mark turns | | 9 | 2,751,883 | 148 s | holds | `runs/mg-none-mpt1-close-switch-d9.out` |
| Three groups: `any`; `none`; `none` with switches | | 6; 8; 8 | 83,203; 2,233,453; 2,244,878 | 4 s; 121 s; 119 s | lost in 6 steps; holds; holds | `runs/mg3-*-mpt1-close*-d8.out` |
| Three groups, two marks then one: `none`; `any` (new) | | 8; 6 | 2,237,109; 83,227 | 119 s; 3 s | holds; lost in 6 steps | `runs/mg3-*-mpt2-close-d8.out` |
| Two groups, `any`, segment switches between mark turns (new) | | 6 | 32,519 | 1 s | lost in 6 steps | `runs/mg-any-mpt1-close-switch-d9.out` |
| `own_reclaim_forced.py` | Two groups, `own` against `none`: g1's checkpoint in segment 2, g0's write torn, both forgetful | 9 steps | | <1 s | `own` frees segment 1 after g0's mark turn while g1's mark is pending; `none` keeps it | `runs/own_reclaim_forced.out` |
| `forgetful_check` / `forgetful_model.py`, `runs_r4.sh lead5` | `lead5`, `heard5l`, `lead5_loss`, each barrier | 13 to 14 | | 3 s | byte for byte `results/r4/schedules-lead5.out` | `runs/repro/schedules-lead5.out` |
| `term_index_check5` | Check 5 on one dictionary of 200 terms; five cases | | | <1 s | the honest one and a widened alphabet pass; the planted index, a planted index key, and a 40-entry re-cut fail | `plant/term_index_check5.out` |
| `dead_stats_plant` | strata `b7c6c78`: four live documents, three phantom ones dropped before the flush (A) or flushed and then deleted (B) | | | <1 s | F21 digest equal; doc_count 4 against 7, total length 8 against 14, df(alpha) 1 against 4; "alpha OR beta" ranks docid 1 first in A and outside the top three in B | `plant/dead_stats_plant.out` |

### New findings

1. **DM-R33. A merge output's dead ordinals reach ranking statistics, and no install check covers
   them** (D18 checks 2, 3, and 5, whole images, Q9, the threat row, the fault table, T4).
   - A merge carries the documents deleted while it ran into its output as dead ordinals
     (`install_merge`, `index.hh:1386-1414`); a flush drops them (`index.hh:1931-1945`). F21's
     records are those of live ordinals only (`index.hh:667-674`; `reader.hh:1894-1901`), and check
     3 compares a merge output's live records with its inputs' live records "under the `Image`'s
     bitmap" (DM:1861-1867), precisely so it needs no merge-start position. Ranking reads every
     ordinal, dead ones included: the snapshot's document count and total length sum each segment's
     (`searcher.hh:92-101`, "deleted ones included (D13)"), document frequency counts deleted
     documents (`searcher.hh:118-121`), and BM25's IDF and average length come from them
     (`search.hh:72-96`). Check 5 checks the statistics against the segment's own walk
     (DM:1892-1895), which walks the dead ordinals too, so it agrees with whatever they hold; check
     2 cannot tell a document the leader carried dead from one it planted dead without the merge's
     start, which check 3 avoids needing.
   - So a leader that adds dead ordinals to a merge output, or alters a carried one, passes checks
     1 to 5 and shifts IDF and average length for terms it chooses. `r4/plant/dead_stats_plant.cc`
     against strata at this commit: index A holds four live documents (docid 1 with `alpha`,
     docids 2 to 4 with `beta`); index B the same, plus three `alpha` documents flushed and then
     deleted, the dead ordinals a planted merge output would carry. Their F21 digests are equal
     (`40e06b889d9fd59d`: equal live records, so equal keyed sums under any key); doc_count is 4
     and 7, total length 8 and 14, df(`alpha`) 1 and 4; "alpha OR beta" ranks docid 1 first in A
     (0.5473 against 0.1621) and outside the top three in B (`plant/dead_stats_plant.out`). An
     honest leader-side merge fault in a carried document's postings is missed the same way. It
     buries a document; it hides none from a match, since a dead ordinal never matches and only
     raises a pruning bound.
   - The same holds for a mode switch and D8's catch-up reinstall (DM:1959-1962): the follower
     compares the keyed sum of its own image's records with the leader's image's, live records
     both, and its own image's dead set differs honestly from the leader's, since their merges ran
     at different times. And D18's "Within a segment it leaves nothing a reader consults unchecked"
     (DM:1935), Q9's "none missed in a segment, records or structures" and "catches every honest
     fault in a segment at its first install" (DM:2263, 2286-2287), the threat row (DM:2058),
     the fault table (DM:2081), and the summary (DM:310-312) overstate it.
   - Fix, in D18, Q9, the threat row, the fault table, and T4, before step 5's verifier: check 3
     covers every ordinal of a merge output. Each dead ordinal's `D` record names a document of the
     inputs, by docid and version, that is dead at the `Image`'s position, at most once, and the
     keyed sum of the records of every output ordinal, dead ones included, equals the keyed sum of
     the inputs' records of the documents the output holds (its live ones, and the dead ones it
     names), the inputs' dead records walked as their live ones are. The check keeps the docids the
     output names dead while it runs, 8 bytes per carried document. For a mode switch or a catch-up
     reinstall, each dead ordinal of the leader's image is either a document version the follower's
     own image holds, with equal records, or counted and its residual stated. T4 gains a mutant
     leader that adds dead ordinals to a merge output (`dead_stats_plant.cc`'s shape) and one that
     alters a carried document's postings, each caught at its first install. Or the design states
     the residual in each of those places: a compromised leader, or a leader-side merge fault, can
     shift ranking statistics through dead ordinals, undetected, burying documents without hiding
     them from a match.

2. **DM-R34. A member whose own whole image was installed unvouched still vouches, so two such
   members can vouch for each other** (D18's whole images, D17's `Vouch`, T4).
   - A member answers `Vouch` yes "when it is a segment-mode follower whose log holds the same entry
     ... at or below the position through which its own install checks passed" (DM:1966-1970;
     D17 DM:1737-1742). A receiver that installed a whole image from the leader alone counts
     `install_unvouched` and "serves no whole image to another receiver meanwhile" (DM:1974-1978),
     but nothing stops it from vouching: its later installs pass checks 1 to 5 against that base, so
     the position through which its checks passed moves past entries whose content it never checked.
   - With a compromised leader and its other followers unreachable (DM:1974-1975 names that
     case), receiver N1 installs a planted image unvouched; later N2 joins, cannot fetch from N1
     (which serves nothing yet), installs the leader's image, and asks N1, which answers yes, so N2
     counts nothing; N1 then asks N2, which answers yes, and `install_unvouched` clears on both.
     Every check the design names passes, and the planted content is now "vouched". This is the
     rule's completeness, not the threat model's; the security reviewer reviews D18 beside it.
   - Fix, in D18's whole images, D17's `Vouch`, and T4: a member answers yes only for entries whose
     content its own checks covered from a base it built itself or installed vouched; while its own
     whole image is unvouched it answers not yet. T4 gains the case: two members that each installed
     the leader's image alone neither vouch for each other nor clear `install_unvouched`.

3. **DM-R35. Precision** (D3, the summary, D18).
   1. D3's "why no slice" says the per-group filter "buys no reclamation, since a segment leaves only
      when none of its records is live (`turn_log.h:380`), so a slice that skips an unmarked group's
      records cannot free their segment before that group is marked anyway" (DM:648-652; the
      changes table, DM:97). The second half is true; the first is not: a segment whose live
      records all belong to groups already marked can leave under `own` before the last mark turn.
      `r4/own_reclaim_forced.py`: g1 checkpoints into segment 2, g0's write tears, both groups
      recover forgetful; under `own` g0's mark turn copies g0's segment-1 record and the barrier
      frees segment 1 while g1's mark is pending; under `none` segment 1 stays until the marks are
      done (`runs/own_reclaim_forced.out`). The choice stands on the bound D3 already states: while
      the marks are pending the core admits nothing else, so `none` holds back at most the marks'
      own bytes past `retain_segments` (DM:655-662). The paragraph says that instead.
   2. The revision's summary says the owner's rule loses in 13 steps "and revision 0's rule in 14
      (`heard5l`)" (DM:72-75). Revision 0's rule as written (`timeout`, `FM:60`) loses `lead5` in 13
      under every barrier (`schedules-lead5.out:16-29` and its `candidate` and `all` blocks);
      `heard5l` shows its every-voter-heard condition alone (`heard`, `FM:59`). D6 (DM:968-969) and
      the model table (DM:1066) state it right.
   3. Check 5's "Skip data and competitive pairs" names each skip entry's pairs (DM:1896-1900). A
      term's dictionary value carries more of the leader's bytes (`format.hh:442-455`): its
      whole-list pairs, stored for terms of 128 postings or more, which bound the term in MaxScore
      (`search.hh:317`, `1028`) and serve as the bound past its last skip block
      (`search.hh:1228-1232`; `reader.hh:1249-1256`); its collection frequency; a single posting's
      wdf; its flags; and its postings' and positions' lengths. Check 5 re-encodes the values as
      received, so byte equality does not check them. Name each with what it is checked against:
      the whole-list pairs equal those computed from the walked postings and norms, the collection
      frequency the walked sum of wdf, and the rest equal what the walk decoded through them. A
      planted low whole-list pair would let pruning pass a matching document; T4's structure mutant
      includes one.

### New blocking items

None blocks a start. Two block landings:

1. DM-R33: check 3 covers a merge output's dead ordinals (or the residual is stated everywhere
   option 3's claim is made), in D18, Q9, the threat row, the fault table, and T4. Blocks step 5's
   verifier from landing (C28).
2. DM-R34: no `Vouch` yes from a member whose own whole image is unvouched, in D18, D17, and T4.
   Blocks step 5's whole-image rule from landing (C28).

### Conditions

1. **C27. Step 1** (supersedes C23): approved. Its turn driver and turn log land with
   `hold_forward` as D3 rule 2 and D16 state it, and its code review runs T9's DM-R29 case and T6's
   mutant against the journal's code (the marks across two turns and across a segment switch; a
   slice in a mark turn caught), with `mark_groups.py --ride any` among T9's mutants (DM:2208).
2. **C28. Step 5** (widens C25): D18's verifier lands only with DM-R33's dead-ordinal coverage in
   check 3 and the whole-image rule, or the residual stated where option 3's claim is made, and with
   DM-R34's vouching rule; T4 carries the dead-ordinal, carried-document, and mutual-vouch cases and
   DM-R35 item 3's whole-list pair. Q9's recommendation is restated on that basis before the owner
   answers it. The lead routes DM-R33 and DM-R34 to the security reviewer's review of D18.
3. **C29. Calibration**, for the lead, a row on my own fix: "2026-10-19 | DM-R31's fix in the
   re-check of revision 3 | It listed the structures a query reads beside F21's records and
   missed the dead ordinals a merge output carries, which every ranking statistic counts and no
   check covers (DM-R33) | The re-check of revision 4, `dead_stats_plant.cc` | For every check that
   compares live content, list what the reader computes from the dead content beside it
   (statistics, bounds, counts), and construct the output an honest writer may legitimately differ
   in (a merge's carried deletions)".
4. **C17** stays open (the lead's): K2 carries DM-R24's T9 rows and DM-R29's case; K3 carries the
   mutants the ledger lists (ST:233-234), DM-R29's, and `fplus` killed by `lead5`.

Not run: no benchmark, timing, or A/B (Q9's and D18's CPU figures are arithmetic over the design's
labeled assumptions; no SipHash, EVP, segment-walk, check-5, TLS, or network rate measured); no
strata, consensus, or journal suite (the branch changes no library code; one strata probe was built
against `b7c6c78` with its pins); no simulator (unbuilt); no Linux, FreeBSD, or arm64; no fuzzing;
the designer's deepest searches not repeated (the journal model at depth 11 with 64-bit keys, the
window searches from `six`'s step 37 and `grow67`'s step 65, the five- and three-voter restart
searches to depths 7 and 11), and the four searches from `lead5`'s step 9 compared with my
revision-3 runs rather than rerun; `mark_groups.py` to depth 9 with two groups and 8 with three, two
entries per group, at most two marks per turn, reclamation of whole segments, no sector subsets
within a turn, and Python's full-state tables; `own_reclaim.py`'s search only to depth 7 with three
groups (no witness there; the forced schedule is the evidence); check 5 run for one dictionary
against termdict alone, with no strata segment written, the key filter, norms, statistics, skip
entries, and pairs argued from the code; the dead-ordinal plant shown as an honest flush whose
documents were deleted after it, its merge-output form argued from `install_merge` and D18's check
3; no forgery through strata's writer; `Vouch`'s chain argued from D18's text, not modeled; the
threat model of D18 and `Vouch` left to the security reviewer; no Xapian or Xapiand GPL source
opened.

## Re-check (revision 5)

- Date: 2026-10-19, read and run from 15:20 to 15:50 PDT. Scope: the architect's DM-R33 to DM-R35
  and C27 to C29, and the security reviewer's D18 review (DMS-15 to DMS-19, security C13 to C17), so
  this one re-check rules on both.
- Reviewed: `design/durability-modes` at `d8e4ddc`, `git log 3cd9f0e..d8e4ddc` (revision 5 at
  `2159dd5`, the ledger and calibration at `d8e4ddc`): `docs/design/durability-modes.md` ("Revision
  5" and "Changes in revision 5" first, then D8, D16, D17, D18, the threat model, the fault table,
  Q9, T4, and the first steps), `docs/status/durability-modes.md` (K1 to K6), `calibration.md`,
  `bench/durability/model/check3_model.py`, `vouch_model.py`, `runs_r5.sh`,
  `bench/durability/term_value_check.cc`, and every file in `results/r5/`.
- Checked against: my re-check of revision 4 above (DM-R33 to DM-R35, C27 to C29, C17); the
  security reviewer's "D18 review (revision 4)" in `durability-modes-security.md` and its probes in
  `strata-wt/review-durability-sec/.scratch/durability-sec-r4/` (`vouch_position.py`,
  `impacts_roundtrip.cc`); C1, C6, and C17 for the done criteria; strata at `d8e4ddc`, whose
  `include/` is unchanged since `b7c6c78` (`git diff b7c6c78 d8e4ddc -- include src CMakeLists.txt`
  is empty): `merge.hh:82-175`, `keys.hh:72-121`, `tuple_buffer.hh:384-405`, `840-960`,
  `writer.hh:229-345`, `600-640`, `format.hh:430-665`, `reader.hh:60-100`, `380-545`, `615-640`,
  `1133-1160`, `1739-1760`, `1885-1925`, `index.hh:455-520`, `600-700`; Kronuz/termdict `eaf83b3`
  (`r4/plant/include/termdict.hh`); `calibration.md`'s fifteen rows, applied below (C29's row asks
  for the output an honest writer may legitimately differ in, which DM-R36 is for the alphabet and
  DM-R37 for a mode switch's dead ordinals).
- Checked by:
  - `quiet check` before every launch: exit 2 throughout (a light hold, `f13q-noef3`), so every run
    and the one build used `nice -n 19`, the runs one at a time, the build three jobs. The largest
    model run peaked at 848 MB resident (`vouch-entry-e3-d13`, `r5/runs/repro/*.time`).
  - Reproduced: every `results/r5/` model run, from snapshots of the two models at `d8e4ddc`
    (`r5/check3_model-d8e4ddc.py`, sha256 `25b8e85d…`; `r5/vouch_model-d8e4ddc.py`, `6e5beb88…`) by
    `r5/runs_repro.sh`: all eight outputs equal the designer's but for timings, with the same exit
    codes (`r5/runs/repro/`).
  - Built: `r5/plant/check35_real.cc` against strata at `d8e4ddc` and its pins (`r5/plant/
    CMakeLists.txt`, the fetched sources of `strata/build/_deps`, each at `r4/pins.txt`'s commit and
    clean; Homebrew clang, RelWithDebInfo, no warning).
  - Probes, all in `strata-wt/review-durability/.scratch/review-durability/r5/`, kept with their
    outputs: `plant/check35_real.cc` (`plant/check35_real.out`): check 5's rebuilt term values and
    the four alphabet rules over segments strata's own writer and merge produced, and check 3 under
    revision 4's and revision 5's rules over real merge outputs, the planted-dead-entry probe rerun;
    `vouch_position_r5.py` (`runs/vouch_position_r5.out`): the security reviewer's schedule under
    revision 5's rule, with and without the plant removed before the later entry; `runs_repro.sh`
    and `runs/repro/`. The revision-1 to revision-4 probes are in `r1/` to `r4/`, unchanged.
  - Not run: the last line of this section.

Citations: `DM:N` is `docs/design/durability-modes.md` at `d8e4ddc`; `ST:N` is the ledger there;
strata files are `include/strata/` at `d8e4ddc`; `r5/` is `.scratch/review-durability/r5/`.

### Verdict

**APPROVE WITH CONDITIONS; ready for the full design review once revision 6 folds DM-R36 and
DM-R37 (text and T4 cases, no new model), with no further re-check before it.** Revision 5 closes
DM-R33, DM-R34, and DM-R35 and the security reviewer's DMS-15, DMS-17, DMS-18, and DMS-19, and closes
DMS-16 for every incremental install. Check 3 over every ordinal holds: over real merge outputs,
revision 4's rule passes phantom content under the deleted docids, phantom dead docids, and an
altered carried document, and revision 5's refuses all three while passing honest outputs that drop
the deleted documents or carry them (`check35_real.out`, part B). The rebuilt term value matches
every value strata's writer and merge wrote, in six segments up to a merge of a merge with a dead
ordinal (part A: 0 of 20 values differ). `Vouch` per entry from a grounded image gives the security
schedule `skipped` for the planted entry and refuses the variant that keeps the plant
(`vouch_position_r5.out`). The designer's deviation from DMS-16 is right where it applies: the
walked terms' alphabet refuses an honest merge, and in a merge of a merge so does the inputs' walked
terms' union; only the inputs' stored alphabets, which merge.hh:92-99 reads, pass. But it cannot
apply where no inputs are at hand, and every whole image runs check 5 on every segment: no rule D18
states passes an honest whole image holding such a merge output (DM-R36). And a mode switch's
unmatched dead ordinals carry records nobody checked, not records bounded as check 3's residual is.
The reinstall then grounds the image, so its member vouches for them (DM-R37). The stated merge
residual is sound.**

What holds, and reproduces:

- **Check 3 over every ordinal (DM-R33).** Every output ordinal names an input document by docid and
  version, at most once; it is dead exactly when that document is dead at the `Image`'s position;
  and the keyed sum covers every ordinal's records (DM:1959-1983). Check 1 ignores a flush's bitmap
  (DM:1938-1949), and check 2's bits for a merge follow the documents named (DM:1957-1958).
  `check35_real.out` part B: inputs are docids 1 to 9 with 5 to 7 deleted. Revision 4's rule passes
  phantom content under docids 5 to 7 (total length 22 against the inputs' 18), three phantom dead
  docids 10 to 12, and docid 5's postings altered. Revision 5's refuses each, and both refuse a live
  document marked dead. `check3_model.py` agrees over every output it enumerates. Its abstraction
  (a document's records as one value) holds, since every `N` and `P` record carries its docid
  (`reader.hh:625-640`).
- **The stated residual is sound.** Which deleted input documents a merge keeps (DM:1984-1995) is
  not checkable without the merge's start. Part B's carried output is byte for byte what an honest
  merge writes when the deletes arrive while it runs, and an index whose merge policy never merges
  those segments keeps the same statistics. It stays inside the inputs' statistics, since every
  carried ordinal must be an input document with that document's own records, so a chain of merges
  only ever shrinks the set. It does bury: docid 1 goes from first alone (0.7002) to a three-way tie
  (0.3630) when docids 5 to 7 are carried. The design says so in substance ("moves IDF and average
  length", "hides no document from a match", DM:1990-1993), and `merge_dead_carried` (DM:1993-1995)
  is the right signal. One wording slip: "for terms it chooses" means the terms of the deleted
  documents it keeps, not any term (DM-R38 item 3).
- **Check 5's rebuilt term values (DMS-16, DM-R35 item 3).** D18 builds each value from the walk as
  `writer.hh:302-334` does (DM:2031-2051) and names both skip levels and their fields
  (DM:2052-2063). `term_value_check.cc` refuses all eleven of its plants, as `results/r5` says, but
  its honest values come from its own `from_walk`, so its honest pass is circular. Part A closes
  that: values rebuilt from a walk of segments strata wrote (flushes with a single posting, counts
  unequal to wdf, positionless postings, and a 200-posting term with whole-list pairs; a merge, a
  merge of a merge, and the same with a dead ordinal) equal the received values in all 20 terms. The
  restart and `absolute` flags follow the builder's `next_is_restart()`, and the flags match what
  `merge.hh:142-151` and `tuple_buffer.hh:913-956` recompute when documents are dropped. Lengths
  were taken as received, so "the bytes the walk's decoders consumed" (DM:2039) is untested here
  (DM-R38 item 2).
- **`Vouch` per entry (DM-R34, DMS-15, DMS-17).** Grounded and unvouched are defined (DM:2140-2145).
  The answers are `yes` only inside the member's interval of entries it checked itself from a
  grounded image, `not yet` while its image is unvouched, `skipped` below the interval, and `no`
  (DM:2170-2183), with the echoed entry refused when it differs (DM:1716, 2185). A `no` never sends
  the re-fetch to its sender (DM:2186-2192). An unvouched member refuses every `Request`
  (DM:1824-1825, 2161-2163), and an unserved `Request` goes to another member (DM:1825-1827). The
  security schedule under this rule answers `skipped` for E. N is grounded at E′ by a `yes` for E′,
  whose files equal those F1 checked, the counter stays at 1, and E stays logged as never vouched,
  which is exactly the residual (DM:2193-2203). When the plant is not removed, F1 and F2 fail E′'s
  whole comparison, answer `no`, and N stays unvouched (`vouch_position_r5.out`). `vouch_model.py`
  reproduces: the rule holds over every reachable state with two entries (74,225) and to depth 13
  with three (917,782), and its three controls lose in 4, 6, and 6 steps.
- **DMS-18 and DMS-19.** The keys sit in one `mlock`ed page excluded from dumps and are zeroed at
  exit. A `getentropy` failure leaves the follower in document mode, an `mlock` failure is logged
  and counted, and no sum is logged (DM:1892-1906, 2078). `verify_failed{check}` has one series per
  check. A check-1 failure under an honest leader is a strata bug, gated by T4's honest control over
  every part shape (DM:2076-2087, T4 DM:2431), and the client row states the lever (DM:2289).
- **DM-R35 items 1 and 2.** "Why no slice" now rests on the accumulation bound and cites
  `own_reclaim_forced.py` (DM:707-725). Revision 4's summary is corrected: revision 0's rule loses
  `lead5` in 13 steps, and its every-voter-heard condition alone loses `heard5l` in 14
  (DM:131-137).

What blocks (landing, not starting): DM-R36 and DM-R37, both for step 5's verifier (C30).

### Status

| Item | Status | Where, at `d8e4ddc` |
|---|---|---|
| DM-R33 Dead ordinals | **Closed** as asked: check 3 covers every ordinal (DM:1959-1983); check 1 ignores the bitmap (DM:1938-1949); check 2 for merges (DM:1957); the residual stated in D18 (DM:1984-1995, 2112-2115), Q9 (DM:2498-2506, 2524-2533), the threat row (DM:2286), the fault table (DM:2309), and the summary (DM:368-376); T4's dead-ordinal and carried-document mutants (DM:2431); option 2's sampled merges use check 3 (DM:2231-2238); `check35_real.out` part B. My fix offered "counted and its residual stated" for a mode switch, and the residual revision 5 states there is wrong (DM-R37) | DM-R37 |
| DM-R34 Mutual vouch | **Closed**: grounded images (DM:2140-2145); `not yet` while unvouched (DM:2177-2178); T4's two-member case and its mutant (DM:2431); `vouch_model.py`'s `entry_ungrounded` control loses in 6 steps, reproduced | |
| DM-R35 Precision | **Closed**, all three: 1 DM:707-725; 2 DM:131-137; 3 DM:2031-2063, `check35_real.out` part A | DM-R38 |
| DMS-15 Vouch by position | **Closed**: per-entry interval and `skipped` (DM:1811-1819, 2170-2183); the residual (DM:2193-2203); T4 runs `vouch_position.py`'s schedule with the position rule as its mutant (DM:2431); `vouch_position_r5.out`; `vouch_model.py`'s `position` and `position_grounded` controls lose in 4 and 6 steps | |
| DMS-16 Whole-list pairs and `cf` | **Partially closed**: rebuilt values (DM:2031-2051), both skip levels (DM:2052-2063), `cf` unread today (DM:2047-2048), T4's plants (DM:2431); the writer's alphabet is right for every incremental install (DM:2003-2011), and the deviation from DMS-16's "from the walked keys" is sound and needed (below); open for whole images (DM-R36) | DM-R36 |
| DMS-17 Wire and liveness | **Closed** in the design: the echo (D16 DM:1716, D17 DM:1811-1819, D18 DM:2185); `no` routing (DM:2186-2192); unvouched serves nothing (DM:1824-1825, 2161-2163); the second source (DM:1825-1827); step 8 carries them (DM:2591-2594, step 8's row) | |
| DMS-18 Key hygiene | **Closed** (DM:1892-1906, 2078) | |
| DMS-19 Check 1 as a lever | **Closed** (DM:2076-2087, 2289; T4's honest control DM:2431). T4's honest control should also cover DM-R36's whole images | DM-R36 |
| Security C13, C15, C16, C17 | **Closed** as the rows above; C15 binds step 8's encoding as ruled | |
| Security C14 | **Partially closed** (DMS-16 above) | DM-R36 |
| C27 Step 1 | **Holds** as ruled; the first steps carry it (DM:2582-2587) | |
| C28 Step 5 | **Closed** for DM-R33, DM-R34, and DM-R35 item 3, and Q9 restated (DM:2524-2533); widened by C30 | C30 |
| C29 Calibration | **Closed**: `calibration.md:22` (`d8e4ddc`), the row as worded | |
| C17 Done criteria | **Closed** for what C1, C6, and C17 asked, below | C31 |

**The done criteria against C1, C6, and C17** (ST:23-32):

- C1 asked for six criteria. K1 is step 2 on Linux, macOS, and FreeBSD with Detent's suite, K2 is
  step 1 with T9's rows (DM-R2's and DM-R6's), K3 is step 3 with T6's mutants (DM-R1's and DM-R5's)
  and T12, K4 is the three sanitizers and coverage, K5 is CI and Detent's suite at each library
  commit, and K6 is step 4 after the full review and Q8. All six are carried, word for word in
  substance.
- C6 asked for DM-R19's and DM-R20's mutants in K3: carried.
- C17 asked for DM-R24's T9 rows in K2 (carried, with DM-R29's two-group case) and for DM-R24's and
  DM-R26's mutants in K3. K3 names DM-R1's, DM-R5's, DM-R19's, DM-R20's, and DM-R29's and says
  "T6's mutants", whose row names DM-R24's (the first-turn mark controls) and DM-R26's (`manual`
  deciding on the timeout, `six`) at DM:2433. They are carried, but by reference only.
- Two placements are off, though neither drops anything. DM-R29's mutant is a journal mutant, step
  1's, which C27 asks step 1's review to run, yet K3 lists it under step 3; K2's "T9's rows" carries
  it already through `mark_groups.py --ride any` (DM:2436). And step 5 has no criterion of its own:
  K3's tail names "revision 5's T4 mutants" and nothing of C18's forging mutant, C25's DM-R31
  structure plants, the honest control (security C17), or step 8's controls (security C15), and no
  criterion carries C4's matrix of both replication modes. These are C31, for the lead.

### New findings

1. **DM-R36. Check 5's alphabet rule cannot run on a whole image, and its merge rule needs the
   inputs' stored alphabets, not their bytes** (D18 check 5, whole images, T4).
   - The deviation from DMS-16 is right for an incremental install. `merge.hh:92-99` builds a merge
     output's term alphabet from its inputs' stored `alphabet_bytes()`. Each stored alphabet passed
     its own segment's check 5 under its writer's rule, so by induction the union is the bytes of
     every term in every flush the output descends from. That is a function of content the
     follower checked, not of the leader's bytes, and no plant reopens: an alphabet sets only the
     codes, and `r4/plant/term_index_check5.out` shows a widened one reads the same.
     `check35_real.out` part A, over strata's own segments:
     - Deleting the only document with `zebra` and merging gives M1, stored alphabet
       `abceglmnoprstz`, walked terms `abceglmnopst`. DMS-16's walked rule refuses it; the inputs'
       union passes it.
     - Merging M1 with a new flush gives M2, stored `abcdeglmnoprstz`. The walked rule and the
       inputs' walked terms' union both refuse it, and only the inputs' stored alphabets pass it.
     So D18's "every byte its inputs' term dictionaries use" (DM:2009) has to say "the union of the
     inputs' stored alphabets, each verified at its own install", since the reading "the bytes the
     inputs' terms use" refuses honest merges of merges.
   - A whole image has no inputs at hand. Every whole image runs check 5 on every segment: the
     mode-switch and catch-up reinstalls (DM:2148-2149), a new member or a repair from a grounded
     member (DM:2163-2164), and one from the leader alone (DM:2195). The segments' inputs are
     merged away, or were never held. The only rule left that a follower can compute is the walked
     terms' alphabet (DM:2004-2008, which forbids "the one the received index names"), and it
     refuses M1 and M2. Any honest image with a merge output whose dropped documents held a byte no
     surviving term uses therefore fails check 5 on install. The follower then falls to document
     mode on a reinstall, and a new member never installs. Merges that drop documents are routine,
     so this is the common case, not an edge.
   - Fix, in D18's check 5 and T4, before step 5's verifier: for a segment whose inputs the follower
     does not hold, the term dictionary's alphabet is the received one, provided it contains every
     walked term's byte. Re-encoding under it must still match byte for byte, which rejects a
     non-canonical alphabet. State why that is safe: the alphabet only sets codes, and a superset
     reads the same (`term_index_check5.out`). DM:2009 says "stored alphabets". T4's honest control
     gains whole images, from a member and from the leader, holding a merge of a merge whose dropped
     documents took a byte. One rule everywhere (received, containing the walked bytes) is also
     sound and simpler, at the cost of the "function of the walk alone" property security C14 asked
     for. The security reviewer should rule on which.

2. **DM-R37. A mode switch's unmatched dead ordinals carry unchecked records, and the reinstall
   grounds them** (D18 whole images, Q9, the summary, T4).
   - At a mode switch from document mode, a leader dead ordinal that matches neither the
     follower's image nor its log is "counted (`reinstall_dead_unmatched`) and logged at warning
     level, and carries records the follower cannot check, which move ranking statistics as check
     3's residual does, by at most that count of documents. The reinstall grounds the image"
     (DM:2154-2158). Check 3's residual is bounded because every carried ordinal must be an input
     document with its own records (DM:1984-1993). An unmatched ordinal has no such anchor. Its
     length, terms, wdf, and positions are whatever the leader wrote: one dead ordinal of length
     2³² − 1 moves every query's average length, and N of them raise any chosen term's df by N.
     That is `dead_stats_plant.cc`'s phantom shape, which check 3 now refuses everywhere except
     here. The count has no bound but the docids below the follower's log base that its own merges
     dropped.
   - Grounding launders it. The member that compared at the switch answers `yes` for that entry
     ("compared whole against its own image there", DM:2170-2172). So new members fetch from it or
     are vouched by it, grounded, with records no member checked. Check 3 lets every later merge
     carry those dead documents onward. Q9's residual calls this "counted" (DM:2505-2506) and its
     recommendation "a bounded move of ranking statistics" (DM:2531-2532). The summary (DM:374-376)
     and the threat row (DM:2286) omit it.
   - Fix, preferred, with no residual: make the switch match every dead ordinal, as catch-up does.
     1. From the switch command's log position, the follower merges none of the segments it then
        holds (D8's catch-up rule, DM:1445-1447, applied to the switch).
     2. The leader's first `Image` after the switch carries no dead ordinal of a document deleted
        before that position: it expunges those deletes first, one merge per segment holding
        deletes, at a switch D15 already marks heavy (DM:1689).
     Every dead ordinal of that `Image` is then a document live at the command, so it is in the
     follower's unmerged segments or its log, and an unmatched one fails the comparison like any
     other mismatch. Alternatively, state the residual exactly where the claim is made: the
     unmatched ordinals' records are the leader's, unbounded per document, and a member whose
     switch counted any answers `skipped` for that entry and serves no file until a later entry's
     checks ground it. T4 gains a mode switch whose leader plants phantom dead ordinals: refused
     under the preferred fix, and counted and never vouched under the alternative.
   - This is a miss in my own DM-R33 fix, which offered "counted and its residual stated" without
     asking whether the counted records were checked or what grounding then did with them (C32).

3. **DM-R38. Precision** (D8, D18).
   1. Catch-up's "every dead ordinal matches one or the other" (DM:2152-2154) also needs the
      follower's log to keep every write since its last installed image during catch-up. That holds
      only if the checkpoint ceiling (DM:1428-1429) bars checkpointing the follower's own
      catch-up segments. D8 should say so in the catch-up bullet (DM:1438-1447).
   2. "The bytes the walk's decoders consumed" (DM:2039) needs the decoders to report their end:
      `postings_iterator` bounds its reads at `info.postings_length` and does not expose where it
      stopped (`reader.hh:1145-1147`). Step 5 adds that accessor, and T4's "postings length longer
      than the walk consumed" plant runs in a segment, not only in `term_value_check.cc`.
   3. "Moves IDF and average length for terms it chooses" (DM:1992) should read "for the terms of
      the deleted documents it keeps".

### New blocking items

None blocks a start or the full design review once revision 6 folds them. Two block a landing:

1. DM-R36: check 5's alphabet rule for segments whose inputs the follower does not hold, and the
   stored-alphabet wording; T4's whole-image honest control. Blocks step 5's verifier (C30).
2. DM-R37: a mode switch matches every dead ordinal (or states the unbounded residual and withholds
   `yes`), in D18, Q9, the summary, the threat row, and T4. Blocks step 5's verifier (C30).

### Conditions

1. **C30. Step 5** (widens C28): D18's verifier lands only with DM-R36's whole-image alphabet rule
   and DM-R37's mode-switch rule, with T4's whole-image honest control and the planted mode-switch
   mutant. Q9's recommendation states the mode switch as revision 6 rules it before the owner
   answers. The lead routes DM-R36's choice of rule to the security reviewer (security C14).
2. **C31. Done criteria**, for the lead:
   - K2 names DM-R29's mutant (`mark_groups.py --ride any`) and K3 drops it.
   - A K7 for step 5: T4 as revision 6 writes it, every mutant and control, C18's forging mutant
     and C25's structure plants included, DM-R33's carried-document case, DM-R36's and DM-R37's
     cases, and the honest control over every part shape.
   - A K8 for step 8: DMS-17's echo, routing, refusal, and second source as T12 fixtures.
   - C4's matrix (T3 to T5, T8, T11, and T13 in both replication modes) in K3's or K7's text.
3. **C32. Calibration**, for the lead, a row on my own fix: "2026-10-19 | DM-R33's fix in the
   re-check of revision 4 | For a mode switch it allowed unmatched dead ordinals to be 'counted
   and the residual stated', and revision 5 stated it as bounded like check 3's, while their
   records are unchecked and the reinstall grounds and vouches for them (DM-R37) | The re-check
   of revision 5 | For every residual that is counted rather than refused, say whether the counted
   content was checked against anything, and trace every later rule (grounding, vouching, serving,
   merging) that treats it as checked".
4. C27 holds for step 1; security C15 binds step 8; K6 keeps step 4 behind the full design review.

Not run: no benchmark, timing, or A/B (Q9's and D18's CPU figures stay arithmetic over labeled
assumptions; no SipHash, walk, check-5, TLS, or network rate measured); no strata, consensus, or
journal suite (the range changes no library code; one strata probe built against `d8e4ddc` and its
pins); no simulator (unbuilt); no Linux, FreeBSD, or arm64; no fuzzing; `vouch_model.py` not taken
past the designer's bounds (two entries exhaustively, three to depth 13), and no model of a mode
switch or of whole images holding merge outputs (DM-R36 and DM-R37 are argued from D18's text and
shown on strata's segments in `check35_real.out` part A, not in a vouch or install model); check 3
run over real segments with xxh64 under a fixed seed in place of SipHash (the rule's logic, not the
PRF); check 5's rebuilt values run for 20 terms of six segments with the regions' lengths taken as
received, and the skip levels' fields, the key filter, norms, and statistics argued from the code;
no keyed dictionary from a keyed index (every probe index keyless); `term_value_check.cc` and
`impacts_roundtrip.cc` not rebuilt (`results/r5/term_value_check.out` read, and its plants
superseded by part A's real values for the honest side); no Xapian or Xapiand GPL source opened.

## Full design review (revision 6)

- Date: 2026-10-19, read and run from 15:55 to 17:14 PDT. The review the first steps' landing waits
  on (K6, and every "nothing lands before the full design review clears").
- Reviewed: `design/durability-modes` at `4d5acb8` (revision 6; `git log 8b87d1b..4d5acb8`: the
  ledger and calibration at `8d4ea22`, the design at `4d5acb8`), read whole: the design end to end,
  the ledger (K1 to K9, the owner's answers, the lead's calls on Q9 and Q10), every section of this
  file and of `durability-modes-security.md`, `calibration.md`'s sixteen rows, and `PLAN.md`. The
  models in `bench/durability/model/` and their results, snapshotted with sha256 in
  `r6/model/models-4d5acb8.sha256` (`forgetful_check.cc` `75e02353…`, identical to my `r3/`
  snapshot; `check3_model.py` `25b8e85d…` and `vouch_model.py` `6e5beb88…`, revision 5's;
  `mark_groups.py` `6d0b4fb7…`, revision 4's); `bench/` is unchanged since `2159dd5`. `origin`
  moved to `ca70579` during this review: the security reviewer's full design review of revision 6,
  which changes no design text and which I read (DMS-20 to DMS-24, security C18 to C22).
- Checked against: strata `main` `d058cd9` (`git diff 026ee10 d058cd9`: `index.hh`, `files.hh`,
  `faultfs.hh`, `CMakeLists.txt`, `format-13.md`; `test/directory_test.cc:512-530`); completion
  `20a1498` (`main`'s pin) and `5251cf9` (the branch's); consensus `61149a2` (`core.h:439-500`,
  `types.h:49-54`); journal `dbf753f`; strata `include/` at `4d5acb8`, unchanged since `b7c6c78`
  (`index.hh:276-286`, `500-520`, `715-745`, `2184-2245`; `keys.hh:60-125`; `merge.hh:80-110`;
  `tuple_buffer.hh:380-410`; `reader.hh:225-300`, `380-420`); Kronuz/termdict `eaf83b3`
  (`termdict.hh:60-215`); `track-c.md` D9 (`:389-415`); `design/peer-transport` at `38a6c6f`
  (revision 2, awaiting its re-check); `perf/small-indexes`' design (10,000 indexes open per process,
  its T1 idle heap); format 13's F20 ruling on `main`. Consensus, journal, completion, hashes,
  random, and threadpool: each `HEAD` equals its `origin`. The calibration rows' checks, applied
  where they bite: row 4 (every
  ruling addressed to this design: C1 to C32 and security C1 to C22, in the status table below),
  row 5 (`origin` past the reviewed commit, named above), row 7 (DM-R43's mechanism found in code),
  rows 11 and 12 (the window searches with restarts), 13 (the mark through a consensus install;
  DM-R43's generation through retention), 14 (two and three groups), 15 (every output an honest
  switch may hold, `switch_model.py`), and 16 (every grounding traced to a check of its content:
  DM-R40, DM-R41).
- Checked by:
  - `quiet check` before every build and launch (`r6/runs/quiet.log`): exit 0, or 2 under another
    session's light holds, never 1; every build and run at `nice -n 19`, at most three runs at
    once, each under `r6/run_capped.sh`'s wall-clock and resident-memory caps (peaks in each
    `.time` file; the largest 5.7 GB, `mark_model.py` at depth 11). Swap stood near 17 of 18 GB
    used.
  - Builds, Homebrew clang 23.1.2: `forgetful_check` from `4d5acb8` (`-O2 -Wall -Wextra`, no
    warning); `r6/plant/alphabet_whole` against strata `4d5acb8` and its pins (the fetched sources
    of `strata/build/_deps`, each at its pin and clean), RelWithDebInfo, no warning.
  - The decisive searches rerun at the design's stated depths, from this commit's models
    (`r6/runs_arch_r6.sh`): every count equals the design's (table below). Every forced schedule,
    both checkers, each barrier mode: `schedules.out` and `schedules-lead5.out` byte for byte
    (`r6/runs/forced/`). The journal model's four controls (`r6/runs/markctl/`).
  - New shapes, one per area (`r6/runs_new_r6.sh`, `r6/runs_win_r6.sh`): the window searches with
    same-boot restarts and torn tails; `r6/model/switch_model.py` (DM-R37's switch and catch-up,
    exhaustive); `r6/model/vouch_model_r6.py` (catch-up open to new members; a `verify` switch);
    `r6/plant/alphabet_whole.cc` (DM-R36 over strata's own segments); `r6/cite/map_citations.py`
    (every `index.hh` and `format-13.md` citation mapped to `main`).
  - Probes, all kept in `strata-wt/review-durability/.scratch/review-durability/r6/` with their
    outputs; `r1/` to `r5/` unchanged.
  - Not run: the last line of this section.

Citations: `DM:N` is `docs/design/durability-modes.md` at `4d5acb8`; `ST:N` is the ledger there;
strata files are `include/strata/` at `4d5acb8` unless "at `main`"; `r6/` is
`.scratch/review-durability/r6/`.

### Verdict

**APPROVE WITH CONDITIONS. The formats, the wire, D1 to D7, D6's recovery rule, and D17 clear:
steps 1, 2, 3, 4, 6, 7, 8, and 9 may land under their conditions and outside dependencies, and
step 5's checkpoints, `Create`, apply, `Image`, incremental install, catch-up, and levels may land
under C34 and C35. Step 5's verifier for whole images does not land: as written it grounds a member
whose image was never checked (DM-R40), it has no rule for an existing group's switch to `install`
(DM-R41), and the mode switch's text admits two readings that refuse an honest switch and leaves the
comparison's point unstated (DM-R42). Those three are revision 7's, with a design-only re-check
(C33), beside the security reviewer's DMS-20 to DMS-24 (`ca70579`, after the reviewed commit); its
ruling on DM-R36, one alphabet rule everywhere, I concur with.**

What holds, and reproduces:

- **D6's recovery rule (the f + 1 start, the majority of the candidate's membership at the round's
  own term).** Every search the design rests on reproduces state for state at its stated depth:
  three voters with two writes, the mark, torn tails, and retried rounds to depth 11 (54,757,724
  states); five voters to depth 7 (17,649,656); rf = 5's
  window from `six`'s step 37 to depth 6 under the majority count (84,841,965), where f + 1 loses at
  6 steps; `manual`'s f + 1 admission with every voter answering (65,219,423); rf = 5 to 7 from
  `grow67`'s step 65 to depth 5 (131,291,763), where f + 1 loses at 5; and `lead5`'s step 9 to depth
  6, where `round` holds (29,609,357 and 26,856,533 states under the candidate's and every barrier)
  and `fplus` loses at 4. Every forced schedule (`dmr1`, `restart3`, `torn3`, `six`, `six_loss`,
  `grow67`, `grow67_loss`, `six_heard`, `grow67_heard`, `fplus5`, `heard5`, `fplus5b`, `live3`,
  `lead5`, `lead5_loss`, `heard5l`) replays byte for byte in both checkers under each barrier mode.
  The argument (DM:1028-1059) reads as at revision 3 and is still the one the windows need. The new
  shape, the windows with same-boot restarts and torn tails, which no window search had, holds: under the majority
  count from `six`'s step 38 to depth 5 (12,953,406 states, where f + 1 loses at 5) and to depth 6
  (115,518,124 states), and from step 37 to depth 5 under both counts (21,689,738 states; f + 1's
  loss needs 6 there).
- **The persisted marks and no slice before the last mark turn (D3, D4).** `mark_model.py` holds to
  depth 11 (5,111,474 states, 64-bit keys); `mark_groups.py` with `--ride none` holds with two groups
  to depth 9 (2,716,835) and three to depth 8 (2,233,453), and `--ride any` loses in 6 steps with
  each (32,286 and 83,203 states): every count the design's. The journal model's controls reproduce
  the design's table: rule (c) alone loses in 6 steps (10,133 states), newest by journal order in 7
  (34,543), the open record left reclaimable loses precision in 4
  (598,427), and an unreadable boot identity read as this boot's loses in 5 (1,668). A consensus
  install keeps the mark by construction once it is a hard-state field: the install path starts
  from `auto next_hard = durable_` (`core.h:497`) and publishes it (`:454`); C38 asks for the T6
  mutant all the same.
- **Failed checkpoints and `Create` replay (D1).** A failed checkpoint fences the index, restores
  it at its recorded generation from the intact journal, and leaves the voter as it was; a failed
  journal barrier stays D5's. The new shape is a crash after a checkpoint succeeded but before its
  record's barrier: it holds today only through a retention default the design never names
  (DM-R43). The first-`Create` rule holds across a checkpoint record's supersession: replay starts
  after the newest checkpoint record of the index, so every `Create` it replays follows that record
  or the creation record, and none is taken for a creation.
- **DM-R37's mechanism.** Under the strict readings (the ceiling below the command, the output of
  a merge in flight at the command held), every document a whole comparison needs is on an honest
  follower: the switch over 6 operations and 2 keys (16,241,982 cases), 5 and 3 (11,083,575), 5 and
  2 with four segments (1,769,706); catch-up over 6 and 2 (6,403,536) and 5 and 3 (4,319,922); and
  the expunge, the frozen segments, and the ceiling are each needed (`r6/runs/sw-*.out`).
- **DM-R36's whole-image alphabet rule.** Over strata's own segments at this commit, every flush, M1
  (a merge whose dropped document took 'z'), and M2 (a merge of a merge) passes it byte for byte, and
  the incremental rules pass too; a planting leader's widened alphabet passes and every lookup
  returns its walked value (a term the walk lacks is not found; the index grows by one byte), while a
  narrowed, an unordered, and a repeated alphabet are refused (`r6/plant/alphabet_whole.out`).
  termdict's own parser refuses an alphabet that is not strictly ascending at open
  (`termdict.hh:197-213`), so that clause of the rule costs nothing. This agrees with the security
  reviewer's ruling (`ca70579`, DMS-20: one rule everywhere, the received alphabet containing the
  walked bytes, re-encoded byte for byte), which I concur with: the exact incremental rule refuses
  only alphabets that read the same, and one rule is one implementation.
- **D18's models.** `check3_model.py` passes no phantom or altered record and refuses no honest
  output over 4,743,860 outputs (two inputs, four ordinals) and 12,904,310 (three and three);
  `vouch_model.py`'s
  rule holds over every reachable state with two entries (74,225) and to depth 13 with three
  (917,782), and its controls lose in 4, 6, and 6 steps: the design's counts. What the vouch model
  leaves out is DM-R40.
- **The gate rows.** Each has a procedure that can be rerun and a target; every ratio names its
  base (a share of document mode's CPU, or a p99 against a named run); every cost is a measurement
  with its raw file or labeled an estimate. Where a row falls short: T1 (DM-R39), T2 and T13
  (DM-R45), T4 and T10 (DM-R47), T14 (DM-R44), and T16 (DM-R43).
- **The wire and formats (D16).** The sizes, value rules, and "For Detent"'s increments agree (kinds
  1 +8, 2 +8, 3 +32, 4 +16, 6 +8, records 9 and 10 +16, `Begin` +52, `Vouch` 184 and 185); no
  finding below changes a field, a size, or a magic.
- **Owner goals.** Choose-your-side holds on one mechanism: D10's levels and presets with floors
  and per-request overrides, D12's side logs merged by one command on the one log, D13's rejection,
  and one byte mover (D17). The engine path loses nothing: D1 takes the checkpoint off the
  acknowledgement path through small-indexes' one batcher (a plain index's `commit()` becomes its
  entry, one implementation), D3 shares one barrier per turn, and nothing else on a plain index's
  path changes. For engine speed first: steps 2 (the cross-IO directory barrier, SI-R10) and 9, and
  D1's batcher through small-indexes, serve the engine's many-index rows now; steps 3 to 6 and 8 are
  Phase 3R's and block nothing on the engine path, so they can wait for spare capacity (the lead's
  sequencing). Where the design adds per-group state and handles environment risks, DM-R44 and
  DM-R46 apply.

What blocks: DM-R40, DM-R41, and DM-R42 block step 5's whole-image verifier, the mode switch, and
the `verify` switch from landing, beside the security reviewer's DMS-21 and DMS-22; nothing blocks
a start or another step.

### Per-step landing

| Step | Landing | Its conditions | Waits for, outside this review |
|---|---|---|---|
| 1. `RFTW0002`, the marks, `hold_forward` (journal) | **Approved** | C27; K2, K4, K5; journal's code review and a second-model opinion on same-boot recovery and the marks (the format tier); C37's verdict reason (additive, may follow) | Nothing else |
| 2. Boot identity, directory barrier (completion) | **Approved** | K1, K5; C37 (the unknown identity distinct from any value) | Nothing else |
| 3. Cursors, forgetful voters, the recovery election, `Recovery`, rewind (consensus) | **Approved with conditions** | C15, C23 (both hold in the design); C38; K3, K4, K5; its code review and a second-model opinion on the recovery election and the rewind (the durability tier) | Steps 1 and 2 |
| 4. Version-3 wire fields (consensus) | **Approved**: K6 is met by this review and Q8, and no finding here changes a field | K4, K5; T12; its code review and a second-model opinion (the wire tier) | The peer transport's version 2 (its design at revision 2, awaiting its re-check, and parked); Detent's agreement in `DEPENDENCY_UPGRADES.md` |
| 5. strata::group | **Approved with conditions** for checkpoints, `Create`, apply at `commit_memory`, `Image`, incremental install, catch-up, levels and floors (C34, C35, DM-R42's comparison point for catch-up); **rejected** for D18's whole-image grounding, `Vouch` answering, the mode switch, and the `verify` switch (C33) | C25, C28, C30 (as ruled); C33 to C36; security C18 to C22; K7 as C39 completes it; K4, K5, K9; its code review and the security review | Phase 3R's start; small-indexes' batcher and journaled create |
| 6. The simulator's fault classes and schedules | **Approved** (its own reviews gate it) | C10, C19, C24 (its schedules: T3's list stands) | Nothing else |
| 7. T1 on Linux and FreeBSD (the lead) | **Approved** | DM-R39: "today" at `main` | Nothing else |
| 8. D17's file sets and controls (consensus) | **Approved with conditions** | Security C15; K8 as C39 completes it; its code review and the security review (untrusted input) | Step 4; the peer transport's version 2 |
| 9. SipHash-2-4 (Kronuz/hashes) | **Approved** (as ruled at revision 4) | Its CI on every platform | Nothing else |

### Status

| Item | Status | Where, at `4d5acb8` |
|---|---|---|
| DM-R36 Whole-image alphabet | **Closed**: D18 check 5 (DM:2069-2092), T4 (DM:2522), step 5 (DM:2685-2686, 2697), the changes row (DM:113); verified on strata's segments (`alphabet_whole.out`). The choice is ruled after `4d5acb8`: one rule everywhere (`ca70579`, DMS-20, security C18), which revision 7 writes | Security C18 |
| DM-R37 The mode switch | **Closed as a mechanism** (D8 DM:1507-1515, D15 DM:1756, D18 DM:2237-2249, Q9 DM:2625-2627, the threat row DM:2377, the fault table DM:2400, T4 DM:2522); `switch_model.py` holds under the strict readings. Its text admits two losing readings and omits the comparison's point (DM-R42) | DM-R42 |
| DM-R38 Precision | **Closed**: item 1 DM:1502-1503; item 2 DM:2120-2123, T4 DM:2522, step 5 DM:2697; item 3 DM:2059-2060 | |
| C30 Step 5 | **Closed** for DM-R36 and DM-R37 as ruled (D18, T4, Q9, step 5); DM-R36's choice was routed and ruled after the reviewed commit (`ca70579`) | Security C18 |
| C31 Done criteria (the lead's) | **Partially closed**: K2 names DM-R29's mutant and K3 drops it (ST:27-31); K9 carries C4's matrix (ST:37-38); K7 (ST:32-34) omits DM-R36's and DM-R37's cases, and K8 (ST:35-36) DMS-17's refusal and second source | C39 |
| C32 Calibration | **Closed**: `calibration.md:23` | |
| C27 Step 1; C15, C23 Step 3; C12 and security C15 Step 8; K6 Step 4 | **Hold**, carried into the per-step table | |
| Security C13 to C17 | **Closed** as the security reviewer's full review rules them (`ca70579`, ruling 3; C14 under its C18) | |
| Security C18 to C22 (DMS-20 to DMS-24, `ca70579`) | **Open**, the security reviewer's; they bind step 5's verifier beside C33 | Security C18 to C22 |
| C1 to C26, C28, C29; security C1 to C12 | **Hold as ruled**: none is reopened here; those that bind a step bind it still | |

The reruns (`r6/runs/`, each one thread at `nice -n 19`; times as the runs printed, on a host under
other sessions' light holds):

| Run | Depth | States | Time | Result | The design's |
|---|---:|---:|---:|---|---|
| `round`, the mark, torn tails, retried rounds, 3 voters, two writes, terms to 4, three OS crashes, two restarts (`cc-persist-torn-retry-n3-p2-d11`) | 11 | 54,757,724 | 536 s | holds | the same |
| The same, 5 voters, one write, one restart (`cc-persist-torn-retry-n5-d7`) | 7 | 17,649,656 | 316 s | holds | the same |
| rf = 5's window from `six`'s step 37, majority / f + 1 (`cc-six37-*-d6`) | 6 | 84,841,965 / 84,841,967 | 472 s / 493 s | holds / `memory` at 6 steps | the same |
| `manual`'s f + 1 admission, every voter answering, from `six`'s step 37 (`cc-six37-heard-f1-d6`) | 6 | 65,219,423 | 457 s | holds | the same |
| rf = 5 to 7 from `grow67`'s step 65, majority / f + 1 (`cc-grow67-65-*-d5`) | 5 | 131,291,763 / 131,291,764 | 1,112 s / 1,079 s | holds / `memory` at 5 | the same |
| From `lead5`'s step 9, `round` / `fplus`, the candidate's barrier (`cc-lead5-9-*-candidate-d6`) | 6 | 29,609,357 / 16,377,089 | 118 s / 55 s | holds / `memory` at 4 | the same |
| The same, every barrier (`cc-lead5-9-*-all-d6`) | 6 | 26,856,533 / 14,429,542 | 87 s / 50 s | holds / `memory` at 4 | the same |
| `mark_model.py`, the first-turn mark, newest by sequence, the open record copied (`mark-firstturn-copy-d11`) | 11 | 5,111,474 | 266 s | holds | the same |
| `mark_groups.py`, two groups, `none` / `any` (`mg-*-mpt1-close-d9`) | 9 | 2,716,835 / 32,286 | 154 s / 1 s | holds / lost in 6 | the same |
| The same, three groups (`mg3-*-mpt1-close-d8`) | 8 | 2,233,453 / 83,203 | 118 s / 4 s | holds / lost in 6 | the same |
| The journal model's controls (`markctl/`): `literal`, `jorder`, `none`, `same` | 6, 7, 10, 5 | 10,133; 34,543; 598,427; 1,668 | <30 s | lost in 6; 7; precision in 4; 5 | the same |
| `check3_model.py` (`check3-in2-out4`, `check3-in3-out3`) | | 4,743,860 / 12,904,310 outputs | 12 s / 35 s | holds, 0 honest refused | the same |
| `vouch_model.py`, `entry` (`vouch-entry-e2-all`, `-e3-d13`) | all / 13 | 74,225 / 917,782 | 2 s / 28 s | holds | the same |
| Its controls, `position`, `entry_ungrounded`, `position_grounded` | 13 | 457; 5,006; 4,961 | <1 s | lost in 4, 6, 6 | the same |
| Every forced schedule, both checkers, each barrier (`forced/`) | | | seconds | byte for byte | `results/r3/schedules.out`, `results/r4/schedules-lead5.out` |
| New: rf = 5's window with same-boot restarts and torn tails, from `six`'s step 38, majority / f + 1 (`cc-six38-restart-*-d5`) | 5 | 12,953,406 / 12,953,408 | 40 s each | holds / `memory` at 5 | none: no window search had restarts |
| New: the same, majority, one step deeper (`cc-six38-restart-major-d6`) | 6 | 115,518,124 | 433 s | holds | none |
| New: the same from step 37, both counts (`cc-six37-restart-*-d5`) | 5 | 21,689,738 each | 77 s each | both hold (f + 1's loss needs 6 there) | none |
| New: `switch_model.py`, the switch, strict readings (`sw-switch-o6-k2`, `-o5-k3`, `-o5-k2-s4`) | 6 ops, 2 keys; 5, 3; 5, 2, 4 segments | 16,241,982; 11,083,575; 1,769,706 cases | 167 s; 105 s; 19 s | every needed document present | none (argued, DM:2237-2249) |
| New: the same, catch-up (`sw-catchup-o6-k2`, `-o5-k3`) | 6, 2; 5, 3 | 6,403,536; 4,319,922 cases | 254 s; 125 s | every needed document present | none |
| New: its readings and mutants: `--inclusive`, `--inflight-open`, `--no-expunge`, `--merge-held`, `--no-ceiling` (switch); `--merge-held`, `--no-ceiling` (catch-up) | 5, 2 | | <3 s each | each loses an honest comparison within its first 3 operations | none |
| New: `vouch_model_r6.py`, D18 as written, new members may catch up (`vr6-entry-catchup-e3-d13`) / kept out (`vr6-entry-e3-d13`) | 13 | 1,926 / 648,704 | <1 s / 34 s | P1 lost in 5 / holds | none |
| New: the same under DM-R40's fix (`vr6-basegrounded-catchup-e2-all`, `-e3-d12`) | all / 12 | 206,559 / 1,333,398 | 11 s / 46 s | holds | none |
| New: a `verify` switch, a missed plant, as written / under the fix, with and without the plant (`vr6-*startsample*`) | 12 / all | 45 / 14,454 each | <1 s | P1 lost in 3 / holds, no member ever grounded | none |
| New: `alphabet_whole` over strata's own segments at `4d5acb8` | | 5 segments, 4 planted variants | <1 s | honest pass, widened passes reading the same, narrowed, unordered, repeated refused | none |

### Findings

1. **DM-R39. strata `main` has moved under the design: T1's "today", three citations, and
   completion's pin no longer hold at `d058cd9`** (the header, "What exists today", D1, T1, the
   summary).
   - The design reads strata at `026ee10` (DM:18-19), where every strata `file:line` holds. `main` is
     `d058cd9`, 16 commits later: `index.hh`, `files.hh`, and `faultfs.hh` changed, and
     `CMakeLists.txt` pins completion at `20a1498`, not the `5251cf9` DM:21 names.
     `r6/cite/map_citations.out` maps the design's 37 `index.hh` citations: 34 still name the same
     text, shifted by -8 to +46 lines, and 3 no longer hold:
     1. T1's create row (DM:534): `owner.lock` (completion `posix.h:348-358`) and `clear_uncreated`'s
        directory sync (`index.hh:2285-2297`). At `main` the owner lock is the directory's own flock,
        which writes nothing (`9e4f2dd`; `lock_directory`, `index.hh:2302` at `main`), and
        `clear_uncreated` syncs the directory only when it removed something (`204daa1`;
        `:2327-2342`). `main`'s test pins it: "a create over an empty directory makes 4 durability
        calls: 2 file syncs (the metadata file, the manifest) and 2 directory syncs"
        (`test/directory_test.cc:512-530` at `main`).
     2. D1's creation bullet: the UUID `create()` takes at `index.hh:339`, `348` (DM:616). At `main`,
        `create()` is `create_deferred` and the commit of generation 1 (`:357-361`, `:2273-2289`),
        the shape D1's journaled create needs.
     3. D1's replay: "those `clear_uncreated` removes, `index.hh:2292-2294`" (DM:629), now
        `:2333-2341`.
     `format-13.md`'s seven citations hold, four shifted by +40 (F20's text).
   - So T1's "Today (measured, macOS): 10 calls and 10 device flushes for create plus the first
     commit" (DM:2519), the T1 table's create row (DM:532-538), D1's "create's 6 fold into the first
     checkpoint" (DM:642), and the summary's "A tiny index's 10 device flushes" (DM:402-403) describe
     `0b407c9`. At `main` create plus the first commit is 8 durability calls by its own test (4 and
     4), not measured as T1. T1's targets stand, since they are absolute (no sync of its own on the
     acknowledgement path; at most 4 per checkpoint); what moved is the comparison the summary
     quotes.
   - Fix, before step 5 builds on `main`: re-pin the strata citations to `main` at step 5's start
     (`map_citations.out` gives every new line); T1's "today" from `main` (4 for a create, 8 with the
     first commit, `directory_test.cc:512-530`), or step 7's run measures it on macOS too; D1's
     "create's 4"; the header's completion pin `20a1498`, beside which step 2's boot identity lands.

2. **DM-R40. A whole comparison grounds a member whose own image was never grounded, so an
   unvouched install becomes a grounded member that vouches and serves** (D18's whole images, D8's
   catch-up, D17's `Vouch`, the threat row, T4).
   - D18: a member's image is grounded when it is "one it built itself (document mode, or D8's
     catch-up), one it compared whole against its own, one it fetched from a grounded member that is
     not the leader, or one a member vouched for" (DM:2225-2229); the catch-up reinstall has "an
     independent image: the follower applied the log itself up to the `Image`'s position"
     (DM:2230-2232); "A reinstall that passes grounds the image" (DM:2248-2249). D8's catch-up applies
     to every segment-mode follower whose install lag passes D9's bound (DM:1494-1497), a new member
     included. A new member that installed the leader's image alone holds whatever the leader
     planted (`install_unvouched`, DM:2284-2294). The leader floods it into catch-up (the lever DMS-15
     named): it applies the tail on top of that base, so its "own image" carries the plant, and the
     leader's next image, plant kept, compares equal. Under the list as written that grounds it: it
     answers `yes` for the entry ("compared whole against its own image there", DM:2261-2266),
     serves files (DM:2253-2254), and the `unvouched` gauge clears.
   - `vouch_model.py` cannot show it. Only F1 and F2, which start grounded, may enter catch-up (its
     successors' catch-up clause, `i < 2`), and a catch-up reinstall is refused exactly when the
     leader's latest image is tainted, which is right only for a follower whose own image is
     independent. `r6/model/vouch_model_r6.py` changes two things: new members may catch up too, and
     a reinstall compares the member's own content (its base's plants, less those a later delete
     removed) with the leader's. Under D18 as written, P1 fails in 5 steps: the leader plants entry
     1; N1 joins with the leader's image at 1, unvouched, while F1 and F2 are behind entry 1 (the
     design's own residual case, DM:2284-2286); the leader appends entry 2; N1 enters catch-up; N1
     reinstalls entry 2 whole, equal, grounded, holding the plant
     (`runs/vr6-entry-catchup-e3-d13.out`). F1 and F2 refuse entry 1 when they reach it and answer
     `no`, but a `no` sends the asker to a grounded member that is neither the leader nor the sender
     (DM:2278-2281), which N1 now is. With new members kept out of catch-up it holds, as the
     designer's model does (`vr6-entry-e3-d13.out`, 648,704 states to depth 13). Under the fix below
     it holds over every reachable state with two entries (206,559 states) and to depth 12 with
     three (1,333,398; `vr6-basegrounded-catchup-*.out`).
   - "One it built itself (... D8's catch-up)" grounds the same image by the other clause.
   - Fix, in D18's definition and whole images, D17's `Vouch`, the threat row, and T4, before step
     5's whole-image rule: a catch-up builds on the follower's installed image, so a catch-up image,
     and a whole comparison against it, ground only when that installed image was grounded when
     the catch-up began. A member whose image is unvouched that passes a catch-up reinstall installs
     the leader's image and stays unvouched (counted, `not yet` to every ask, serving nothing) until
     a `yes` grounds it. T4 gains the schedule above with D18's list as its mutant, and
     `vouch_model.py` lets every member catch up. My DM-R34 fix said "a base it built itself or
     installed vouched", the right condition; revision 5's definition dropped it for catch-up, and
     my re-check of revision 5 reproduced the model without asking which members it let catch up
     (C40).

3. **DM-R41. A switch to `verify: install` has no grounding rule, and each reading of it fails**
   (D15's `verify` row, D18, Q9).
   - D15: "switching to install checks from the next `Image` on" (DM:1757). An image installed under
     `sample` (option 2, the lead's call, which stands until the owner answers Q9, DM:2635-2637) or
     beside a `replica` was checked against the log only on its samples, so D18 calls it unvouched.
     Every group created before the owner answers Q9 reaches option 3 by this switch.
   - As written, a sampled follower whose image holds a flush the samples missed grounds itself
     through catch-up (DM-R40): `vouch_model_r6.py --start-sample --plant0`, P1 in 3 steps. With
     DM-R40's fix no member is ever grounded: zero reachable states with a grounded segment-mode
     member, with and without a plant (14,454 states each, every reachable state with two entries;
     `runs/vr6-*startsample*.out`). Then no member serves a whole image or vouches, and every new
     member and repair installs from the leader alone and stays `install_unvouched`.
   - The content checked only by samples has no independent base to compare with once checkpoints
     truncated its log range, so the choice is real: (a) at the switch each follower's image counts
     as grounded, with the residual stated: content installed before the switch was checked as
     `sample` (or `replica`) checked it, counted (`grounded_by_switch`), and option 3's "detected at
     its first install" holds for installs after the switch; (b) the images stay unvouched, and the
     group's new members and repairs come from the leader alone, counted, until each is vouched; (c)
     option 3's full guarantee for existing content only by re-indexing from the client's source.
     I recommend (a), since (b) loses the whole-image path for good and the residual of (a) is
     option 2's, which the group already ran under.
   - Fix, in D15, D18, Q9's options 3 and 4, and T4, before step 5's `verify` switch: the rule
     chosen, its counter, and T4's switch with a missed planted flush (under (a): grounded with the
     counter; never `yes` from a member that checked the planted entry, since none did).

4. **DM-R42. The mode switch's text admits two readings that refuse an honest switch, and the
   whole comparison's point is unstated** (D8's switch and catch-up, D15, D18's whole images, T4).
   - `r6/model/switch_model.py` checks DM-R37's claim exhaustively: one log of puts, replaces, and
     deletes over a few keys; a leader and a follower that each flush and merge on their own
     schedules (a merge carries the deletions that arrive while it runs); the command after any
     operation; the leader's first `Image` at any p from the command on, with its pre-command
     deletes expunged; the follower comparing at any applied position a ≥ p. It asks whether every
     document the comparison needs is on the follower, in a segment, its buffer, or its retained
     log: every dead ordinal of the leader's image, and every document live at p and dead by a.
     Under the strict readings below it always is (the counts under "What holds"), and dropping the
     expunge, the frozen segments, or the ceiling each loses an honest switch (`--no-expunge`,
     `--merge-held`, `--no-ceiling`). Two readings of the text lose too:
     1. **"Its checkpoint ceiling stays at the command"** (DM:1510). The command is a log entry; a
        refresh right after the follower applies it has the command's position, so a ceiling at the
        command admits checkpointing it. That flush holds what the follower had buffered at the
        command, and its segment is not one the follower "then holds" (DM:1509), so it may be merged.
        A document buffered at the command, deleted after it, and dropped by that merge is then
        nowhere: in no frozen segment, and not in "every write since" the command (DM:1510-1511),
        since its write came before. `--inclusive` loses at 3 operations (a put and two replaces of
        one key; the command after the first; p = 1, a = 2: docid 1 is live at p and gone from the
        follower; `runs/sw-switch-o5-inclusive.out`).
     2. **"The segments it then holds"** (DM:1509). The output of a merge in flight at the command
        is not a segment the follower then holds, yet it holds documents live at the command.
        `--inflight-open` loses the same way (`sw-switch-o5-inflight-open.out`).
     Under either, an honest comparison fails, the follower counts `verify_failed{whole image}` and
     stays in document mode, so one switch can leave every follower out of segment mode with error
     logs, and T4's honest switch case would fail on correct code. The security reviewer's switch
     model (`ca70579`, ruling 2) takes the first reading's safe side as given ("every document still
     buffered at the command is" above the follower's checkpoint), and its DMS-24 item 2 is the
     leader's side of the second (a leader merge that straddles the command carries pre-command
     deletes into its output, which the expunge must then cover); these two are the follower's.
   - The comparison's point. D18 compares "every live record of its own image there", at the
     `Image`'s position p (DM:2231-2233). The follower has applied past p by the time the entry,
     committed after p, reaches it: during a switch it keeps applying (DM:1756), and in catch-up an
     `Image` "covers its applied position" only once the follower stops (DM:1496-1497). So the live
     sum at p needs the records of the documents deleted in (p, a], and the install at p leaves
     (p, a] to account for. The model shows those records are on the follower under the strict
     readings; the design says neither how the follower computes its image at p nor what its state
     is after the install: its applied position back at p until a later `Image` covers a (as D8's
     promotion rewinds), or (p, a] applied again on top ("its own segments for that range then give
     way", DM:1496-1497).
   - Precision in the same place: (a) the expunge is owed by whichever leader writes the first
     `Image` after the command, a new leader after a failover included; (b) "the deletes made before
     the command" (DM:1511-1512, 1756, 2242-2243) includes replacements, whose old versions are dead
     ordinals; (c) "one merge per segment that holds a dead ordinal" (DM:98-99, 1512) is one per
     segment holding a document deleted or replaced before the command, not any dead ordinal; (d) a
     switch follower may merge its own flushes after the command, as catch-up's does, subject to
     item 1.
   - Fix, in D8, D15, D18, and T4, before step 5's mode switch: the ceiling is the follower's last
     refresh before it applied the command (it checkpoints nothing it refreshed after); the follower
     merges neither the segments it holds when it applies the command nor the output of a merge then
     in flight; D18 states the computation at p (the keyed live sum from its image at a and the log
     over (p, a], adding back the documents deleted in (p, a] from its segments or its log, taking
     out those written there) and the state after the install, for catch-up as for the switch;
     items (a) to (d); T4's switch mutants gain the two readings; `switch_model.py` is the model to
     port. Both readings come from my DM-R37 fix's wording, which revision 6 copied (C40).

5. **DM-R43. The generation that replay opens rests on a retention default the design never
   names**
   (D1's failed checkpoint and creation, T16).
   - D1: a failed checkpoint is restored "at the generation its last checkpoint record names ...,
     never at the highest manifest on disk" (DM:607-610), and replay "opens at the newest one's
     generation" (DM:626-627), while "Retention and reclamation run on each owner after its entry
     completes" (DM:602-603), not after its checkpoint record's barrier. Between the two, the
     durable head is the new generation and the journal still names the old one. A crash there,
     with the old generation's files reclaimed, leaves replay a generation it cannot open ("the open
     requires every file the head names", DM:579-580).
   - strata keeps them today by default: `retained_predecessors = 2` (`index.hh:282`), the
     manifests `retain()` keeps behind the head (`:2196-2203`), and `checkpoint()` and `release()`
     pin generations (`:723-741`). The design relies on neither, and T16 accepts either outcome ("at
     its last recorded checkpoint or the batch's", DM:2534), so it does not test the window.
   - Fix, in D1 and T16, before step 5's checkpoint code: the generation the newest durable
     checkpoint record names stays pinned (strata's pin) until a newer record's barrier returns, or
     `retained_predecessors` ≥ 1 is stated as load-bearing with that mechanism; T16 crashes between a
     checkpoint's completion and its record's barrier, with no pin and `retained_predecessors` 0 as
     the mutant.

6. **DM-R44. The term history grows with every term in every group, with no bound** (D10's
   `session`, D16, D14, `limits.md`, T14; the many-indexes goal).
   - D10: "Each replica keeps a term history per group (the first index of each term with a
     committed entry ...), past every checkpoint" (DM:1625); D16 makes it "an index-level attribute
     ... required in `written` groups" (DM:1813-1814). Every election, every leadership transfer
     (Track D's placement moves leaders), and every rolling restart adds an entry; nothing removes
     one. Kafka's leader-epoch cache, the precedent (DM:2491), is truncated with the log's start. At
     16 bytes an entry, a group whose leader changes daily holds about 5.8 KB after a year, written
     into every manifest it checkpoints (arithmetic); at a million groups per host, 5.8 GB a year,
     more if tokens are checked from memory. PLAN's workload is "thousands to millions of indexes
     per host and tens of thousands open per instance" (PLAN:195-196), and small-indexes' T1 gates a
     200-byte cut in an idle open index's heap, the scale at which per-index bytes count. No cap
     names a mechanism, as `limits.md` requires.
   - The same goal reaches what this design adds per group, which it never totals: two cursors, the
     mark, the `Vouch` interval (16 bytes), each peer's `synced` and rewind allowance, and gauges
     whose scope is unstated: "holds the `unvouched` gauge for the group" (DM:2287-2288) and
     `recovery_stalled` (DM:1169-1171) are a series per group unless they count groups per node.
     T14 runs at 1,000 groups per node (DM:2532), Phase 3R's figure (PLAN:569), where small-indexes
     measures 10,000 indexes open in one process.
   - Fix, in D10, D16, D14, and `limits.md`, before step 5's session tokens: bound the history by a
     horizon with its mechanism (the oldest token a session may present: a token below it answers
     `expired`, which a client treats as a lost read position), keep it on disk and read it only for
     a token below the log's base, T11's case gaining a token below the horizon; state this
     design's memory per idle group beside small-indexes' idle-heap row; make every new gauge a
     count per node, with the group in its log line; T14 adds a run at 10,000 groups per node, most
     idle. This is my fix of DM-R13 item 6, which asked for the
     history "retained across checkpoints" and named no bound (C40).

7. **DM-R45. T2 gates option 2's cost while Q9 recommends option 3, which the estimates put above
   T2's targets, and the switch's expunge is background work no row measures** (T2, the summary,
   Q9, D14, T13).
   - T2 gates "D18's sampled rebuild at its default rate" (option 2) at 0.45× of document mode's group
     CPU at rf = 3, 0.33× at rf = 5, and 0.12× per follower (DM:2520), and reports option 3 beside,
     with no target. Q9 recommends option 3 (DM:2615), estimated at 0.43× to 0.66× and 0.31× to 0.58×
     of document mode's (DM:2585), above T2's targets over most of both brackets. If the owner
     takes the recommendation, the default ships with no gated cost and the gate measures a
     configuration that is not the default. The summary's "0.38× to 0.43× ... 0.25× to 0.32×"
     (DM:421-422) is option 2's.
   - D8's switch adds the expunge, a rewrite of every segment holding a pre-command delete before
     the first `Image` (DM:1511-1515), at sustained ingest. D14 lists every background task the
     design adds (DM:1726-1740), and the expunge is not there; T13 bounds the foreground p99 across
     knob changes but gives the replication-mode switch only "keeps every acknowledged write and the
     digest, the replay within `election_min`" (DM:2531). PLAN: "Background work is measured under
     load" (PLAN:126-131).
   - Fix, before the owner answers Q9: T2's gated row follows Q9's answer (the default's target
     set from T2's measured parts, both options reported); the summary gives both options' costs;
     D14 gains the expunge's row (its debt the bytes of segments holding pre-command deletes,
     reported to the one controller, never a stall, the followers in document mode meanwhile); T13
     bounds the switch's foreground p99 and reports its duration and bytes rewritten. The security
     reviewer's DMS-22 (the followers' retained log while the ceiling is pinned) and DMS-23 (a switch
     rate floor) are the same window's bounds; T13's row reports the retained log across the switch
     too.

8. **DM-R46. Two environment risks change behavior and nothing reports them** (D3, D5, the
   local-access row; the owner's warn-not-refuse rule).
   - "A boot identity of zero or one the platform cannot read reads as unknown, which recovery
     treats as unclean" (DM:838-839). That is the safe reading and refuses nothing, but nothing says
     so: where the platform gives none (FreeBSD before r359053; a container that masks `/proc`'s
     `boot_id`), every process restart makes every `written` group's voters forgetful, so each
     rolling restart costs every group a re-learning, and two restarts at once at rf = 3 take D6's
     recovery election, logged at warning level as a correlated loss though nothing was lost. The
     owner's rule for environment risks, F20 on `main` (non-local filesystems "warn" by default and
     "the index reports the filesystem as state the host reads"; `index_options::nonlocal`,
     `index.hh:331` at `main`; `filesystem_warning()`), proceeds and reports.
   - D3's same-boot rule rests on the page cache holding every turn whose write returned
     (DM:711-716). On a journal directory F20's classification calls not known local, that holds
     only as the network client's cache holds it, and the design says nothing of where the journal
     lives.
   - Fix, in D3, D5, and steps 1 and 2 (additive): completion's boot identity returns "unknown"
     apart from every value; `TurnRecovery` reports why a verdict is unclean (the identity changed,
     was unknown, or the clean-close record is missing); the host reports an unknown identity as
     state and logs it at warning level at open; the journal's directory takes F20's
     classification and warns as an index does; `getentropy`'s fallback (DM:1963-1966) becomes
     state beside its log.

9. **DM-R47. Stale text and precision** (several places).
   1. Q9's heading says "restated with ... revision 5's checks" (DM:2546-2548) and its
      recommendation "restated on revision 6's checks" (DM:2615); and "the re-check of revision 5
      confirms them before the owner answers" (DM:2633-2635) is past: it closed DMS-15, DMS-17,
      DMS-18, and DMS-19, and DMS-16 for incremental installs (`8b87d1b`).
   2. Revision 6's summary (DM:89-91), D18 (DM:2090-2091), the review list (DM:2749), and the
      ledger (ST:278-280, 298-300) leave DM-R36's choice to the security reviewer; it ruled after
      the reviewed commit (`ca70579`, DMS-20: one rule everywhere), which revision 7 writes in D18,
      T4, the changes row, and the ledger.
   3. The header and D18 cite `results/r3/records.out` (DM:60, 1934), and D18's cost and Status
      `results/r4/records.out` (DM:2214, 2734-2735); r3's file still carries the superseded 5.73
      µs per pass. Cite r4's.
   4. The site table's third column is headed "Revision 1" (DM:1415) though revision 2 changed
      its rows (`1151`, DM-R22 item 2); "Revision 1 never holds segment mode" (DM:1547) reads as
      history. Head it "This design".
   5. "Revisions 5's and 6's" (DM:2697).
   6. T10 times a latency ("the latency over loopback", DM:2528) outside the lead's performance
      rows (DM:2512). Count the barriers on the path instead, or list T10 there.
   7. T4's procedure names the simulator and Track C's oracles (DM:2522), while most of its cases
      (installs, whole images, vouches, plants in segments, the switch) need strata::group with
      mutant leaders, step 5's row. Name that harness.
   8. A segment-mode follower whose failed checkpoint restores an older image (DM:604-614) applies
      again `Image` entries whose files no member may still hold, and D17's `Request`
      (DM:1887-1895) does not say what a source answers for an entry whose files it no longer
      holds. State it: the source refuses, and the follower takes D8's catch-up from its restored
      image, whose log range the ceiling kept.
   9. A segment-mode follower with no checkpoint record replays from the creation by removing its
      installed segments (DM:626-634) and then fetches every `Image`'s files again; say so, with
      its cost.
   10. The summary's segment-mode bullet (DM:419-433) and Q9 say nothing of DM-R40 and DM-R41's
       rules; revision 7 restates them there.

### Blocking items

1. **DM-R40**: a whole comparison grounds only a member whose installed image was grounded when its
   catch-up began; D18, D17, the threat row, T4. Blocks step 5's whole-image rule and `Vouch`
   answering.
2. **DM-R41**: the `verify` switch's grounding rule; D15, D18, Q9. Blocks step 5's `verify` switch,
   and Q9's answer for existing groups.
3. **DM-R42**: the switch's ceiling and frozen set stated strictly, the comparison's point and the
   state after it, items (a) to (d); D8, D15, D18, T4. Blocks step 5's mode switch.

### Conditions

1. **C33. Step 5's whole-image verifier** (widens C30): D18's whole-image grounding, `Vouch`
   answering, the mode switch, and the `verify` switch land only with DM-R40's, DM-R41's, and
   DM-R42's fixes in revision 7 and a design-only re-check of `git log 4d5acb8..` (the rechecker can
   run it from `r6/`'s models: `vouch_model_r6.py` holds under the fix and loses as written;
   `switch_model.py` holds under the strict readings and loses under `--inclusive` and
   `--inflight-open`), beside the security reviewer's C18 to C22 (`ca70579`). Revision 7 writes
   DM-R36 as security C18 rules it, one alphabet rule everywhere, which `alphabet_whole.out`
   supports. The lead routes DM-R40 and DM-R41 to the security reviewer's next re-check, since they
   are the threat model's leader row.
2. **C34. Step 5's checkpoint code**: DM-R43's pin (or `retained_predecessors` ≥ 1 named as
   load-bearing) and T16's crash between a checkpoint's completion and its record's barrier, with
   its mutant.
3. **C35. Step 5's session tokens and per-group state**: DM-R44's bound on the term history with its
   mechanism and `limits.md` row, the memory per idle group stated, every new gauge a count per
   node; T14's run at 10,000 groups per node is the lead's, at Phase 3R's gate.
4. **C36. Q9 and the gate rows, before the owner answers Q9**: DM-R45 (T2 gated on the default Q9
   picks, the summary with both options' costs, D14's expunge row, T13's switch bound) and DM-R41's
   rule stated in Q9's options 3 and 4 for groups that switch.
5. **C37. Steps 1 and 2, additive**: DM-R46's reporting (completion's unknown boot identity apart
   from every value; `TurnRecovery`'s reason for an unclean verdict) may land after either step,
   and before strata::group runs a `written` group outside tests.
6. **C38. Step 3**: T6 gains a mutant in which an install while forgetful (`InstallPrepared`,
   `core.h:492-500`) persists its hard state without the mark, killed by a restart before
   re-learning. The mark otherwise rides `durable_` into the install by construction (`core.h:497`,
   published at `:454`), and `Repair` persists no hard state (`:597-609`).
7. **C39. Done criteria** (the lead's; C31's rest): K7 names DM-R36's whole-image alphabet cases,
   DM-R37's and DM-R42's mode-switch cases and mutants, DM-R40's catch-up schedule, and DM-R41's
   switch; K8 names DMS-17's refusal by an unvouched member and the second source for an unserved
   `Request`.
8. **C40. Calibration**, for the lead, three rows on my own reviews:
   - "2026-10-19 | DM-R34's fix in the re-check of revision 4, and the re-check of revision 5 |
     Grounding by a whole comparison was accepted without asking whether the follower's own image
     was grounded, and `vouch_model.py` was reproduced though it lets only grounded followers catch
     up (DM-R40) | The full design review, `vouch_model_r6.py` | For every model a review accepts,
     list which actors it lets take each action and check the design restricts them the same way;
     for every 'grounded' or 'checked' status, trace it back to an independent check of the content
     itself".
   - "2026-10-19 | DM-R13 item 6's fix in the preliminary review | A term history per group
     'retained across checkpoints', with no bound, against the many-indexes workload (DM-R44) | The
     full design review | For every structure a fix adds per group or per index, state its growth
     and its per-idle-group cost at a million groups per host, and name its bound's mechanism".
   - "2026-10-19 | DM-R37's fix in the re-check of revision 5 | It froze 'the segments it then holds'
     and kept the log 'from the command', which leaves out what the follower had buffered at the
     command and the output of a merge then in flight; read the other way, an honest switch fails
     (DM-R42) | The full design review, `switch_model.py` | For every rule keyed to a log position,
     place the state that straddles it (a buffer, a merge in flight, a refresh at the position's own
     index) on each side of it, and model both readings".
9. C27 (step 1), C15 and C23 (step 3), C12 and security C15 (step 8), C10, C19, and C24 (step 6),
   C22 (joint consensus), C3 and C4 (library commits; the both-modes matrix, K9) hold as ruled.

Not run: no benchmark, timing, or A/B (T2's and Q9's CPU figures stay arithmetic over labeled
assumptions; no SipHash, walk, check-5, EVP, TLS, or network rate measured; T1 not measured at
`main`, its count read from `main`'s test); no strata, consensus, or journal suite (the branch changes
no library code; one strata probe built against `4d5acb8` and its pins); no simulator (unbuilt); no
Linux, FreeBSD, or arm64; no fuzzing; the depth-6 search of rf = 5's growth half from `grow6`'s step
42, whose 2²⁸-slot table filled in revision 3, not retried, so that half rests on D6's arithmetic as
the design says; `mark_groups.py` no deeper than depth 9 with two groups and 8 with three;
`switch_model.py` over at most 6 operations, 3 keys, and 4 segments, one follower, one merge in
flight per replica, documents as docids whose records follow from their write, set membership in
place of keyed sums, and no leader change mid-switch; `vouch_model_r6.py` exhaustively with two
entries and to depth 12 with three under the fix (13 stopped at its 1.5 million state cap), and its
`verify` switch only with two entries; `alphabet_whole` over five segments of one probe index, term
dictionaries only (the keys' rule is unchanged); DM-R43, DM-R44, and DM-R46 argued from the
text and the code at the cited lines, not run; Detent's suite not run and Detent not read; the
security reviewer's probes (`durability-sec-r6/`) read only through its review; no Xapian or
Xapiand GPL source opened.
