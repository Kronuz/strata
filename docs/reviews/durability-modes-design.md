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
