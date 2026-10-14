# strata review: Track C design

- Date: 2026-10-14.
- Reviewed: `docs/design/track-c.md` at strata `7f8d7eb`.
- Checked against:
  - PLAN: Goals 3 and 5, "Every gate", Track C (C1–C5, its ordering against the Detent
    split, the gate per item), Track D's ordering (D1 with C5, D3 after C1 and C2), and
    Phase 3R's start rule and gate.
  - REPLICATION: Guarantee, Replica count (`rf = 2 + witness`), consistency modes,
    Checkpoints and repair, Cost, and "Consensus library gaps" G1–G7. DISTRIBUTION: Scale
    contract, Layers, and Open decision 2.
  - `consensus` at `b7073bb`: `include/consensus/{core,types,storage,worker,admission}.h`,
    `include/consensus/{retained_log,checkpoint,snapshot,snapshot_transfer,snapshot_channel}.h`,
    `include/consensus/transport/{codec,hello,profile,peers,session,tls}.h`, and
    `test/consensus_test.cc` (`Host`, `Network`, `reordered_crash_schedules`).
  - `journal` at `2c18fcb`, the commit `consensus` pins (its HEAD, `57e6250`, changes only
    `README.md`): `include/journal/journal.h` and the README.
  - Detent's `docs/AUTHORITY_TRANSPORT.md`, `LIBRARY_BOUNDARIES.md`, and
    `SCALE_TARGETS.md` at Detent `081856e`.
  - My Phase 3 design and lifecycle reviews, for the house style.
  - The Raft literature where a ruling turns on it: Ongaro's dissertation (2014) §3.8,
    §3.10, §4.1–4.3, §6.2, and §9.6; Ongaro's single-server change bug (raft-dev, 2015);
    Howard and Abraham, "Raft does not Guarantee Liveness in the face of Network Faults"
    (2020); etcd's raft (heartbeat commit, CheckQuorum, `ConfChangeV2`); CockroachDB's
    atomic replication changes; TiKV's Raft Engine; Cidon et al., "Copysets" (2013).
- Checked by:
  - Building `consensus` `b7073bb` with its pins (journal `2c18fcb`, completion `373f02a`,
    metrics `9d7e724`, reactor `d5bd39b`): Release, Apple clang 17, Ninja, `-j4`. 14 of 14
    tests pass in 67.9 s; `consensus_consensus_test` runs 37,351 checks with 0 failures.
  - Probes in `.scratch/review-trackc/probes/`, rebuilt and rerun by `run_probes.sh`, with
    the output in `run_probes.log`. Each finding names its probe. The mutant core in
    `probes/mutant/` is a scratch copy; no repository file changed.
  - Not run: sanitizers (no code under review exists yet), benchmarks, fuzzing, Detent's
    suite, the gate VM, the FreeBSD VM, and the M1.

## Verdict

**APPROVE WITH REQUIRED CHANGES.** The decisions have the right shapes. Several are unsafe
or unmeasurable as written, so no C-item's code starts until its required changes land and
a revision check passes.

What holds, and reproduces:

- The starting point matches `b7073bb`: the fixed configuration and its initialization
  record (`core.h:45-50`, `storage.h:55-63, 184`), one storage batch outstanding
  (`core.h:243-248`), one flight per peer (`core.h:204, 626`), quorum reads without leases
  (`core.h:406-425`), `CaughtUp` at or below commit (`core.h:300-305`), non-members
  dropped (`core.h:427`), and the voter list in the fingerprint (`profile.h:14-26`).
- Membership in the log, in force once appended and rolled back with a replaced suffix, is
  the dissertation's rule (§4.1). D3's commit-first rule is the 2015 fix, and one change
  at a time keeps one quorum rule.
- Pre-vote and the leader-contact guard are the dissertation's (§9.6, §4.2.3) and affect
  availability only.
- Learners, the witness's retention rule, and repair from the voter's own log leave Raft's
  safety argument as it is.
- One journal per core, coalesced heartbeats, quiescence, pipelining with per-flight
  correlation, and per-group send queues are the right directions at 1,000 groups per
  node, with precedents in TiKV and CockroachDB.
- The real core passes 2,000,000 randomized steps over 2,000 seeds with restarts that
  interrupt persistence, with 0 failures (`probe_harness_crash_points.cc`).

What blocks:

- Repair through `InstallPrepared` would change the role, term, leader, and commit index,
  and a rewind with a delivery outstanding skips entries (findings 11, 12).
- A coalesced heartbeat's commit is not clamped to the follower's matched index, so a
  follower can apply an entry no leader committed (finding 15).
- D11's one barrier per turn needs a journal that does not exist: an append takes four
  ordering points today, and truncation replaces the whole generation (finding 13).
- Pre-vote and the guard ship without CheckQuorum. Quiescence switches the guard off, and
  a stray message or a follower restart then elects a new leader in a healthy idle group
  (findings 7, 8).
- Member ids are undefined, so the new-incarnation rule does not stop one node from voting
  as two incarnations (finding 5).
- D2's fingerprint clause contradicts T1 and Detent's admission guarantee (finding 21).
- The design states no memory at 1,000 groups; an idle group holds 136,168 bytes today,
  and each group's retained log, up to 64 MiB, lives in memory (finding 14). Snapshots
  move one 64 KiB chunk per round trip under a 60 s total timeout, so a large shard never
  installs across zones (finding 17).
- T3, T6, and T7 have no targets, and T6 measures the best case. T2 defines no persistence
  model, and the existing one cannot see a persistence-ordering bug (findings 18, 22, 24).

## Rulings

| Decision | Ruling | Changes |
|---|---|---|
| D1 Membership in the log | Sound with changes | Base membership in the snapshot descriptor, not the key; members carry their transport identity; every voter-list site restated (R2) |
| D2 Fixed configuration as the initial membership | Sound with changes; its fingerprint clause is wrong as stated | One versioned wire format, or a byte-identical preset from the same code; limits move out of the fingerprint (R1) |
| D3 One voter at a time | Sound with changes | The replacement window and the failure-domain exception stated; joint consensus as the named fallback (R3) |
| D4 Quorums follow the membership | Sound with changes | A removed leader steps down whether or not its transfer succeeds; reads count members only (R3) |
| D5 Who may talk to whom | Sound with changes | Member ids are incarnations named on the wire; admission without a per-connection group binding (R2, R12) |
| D6 Leadership transfer | Sound with changes | When the target acts on `TimeoutNow`; copies and delays (R4) |
| D7 Pre-vote and the guard | Sound with changes | CheckQuorum added; the guard under quiescence (R4, R5) |
| D8 Learners | Sound | Quiescence covers learners (R5); the catch-up bound is R10's |
| D9 Witnesses | Sound with changes | The residual restated; commits stall at the witness's limit; roles at founding (R6) |
| D10 Repair | Sound with changes | Its own event; preconditions on deliveries, captures, and reads (R7) |
| D11 One journal per core | Sound with changes | The journal specified: append protocol, segments, per-group checkpoints, reclamation, recovery, corruption (R8) |
| D12 Coalescing and quiescence | Sound with changes | Commit clamped; wake and campaign rules; cost as a function of peers per node (R5) |
| D13 Forced checkpoints and pacing | Sound with changes | Retention during transfers on disk; timeouts; the catch-up condition (R9, R10) |
| D14 The prototype | Sound with changes | Peers per node varied, the measured node's own storage, memory, recovery, both journal shapes (R14) |
| D15 Pipelining | Sound with changes | Follower-side batching, keepalives and probes, the transport's queue, the default (R11) |
| D16 Flow control and multiplexing | Sound with changes | Per-group receive credit, one lane per group's appends, authentication at scale (R12) |

## Findings

### Membership and roles (C1)

1. **D1: configuration on append, rollback, and the base membership.**
   - Using the newest `Configuration` entry from append, committed or not, and reverting
     when a replaced suffix held it, is the dissertation's rule (§4.1). In this core a
     node cannot act on an entry before it is durable: one batch is outstanding at a time
     and `drive` returns while busy (`core.h:243-248, 581`). "From the moment it is
     appended" (`track-c.md:42-43`) means from the moment it is durable; the text says so.
   - The membership in force at `base_index` belongs in `RecoveredState`, the checkpoint
     bundle, and the install. It does not belong in `SnapshotKey` (`track-c.md:45-46`).
     The key is a correlation token on `SnapshotResponse` (`codec.h:86-93`) and in the
     fixed 128-byte transfer envelope (`snapshot_transfer.h:37`), and a membership is
     variable-length. The portable snapshot descriptor carries it instead; today's is a
     fixed 72-byte `RFTSNP01` frame (`snapshot.h:48, 70`), so it takes a new version. A
     receiver that keeps its suffix (`core.h:307`) uses the newest entry in that suffix,
     else the descriptor's base membership.
   - A `Membership` lists ids and roles (`track-c.md:39-41`). The transport needs a
     member's address and certificate pin before it can dial or admit it: addresses are
     fixed at construction and must match the voters (`peers.h:64-71`), and pins are bound
     to the configured voters (`tls.h:21-36, 45-66`). Entries carry each member's
     transport identity, or the design names the directory that supplies it.
   - The fixed voter list is consulted at 21 sites in 9 files: `core.h:45-50, 60, 207-208,
     293, 427, 589, 612`; `storage.h:57-61, 116-118, 131, 181-182, 222`;
     `checkpoint.h:77-79`; `worker.h:251, 549-551, 761-768`; `snapshot_channel.h:41-42`;
     `transport/peers.h:64-71`, `profile.h:14-26`, `session.h:112-125`, `tls.h:21-36`. D2
     keeps them for fixed configurations. The design states what each becomes in
     membership mode. Two need decisions: a recovered ballot may name any id the group
     ever admitted, not only a current voter (`core.h:60`, `storage.h:131, 222`); and the
     snapshot channel's peers are fixed at construction (`snapshot_channel.h:41-42`).

2. **D3: single-server changes against joint consensus.**
   - Safety holds. Rule (a) is the fix that accompanied the 2015 bug report: no
     configuration entry until the leader has committed an entry of its term. Rule (b)
     keeps one change outstanding. With both, configurations that can coexist differ by
     one voter, and their majorities intersect (§4.1). Witnesses count as voters, so the
     argument covers `rf = 2 + witness`.
   - Availability is where joint consensus (§4.3; etcd's `ConfChangeV2`; CockroachDB's
     atomic replication changes) differs:

     | Replacement | Add, then remove | Joint consensus |
     |---|---|---|
     | `rf = 3`, a move within one zone, all alive | 4 voters, 2 in one zone. That zone's outage leaves 2 of 4, under the quorum of 3; the group stalls until the zone returns | `{A,B,C}` and `{A,B,D}`: A and B satisfy both majorities; available |
     | `rf = 3`, a dead voter replaced | A, B, and the new voter all required (3 of 4) | A and B required; the new voter may lag |
     | `rf = 2 + witness`, a dead data voter replaced | A, the new data voter, and W required; every commit in the window is on two data copies | A and W required; commits in the window are on one data copy |

   - Remove, then add passes through two voters with no tolerance, which REPLICATION rules
     out as a configuration (`REPLICATION.md:113-115`).
   - The window is two commits, since learners catch up first (D3(c)). A leader change
     between the two steps extends it until someone proposes the removal again, and D3
     does not say who. During the window the group breaks DISTRIBUTION's failure-domain
     row (`DISTRIBUTION.md:21`).
   - Ruling: keep single-server changes. The design states the window per configuration as
     above, the failure-domain exception with a bound on concurrent replacements per
     domain, that the host drives a replacement to completion across leader changes and
     crashes, and the measurement that brings joint consensus in: Track D's zone-outage
     simulation showing more unavailable groups than a stated bound.

3. **D4: self-removal, reads, and removed peers.**
   - A self-removing leader whose target is unreachable "aborts after one election
     timeout, when proposals resume" (`track-c.md:100-101`). If it resumes, it keeps
     sending to its former followers, D7's guard then refuses every other candidate, and
     the group keeps a leader outside its membership. The dissertation steps it down once
     the removal commits (§4.2.2). It steps down at commit, transfer or not.
   - Reads seed their acknowledgements with the leader (`core.h:417`) and compare a count
     to `quorum()` (`core.h:211, 667`). A removed leader's own acknowledgement, and one
     from a voter removed after the read was admitted, do not count. Reads count
     acknowledgements from voters of the membership in force when the read becomes ready.
   - A removed voter learns of its removal only from the entry itself. The leader keeps
     replicating to it until the entry commits, then drops its flight and releases any
     snapshot to it with a new `SnapshotReleaseReason`, so the host unpins the source.

4. **D5: admission from any authenticated peer of the group.**
   - Safe under the fault model. Raft's safety does not depend on whether a sender is in
     the receiver's configuration; servers accept appends and grant votes without
     consulting it (§4.1), and terms and the log comparison decide. Today's check
     (`core.h:293, 427`) is defense in depth.
   - Cost: a vote granted to a non-member spends the voter's ballot for that term, which
     pre-vote limits; the transport admits former and future members, not only current
     voters.
   - "The transport binds the group identity" (`track-c.md:85`) holds for today's
     per-group connections (`hello.h:48-53`) and fails under D16, where one connection
     carries every group. A frame's group id then selects the group, and the receiver
     admits it only if it hosts that group. Group identities are never reused across a
     group's deletion and re-creation, or a stale replica can join the new group.
   - The join record is right. A joining node persists it before acknowledging anything
     and denies vote and pre-vote requests; an entry naming it a learner changes neither.

5. **D5: the new-incarnation rule needs ids defined.**
   - The rule is right, and its reason is wider than double voting: a node that lost its
     log can also forget entries it acknowledged, which loses committed entries (§3.8).
   - "Id" is undefined. Today it is the authenticated node id (`types.h:133`), with one
     pin per node (`tls.h:45-66`). If member ids are node ids, a new incarnation needs a
     new node identity and certificate, and every group on that node changes at once. If
     they are per-group replica ids, frames name the source and destination replica: in an
     add-then-remove on one node, the old and the new incarnation are both voters there,
     and a candidate that maps node ids to members can count one node's grant for either.
   - Member ids are per-group incarnation ids bound to a node identity; frames carry them;
     a node answers only for incarnations it holds; and the design states the order of a
     same-node replacement, or forbids one.

6. **D6: `TimeoutNow` copies and delays.**
   - A flag that bypasses the guard is needed (§3.10, §4.2.3). A late or duplicated
     `TimeoutNow` still starts elections the leader did not ask for. A copy that arrives
     after the transfer aborted, in the same term, elects the target against the leader's
     decision. A copy that reaches a voter removed in the meantime, which has not yet seen
     its removal, bypasses the guard and raises a healthy group's term, which T3 forbids.
   - `TimeoutNow` carries the leader's term, its last index, and a transfer token. The
     target acts only as that leader's follower in that term, with its log at that index,
     once per token, and only while it is a voter in its own membership. Learners and
     witnesses are never targets. T3 delivers duplicated and delayed copies.

7. **D7: pre-vote and the guard need CheckQuorum.** (`probe_asymmetric_leader.cc`)
   - A leader that can send but not receive keeps its followers in contact, so the guard
     refuses every candidate while nothing commits. Today's core already stalls this way:
     with every message to the leader lost, it stays leader in term 1 for 3,000 ms (20
     election timeouts), its followers never campaign, and a write never commits. A leader
     steps down only on a higher term (`core.h:432-434`).
   - The dissertation steps a leader down after an election timeout without a successful
     round to a majority (§6.2). Howard and Abraham (2020) show pre-vote without that
     check failing to recover from partial network faults; etcd ships both.
   - A leader steps down when its voters have not answered within an election timeout. A
     pre-vote never changes the receiver's term; a refusal carrying a higher term updates
     the pre-candidate's.
   - D7 does not say whether fixed configurations use pre-vote. If they do, T1's
     byte-identical wire fails (finding 21).

8. **D7 with D12: quiescence turns the guard off.**
   - A quiescent group's followers have not heard from the leader within `election_min`,
     so they grant any pre-vote with an up-to-date log. "Any proposal, read, or message
     wakes the group" (`track-c.md:171`), and a quiescent leader sends nothing, so a woken
     follower times out and wins. Three things then elect a new leader in a healthy idle
     group: a stray message from a removed or partitioned node; a follower's restart,
     which starts its timer and hears nothing (a node restart does it for each of its
     follower groups, about 667 of 1,000 at `rf = 3`); and a false suspicion from gossip.
   - The opposite failures: a leader whose group fences on a live node sends nothing while
     gossip reports the node alive, so its followers never wake (under D11 one journal
     fault fences every group on that core); and followers cut off from a live leader that
     gossip still reaches through indirect probes stay quiescent too.
   - Gossip is Track D2, which waits on the membership-transport decision
     (`PLAN.md:383-385, 400-402`). C2's prototype and Phase 3R run before it exists.
   - A woken follower asks the leader to resume before its own timer runs, and campaigns
     only when the leader does not answer within an election timeout. A quiescent follower
     grants a pre-vote only after the same check fails. A leader that fences, steps down,
     or transfers tells its followers. A restarting replica asks before it campaigns.
     Until Track D2 lands, the failure detector is the node-pair connection's keepalive,
     and gossip later accelerates it. T3 covers each case.

9. **D8: learners.** Sound. Quiescence counts voters only (`track-c.md:169-170`), so a
   lagging learner in an idle group receives nothing. The leader keeps replicating to
   unmatched learners while quiescent, or quiescence requires every member matched. A
   learner that never catches up is finding 17's.

10. **D9: the witness.**
    - The election and the hand-off are right: the witness wins only with the most current
      log, ships it, and transfers. The retention rule keeps every entry a lagging data
      voter lacks.
    - The stated residual (`track-c.md:130-132`) cannot arise under that rule: a data
      voter cannot "lag past what the witness retained" while the witness truncates only
      through its matched index. It arises in three cases the design does not name: the
      witness's log reaches its limit; the witness was replaced by a new incarnation while
      a data voter lagged; or the lagging data voter is a new incarnation that must
      install.
    - At the limit, today's follower capacity check refuses appends (`core.h:503-518`), so
      commits stall rather than truncate. The design makes that the rule: truncating would
      leave committed entries on one data copy and break REPLICATION's guarantee for `rf =
      2 + witness` (`REPLICATION.md:8-12, 111`). "Down" in the residual means crashed; a
      lost disk on the only current data voter is a second failure.
    - A `2 + witness` group cannot be founded. D2 makes every founding member a voter
      (`track-c.md:50-51`), and building one up from fewer members passes through a
      two-voter group, which REPLICATION rules out (`REPLICATION.md:113-115`). The initial
      membership carries roles.
    - The witness's truncation point is a new field in appends (`track-c.md:129-130`), a
      wire change for finding 21. The design allows one witness per group, in `rf = 2 +
      witness`, or states the other mixes it allows.

### Repair (C3)

11. **D10 cannot reuse `InstallPrepared` as it is.** (`probe_install_side_effects.cc`)
    - On both sides of the commit index the path adopts the request's term
      (`core.h:298-299`) and follows the sender as its leader (`core.h:301, 317`); above
      it, it also sets the commit index to the boundary (`core.h:308`). A leader receiving
      node 2's image at its own commit index becomes node 2's follower in the same term
      and rejects proposals; a follower receiving node 3's image records node 3 as its
      leader.
    - The install branch (`core.h:307-321`) taken with a repair flag and `b < commit`
      lowers the commit index and moves leadership to whichever peer supplied the image.
    - Repair is its own event. It changes no hard state, role, leader, or commit index;
      any member may supply the image; and it raises `base_index` to `b` only when `b >
      base_index`, keeping the suffix.

12. **D10: what a rewind meets.**
    - A delivery in flight. `Applied{x}` passes when `x` equals `delivered_through_`
      (`core.h:383-390`). With `a + 1 … x` delivered and a rewind to `b < a`, that batch's
      completion sets `applied_` to `x`, and entries `b + 1 … a` are never applied to the
      restored image. Install refuses the same race by fencing (`core.h:324`). Repair is
      refused while a delivery is outstanding or the core is busy, and the Worker's
      `delivered_` and `application_` (`worker.h:194-201`) reset with it.
    - A capture. `LocalCheckpoint` checks only `through <= applied_` (`core.h:357`). A
      capture of the damaged state taken before the repair, submitted once `applied_`
      passes its `through` again, publishes the damage as the new base and truncates the
      log that could repair it. Repair advances an application epoch, captures carry it,
      and the core rejects older ones.
    - Reads. Linearizable reads wait as stated. A follower's `nearest` and `session` reads
      come from its visible snapshot, which moves back to `b`; the host serves none until
      `applied` returns to its visible position before the repair, as REPLICATION's
      fencing already implies (`REPLICATION.md:178-179`). A leader that must repair
      transfers leadership first, unless it is the only current voter.
    - Redelivered entries reproduce verdicts the leader already reported; the host does
      not report them again.
    - Rewinding and redelivering is safe once these hold: the image at `b` is the state of
      a committed prefix, apply is deterministic (REPLICATION's contract), and `b + 1 …
      commit` comes from the voter's own durable log. The range `[base_index, commit]`
      with the term check is right: the boundary is committed, and Log Matching makes both
      prefixes equal. Rejecting a boundary below `base_index` is right.

### Multi-group (C2)

13. **D11: the journal it assumes does not exist.** (`probe_append_barriers.cc`)
    - One 1 KiB `append_batch` issues 2 file syncs, 1 rename, and 1 directory sync, and
      creates a temporary manifest, in each of 100 appends. A batch is durable only once
      the manifest names it, because recovery discards bytes past the manifest's frontier
      (`journal.h:81, 289-293`). One barrier per turn needs a new append protocol whose
      recovery scans past the frontier and keeps a contiguous, checksummed tail. That is
      REPLICATION's two-barrier frontier design (`REPLICATION.md:153-157`), which the
      design drops from G7 (`track-c.md:11`).
    - The journal truncates only by publishing a checkpoint that covers its exact current
      sequence, as a new generation with at most 8 dependencies (`journal.h:368-374, 410`;
      README line 34). Per-group checkpoints and "reclaims a segment once no group needs
      it" (`track-c.md:160-162`) need segments, per-group checkpoint records, per-group
      sequence numbers (recovery checks one contiguous sequence today, `storage.h:163`),
      and a per-group low-water mark. One idle or lagging group pins every segment after
      its base; TiKV's Raft Engine rewrites such groups' live entries forward so old files
      can go. "A group tag" in the record framing (`track-c.md:254`) understates the
      change.
    - Corruption. Recovery stops at the first bad record (`journal.h:262-287`). In a
      shared journal, every group with a record after that point loses its tail, and by D5
      each one rejoins as a new incarnation and copies its data again: one bad sector
      costs up to every group on the core. The design states that blast radius, or uses a
      format that resynchronizes at block boundaries, so a bad block costs only the groups
      with records in it.
    - Ruling: one journal per core is the right direction at 1,000 groups (fewer files and
      barriers; CockroachDB and TiKV share one log across groups). Its design lands before
      journal code, and T6 measures the per-group alternative too: Linux filesystems
      coalesce concurrent fsyncs into one log commit, and on macOS one `F_FULLFSYNC`
      covers earlier device writes (journal README line 113), so per-group journals with
      batched barriers may meet the budget with less new format.

14. **Memory and storage per group at 1,000 groups.** (`probe_group_memory.cc`)
    - An idle three-voter group holds 136,168 heap bytes today (Worker, Core, Store, and
      SnapshotChannel), the same per group at 1, 10, and 100 groups. 131,376 of them are
      the snapshot channel's two input buffers, allocated per peer at construction
      (`snapshot_channel.h:41-42, 165`; `snapshot_transfer.h:37`). At 1,000 groups that is
      136 MB before any log entry.
    - The retained log lives in memory as an AVL tree of entries with their payloads
      (`retained_log.h:35-40`), up to `log_bytes` per group: 64 MiB by default
      (`types.h:79`), 64 GiB for 1,000 groups. Recovering a shared journal rebuilds all of
      it.
    - Each Worker owns a Store with its own admission pools and checkpoint replacement
      reservation (`worker.h:87-90, 206-215`), so storage reservations grow with the group
      count too.
    - D11–D14 state no memory, and T6 measures none. DISTRIBUTION requires the memory per
      idle index stated (`DISTRIBUTION.md:29`), and Goal 5 makes memory a must-win row.
    - The design states memory per idle and per active group at 1,000 groups; bounds the
      retained log by a node-wide budget, or keeps it in the journal behind an in-memory
      index; allocates snapshot buffers per transfer; and admits storage from one node
      pool on `ledger`'s capacity and permit core (`PLAN.md:283-286`) instead of 1,000
      worst-case reservations.

15. **D12: the coalesced heartbeat's commit is unsafe as written.**
    - `(group, term, commit)` (`track-c.md:166`) carries no previous index or term, so the
      follower cannot check Log Matching. Today a follower raises its commit index only to
      `min(request.commit, previous + entries)` after that check (`core.h:480-486,
      524-525`). A follower holding a divergent, uncommitted suffix from an older term
      that adopts the leader's commit from a heartbeat applies an entry no leader
      committed. etcd's raft sends `min(matched, committed)` in heartbeats for this
      reason. The frame carries the commit clamped to that follower's matched index in the
      current term.
    - The design does not say whether a coalesced reply acknowledges a read probe
      (`core.h:209-214, 540-542`). If it does, the frame carries per-group probe tokens;
      if not, reads keep using appends.

16. **D12: what coalescing saves depends on placement.**
    - At `rf = 3`, a node with 1,000 replicas leads about 333 groups, with 666 follower
      replicas, and follows 667. Placed at random over 100,000 nodes, they reach about
      1,325 distinct peers, so a node pair shares about one group and coalescing removes
      almost nothing: at the 20 ms heartbeat (`types.h:88`) with every group
      non-quiescent, about 133,000 frames per second per node, coalesced or not. With 16
      partner nodes per node (copysets), at most 3,200.
    - "10^3 groups per node cost about what one does at rest" (`DISTRIBUTION.md:58-60`)
      therefore rests on quiescence and on a placement property that Track D3 must
      provide. The design states the cost as a function of distinct peers per node, the
      bound placement must meet, and the connections per node that follow (finding 20).

17. **D13: snapshots cannot catch up a large shard.**
    - The sender keeps one 64 KiB chunk in flight and waits for its credit
      (`snapshot_channel.h:140-144`), so a transfer moves 64 KiB per round trip: 6.55 MB/s
      at 10 ms. Both ends give up after 60 s in total, not 60 s idle (`types.h:90`;
      `core.h:345, 597`; `snapshot_channel.h:39, 113`). One attempt moves at most 375 MiB
      at 10 ms and 3.66 GiB at 1 ms. A 10 GiB shard needs 27 minutes at 10 ms, so it never
      installs and its learner never catches up.
    - While a snapshot is sending, the leader cannot checkpoint (`core.h:361-363`; the
      test asserts it, `consensus_test.cc:1077-1078`), so its in-memory log grows until
      proposals fail with `LogFull` (`core.h:398`). At 1 MB/s of writes the 64 MiB default
      fills in 67 s.
    - A queued transfer times out as well: `AwaitSource` runs on the same clock from the
      request (`core.h:597, 629`), so a node-wide limit (`track-c.md:179-180`) turns into
      repeated releases and requests.
    - The transfer keeps a window of chunks in flight within the bulk lane's credit; its
      timeout measures inactivity; retention during a transfer costs disk, not memory
      (finding 14), so writes do not stall; a queued request does not time out; and the
      design states the catch-up condition (the image's bytes over the transfer rate,
      against the retained log over the write rate) and what the host does when it fails.

18. **D14 and T6 measure the best case.**
    - Three nodes in one process put every group's followers on the same two nodes
      (`track-c.md:184-186`), so coalescing is perfect, which finding 16 shows it is not
      at scale.
    - The simulated nodes share one device, so the measured node's barriers and latency
      include its peers'.
    - Not measured: memory per group; journal recovery time at 1,000 groups; reclamation
      with one lagging group; a node returning after downtime with lagging groups; a heavy
      group's effect on a light group's p99 in the shared turn; strata's own commit and
      checkpoint barriers (Phase 3's two directory syncs per commit) in the node's budget;
      and the per-group journal alternative.
    - Platforms: Linux and macOS. FreeBSD's filesystems order and coalesce barriers
      differently, and LIBRARY_BOUNDARIES' first gate covers all three
      (`LIBRARY_BOUNDARIES.md:198`).

### Pipelining and transport (C4)

19. **D15: pipelining.**
    - Per-flight tokens, probes counted for the flight that carried them, and `matched`
      never moving back keep today's correlation (`core.h:531-538`). Keepalives stay
      outside the window: their responses are ignored (`core.h:536`) and they carry no
      probe (`core.h:618-622`).
    - Throughput. A follower persists one batch at a time and drops messages while busy
      (`core.h:243-248, 427`), and the transport holds one decoded input per connection
      until it is admitted (`peers.h:355-375`). A follower therefore takes one barrier per
      append RPC: pipelining hides the round trip, not the barrier. The follower merges a
      group's queued appends into one batch, or the design states the throughput bound.
    - `send` drops a frame once four are queued on a connection (`peers.h:33, 121-126`),
      so a deeper window, or one shared by many groups, loses frames at the sender. D16's
      queues replace that bound with per-group bounds in frames and bytes.
    - The default pipeline for membership groups is not stated, and T7 compares against
      "the default".

20. **D16: one busy group blocks the connection.**
    - Send-side round-robin is right. On the receive side, the reader decodes one frame,
      holds it, and reads nothing more until the worker admits it; a `Busy` worker ends
      the admission pass (`peers.h:151-189, 355-375`). With every group on one connection,
      a group waiting on its barrier stalls every other group on it. Per-connection credit
      alone does the same: one group's buffered frames consume the window. HTTP/2 and QUIC
      keep per-stream windows beside the connection window for this reason (RFC 9113 §5.2;
      RFC 9000 §4).
    - The receiver keeps per-group queues with per-group credit, or drops a busy group's
      frames, which Raft tolerates. T7 slows a follower's receive path, not only its link.
    - Lanes. "Large appends" on the bulk lane and small ones on the control lane
      (`track-c.md:204-206`) reorder one group's appends across two connections, which
      turns pipelining into rejections. A group's appends stay on one lane.
    - Authentication. Per-node pins and fixed addresses (`tls.h:45-66`, `peers.h:64-71`)
      do not follow a membership. The design names where a node-pair connection learns a
      peer's pin (the union of its groups' membership entries, or a cluster authority),
      and the peer set changes without a restart. Connections per node are twice the
      distinct peers: about 2,650 TLS connections under random placement (finding 16).
    - The snapshot channel has its own per-chunk credit (`snapshot_channel.h:140-144`);
      with D16 that is two flow-control mechanisms on one lane. One remains.

### Compatibility (D2, T1)

21. **D2 against T1, Detent, and the transport.**
    - The initialization record stays compatible. It is immutable (`storage.h:55-63,
      184`), a founding member's record is today's, and a joining member writes a new
      record type. A storage batch's version is per batch (`storage.h:41, 191`). Today's
      reader rejects an unknown entry kind (`storage.h:205`) as corruption, so the new
      kind rides a new batch magic. The checkpoint bundle embeds the initialization record
      (`checkpoint.h:77-79`), so a joined member's bundle needs a new version too.
    - The fingerprint clause contradicts T1. `DTPROF01` hashes the sorted voters and every
      consensus limit (`profile.h:14-30`). Dropping the voters changes a fixed
      configuration's hello, and Detent documents that a different quorum fails admission
      (`AUTHORITY_TRANSPORT.md:9`). New limits (`members`, `pipeline`, `window_bytes`)
      change the hash as well.
    - Other wire changes: `PreVote`, `TimeoutNow`, the transfer flag, coalesced frames and
      replies, window updates, the witness's truncation field, group ids in frames, and
      the snapshot descriptor with a membership. `DTC1` rejects unknown tags and entry
      kinds (`codec.h:61-62, 136-138, 171-172`).
    - A per-group fingerprint cannot exist on a connection that carries every group (D16).
    - Limits in the fingerprint conflict with Detent's requirement that limits be raisable
      online at a log index (`SCALE_TARGETS.md:35, 59`), and with rolling changes: members
      with different limits cannot connect.
    - LIBRARY_BOUNDARIES allows format changes as their own versioned commits, since
      Detent is greenfield (`LIBRARY_BOUNDARIES.md:25, 187, 201`). Two wire paths, one for
      fixed groups and one for membership groups, contradict one implementation per
      mechanism.
    - Detent asks for voter replacement in `consensus` (`SCALE_TARGETS.md:34, 181`): C1 is
      Detent's item too, not a change Detent only tolerates.

### Gate rows

22. **T2's model cannot see what it must.** (`probe_harness_crash_points.cc`)
    - The test's `Host` completes every `Persist` inside the step that emits it
      (`consensus_test.cc:97-99`), and restarts fall between steps
      (`consensus_test.cc:750`), so no restart finds a pending batch: 0 of 24,128 restarts
      over 2,000 seeds of 1,000 steps.
    - A mutant core that sends vote grants and append acknowledgements before their batch
      is durable passes that model over 2,000,000 steps with 0 failures. A model whose
      restarts keep or lose a pending batch at random catches it in 5 of 2,000 seeds (209
      failed prefix checks), and in none of the first 200. The real core passes both
      models with 0 failures.
    - "Every committed membership a single-voter step from the previous one"
      (`track-c.md:225`) misses the 2015 bug: its two conflicting configurations are each
      one step from the first. Leader completeness, checked at every election against a
      global record of committed entries, catches it.
    - A ballot oracle (one grant per voter per term, across restarts) and a durability
      oracle (every grant and successful acknowledgement matches the sender's durable
      state when sent) catch persistence-ordering bugs at their first occurrence instead
      of through a later divergence.
    - Clock skew is missing; Phase 3R's gate has it (`PLAN.md:424-426`). Safety does not
      depend on clocks, since there are no leases (`track-c.md:25`), but D7's guard and
      D12's timers read local time.
    - T2's harness is C5's first version, so Track D1 and Phase 3R grow one harness
      (`PLAN.md:380-382, 400`).

23. **Failure modes and where they stand.**

    | Failure mode | Where | Status |
    |---|---|---|
    | A crash at each persistence point of a membership change | T2 | The model never crashes with a batch pending (finding 22); a crash between the promotion and the removal leaves four voters until the host acts (finding 2) |
    | A configuration entry in a replaced suffix | D1, T2 | The rule is right; random schedules rarely produce it, so T2 forces it and counts its occurrences per seed |
    | A leader removed while a read or snapshot is in flight | D4 | Reads count its own acknowledgement; flights to removed peers are not released (finding 3) |
    | Clock skew | T2, T3 | Not in either row (finding 22) |
    | Duplicated or delayed `TimeoutNow` | D6, T3 | Unbounded elections (finding 6) |
    | A learner that never catches up | D8, D13, T4 | Never installs past 375 MiB at 10 ms; quiescence starves it (findings 9, 17) |
    | Corruption in a shared journal | D11 | Blast radius unstated (finding 13) |
    | A leader that can send but not receive | D7, T3 | Stalls without CheckQuorum (finding 7) |
    | A fenced group on a live node, while quiescent | D12, T3 | Followers never wake (finding 8) |

24. **Targets and procedures.**

    | Row | Target | Procedure | Missing |
    |---|---|---|---|
    | T1 | Byte-identical records and wire for fixed configurations | Fixtures; Detent's suite | Fixtures generated at `b7073bb` by a named deterministic schedule (the repository has none); Detent's commit, options, clean clone, and serial run (`LIBRARY_BOUNDARIES.md:199`); macOS, Linux, and FreeBSD (`:198`); restated by R1 |
    | T2 | 10^6 steps over 1,000 seeds | Randomized simulation | Finding 22 |
    | T3 | "Within a bound of election timeouts" | After healing | The bound in milliseconds from a stated timing profile (Detent's 100–200 ms elections campaigned continuously over real storage, `AUTHORITY_TRANSPORT.md:37`); CheckQuorum, quiescence, and `TimeoutNow` cases (findings 6–8) |
    | T4 | Functional | Not stated | The witness at its limit with a data voter lagging (commits stall, nothing truncates); a witness replaced while a data voter lags; founding `2 + witness`; a learner installing a shard larger than one attempt moves today, under writes, at 10 ms |
    | T5 | Functional | Not stated | Repair with a delivery outstanding, a capture pending, reads pending on a leader, and a late `Applied`; "equals an undamaged replica's" defined as the applied sequence or the digest |
    | T6 | None: every figure is reported, none passes or fails | D14 | Targets (at most 1 barrier per core per turn at every group count; 0 barriers and a stated frame rate at 1,000 quiescent groups; a light group's p99 within a stated ratio of one group's beside a heavy neighbor; memory per group); what counts as a barrier; finding 18's gaps; BENCHMARKS' trial rules |
    | T7 | "A stated bound" | `netem` | The bound; the default pipeline; real storage on the follower; a slowed receive path; snapshot throughput |
    | T8 | One CPU-hour per new decoder | Fuzzing, sanitizers, coverage | Decoders: the join record, the checkpoint bundle with a membership, the snapshot descriptor, `PreVote`, `TimeoutNow`, and the shared journal's segment and checkpoint records; a fixture per format change (`PLAN.md:111`); a scale statement per component (`PLAN.md:115-116`); Goal 5's audit of the changed libraries (`PLAN.md:286-287`) |

### Plan and documents

25. **Consistency.**
    - REPLICATION closes G1, G3, G4, and G6 before Phase 3R (`REPLICATION.md:203`). PLAN
      starts Phase 3R after C1–C4 (`PLAN.md:415`), which adds G2 and G5, and puts G6
      inside Phase 3R (`PLAN.md:419`), as the design does (`track-c.md:10-11`).
    - G7 is "Linux NVMe measurement, fewer barriers per append, and defaults retuned for
      bulk ingest" (`REPLICATION.md:200-202`). The design keeps the measurement only
      (`track-c.md:11`), and D11 depends on the fewer barriers (finding 13).
    - PLAN starts Track C after coordination with the Detent work (`PLAN.md:268-269`), and
      DISTRIBUTION routes transport changes through it first (`DISTRIBUTION.md:129-130`).
      The design records no coordination for D2, D11, or D16.

## Unaffected

These stand as written:

- The starting point (`track-c.md:15-29`), checked against `b7073bb`.
- Membership as an entry kind, the newest entry in force, and rollback (D1).
- A fixed configuration as the initial membership (D2, its first bullet).
- D3's rules (a)–(c) and one quorum rule.
- Learners (D8), and the witness's election, hand-off, and retention rule (D9).
- Repair keeping the log and hard state, its boundary range, and loss of state as a new
  incarnation (D10).
- Group commit across groups, per-group checkpoints, forced checkpoints for laggards, and
  a node-wide transfer limit (D11, D13), as directions.
- Optimistic `next`, one flight after a failure, and `pipeline = 1` for fixed
  configurations (D15).
- One connection per node pair and lane, a group id in every frame, and send-side
  round-robin (D16).

## Ruling

**APPROVE WITH REQUIRED CHANGES.** Each required change lands in the design, and the
revision check confirms it, before the code it governs:

- Before any C1 code: R1–R6 and R13, with R14's rows for T1–T4.
- Before any C3 code: R7, with R14's row for T5.
- Before the D14 prototype: R5's cost statement and R14's row for T6. Before any journal
  format code: R8 and R9.
- Before any C4 code: R10–R12, with R14's row for T7.
- R15 with the revision.

A revision check covers R1–R15 and T1–T8. Phase 3R's start rule is unchanged: C1–C4 pass
first.

## Required changes

1. **R1. Wire and formats (D2, T1; finding 21).** The design picks one: (a) a single
   versioned wire format that Detent adopts in a coordinated commit, with T1 restated as
   storage records byte-identical plus the listed wire changes; or (b) fixed
   configurations emit today's bytes from the same code, as a preset. The fingerprint
   covers the wire version and the node's application profile, not per-group voters.
   Consensus limits move into the replicated configuration, applied at a log index, or the
   design states that a group's limits change only by restarting it. The design lists
   every format and wire change (the batch magic, the join record, the checkpoint bundle,
   the snapshot descriptor, `PreVote`, `TimeoutNow`, the transfer flag, coalesced frames,
   window updates, the witness's field, group ids) with its version tag, its fixture, and
   a test that the previous reader rejects it. It states whether fixed configurations use
   pre-vote and CheckQuorum.
2. **R2. Members and ids (D1, D5; findings 1, 4, 5).** A member is a per-group incarnation
   id, a node identity, a role, and a transport identity (address and pin), or the design
   names the directory that holds the last two. Frames name the source and destination
   incarnation; a node answers only for incarnations it holds; a same-node replacement has
   a stated order or is forbidden. The base membership lives in `RecoveredState`, the
   checkpoint bundle, the install, and a versioned snapshot descriptor, not in
   `SnapshotKey`. Group identities are never reused. The design states the membership-mode
   rule at each of the 21 voter-list sites in finding 1, including which ids a recovered
   ballot may name.
3. **R3. Changes and removal (D3, D4; findings 2, 3).** The design states, per
   configuration (`rf = 3`, `rf = 5`, `rf = 2 + witness`), what a replacement's window
   tolerates; the failure-domain exception to `DISTRIBUTION.md:21` and a bound on
   concurrent replacements per domain; that the host completes a replacement across leader
   changes and crashes; and the zone-outage result that would bring joint consensus in. A
   removed leader steps down when the removal commits, whether or not its transfer
   succeeds. Reads count acknowledgements only from voters of the membership in force when
   they become ready. A removed voter receives appends until its removal commits, and its
   flights and snapshots are then released.
4. **R4. Elections (D6, D7; findings 6, 7).** CheckQuorum: a leader steps down when its
   voters have not answered within an election timeout. A pre-vote never changes the
   receiver's term. `TimeoutNow` carries the leader's term, last index, and a transfer
   token; the target acts once per token, as that leader's follower in that term, at that
   index, while a voter in its own membership; learners and witnesses are never targets.
   The design states the timing profile T3's bounds derive from.
5. **R5. Quiescence and coalescing (D12; findings 8, 9, 15, 16).** The heartbeat carries
   the commit clamped to the follower's matched index. The design states the wake and
   campaign rules of finding 8, notification by a leader that fences, steps down, or
   transfers, the restart path, the failure detector before Track D2 (the node-pair
   keepalive), whether coalesced replies acknowledge read probes, and that quiescence
   keeps replicating to unmatched learners. It states frames, bytes, and connections per
   node per second as functions of distinct peers per node, at rest and under load, and
   the bound on peers per node that Track D3's placement must meet.
6. **R6. Witness (D9; finding 10).** The residual is restated as the three cases of
   finding
   10. At the witness's retained-log limit, commits stall and nothing truncates. The
       initial membership carries roles, so `2 + witness` can be founded. The design
       states which witness mixes it allows.
7. **R7. Repair (D10; findings 11, 12).** Repair is its own event that changes no hard
   state, role, leader, or commit index, and accepts an image from any member. It is
   refused while the core is busy or a delivery is outstanding; it resets the Worker's
   delivery state; it advances an application epoch that captures carry, and older
   captures are rejected. The host verifies the image whole before activation, serves no
   reads until `applied` returns to the pre-repair visible position, and does not report
   redelivered verdicts again. A leader transfers before repairing unless it is the only
   current voter.
8. **R8. The shared journal (D11; finding 13).** Before journal code, the design
   specifies: the append protocol and its barriers per turn (the two-barrier frontier, or
   one), with recovery of a torn tail; segments; per-group checkpoint records and sequence
   numbers; the per-group low-water mark; reclamation in bounded slices, with lagging
   groups' entries rewritten forward or a forced checkpoint, and the space bound that
   results; recovery time and memory at 1,000 groups; whether a group can move between
   cores; and the corruption policy with its blast radius, or a format that resynchronizes
   at block boundaries.
9. **R9. Memory and storage per group (C2; findings 14, 17).** The design states memory
   per idle and per active group at 1,000 groups, with today's 136,168 bytes per idle
   group as the baseline. The retained log is bounded by a node-wide budget or kept in the
   journal behind an in-memory index, so retention during a transfer costs disk. Snapshot
   buffers are allocated per transfer. Storage admission is one node pool on `ledger`'s
   capacity and permit core.
10. **R10. Snapshots (D13; finding 17).** The transfer keeps a window of chunks in flight
    within the bulk lane's credit, and its timeouts measure inactivity. A queued request
    does not time out. The design states the catch-up condition and the host's action when
    it fails. One flow-control mechanism carries snapshots (finding 20).
11. **R11. Pipelining (D15; finding 19).** The default pipeline for membership groups; a
    follower merges a group's queued appends into one batch, or the design states the
    throughput bound; keepalives stay outside the window and never count for probes; the
    sender's per-group queue bounds in frames and bytes; and the window's memory per node.
12. **R12. Multiplexing (D16; findings 4, 20).** Per-group receive queues with per-group
    credit, or the receiver drops a busy group's frames. A group's appends stay on one
    lane. The design states how a node-pair connection authenticates a peer and learns its
    pin, that the peer set follows membership without a restart, and connections and
    memory per node as functions of distinct peers.
13. **R13. T2's harness (findings 22, 23).** Restarts keep or lose pending batches at
    random; the oracles add leader completeness at every election, one grant per voter per
    term across restarts, and durability of every grant and acknowledgement when sent;
    clock rates vary per node. Rare schedules are forced and counted per seed: a
    configuration entry in a replaced suffix, a crash between promotion and removal, a
    removed leader with a read and a snapshot in flight, and a delayed `TimeoutNow`. The
    harness detects each of these seeded faults within the seed budget, with the seeds per
    fault recorded: D3(a) off, D1's rollback off, a self-removed leader counting itself,
    sends before durability, an unclamped heartbeat commit, and repair below `base_index`.
    The harness is C5's first version.
14. **R14. Targets and procedures (findings 18, 24).** T1–T8 each have the target and the
    procedure that finding 24 lists as missing. T6 varies distinct peers per node (2, 16,
    and at least 1,000 simulated), keeps the measured node's storage to itself, adds
    memory per group, recovery time, reclamation with a lagging group, a returning node, a
    heavy and a light group on one core, strata's commit and checkpoint barriers, and the
    per-group journal alternative, and runs on Linux, macOS, and FreeBSD under BENCHMARKS'
    trial rules. T8 lists every new decoder and fixture, a scale statement per component,
    and where Goal 5's audit of the changed libraries lands.
15. **R15. Documents and coordination (finding 25).** REPLICATION's line on which gaps
    close before Phase 3R agrees with PLAN and the design. The design carries G7 whole, or
    PLAN and REPLICATION move its parts. The design records the Detent coordination for
    D2, D11, and D16: what changed in the shared libraries, and Detent's agreement or its
    commits. DISTRIBUTION's claim about 1,000 groups at rest names its conditions:
    quiescence and bounded peers per node.

## Revision check

- Date: 2026-10-14.
- Reviewed:
  - `docs/design/track-c.md`, `docs/REPLICATION.md`, and `docs/DISTRIBUTION.md` at strata
    `fd1ba2c` (`git diff 7f8d7eb fd1ba2c`), against this review as committed at `bd66be1`.
    Line numbers below without a file are `track-c.md`'s at `fd1ba2c`.
  - `completion` `64d958b` (`IO::sync_files`), which D11 names, and its
    `docs/PLATFORM_FINDINGS.md`.
- Checked against: R1–R15, findings 1–25, and the gating table of the first review; PLAN's
  Track C and Phase 3R; AUTHORITY_TRANSPORT, LIBRARY_BOUNDARIES, and SCALE_TARGETS at
  Detent `081856e`.
- Checked by:
  - Reading each revised section against its R-item.
  - Two probes, added to `run_probes.sh` (output in `run_probes.log`):
    `probe_journal_spec.cc` models D11's block format and recovery rules as written, under
    512-byte sector tearing; `probe_timing_profile.cc` runs `b7073bb`'s core in simulated
    time (three voters, 1 ms links, 500 seeds per profile) under the revision's timing
    profile and Detent's.
  - Not run: benchmarks, fuzzing, sanitizers (no new code), `completion`'s tests at
    `64d958b`, Detent's suite, the gate VM, the FreeBSD VM, and the M1.

### Verdict

**APPROVE WITH REQUIRED CHANGES.** Seven items close (R3, R5, R6, R7, R12, R13, R15),
eight close in part (R1, R2, R4, R8, R9, R10, R11, R14), and the revision's own answers
raise five new ones (R16–R20). C3 and the C2 prototype start now; C1 starts except for
four named parts; journal format code and C4 wait.

What holds:

- One versioned wire format that Detent adopts, a fingerprint without voters or limits,
  and every format change tagged and fixtured (`56-97`).
- Incarnations as members, the node directory, and a rule at each of the 21 voter-list
  sites (`101-147, 196-214`).
- Replacements that the core completes through a `leaving` marker, with their windows and
  the joint-consensus trigger stated (`149-181`).
- CheckQuorum, `TimeoutNow`'s acceptance rules, the commit clamp, the wake rules, and
  costs as functions of distinct peers per node, bounded at 64 (`216-243, 364-403`).
- Repair as its own event with an application epoch (`288-314`).
- T1–T8 with targets, and T2's harness as C5's first version (`499-574`).

What blocks:

- D11's block CRC makes a later turn rewrite a synced block; in 254 of 256 power-loss
  outcomes that rewrite costs the previous turn's acknowledged records (R8).
- The timing profile fails on the storage it names. With 50–80 ms barriers, each entry is
  sent 14.2 times per follower, leaders change 74 times in 500 runs of 2 s, and a cold
  start takes 15.4 s at p99 (R4).
- CheckQuorum steps down every quiescent leader after `election_max` (R18).
- T4's 10 GiB install cannot converge at 1,000 groups per node under D17's budget (R10).

### Status

1. **R1. Partially closed.**
   - Closed: `DTC2` for every group in one coordinated commit (`65-67`); `DTPROF02`
     without voters or limits (`68-70`); limits in the replicated configuration (`71-74`);
     pre-vote, CheckQuorum, and the guard for fixed configurations (`75`); the table of
     tags, each with a fixture and a previous-reader rejection test (`77-97`); T1 restated
     (`504-509`).
   - Open: the table omits `Wake` and `LeaderGone` (`375-383`), the node-pair keepalive
     (`384-387`), and the bulk lane's transfer framing once D18 drops the per-chunk credit
     (`496-497`). Today's decoder rejects unknown tags (`codec.h:171-172`), so every tag
     is fixed before the coordinated `DTC2` commit, or a later commit bumps the version
     again.

2. **R2. Partially closed.**
   - Closed: the member (`106-108`), the node directory (`109-112`), the base membership
     outside `SnapshotKey` (`116-119`), group identities (`120-122`), frames and grants by
     incarnation with the same-node order (`198-214`), and the site table (`124-147`) but
     for two of its rules.
   - Open, those two rules:
     - Ballots (`131`). D5 answers vote requests from any authenticated incarnation
       (`200-204`). A voter that has not received the entries adding a candidate grants it
       when the candidate's log is more current; its durable ballot then names an
       incarnation at or above its own counter, and recovery under the stated rule refuses
       the voter's valid state. The rule is any positive incarnation.
     - `worker.h:251` (`141`). A learner installs a snapshot at the leader's base, which
       can precede the entry that added it, so until its first checkpoint its own base
       membership lacks it. If it then leads, receivers refuse its snapshots. The rule is
       D5's, as at `core.h:293` (`133`): any authenticated incarnation of the group; the
       term decides.

3. **R3. Closed.** `Replace` and the `leaving` marker complete a replacement without the
   host (`157-161`); the window per configuration (`162-170`); the failure-domain
   exception and Track D3's bound (`172-176`); the joint-consensus trigger at 0.1% of a
   zone's groups (`177-179`); a removed leader steps down at commit (`189-191, 224-225`);
   reads count members in force (`187-188`); a removed member receives the entry, then its
   flight and snapshot are released (`192-194`).

4. **R4. Partially closed.** (`probe_timing_profile.cc`)
   - Closed: `TimeoutNow` and its acceptance rules (`218-223`); a pre-vote never changes
     the receiver's term (`234-235`); CheckQuorum (`239-243`).
   - Open, the timing profile (`247-252`):
     - It is not Detent's. Detent runs a 100 ms heartbeat, a 500 ms RPC deadline, and
       1,000–2,000 ms elections, because the 100–200 ms core defaults "repeatedly left all
       three native-storage processes campaigning" (`AUTHORITY_TRANSPORT.md:37`).
     - It contradicts the storage it names. Its 40 ms RPC timeout is under the 80 ms
       barrier, so the leader resends the unacknowledged suffix while followers persist.
       An election round holds two barriers, up to 160 ms, against 100–200 ms elections;
       the dissertation asks for a broadcast time well under the election timeout (§3.9).
     - On `b7073bb`'s core with 50–80 ms barriers and one proposal per 10 ms: each command
       entry is sent 14.2 times per follower, leaders change 74 times over 500 runs of 2
       s, and a cold start reaches a committed leader at p50 1,527 ms and p99 15,368 ms
       (mean term 16.9). With 1–2 ms barriers the same profile gives 1.0 send per entry, 0
       changes, and a p99 of 187 ms. Detent's profile with 50–80 ms barriers gives 0.96
       sends, 0 changes, and a p99 of 2,234 ms.
   - Required: the RPC timeout exceeds the round trip plus the follower's barrier p99;
     `election_min` covers an election round (two barriers and a round trip) with margin
     (§3.9); the profile is stated per platform from its measured barrier p99, or once for
     the slowest; T3's bounds are recomputed from it; and T3 states its procedure:
     simulated time with each platform's measured barrier distribution, and a
     three-process run on real storage.

5. **R5. Closed.** The commit clamp and read probes (`366-371`), quiescence including
   learners (`372-374`), the wake rules (`375-381`), `LeaderGone` (`382-383`), the
   detector (`384-387`), and the cost table with Track D3's bound of 64 peers (`388-403`;
   `DISTRIBUTION.md:59-63`). Its interplay with R4's CheckQuorum is new (R18).

6. **R6. Closed** (`261-284`). One text point: "exactly one witness ... No other
   configuration has a witness" (`265-266`) contradicts D3's replacement windows (`170`:
   three data voters and a witness; replacing the witness puts two witnesses in the
   window). The rule holds outside a replacement's window.

7. **R7. Closed** (`288-314`). The boundary range narrows to `[base_index, applied]`
   (`292`), and T5 matches (`544`).

8. **R8. Partially closed.** (`probe_journal_spec.cc`)
   - Closed: segments (`323-325`), per-group sequences and checkpoint records (`342-346`),
     rewrite-forward reclamation and its space bound (`347-351`), recovery cost
     (`352-355`), group moves (`359-360`), and the alternative named (`361-362`).
   - Open:
     - **Synced bytes are rewritten.** The block header carries a CRC of the whole block
       (`326-327`), and nothing makes a turn start a new block (`331-332`), so a turn that
       continues a block rewrites its header. Under 512-byte sector tearing, 254 of 256
       power-loss outcomes of that rewrite invalidate the block. It then reads as the torn
       tail (`338-339`), or as corruption if a later block survived, and either way the
       previous turn's acknowledged records in it are lost. A voter that forgets entries
       it acknowledged can lose committed entries. Required: the header holds only fields
       fixed when the block is first written, under its own CRC, and records keep theirs
       (`330`); or each turn starts a new block, with the padding cost stated and
       measured.
     - **The torn tail is per block** (`338-341`). One turn's blocks persist in any order
       on power loss: 4 of 8 persisted subsets of an unacknowledged three-block turn read
       as corruption, and D11 then makes their groups rejoin (`356-358`) after an ordinary
       power loss. Required: records carry their turn; the last turn is the torn tail
       whatever its blocks show; a bad block before a later complete turn is corruption.
     - "Epoch" (`327, 338`) is undefined.
     - A lost hard-state record (a vote) is neither "above its commit index" nor "a
       committed record" (`357`), and losing it permits a second vote in that term. Any
       gap in a group's sequence makes the group rejoin.
     - `fdatasync` (`333`) has no primitive: `completion`'s `file_sync` is `fsync`, or
       `F_FULLFSYNC` on macOS (`posix.h:120-128`). Adding one is a `completion` change for
       R15's list. A turn that crosses a segment boundary syncs two files; the design says
       how that counts against one barrier per turn.
     - Segment deletion: freeing a file costs 7–130 ms per GiB and disturbs other I/O
       (`completion`'s `PLATFORM_FINDINGS.md`, §2). Deletion runs from the reclamation
       queue, off the turn.
     - Zero-filling every 64 MiB segment (`323-325`) writes the journal's bytes twice. T6
       reports bytes written per byte appended, or segments are recycled, since the
       header's identity, segment number, and epoch tell a stale block from a current one.

9. **R9. Partially closed.**
   - Closed: the memory target against today's baseline (`426-430`), the retained log in
     the journal behind an index and a bounded cache (`431-435`), per-transfer snapshot
     buffers (`436`), and one node pool on `ledger`'s core (`437-438`).
   - Open:
     - The core is I/O-free and builds `AppendRequest` and `Committed` from in-memory
       entries (`core.h:632-641, 653-663`). Once payloads live in the journal, the design
       states how a cache miss suspends a send or a delivery and who reads the journal:
       for example, the core emits entry references and the Worker resolves them, under a
       bound on reads in flight.
     - The index costs 16 bytes per entry (`354, 429`), and the node budget bounds bytes,
       not entries. Today's batch encoding spends 21 bytes on an empty entry
       (`storage.h:66-77`), so 4 GiB holds about 200 million entries and 3.3 GB of index.
       The design states a node entry budget, or the index's worst case.
     - "Each group's share is its byte budget's share" (`433-434`) is undefined; D13
       (`407`) and D18 (`489-495`) depend on it.

10. **R10. Partially closed.**
    - Closed: a window of chunks (`483-485`), inactivity timeouts and untimed queued
      requests (`486-488`), the catch-up condition and the host's action (`489-495`), and
      one flow-control mechanism (`496-497`).
    - Open: T4's install (`542`: 10 GiB at 10 ms under 10 MB/s of writes) does not
      converge at 1,000 groups per node under D17's defaults. At D18's 104 MB/s the
      transfer takes about 102 s, while the leader writes about 1 GB; an equal share of
      the 4 GiB node budget among 1,000 groups is about 4 MiB, 0.4 s of those writes. Each
      retry needs retention for its own transfer, and the third failure stops the learner
      (`494-495`). The incremental retry rests on strata's immutable segments (`493-494`),
      which `consensus`'s own T4 does not have. Required: the design states how much
      retention a transfer may pin from the node budget, and T4 runs with 1,000 groups on
      the leader's node.

11. **R11. Partially closed** (`442-460`). One point is open: a merged follower batch
    (`452-456`) answers each merged RPC with its own token and `matched` after the batch's
    barrier. A single response for the last RPC leaves the earlier flights to time out,
    which returns the peer to one flight (`444-446`).

12. **R12. Closed** (`462-479`). Putting every Raft message on the control lane raises a
    new item (R19).

13. **R13. Closed** (`510-528`), with the seeded faults and 10^7 steps over 10,000 seeds.
    The new journal format needs a power-loss row of its own (R17).

14. **R14. Partially closed.**
    - Closed: targets for T1, T2, T5, T7, and T8 (`504-528, 543-548, 560-574`), and T6's
      targets and breadth (`412-422, 549-559`).
    - Open:
      - T3: R4's profile and procedure. T4: R10.
      - T6 counts "one durability call" as a barrier (`551-552`). Under `sync_files` the
        per-group alternative issues one `fsync` per file and one `F_FULLFSYNC` on macOS,
        and one `fsync` per file on Linux (`posix.h:133-148` at `64d958b`), so it fails
        the row by definition. T6 counts device flushes apart from calls, and states the
        decision rule before the run: commit p50 and p99, throughput, and CPU per command
        at 1,000 groups.
      - "The bare-metal machine when reachable" (`550`) cannot waive PLAN's requirement
        that the bare-metal machine precede Phase 3R (`PLAN.md:300-305`); it is the plan's
        only arm64 host.
      - T8: decoders for `Wake`, `LeaderGone`, the keepalive, and the transfer framing
        (R1).

15. **R15. Closed.** REPLICATION (`REPLICATION.md:204-206`) agrees with PLAN
    (`PLAN.md:415, 419`) and the design (`21-24`); G7's parts have owners; DISTRIBUTION
    states its conditions (`DISTRIBUTION.md:59-63`); and the coordination is recorded as
    the owner's authorization of 2026-10-13 (`576-585`), which meets PLAN's start rule
    (`PLAN.md:268-269`). Condition 1 carries its terms.

### New required changes

16. **R16. Limits by position (D1, D2).** D2 lets a `Configuration` entry raise limits for
    the entries after it (`71-74`), and every decoder bounds its input by the group's
    limits: the frame (`codec.h:103, 129, 136-138`), the storage batch
    (`storage.h:189-208`), and the checkpoint bundle (`checkpoint_format.h:32-39`). A
    follower still under the old limits refuses a frame that carries the raise and a
    larger entry after it, and a node recovering with its founding limits refuses its own
    batches written after a raise. Required: decoders bound by node-level ceilings that no
    limit may exceed; the core and recovery check each entry against the limits in force
    at its index; the Worker's output bounds (`worker.h:549-551`) follow the ceilings; T2
    raises limits mid-run. D2 also requires an export to lower a limit (`72-73`), while
    Detent's targets lower one online when current usage fits (`SCALE_TARGETS.md:59`); the
    design says which applies to Detent's groups, with Detent's agreement recorded.

17. **R17. A power-loss row for the shared journal (D11).** T2 persists at the
    storage-batch level, so no row tests `RFTW0001` itself. The row: power loss at every
    turn of a randomized multi-group workload, with any subset of a turn's 512-byte
    sectors persisting; zero acknowledged records lost; zero groups declared lost by power
    loss alone; one corrupted block costing only the groups with records in it; a second
    power loss during recovery; and the crash points and seeds recorded.

18. **R18. CheckQuorum and quiescence (D7, D12).** CheckQuorum steps a leader down when
    its voters have not answered within `election_max` (`239-241`), and a quiescent leader
    sends nothing (`374`). Every quiescent leader therefore steps down 200 ms after
    quiescing, its followers campaign on `LeaderGone` (`382-383`), and the next leader
    quiesces and steps down in turn. Required: CheckQuorum runs only while the group is
    not quiescent, and waking restarts its window. T3 holds 1,000 idle groups for 60 s
    with no leader change, and T6's 0 barriers at rest stands.

19. **R19. Control frames ahead of data on the shared lane (D16).** Every Raft message now
    uses the control lane (`464-466`), and each group may have 4 MiB of appends in flight
    (`450`). A heartbeat, vote, `Wake`, or keepalive queued behind other groups' appends
    waits for them: 4 MiB takes 34 ms at 1 Gb/s, so three active groups ahead of it take
    101 ms, past `election_min`, and the keepalive detector (`384-387`) and CheckQuorum
    then act on queueing, not failure. CockroachDB gives its system ranges, node liveness
    among them, a connection class of their own so that traffic does not queue behind bulk
    Raft traffic. Required: control frames go ahead of queued append data; appends are
    split into frames of a stated maximum size; unsent bytes in the socket stay bounded
    (`TCP_NOTSENT_LOWAT`); T7 measures control-frame latency p99 under saturating appends
    against a stated bound well under `election_min`.

20. **R20. Today's journal format (D2, D11).** D2 keeps one implementation of each
    mechanism (`58-59`) and keeps stores written before Track C readable (`60-61`), and
    D11 adds `RFTW0001` (`320`). The design states whether today's generation-and-manifest
    journal stays a writer for Detent, which makes two journal implementations and needs
    its reason, or becomes read-only, with a migration into `RFTW0001` on first open that
    is crash-safe at each step and that T1 runs under power loss.

### Which code starts now

| Item | Starts now | Waits for |
|---|---|---|
| C1 | Membership entries and the in-force rule (D1); `Replace` and `leaving` (D3); quorums and self-removal (D4); incarnation framing in the core (D5); transfer (D6); pre-vote, the guard, and CheckQuorum (D7); learners (D8); witnesses (D9) | R2's two site rules, before the recovery and admission code that checks them; R16, before limits enter the configuration or any decoder bounds by them; R1's tags and R20, before the coordinated `DTC2` and storage-format commit; R4's profile, before timing defaults are set and T3 runs |
| C3 | All of D10 | Nothing |
| C2 prototype | D14 | R18, before its quiescence code; R14's flush count and decision rule, before its first reported run |
| C2 journal format | Nothing | R8's open points, R9's, R17, and R20 |
| C4 | Nothing | R10's open point, R11's, and R19 |

### Conditions

1. **Library commits.** The owner's authorization requires that Detent still builds and
   that LIBRARY_BOUNDARIES.md is updated (`579-581`). Neither is recorded for `completion`
   `64d958b`, which changed a library Detent links; LIBRARY_BOUNDARIES at Detent `081856e`
   does not mention it. Both are recorded for it before the coordinated `DTC2` commit, and
   for every library commit after it.
2. **Track D3.** At most 64 distinct peers per node (`DISTRIBUTION.md:61`) also sets the
   scatter width that a lost node's groups re-replicate from (Cidon et al., 2013). Track
   D3 states the re-replication time after a node loss, and the loss probability under
   correlated failures, at that width.

## Second revision check

- Date: 2026-10-14.
- Reviewed:
  - `docs/design/track-c.md` at strata `d8d1f5a` (`git diff 470c6d3 d8d1f5a`), with
    `cd81685` (D10 as built). Line numbers below without a file are `track-c.md`'s at
    `d8d1f5a`.
  - `consensus` `afd456d` (C3: the `Repair` event, the application epoch, the Worker's
    gate, and `repair_at_or_below_commit`), against `b7073bb`; file:line citations into
    `consensus` below are `afd456d`'s.
- Checked against: the revision check's open points in R1, R2, R4, R6, R8–R11, and R14;
  R16–R20; D10 and T5; PLAN's Track C.
- Checked by:
  - Building `afd456d`, exported with `git archive` because the `consensus` working tree
    holds uncommitted work, in Release (Apple clang 17): 14 of 14 tests pass in 63.6 s;
    `consensus_consensus_test` runs 37,735 checks, 8 of them repair boundaries, with 0
    failures.
  - `consensus_consensus_test` and `consensus_worker_test` under ASan and UBSan (Homebrew
    LLVM 23, `-fno-sanitize-recover=undefined`, leak detection off on macOS): 37,735 and
    94,873 checks, 0 failures.
  - Reading `afd456d` against D10 and T5, branch by branch.
  - Not run: TSan, coverage, benchmarks, fuzzing, Detent's suite, the gate VM, the FreeBSD
    VM, and the M1.

### Verdict

**APPROVE WITH REQUIRED CHANGES.** Every open point of the revision check closes except
two. R8 gains four points in the text this revision adds (epochs and recycled segments),
and R19 still owes its T7 row. C3 is built and its core is correct, but it does not meet
T5 yet: two new items (R21, R22) and the test's coverage stand before its gate. C1 and the
C2 prototype start in full and C4 starts; journal format code waits for R8.

What holds:

- Every `DTC2` tag fixed before the coordinated commit, including `Wake`, `LeaderGone`,
  `Ping`, `Pong`, and the snapshot chunk frame (`87-109`).
- Limits by position, with node-level ceilings in every decoder (`71-81`).
- Both site rules: ballots name any positive incarnation (`143`); the snapshot sender is
  any authenticated incarnation (`153`).
- A timing profile derived from barrier and round trip under the dissertation's rule, with
  Detent's real profile as the default (`263-279`), and T3 recomputed from it (`611-623`).
- Immutable block headers, per-fragment CRCs, a per-turn torn tail, a durable epoch, the
  gap rule, `File::sync_data`, one file per turn, recycling, and the migration
  (`355-413`).
- Payloads in the journal behind `EntryRef`, node budgets, defined shares, and a transfer
  reserve (`486-503`); a 10 GiB install that converges at 1,000 groups per node
  (`572-577`).
- Merged follower batches answer every RPC (`523-524`); control frames before data
  (`541-547`); CheckQuorum suspended in quiescence, with the keepalive standing in
  (`256-259`).
- T6's device-flush count and decision rule (`640-654`), the bare-metal machine required
  (`637-638`), and T9 for the journal under power loss (`673-678`).

What blocks:

- **R8.** Recovery treats only the newest turn as the torn tail (`381-385`), so a turn
  left incomplete by one open reads as corruption in every later recovery. A recycled
  block's unwritten rest still holds the earlier use's fragments, which pass their own
  CRC.
- **R21.** Through the Worker, a capture carries no epoch: the Worker stamps the core's
  current one (`worker.h:853`), so D10's guarantee holds only for direct `Core` users.
- **R22.** Reporting a damaged application fences the core (`core.h:379-385`), and a
  repair image must sit at or below `applied` (`core.h:391`). D10 states no host contract
  between detection and repair, and a member's image above a stalled voter's applied
  cursor is refused by both `Repair` and the install path (`core.h:303`).

### Status

1. **R1. Closed.** `Wake` and `LeaderGone` (`104`), `Ping` and `Pong` (`105`), and the
   snapshot chunk frame (`108`) join the table, and every `DTC2` tag is fixed before the
   coordinated commit, a later one bumping the version (`87-89`).

2. **R2. Closed.** Ballots name any positive incarnation (`143`); `worker.h:251` takes
   D5's rule (`153`). `afd456d` adds a site, the repair's voter count (`core.h:389`),
   which in membership mode counts the voters and witnesses in force (Condition 4).

3. **R4. Closed.** The RPC timeout is at least `4·(r + b)` and 100 ms, `election_min` at
   least `3·(2b + r)` and ten heartbeats (`269-273`); the default is Detent's authority
   profile, for the slowest platform measured (`275-279`). At `b` = 80 ms that gives at
   least 324 ms and 483 ms, against the default's 500 ms and 1,000 ms. T3's bounds and
   procedure follow it (`611-623`). Two text points are in Condition 4.

4. **R6. Closed** (`292-296`).

5. **R8. Partially closed.**
   - Closed: block headers hold only fixed fields under their own CRC and are never
     rewritten (`365-368`); fragments carry their own CRCs and records their turn
     (`369-371`); the newest turn is the torn tail (`381-385`); the epoch is defined
     (`361-364`); any gap, a lost vote included, makes a group rejoin (`402-405`);
     `File::sync_data` (`374-376`); a turn never crosses a segment (`377-379`); deletion
     runs off the turn (`394-397`); segments are recycled and T6 counts bytes written per
     byte appended (`355-360`, `655`).
   - Open:
     - **Torn tails across reopens.** Recovery treats only the turn with the highest
       number as the torn tail, and any bad fragment before a later complete turn as
       corruption (`381-385`). After a crash, the next open drops an incomplete last turn
       and appends new turns after it; at every later recovery that dropped turn precedes
       complete turns, reads as corruption, and makes its groups rejoin. Re-judging it
       there is unsafe as well: a turn the open kept because it read back whole, and whose
       records the node has since acted on, would be dropped if one of its blocks later
       failed. Required: an open's first record names the last turn it kept from the
       previous epoch; later recoveries ignore that epoch's turns after it and treat
       damage at or before it as corruption.
     - **Each open starts a segment.** The epoch is "written in the first block of the
       segment the open starts in" (`362-363`), and headers are never rewritten (`367`).
       The design states that each open starts a new segment.
     - **Stale fragments in recycled segments.** A recycled file's block keeps the earlier
       use's bytes after the current turn's last fragment, and an earlier fragment there
       passes its own CRC. The design binds each fragment to its segment number and epoch,
       as RocksDB's recyclable log format binds each record to its log number, or rejects
       a fragment whose turn is not above the previous one. T9 runs with recycled
       segments.
     - **A torn turn's extent.** "Kept if all of them read back whole" (`383`) needs the
       turn's record count or an end marker. Otherwise each whole record of the last turn
       is kept on its own, which is safe because a group stages one batch per turn. The
       design says which.

6. **R9. Closed.** `EntryRef` keeps the core I/O-free, with sends and deliveries held in
   the Worker until their payloads are read (`486-493`); node budgets bound the index at
   384 MiB (`494-496`); shares are a floor plus a write-rate part (`497-499`). Two text
   points are in Condition 4.

7. **R10. Closed.** A node transfer reserve of 8 GiB, at most 2 GiB per transfer
   (`500-503`), and the bound worked through (`572-577`); `consensus`'s T4 application has
   immutable chunks, so the incremental retry runs there (`576-577`). Condition 3 records
   the margin.

8. **R11. Closed** (`523-524`).

9. **R14. Closed.** T3 (`611-623`), T4 at 1,000 groups (`629-630`), T6's device flushes,
   decision rule, and the bare-metal machine (`637-655`), and T8's decoders (`662-671`).

10. **R16. Closed** (`71-81`). The Worker's output bounds are checks, not allocations
    (`worker.h:914, 926`), so following the ceilings costs no memory.

11. **R17. Closed** (`673-678`).

12. **R18. Closed** (`256-259`; T3 at `620-621`).

13. **R19. Partially closed.** The control queue goes before the data queue, appends are
    cut to 256 KiB, and unsent socket bytes stay under 64 KiB, so a control frame waits
    about 2.6 ms at 1 Gb/s (`541-547`). Open: T7 (`656-661`) does not measure
    control-frame latency p99 under saturating appends against a stated bound.

14. **R20. Closed.** The old journal becomes read-only, with a marker-switched migration
    (`406-413`) that T9 runs under power loss at each step (`677-678`).

### C3 against D10 and T5

- **What `afd456d` does right.** `Repair` (`core.h:386-398`) is refused while unstarted,
  busy, or with a delivery outstanding (`388`), on a leader of more than one voter
  (`389`), and outside `[base_index, applied]` or with a term the log does not hold
  (`391`). It then sets `applied_` to the boundary, advances the epoch, emits
  `RepairCompleted`, and delivers the committed entries after the boundary again
  (`395-397`). It touches no hard state, role, leader, commit index, or `base_index`, as
  D10 says (`324-331`). `LocalCheckpoint` rejects another epoch (`core.h:359`). The Worker
  refuses a repair while a checkpoint or a delivery is in flight (`worker.h:565`).
  `Repair` needs no persistence (`admission.h`), and a fenced core ignores it.
- **What the test shows** (`consensus_test.cc:735-793`):
  - A follower restored at every boundary in `[0, 7]` (8 repairs) replays to the leader's
    applied sequence with hard state and role unchanged (`748-760`).
  - Refusals: above `applied` (`762`), another term (`763`), a leader (`764`), below a
    raised base (`774`), and a delivery outstanding (`780`).
  - An epoch-0 capture is rejected and a current one publishes (`766-773`).
  - The repaired voter helps elect a leader and applies what it commits (`783-791`).
- **What T5 still needs** (`631-636`):
  1. R21 and R22 below.
  2. "Damaged at each point": the image is restored after one damage point, every command
     applied. Damage with a delivery outstanding, a second repair during a redelivery, and
     damage before and after a checkpoint are not reached.
  3. "From a local checkpoint and from each member": the test rebuilds the image from its
     own record of applied entries. No image comes from a published checkpoint or over the
     transfer path, which today ends only in `InstallPrepared` above the commit index
     (`core.h:303`); R22 decides which component moves a member's image.
  4. Branches never taken: the busy refusal (`388`, only the delivery case runs), and a
     boundary equal to a nonzero `base_index` (`391`); line `780`'s repair at the raised
     base is refused for its delivery first.
  5. The Worker's repair path: `worker_test.cc` has no repair case, so `worker.h:565` and
     the epoch at `worker.h:853` are untested.
  6. "The node votes throughout": no vote arrives while a redelivery is outstanding.
  7. T2's repairs in the randomized harness (T2 covers C3, `592-596`), and T8's TSan and
     line and branch coverage of the changed files.

### New required changes

21. **R21. The epoch through the Worker (D10, C3).** D10 says captures carry the epoch
    they were taken under (`332-334`). The Worker builds the `LocalCheckpoint` with the
    core's epoch at the moment it steps it (`worker.h:853`), and `CaptureMetadata` has no
    epoch (`worker.h:37`), so a host that took its capture before a repair and attaches it
    after passes the check. The epoch is also in memory only (`core.h:80`) and restarts at
    0. Required: `CaptureMetadata` carries the epoch the host read when it took the
    capture (`Worker` exposes it), and the Worker passes that one; D10 states that
    captures do not outlive the process, or the epoch persists; `worker_test` attaches a
    pre-repair capture after a repair and sees it rejected.

22. **R22. The damaged voter's path (D10, C3).** A host that reports its damaged
    application with `Failed{Application}` on the outstanding delivery fences the core
    (`core.h:379-385`), which ends the repair path. A host that stops acknowledging
    deliveries keeps one outstanding, so `Repair` is refused (`core.h:388`), while the
    commit index moves on. A member's current image then sits above the voter's applied
    cursor: `Repair` refuses it (`core.h:391`), and the install path refuses any image at
    or below the commit index (`core.h:303`). Required, one of:
    - D10 states the host's contract from detection to repair: acknowledge deliveries
      without applying them, never report the damage as `Failed`, so the applied cursor
      keeps pace with the commit index and a member's image falls at or below it.
    - Or `Repair` accepts a boundary in `[base_index, commit]`, advancing the applied
      cursor when the boundary is above it. That is safe for the same reason as a rewind:
      the boundary is committed, and the log holds its term.

    In either case, D10 names the component that moves a member's image for repair, and T5
    repairs from a member's image taken above the damaged voter's applied cursor at
    detection.

### Which code starts now

| Item | Starts now | Waits for |
|---|---|---|
| C1 | All of C1, including the coordinated `DTC2` and storage-format commit (its tags are fixed) | Nothing |
| C3 | Built (`afd456d`) | R21 and R22 in design and code, and T5's items 2–7 above, before its gate |
| C2 prototype | All of D14, quiescence included | Nothing |
| C2 journal format | Nothing | R8's four open points |
| C4 | All of C4 | R19's T7 row before its gate run |

### Conditions

1. **Library commits** (carried). Detent's build and the LIBRARY_BOUNDARIES entry for
   `completion` `64d958b` are still owed, before the coordinated `DTC2` commit, and for
   every library commit after it.
2. **Track D3** (carried). The scatter width that at most 64 peers per node sets.
3. **T4's margin.** At T7's floor of 50 MB/s, the 10 GiB install takes 215 s and pins
   2,147,483,648 bytes of retention, exactly D17's 2 GiB per transfer (`500-503`). T4
   records the transfer rate it achieves and the attempts it takes.
4. **Text.**
   - D12's cost table uses a 20 ms heartbeat (`443-458`), while the default profile's is
     100 ms (`277`); its under-load heartbeat figures are five times too high.
   - T3's transfer bound reads "one election round (two barriers and a round trip, 500 ms
     at the default)" (`615-616`). At `b` = 80 ms an election round is 161 ms; 500 ms is
     about three rounds.
   - D17's index entry lists an index, a term, a kind, a size, and a location in 24 bytes
     (`486-487`); as listed, those take at least 29. D17 states the layout (an implicit
     index, a compact term), or the bound that the larger entry gives.
   - The same-index, same-term content check compares payloads (`core.h:487-491` at
     `b7073bb`). With payloads in the journal, D17 states what it compares: a payload CRC
     kept in the index, or a journal read.
   - The voter-list sites table gains `core.h:389`, the repair's voter count.
   - D11's recovery of 16 GiB at 500 MB/s takes 34 s (`399-400`).

## Third revision check, and C1 as built

- Date: 2026-10-14.
- Reviewed:
  - `docs/design/track-c.md` at strata `b500843` (`git diff 79f7258 b500843`), for R8's
    four open points, R9, R17, R20, and Condition 4; and its progress notes through
    `4fad12e` (`a1ac27b`, `898432c`, `d9c4b4c`, `4fad12e`). Line numbers below without a
    file are `track-c.md`'s at `4fad12e`, whose D2–T9 text is `b500843`'s moved down 31
    lines.
  - `consensus` `main` at `7c60f58`: C1's commits `0307308`, `f050b7a`, `3b20cef`,
    `90d3df0`, `080e1c1`, `91de8ea`, `573bfce`, `e39abee`, `dd82c06`, `056fcb7`,
    `2f8e6e2`, `6cc0d88`, and `7c60f58`. File:line citations into `consensus` are
    `7c60f58`'s.
  - `docs/gates/track-c-c1.md` at strata `f9f7aeb`, the C1 gate report in progress, which
    landed during this review; `docs/design/track-c.md` is unchanged through `83b4306`.
  - Detent `8bf2f83` and `861581a` (`docs/LIBRARY_BOUNDARIES.md`), for Condition 1.
- Checked against: D1–D9, D2's limits and R16, the voter-list site table, the format table,
  T1–T4, T8, and T9; the second revision check's open points and Conditions 1–4; the
  dissertation, §4.1 and §4.2.1.
- Checked by:
  - The existing build at `7c60f58` (Apple clang 17, Ninja, no build type): 18 of 18 tests
    pass in 152.9 s; the unit suite runs 39,372 checks and the Worker suite 94,894, with
    0 failures.
  - ASan and UBSan (Homebrew LLVM 23, `-O1`, `-fno-sanitize-recover=undefined`, leak
    detection off): the unit suite, the Worker suite, the simulation (400 seeds × 10,000
    steps), the liveness test (5 seeds), and the fuzz corpus replay; 0 reports.
  - The shipped simulation at `-O2`: 10,000 seeds × 10,000 steps, 37,308 restarts, 4,037
    committed memberships, no violation. The liveness test at `-O2`, 400 seeds per profile:
    0 failures.
  - Probes in `~/.scratch/review-trackc-c1/probes/`, built by `build.sh`; `harness.h` is a
    deterministic in-memory cluster of real cores. Each finding names its probe.
    `vote-fix/include` is a scratch copy of the headers with M1's change, and
    `sim/simulation_{reordered,heavy}.cc` are scratch copies of the simulation. No
    repository file changed.
  - Not run: fuzzing (`build-fuzz` is in use, so T8's CPU-hours are unverified), TSan,
    coverage, the transport tests under sanitizers, benchmarks, Detent's suite, the gate
    VM, the FreeBSD VM, and the bare-metal machine.

### Verdict

- **D11's journal (`b500843`): APPROVE WITH CONDITIONS.** R8's four open points close, and
  R9, R17, and R20 stay closed. Journal format code for C2 starts now; Conditions 7 and 8
  land in the design before its recovery code, and 9 and 10 before T9.
- **C1 as built: CHANGES REQUIRED.** No check here breaks Raft's safety under membership
  changes, transfers, or witnesses. Six defects lose a group or a node's store after one
  ordinary failure or operation, or leave a stated design rule unbuilt, and T2's harness
  does not show what T2 asks of it. C1's gate waits for M1–M7.

What holds, and reproduces:

- D1: `Configuration` entries carry a validated `Membership` (`membership.h:76-126,
  128-180`); the newest entry in the log is in force and a replaced suffix rolls it back
  (`core.h:167, 248-251, 275-280, 286, 758-759`); the base membership travels in
  `RecoveredState` (`types.h:103`), bundle version 3 (`checkpoint.h:89-94`), the install
  (`core.h:364, 410, 420`), and `RFTSNP02` (`snapshot.h:42, 66, 72-75, 79-94`).
- D3: (a) and (b) (`core.h:516-517`), the single-step rule (`membership.h:108-126`), (c)
  for a learner promoted in place (`core.h:526-531`), and the leaving voter's removal by
  whichever leader holds the committed membership (`core.h:868-874`).
- D4: commit, elections, pre-votes, reads, and CheckQuorum count the voters and witnesses
  in force, the leader only while it is one (`core.h:252-258, 298-300, 692, 696, 844-846,
  950-965`); a leader whose removal committed steps down (`core.h:867`).
- D5: any authenticated incarnation is admitted (`core.h:620`); a ballot names any positive
  incarnation (`core.h:71`; `storage.h:278`); non-voters never campaign (`core.h:832`).
- D6: the target's rules, once per token, `TimeoutNow` resent each heartbeat, and the abort
  after `election_max` (`core.h:550-577, 646, 685-690, 848-850`).
- D7: pre-vote at the next term without a term change at the receiver (`core.h:624-627,
  836-839`), the guard for followers and leaders (`core.h:630, 657`), and CheckQuorum
  (`core.h:842-846`).
- D9: the mixes (`membership.h:95-102`); a witness leader refuses work and hands off to a
  data voter that answered since it was elected (`core.h:511, 581, 599, 851-862`); the
  witness's checkpoint bound (`core.h:460, 763, 937, 940-949`).
- Records: `RFTINIT1` unchanged for all-voter groups, `RFTINIT2` canonical, `RFTJOIN1`,
  and `RFTBTCH2` for batches holding a configuration (`storage.h:41-48, 62-85, 118-119,
  201-236`).
- R16 in the decoders: memberships, storage batches, and bundles are bounded by the
  ceilings (`membership.h:35-38, 147-180`; `storage.h:238-265`; `checkpoint.h:98-106`);
  commands are checked by position at the leader (`core.h:583-591`), and every entry at
  followers and in recovery (`core.h:728-756, 73-81`).
- Safety under randomized faults with crashes on membership seeds, which the shipped
  harness never reaches (M7): `sim/simulation_reordered.cc` (4,000 seeds × 10,000 steps:
  30,111 restarts, 126 committed memberships) and `sim/simulation_heavy.cc` (40,000 seeds
  × 10,000 steps: 303,839 restarts, 5,330 committed memberships), with every oracle
  passing.

What blocks:

- M1: one failure in a promotion's window leaves the group without a leader, for good if
  the failed leader's disk is lost.
- M2: a learner that installs from a founding leader cannot reopen its store.
- M3: through the Worker, learners and joiners never install, and later leaders never send.
- M4: limits by position fail in recovery and for `Configuration` entries, and the
  transport deferral's invariant is false.
- M5: a removed member keeps its flight and its snapshot, and the snapshot blocks the
  leader's checkpoints.
- M6: a voter can join with nothing matched.
- M7: T2's harness never crashes a membership seed and misses three of the seeded faults.

### Status: the third revision check

1. **R8. Closed.**
   - Torn tails across reopens: each open's first record names the last turn it kept from
     the previous epoch; later recoveries ignore that epoch's turns after it and treat
     damage at or before it as corruption (`402-406, 429-432`).
   - Each open starts a segment (`402`).
   - Stale fragments: each fragment carries its segment number and epoch under its CRC
     (`411-413`).
   - A torn turn's extent: an end-of-turn record holds the record count, and the last turn
     is kept only if it and every record it counts read back whole (`416, 427-429`).
   - Three new points are Conditions 7–9; T9's recycled segments, which the second
     revision check required, is Condition 10.
2. **R9. Closed.** The index entry is 32 bytes (`535-537`) and the entry budget 8 Mi, so
   the index stays within 256 MiB per node (`547-549`). Two text points are in Condition
   11.
3. **R17. Closed** (`729-734`), with Condition 10.
4. **R20. Closed** (`454-461`); `b500843` leaves it unchanged.
5. **Conditions 1–4.**
   - Condition 1: closed for `completion` `64d958b` (Detent `8bf2f83`: a clean clone at
     `dcd729a`, every option on, 191 of 191 tests run serially), and recorded for
     `consensus` through `7c60f58` (Detent `861581a`). Detent's build at the coordinated
     commit remains, as before.
   - Condition 2: carried.
   - Condition 3: closed as text (`683-686`); T4 reports the rate and the attempts.
   - Condition 4: closed (D12 at the 100 ms heartbeat, `491-507`; T3's transfer bound,
     `668-670`; D17's layout, `535-537`; the repair site, `186`; recovery time, `447-448`),
     but for the two text points in Condition 11.

### Blocking items

1. **M1. Non-voters refuse votes and pre-votes (D5; `core.h:625, 678`; `249-251`).**
   (`probe_promotion_window.cc`)
   - Both grant paths require the receiver to be a voter in its own membership in force,
     as D5 states for joiners, and the code applies it to every learner and joiner.
   - A promoted learner holds its promotion only once the leader's append reaches it, and
     the promotion commits on three of four voters without it. If leader 1 then fails, 2,
     3, and 4 are a quorum of the four-voter membership, but 4 counts itself a learner and
     refuses both rounds, so 2 or 3 gets two of four. When only 2 holds the promotion, 3
     still uses three voters but cannot win either, since 2's log is ahead. The probe's
     survivors stay leaderless for 30 s at term 1 in both cases, and with the leader's disk
     lost nothing ever changes. D3's table promises one tolerated failure in this window
     (`211`).
   - The dissertation grants votes without consulting the receiver's configuration, with
     this case as its reason: a fourth server's vote is needed once one of three fails
     (§4.1). Safety does not depend on the receiver's role: a candidate counts grants only
     from voters of its own membership (`core.h:692, 696`), and a grant persists before it
     is sent.
   - With grants on term, ballot, and log alone (the probe's `-fix` build), both cases
     elect node 2 at term 2 and commit a new entry. The unit suite (39,372 checks) and 400
     simulation seeds pass with the change. Fixed groups have no learners, so Detent's
     behavior does not change.
   - My first review approved this rule (finding 4). That ruling was wrong.
   - Required: grants depend on the term, the durable ballot, the log comparison, and, for
     pre-votes, leader contact; never on the receiver's role. Non-voters still never
     campaign (`core.h:832`). D5 says so. T3 adds the leader failing in a promotion's
     window and in a replacement's, with its node lost.

2. **M2. A joiner's base membership (D1, D5; `core.h:161, 270-273, 364, 410, 468`;
   `snapshot.h:42`).** (`probe_joiner_recovery.cc`)
   - "Absent" means the sender's founding voters: a source omits a base membership equal
     to its own founding one (`core.h:161, 270-273`), and a receiver keeps the base it had
     (`core.h:364`). A joiner has no founding voters, so it keeps its empty membership.
   - Case A: a leader whose latest checkpoint precedes every change, as it does for a
     group's first learner once the leader has checkpointed, sends `RFTSNP01` to that
     learner. The joiner persists an install with no membership (`core.h:410`), and
     reopened from that bundle the core throws "invalid voters" (`core.h:55-58`).
   - Case B: a joiner that replicates from index 1 and checkpoints below the entry that
     added it records an empty base, which `retain_base_membership` drops because it
     equals the joiner's empty founding membership; the reopen throws the same.
   - Either way, a restart before the joiner's next checkpoint past its own addition
     leaves a store that cannot be opened, and the node rejoins as a new incarnation and
     copies its data again. `7c60f58`'s test covers only a base that already names a
     learner, and turns replay checks off after the install (`consensus_test.cc:1878`).
   - Required: a source sends the base membership (`RFTSNP02`) to any member that is not
     one of its founding voters, so fixed groups keep `RFTSNP01`'s bytes; a node with no
     founding voters refuses an install without a membership (`Invalid`) and a local
     checkpoint whose base membership is empty; tests reopen a joiner after each case.

3. **M3. The Worker's site rules (`184, 185, 187`; `worker.h:269, 583-585, 797,
   803-804`).** (`probe_worker_membership.cc`)
   - `reserve_snapshot` accepts a sender only from the fixed voters (`worker.h:269`). A
     joiner's Worker refuses every sender ("invalid snapshot sender context"), and a
     founding member's Worker refuses incarnation 5, added later; founding voter 1 passes.
   - The source scan visits only `configuration_.voters` (`worker.h:797, 803-804`), so a
     leader never serves a learner's `SnapshotNeeded`.
   - The output bounds use `limits.voters` (`worker.h:583-585`). By their arithmetic, with
     64 members a leader that sends more than 41 full 2 MiB RPCs in one step passes
     `output_payload_` (64 MiB plus 9 × 2 MiB), and the Worker fences (`worker.h:948-960,
     511`). Not probed.
   - The site table puts these rows in C1; "Still owed for C1" (`39-50`) omits them. T4's
     install row passes only at the core.
   - Required: the three rows as the table states them, and T4's learner install through
     two Workers.

4. **M4. Limits by position in recovery and for `Configuration` entries (R16;
   `core.h:53, 78, 524-525, 535-547, 733`).** (`probe_raised_members.cc`,
   `probe_r16_transport.cc`)
   - The core validates a recovered base membership against the node's limits
     (`core.h:53`). A group that raises `members` to 5 on nodes whose limit is 3 adds a
     fourth member; after a checkpoint, the leader cannot reopen ("membership size"). An
     install's membership takes the same path. R16 exists to prevent exactly this for
     batches.
   - Followers (`core.h:733`) and recovery (`core.h:78`) check `Configuration` entries
     against `command_bytes`, but the leader appends them unchecked (`core.h:535-547`), and
     `validate_limits` accepts any positive `command_bytes` (`membership.h:41-50`). A group
     that lowers `command_bytes` to 64 bytes cannot commit its next `Configuration` entry
     (188 bytes; its followers stay at index 3), so it never raises the limit back, its
     log stops committing, and the leader's own log no longer recovers ("invalid recovered
     entry").
   - The deferral's invariant ("A leader refuses group RPC limits above its node's, so
     frames fit", `41-44`) does not hold. The core admits a group `command_bytes` up to the
     node's `rpc_bytes` (`core.h:524-525`), while the Worker's admission
     (`admission.h:60, 73`) and the codec (`codec.h:83, 170`) bound entries by the node's
     `command_bytes`. On default nodes, a 1.5 MiB command under a group limit of 2 MiB is
     refused by all four. The `limits_by_position` test passes only at the core.
   - Required: recovery and installs validate the base membership against the ceilings and
     its own limits; `Configuration` entries are bounded by the membership encoding's
     maximum, not `command_bytes`, at the leader, followers, and recovery (or
     `validate_limits` keeps `command_bytes` above that maximum); the core refuses a group
     `command_bytes` above the node's until C4 moves admission and the codec to the
     ceilings; the design states that a group's nodes run equal node limits until then;
     tests for each.

5. **M5. D4's removal (`233-238`; `core.h:542-544, 866-867, 878, 922-925`;
   `types.h:212`).** (`probe_d3_d4.cc`, case B)
   - A removed member stays in `peers_`: `propose_membership` adds peers
     (`core.h:542-544`), and nothing removes them outside role changes and retirement
     (`core.h:124, 328, 334, 902`). After 4's removal commits, the leader sends it 50
     appends in the next second, and a snapshot it was sending is never released; there is
     no `SnapshotReleaseReason::Removed` (`types.h:212`).
   - That snapshot keeps the leader's checkpoints refused (`core.h:464-466`): the probe's
     leader, not busy, is refused `Busy`. A dead member's transfer times out after 60 s and
     is requested again once the leader's base moves past it (`core.h:878, 922-925`), so
     removing a dead voter does not end the cycle the first review's finding 17 describes.
   - A self-removing leader steps down without `TimeoutNow` (`core.h:866-867`), so every
     self-removal costs an election timeout, 1–2 s at the default profile, which D4 says it
     avoids (`234-235`).
   - Required: when a removal commits, the leader drops the member's `Peer` and releases
     its snapshot with `Removed`; the self-removing leader sends `TimeoutNow` first, or D4
     drops that clause and states the gap; tests for both.

6. **M6. D3(c) for new voters (`195-200`; `core.h:526-531`).** (`probe_d3_d4.cc`, case A)
   - (c) is checked only for a learner promoted in place. A new incarnation with the Voter
     role passes `validate_change` (`membership.h:113-116`) and is admitted with nothing
     matched. With the new node unreachable and voter 3 down, nothing commits (2 of 4).
   - Required: a new incarnation enters only as a learner, or (c) applies to it and refuses
     it until it has matched the commit index.

7. **M7. T2's harness (`645-663`; `simulation_test.cc:127-149`; `tools/mutants.sh:47-49`).**
   - Dispatch: on membership seeds the membership branch takes picks 701–711 ahead of the
     crash (701) and partition (702–705) branches, so no membership seed ever crashes or
     partitions. Seeds 2, 4, 6, 8, and 10 each run 10,000 steps with 0 restarts, against
     5–12 on seeds 1–9 odd. T2 asks for both together (`646-649`), and for a crash between
     promotion and removal (`656`).
   - Coverage: the harness issues no reads and no checkpoints, so no read, install, or
     snapshot crosses a membership change; it never raises or lowers limits (`112`); it
     counts none of the four forced rare schedules (`655-657`).
   - Seeded faults: `tools/mutants.sh` stops at the unit suite (`47-49`), so it records no
     seed for most mutants. Against the simulation alone (`sim/mutants_sim_only.sh`, 2,000
     seeds × 10,000 steps), D3(a) off, D1's rollback off, and a follower checking by its
     node limits are never caught, on either dispatch; a removed leader counting itself is
     caught at seed 258 on the shipped dispatch and not at all on the reordered one; four
     others are caught by seed 15. A leader granting pre-votes is a liveness fault, which
     the unit suite catches and T2 need not.
   - With crashes and partitions on membership seeds the core still passes (What holds),
     so the gap hides no failure I could find; it leaves the harness unable to show it.
   - Required: crashes and partitions on every seed; reads, checkpoints, installs, and limit
     changes in the mix; the four forced schedules counted per seed; each seeded fault
     detected by the randomized harness alone, with its seed recorded; the gate report
     states T2's budget (at least 10^7 steps over 10,000 seeds).

### What C1's gate still owes

Beyond M1–M7, these are not built or not run, and "Still owed for C1" (`39-50`) lists only
part of them:

- T1 (`639-644`): fixtures written at `b7073bb` by a named schedule, and tests that
  `b7073bb`'s readers reject each new tag. The fuzz corpus cannot stand in: its replay
  catches every exception (`fuzz_decoders.cc:105`), so a fixture that stops decoding still
  passes.
- T3 (`664-677`): M1's rows; a removed node's campaigns; the gate's seed count (`ctest` runs
  20 per profile); per-platform measured barriers and three-process runs, deferred below;
  the quiescence rows, which need C2.
- T4 (`678-686`): a node that lost its disk rejoins, which M2 and M3 block today; the
  witness at its limit stalls commits and truncates nothing; a witness replaced while a
  data voter lags; the learner install through the Worker (M3); the 10 GiB install, which
  needs C4's transfer window.
- T8 (`718-727`): one CPU-hour per decoder, reported per decoder, since the target
  multiplexes seven decoders on `data[0] % 7`; TSan; line and branch coverage; the scale
  statements.
- `DTPROF02` (`140`) and the transport's peer and pin sites (`189-191`) move with the
  coordinated commit. Until then membership groups run only at the core and the Worker, and
  `profile_fingerprint` refuses a joiner (`transport/profile.h:16-19`). Required: "Still
  owed for C1" names both and states that limit.

### The gate report at `f9f7aeb`

- T3's table (400 seeds per profile at `6cc0d88`) reproduces exactly at `7c60f58`
  (`sim/liveness-400.log`): under the default profile, a healed partition commits at p99
  250 ms (779 ms worst), a transfer at p99 205 ms, a deaf leader steps down within
  2,019 ms, and a new leader commits at p99 4,233 ms, with 0 of 400 healthy terms raised;
  the other three profiles and the witness rows match too. It has no membership-change
  rows, so M1 does not show.
- T4's install row, "a learner catches up by install ... Pass", holds only at the core. The
  test turns replay checks off after the install (`consensus_test.cc:1878`), which hides
  M2, and no learner can install through the Worker (M3). The row is not a pass.
- T4's append row cites `simulation_test` joiners over 2,000 seeds; no membership seed
  restarts a node (M7), so it shows catch-up without crashes.
- T8: "a six-CPU-hour run" for one target that multiplexes seven decoders is not one
  CPU-hour per decoder; the report states each decoder's share.
- Its "Still owed" lists omit M1–M7, T1, and the Worker's site rules.

### The listed deferrals

1. **R16's transport half to C4** (`41-44`): accepted once M4's last point holds, so frames
   fit by construction. As stated today, the claim is false.
2. **The coordinated `DTC2` commit** (`45-47`): accepted. One version bump for the frame
   header and C2's and C4's tags beats three, and Detent's pins stay where they are until
   then.
3. **Per-platform journal barrier p99:** accepted until C2's journal exists, since today's
   four-barrier journal is being replaced. T3's rows rerun under the measured profiles
   before Phase 3R; until then the profiles in `liveness_test.cc:351-360` are estimates.
4. **Three-process real-storage T3 runs:** accepted until the coordinated commit, when the
   transport can carry membership groups. The fixed-group rows can run now under Detent's
   profile.
5. **Quiescence rows:** accepted; C2 has no quiescence code yet.

### Conditions

5. **The witness's bound (D9; `core.h:763, 937, 940-949`).** (`probe_witness_bound.cc`)
   The witness keeps the largest `min(data_matched, verified)` it has seen, so a later,
   lower report (a promoted data voter that lags) does not lower it: after reports of 5,
   then 2, the witness still checkpoints through 5. D9's rule is the leader's report
   (`333-336`). The bound is the current leader's latest report, clamped to that leader's
   commit index, so the argument rests on committed prefixes only; a test lowers it with a
   promotion.
6. **Timing defaults (R4; `types.h:110-114`).** `Timing{}` keeps 20 ms, 40 ms, and
   100–200 ms. The design's default is Detent's profile (`307-311`), and the first revision
   check measured the old values on 80 ms barriers: 74 leader changes in 500 runs. The
   default becomes the design's, or the design states that hosts pass a profile and
   `Timing{}` is for tests.
7. **One turn in flight (D11; `417-425, 426-433`).** The torn-tail rule judges only the last
   turn of the newest epoch, and damage before a later complete turn is corruption. If a
   core writes turn k + 1 before turn k's barrier returns, a power loss can tear k and keep
   k + 1 whole, and recovery declares k's groups lost although nothing in k was
   acknowledged. The design states that a core writes a turn only after the previous
   turn's barrier returns, and T9 checks it.
8. **The open record (`402-406, 426-433`).** The design does not say when the open record
   becomes durable. Written under the epoch's first barrier, a power loss can tear it and
   keep that turn, and recovery has no rule for an epoch without a readable open record.
   The open syncs its record before the epoch's first turn; an epoch with no turns and an
   unreadable record is discarded; an unreadable record in an epoch with turns is
   corruption, and costs the groups in the previous epoch's last turn, which recovery can
   no longer judge (`450-453`).
9. **Rewritten records (`434-446`).** Reclamation copies a group's live records forward, so
   until the old segment goes, recovery reads two copies. A copy keeps its group and
   sequence number and takes the turn it is written in; recovery keeps one copy per group
   and sequence; a damaged block whose records all have intact copies costs nothing.
10. **T9 with recycled segments (`729-734`).** The second revision check required it for the
    stale-fragment rule, and T9 omits it.
11. **Text.**
    - D17's target counts 16 bytes per index entry (`532-533`); the entry is 32 bytes
      (`535-537`).
    - The content check reads a payload back from the journal when size and CRC match
      (`538-540`). The core is I/O-free, and every retransmitted entry matches. The check
      compares size and CRC-32C only, or the design names the Worker as the reader, before
      the core steps the append, with its bound.

### Which code starts now

| Item | Starts now | Waits for |
|---|---|---|
| C1 | M1–M7's fixes | Its gate: M1–M7 closed, and the owed rows above |
| C2 journal format | `RFTW0001`'s format and append code | Conditions 7 and 8 before its recovery code; 9 and 10 before T9 |
| C2 prototype | All of D14 | Nothing |
| C3 | Built | Its gate, as the second revision check states |
| C4 | All of C4 | R19's T7 row before its gate run |
