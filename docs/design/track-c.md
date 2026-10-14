# Track C design: consensus gaps before Phase 3R

PLAN, Track C: C1–C4 pass before Phase 3R starts; C5 (the deterministic simulation
harness with a linearizability checker) follows, and T2's harness is its first version.
This document specifies C1 (membership changes and voter roles), C3 (repair at or below
the commit cursor), C2 (multi-group operation), and C4 (pipelined replication and flow
control). The code lands in `Kronuz/consensus` and `Kronuz/journal`; each item's gate is
tests and measurements there, reviewed by the architect.

The architect's review (`docs/reviews/track-c-design.md`) approved the shapes with
required changes R1–R15; each section names the changes it carries. Code for an item
starts only after the revision check confirms its required changes:

| Item | Required changes before its code |
|---|---|
| C1 | R1–R6, R13, and T1–T4's targets (R14) |
| C3 | R7 and T5's target |
| C2 | R5's cost statement and T6's target before the prototype; R8 and R9 before journal format code |
| C4 | R10–R12 and T7's target |

REPLICATION's gaps map to the items: G1 and G2 are C1, G3 is C3, G4 is C2, G5 is C4. G6
(checkpoint bundles that reference strata manifests) is Phase 3R's. G7 is carried whole:
its Linux and macOS measurement and the fewer barriers per append are C2's (D11, T6);
its defaults retuned for bulk ingest are Phase 3R's, measured on strata's workload (R15).

## Starting point

`kronuz::consensus::Core` (`include/consensus/core.h`, `consensus` `b7073bb`) is an
I/O-free Raft core for a fixed voter set:

- `FixedConfiguration` (cluster and configuration identities, the local id, 1–31
  voters) is checked at construction, written once by `storage.h`'s initialization
  record, and compared on every recovery; editing it fails recovery.
- One storage batch is outstanding at a time; dependent actions wait for its completion,
  so nothing acts on an entry before it is durable. Terms and votes persist before
  election traffic; commit advances only from distinct voters' durable progress at a
  current-term index.
- One Append RPC is in flight per peer. Reads are quorum probes after a committed
  current-term entry; there are no leases. A leader steps down only on a higher term.
- Local checkpoints truncate the retained log, which lives in memory (an AVL tree of
  entries, up to `log_bytes`, 64 MiB by default). `InstallPrepared` installs a peer's
  image above the commit index.
- Messages from nodes outside the voter set are dropped. The transport's hello
  fingerprint (`DTPROF01`) covers the sorted voters and every consensus limit.
- `journal`'s append takes four ordering points (journal sync, temporary manifest sync,
  rename, directory sync; about 62 ms p50 per durable group on the development Mac), and
  recovery discards bytes past the manifest's frontier.
- An idle three-voter group holds 136,168 heap bytes, 131,376 of them the snapshot
  channel's two per-peer input buffers.

Detent links this core for its authority (3 or 5 fixed voters) and asks for voter
replacement in it (`SCALE_TARGETS.md`): C1 serves Detent too.

## Formats and wire (D2, R1)

### D2. One versioned format, adopted by Detent

- There is one implementation of each mechanism. Fixed and membership groups run the same
  code; a fixed configuration is a founding membership that never changes.
- **Storage.** Stores written before Track C stay readable: the initialization record and
  today's batch magic are read as they are. A batch that holds a `Configuration` entry,
  a join record, or the new checkpoint bundle uses new magics, so an older reader rejects
  them instead of misreading them. A founding member's initialization record is today's
  record, with roles in a new version only when a member is not a voter.
- **Wire.** The codec moves from `DTC1` to `DTC2` for every group, fixed ones included,
  in one coordinated commit that updates Detent's pin and its tests (R15). `DTC2` adds
  the messages and fields below. Two wire paths, one per group kind, are not kept.
- **Fingerprint.** `DTPROF02` covers the wire version, the cluster identity, and the
  node's application profile. It covers neither voters nor consensus limits: one
  connection carries many groups (D16), and membership and limits are the log's.
- **Limits** live in the replicated configuration (D1). A `Configuration` entry may raise
  any limit; the new limit applies to entries after it. Lowering a limit is not an online
  operation: it takes an export into a new group. Raising is safe because every entry
  valid under the old limits is valid under the new.
- Fixed configurations use pre-vote, CheckQuorum, and the leader-contact guard too (D7).

### Format and wire changes

Each change has a version tag, a fixture generated at the item's commit by a named
deterministic schedule, and a test that the previous reader rejects it.

| Change | Tag | Item |
|---|---|---|
| Storage batch with a `Configuration` entry | batch magic `RFTB0002` | C1 |
| Join record (group identities, own incarnation, no membership) | `RFTJ0001` | C1 |
| Initialization record with roles | `RFTI0002` | C1 |
| Checkpoint bundle with the base membership | bundle version 3 | C1 |
| Snapshot descriptor with the base membership | `RFTSNP02` | C1 |
| `PreVote`, `PreVoteResponse` | `DTC2` tags | C1 |
| `TimeoutNow` (term, last index, token) | `DTC2` tag | C1 |
| Vote request transfer flag | `DTC2` field | C1 |
| Witness truncation point in appends | `DTC2` field | C1 |
| Source and destination incarnations, group id in every frame | `DTC2` frame header | C1, C4 |
| Coalesced heartbeat and reply | `DTC2` tags | C2 |
| Shared journal segment block, group record, group checkpoint record | `RFTW0001` | C2 |
| Per-group and connection window updates | `DTC2` tags | C4 |
| Hello profile | `DTPROF02` | C1 |

## C1: membership changes and voter roles

### D1. Membership lives in the log (R2)

- A `Configuration` entry carries a `Membership`: the members, sorted by incarnation;
  the next incarnation number; the limits (D2); and, during a replacement, the incarnation
  it replaces (D3).
- A **member** is a per-group incarnation id (a positive integer, assigned from the
  membership's counter and never reused within the group), the node identity it runs on
  (the certificate-bound node id), and a role: `Voter`, `Learner`, or `Witness`.
- A member's transport identity (address and certificate pin) comes from the **node
  directory**, keyed by node id, which the host supplies: Detent's configuration file
  today, and the root group's node table under Track D. Pins rotate there without a
  `Configuration` entry.
- A node uses the newest `Configuration` entry in its log, committed or not, from the
  moment it is durable (dissertation §4.1). Replacing a suffix that held one rolls the
  node back to the newest remaining entry, or to the base membership.
- The membership in force at `base_index` lives in `RecoveredState`, in the checkpoint
  bundle, in the install, and in the versioned snapshot descriptor (`RFTSNP02`). It never
  enters `SnapshotKey`, which stays a fixed correlation token. A receiver that keeps its
  suffix uses the newest entry there, else the descriptor's.
- Group identities are 128-bit random values, never reused: deleting a group leaves a
  tombstone in its placement group (Track D), so a stale replica cannot join a group
  re-created under the same name.

### The voter-list sites (R2)

Each site that consults the fixed voter list today, and its rule in membership mode:

| Site | Today | With membership |
|---|---|---|
| `core.h:45-50` | Validates the fixed voters | Validates the recovered membership: positive unique incarnations, the local incarnation present (or a join record), at most `limits.voters` voters and witnesses |
| `core.h:60` | A recovered ballot names a voter | A ballot names any incarnation below the membership's next-incarnation counter: a vote may go to a candidate removed since |
| `core.h:207-208` | `member`, `quorum` | Over the voters and witnesses of the membership in force |
| `core.h:293` | Install from a voter | From any incarnation of the group the node directory authenticates (D5) |
| `core.h:427` | Receive from a voter | As above; the frame's destination incarnation must be held locally |
| `core.h:589` | Vote requests to voters | To the voters and witnesses in force |
| `core.h:612` | Peers on election | Every member but the local one; learners included for replication |
| `storage.h:57-61, 181-182` | Initialization record | Founding record unchanged; roles in `RFTI0002`; joining members write `RFTJ0001` |
| `storage.h:116-118` | Recovery expects the fixed set | Recovery derives the membership from the record, then the newest durable `Configuration` entry |
| `storage.h:131, 222` | Ballots name voters | As `core.h:60` |
| `checkpoint.h:77-79` | Bundle embeds the init record | Bundle version 3 carries the base membership and the record |
| `worker.h:251` | Snapshot sender is a voter | Sender is a member of the membership in force at the descriptor's boundary |
| `worker.h:549-551` | Output bounds from `limits.voters` | From `limits.members` |
| `worker.h:761-768` | Snapshot sources scan voters | Scan the leader's peers (members) |
| `snapshot_channel.h:41-42` | Channels per voter, allocated at construction | Allocated per transfer (R9), for any member |
| `peers.h:64-71` | Addresses for the fixed voters | Addresses from the node directory, for the union of the local groups' members (D16) |
| `profile.h:14-26` | Hashes voters and limits | `DTPROF02` (D2) |
| `session.h:112-125`, `tls.h:21-36` | Pins for the fixed voters | Pins from the node directory; the authenticated node id selects nothing until a frame names a group and incarnation |

### D3. One voter at a time; the core completes replacements (R3)

- A `ChangeMembership` proposal adds or removes at most one voter or witness, and any
  number of learners. Promoting a learner is adding a voter.
- The leader admits it only when (a) it has committed an entry of its current term,
  which closes the single-server bug (Ongaro, raft-dev, 2015); (b) no `Configuration`
  entry sits above the commit index; and (c) a promoted learner's matched index is at
  least the commit index at admission.
- **Replacement** is `Replace{new learner, old incarnation}`: one entry promotes the
  learner and records the old incarnation as `leaving`. Any leader that finds a committed
  membership with a `leaving` member proposes its removal as its next configuration
  change. The replacement therefore completes across leader changes and crashes without
  the host; the host only starts it.
- The window is the time between the promotion's and the removal's commits, two commits
  after the learner caught up. What it tolerates:

  | Configuration | Voters in the window | Tolerates in the window |
  |---|---|---|
  | `rf = 3`, all alive | 4, quorum 3 | One failure; a move within one zone puts two voters there, and that zone's outage stalls the group until it returns |
  | `rf = 3`, a dead voter replaced | 4, one dead, quorum 3 | No further failure |
  | `rf = 5` | 6, quorum 4 | Two failures; a zone holding two voters may fail |
  | `rf = 2 + witness`, a dead data voter replaced | A, W, and the new data voter D, plus the dead one; quorum 3 | No further failure; every commit in the window is on two data copies |

- **Failure domains.** DISTRIBUTION's rule that replicas never share a domain holds except
  inside a replacement's window, when the leaving and the new voter may share one. Track
  D3's placement bounds the replacements in flight per failure domain (default one per
  zone per placement group), so a zone outage stalls at most that many groups beyond
  those it would stall anyway.
- **Joint consensus** (dissertation §4.3) is built if Track D's zone-outage simulation
  shows more than 0.1% of a zone's groups unavailable for longer than one replacement
  window during an outage.
- Remove-then-add is never used: it passes through two voters, which REPLICATION rules
  out.

### D4. Quorums follow the current membership (R3)

- Commit, elections, and read probes count the voters and witnesses of the membership in
  force. Learners are never counted.
- A read counts acknowledgements only from voters of the membership in force when it
  becomes ready; the leader's own counts only while it is a voter there.
- A leader that removes itself keeps replicating and does not count itself. When the
  removal commits, it steps down, after sending `TimeoutNow` (D6) to the most caught-up
  voter if one is reachable, and whether or not the transfer succeeds.
- A removed member receives appends until its removal commits. Then the leader drops its
  flight and releases any snapshot to it with `SnapshotReleaseReason::Removed`, so the
  host unpins the source.

### D5. Who may talk to whom, and incarnations (R2)

- Frames carry the group id and the source and destination incarnations. A node answers
  only for incarnations it holds; a frame for any other is dropped.
- Append and install traffic is accepted from any incarnation of the group whose node the
  directory authenticates, since a follower can lag the membership that made the sender
  leader; the term decides. Vote and pre-vote requests are answered the same way, and a
  candidate counts only grants from its membership's voters and witnesses, by
  incarnation.
- A joining node persists a join record (`RFTJ0001`: group identities, its incarnation,
  no membership) before acknowledging anything. It denies vote and pre-vote requests and
  never campaigns until a `Configuration` entry in its log names it a voter or witness.
- A node that lost its durable hard state or log never answers under its old incarnation:
  it could vote twice in one term and forget acknowledged entries (§3.8). It joins as a
  new incarnation, and a replacement (D3) removes the old one.
- A node holds at most one incarnation of a group. A same-node replacement (the disk was
  lost, the node remains) runs only once the old incarnation's state is gone from that
  node: add the new incarnation as a learner, then `Replace`. Since frames and grants are
  keyed by incarnation, one node's grant counts for one incarnation.

### D6. Leadership transfer (R4)

- `TransferLeadership{target}`: the leader stops admitting proposals, brings the target
  to its last index, and sends `TimeoutNow{term, last_index, token}`.
- The target acts once per token, and only if: its term equals `term`; it is the sender's
  follower in that term; its last index equals `last_index`; and it is a voter in its own
  membership. Learners and witnesses are never targets. It then campaigns at once with
  the transfer flag, which bypasses the leader-contact guard (D7).
- The transfer ends when the leader sees a higher term, or aborts after one election
  timeout, when proposals resume (except D4's self-removal, which steps down anyway).
- Used by a self-removing leader (D4), a witness leader (D9), a leader that must repair
  (D10), and the host (draining a node).

### D7. Pre-vote, the guard, and CheckQuorum (R4)

- **Pre-vote** (§9.6). A follower whose election timer fires sends `PreVote` at its next
  term without changing its own term or ballot. A peer grants it when the candidate's log
  is at least as up to date and the peer has not heard from a current leader within
  `election_min`. A pre-vote request never changes the receiver's term. A refusal that
  carries a higher term updates the pre-candidate's term. Only a pre-vote quorum starts a
  real campaign.
- **The guard** (§4.2.3). A peer that has heard from a current leader within
  `election_min` ignores a `VoteRequest` without the transfer flag.
- **CheckQuorum** (§6.2). A leader steps down when fewer than a quorum of its voters and
  witnesses (counting itself only while it is one) have answered any RPC within an
  election timeout (`election_max`). A leader that can send but not receive therefore
  steps down, and pre-vote with the guard cannot hold the group hostage (Howard and
  Abraham, 2020).
- All three affect availability only; votes still require the log comparison and the
  durable ballot.

### Timing profile (R4)

T3's bounds derive from Detent's authority profile, used by fixed and membership groups
alike unless a host states another: heartbeat 20 ms, RPC timeout 40 ms, elections drawn
from 100–200 ms, over real storage whose barrier p99 is at most 80 ms (the development
Mac's measured journal append p99 is 78 ms).

### D8. Learners

- Learners receive appends and snapshots, never vote, are never counted, and never
  campaign. The leader reports each member's matched index (`progress(incarnation)`); D3's
  catch-up check is the core's floor. Quiescence (D12) requires every member matched,
  learners included, so an idle group keeps replicating to a lagging learner.

### D9. Witnesses (R6)

- A witness is a voter whose host keeps the log and hard state and never applies commands
  to an index. Its checkpoints carry a boundary and the membership, no image.
- Allowed mixes: exactly one witness, in `rf = 2 + witness` (two data voters and the
  witness). No other configuration has a witness.
- The founding membership carries roles (`RFTI0002`), so a `2 + witness` group is created
  as one; it never passes through two voters.
- A witness that wins an election accepts no proposals or reads and transfers leadership
  (D6) to the most caught-up data voter at once.
- A witness truncates its log only through an index every data voter has matched, as the
  leader reports it in its appends. At its retained-log limit it refuses appends, so
  commits stall; it never truncates past that point, which would leave committed entries
  on one data copy.
- The residual, in its three cases, each needing a second failure or a lagging copy:
  1. The witness reaches its retained-log limit while a data voter lags: commits stall
     until the data voter catches up or is replaced.
  2. The witness is replaced by a new incarnation while a data voter lags: the new
     witness holds only what it received, so if the current data voter then crashes, the
     group waits for it to return.
  3. The lagging data voter is a new incarnation that must install: it installs from the
     current data voter; while that one is down, the group waits.
  "Down" means crashed. A lost disk on the only current data voter is a second failure,
  which `rf = 2 + witness` does not tolerate.

## C3: repair at or below the commit cursor

### D10. Application repair (R7)

- A voter whose application state fails a checksum (a strata segment, live-docs file,
  or manifest) keeps its log and hard state and keeps voting. Its host restores an image
  at a boundary `b` with `base_index ≤ b ≤ applied`: a pinned local checkpoint, or an
  image from any member of the group, verified whole before activation.
- `Repair{b, term}` is its own event. It changes no hard state, role, leader, or commit
  index. The core accepts it only when the log holds `term` at `b` (or `b` is the base
  boundary), the core is not busy, and no delivery is outstanding; it then sets the
  applied cursor to `b`, raises `base_index` to `b` only when `b > base_index` (keeping
  the suffix), and delivers `b+1 … commit` again from its own log. The Worker resets its
  delivery and application state with it.
- **Epochs.** Repair advances an application epoch. `LocalCheckpoint` captures carry the
  epoch they were taken under, and the core rejects older ones, so a capture of the
  damaged state never becomes the base.
- **Reads.** Linearizable reads wait, as now, for the applied cursor to reach their
  captured index. The host serves no `nearest` or `session` read until `applied` returns
  to its visible position before the repair. Redelivered entries reproduce verdicts the
  leader already reported; the host does not report them again.
- A leader that must repair transfers leadership first (D6), unless it is the only
  current voter.
- A boundary below `base_index` is rejected: the entries after it are gone, so the host
  installs a member's image at or above `base_index`. Loss of the log or hard state is not
  repair: the node rejoins as a new incarnation (D5).
- Safety: the image at `b` is the state of a committed prefix, apply is deterministic
  (REPLICATION), and `b+1 … commit` comes from the voter's own durable log, whose prefix
  equals every other voter's by Log Matching.

## C2: multi-group operation

### D11. One journal per core (R8)

Each owner core runs one journal (`RFTW0001`) for every group on that core. Its
specification lands with this design; its code starts after the revision check.

- **Segments.** The journal is a sequence of segment files of 64 MiB, each preallocated,
  zero-filled, synced, and named in a durable directory entry by a background step before
  it is needed, so appends never create files or change sizes.
- **Blocks.** A segment is a sequence of 32 KiB blocks. Each block starts with a header
  (journal identity, segment number, block number, epoch, CRC-32C of the block). Records
  are framed inside blocks and may fragment across them (first, middle, last), as in
  LevelDB's log. A record is a group id, a per-group sequence number, a kind (batch or
  group checkpoint), and a payload with its CRC-32C.
- **Append protocol.** A core's turn gathers the batches its groups staged, writes them
  into the current segment in one write, and issues one durability barrier: `fdatasync`
  on Linux (the preallocated segment needs no metadata), `F_FULLFSYNC` on macOS, `fsync`
  on FreeBSD. Each group then gets its `Persisted` completion. That is one barrier per
  core per turn, against four today, whatever the group count. There is no manifest
  frontier: a group's records are durable when the barrier after them returns.
- **Recovery and the torn tail.** Recovery scans every live segment from its first block.
  A block whose header or CRC fails, followed by no valid block of the same epoch, is the
  torn tail: the scan stops there. A bad block followed by valid blocks is corruption: the
  scan resynchronizes at the next valid block, and only the groups with records in the
  bad block lose them (below).
- **Per-group state.** Each group's sequence numbers are contiguous; recovery checks them
  per group. A group checkpoint record names the group's base boundary, membership,
  hard state, and application checkpoint reference, and supersedes everything before it
  for that group. Each group has a low-water mark: the first segment holding a record it
  still needs.
- **Reclamation.** A segment is deleted once every group's low-water mark is past it. A
  lagging or idle group pins old segments, so reclamation rewrites such a group's live
  records forward into the head in bounded slices (at most 1 MiB per turn), or forces
  its checkpoint when its retained log passes its share. Space bound: the node retained-
  log budget (R9) plus two segments per core plus the rewrite slices in flight.
- **Recovery cost.** Recovery reads the live segments once, which hold at most the node
  retained-log budget: at the default 4 GiB per node and 500 MB/s, about 8 s, and with
  R9's in-memory index of 16 bytes per entry, its memory is 16 bytes per retained entry.
  T6 measures it at 1,000 groups.
- **Corruption.** A bad block costs only the groups with records in it. A group that lost
  records above its commit index, or any committed record, treats its log as lost and
  rejoins as a new incarnation (D5); every other group is untouched.
- **Moving a group between cores** is not supported in C2; a group moves as a new
  incarnation installed on the target core.
- The per-group journal with barriers batched by the core's I/O driver
  (`IO::sync_files`, `completion` `64d958b`) is the measured alternative in T6.

### D12. Coalesced heartbeats and quiescence (R5)

- **Coalescing.** One frame per node pair per heartbeat interval carries, for each
  non-quiescent group the sender leads with a follower on the receiver: the group, the
  term, the destination incarnation, and the commit index clamped to that follower's
  matched index in the current term (etcd's rule), so a follower never adopts a commit
  past what Log Matching verified. Replies coalesce the same way. Coalesced replies do
  not acknowledge read probes; reads use appends, which wake the group.
- **Quiescence.** A group is quiescent when its leader has no uncommitted entry and no
  pending read, and every member, learners included, matched its last index and commit.
  A quiescent leader sends no heartbeat; its followers stop their election timers.
- **Wake rules.** A follower wakes on any message for the group, on its own restart, or
  when the failure detector reports the leader's node down. A woken follower sends `Wake`
  to the leader and starts its timer; it campaigns (through pre-vote) only if the leader
  does not answer within an election timeout. A quiescent follower grants a pre-vote only
  after its own `Wake` went unanswered. A restarting replica wakes its groups this way
  before any campaign, so a node restart does not elect new leaders in healthy idle
  groups.
- **Notification.** A leader that fences, steps down, or transfers sends `LeaderGone` to
  its followers, which then campaign without waiting.
- **Failure detector.** Until Track D2's gossip exists, the node-pair connection's
  keepalive (one ping per node pair every 500 ms, three missed pings declare the peer
  down) is the detector; gossip later accelerates it. A fenced group on a live node sends
  `LeaderGone`; if the node cannot send, its connections close and the keepalive fires.
- **Cost per node,** with `P` distinct peers per node, `h` the heartbeat interval (20 ms),
  `k` the keepalive interval (500 ms), `A` the groups not quiescent, and `L` the two lanes:

  | Quantity | At rest (all quiescent) | Under load |
  |---|---|---|
  | Keepalive frames per second | `2P/k` | `2P/k` |
  | Heartbeat frames per second | 0 | at most `2·min(P, peers of the A groups)/h` |
  | Connections | `L·P` | `L·P` |
  | Barriers per core per second | 0 | at most one per turn |

  At `P = 1,325` (1,000 replicas placed at random over 100,000 nodes) that is 5,300
  keepalive frames and 2,650 TLS connections per node at rest, and up to 132,500
  heartbeat frames per second under load. **Bound for Track D3:** placement keeps
  distinct peers per node at most 64 (copysets), so at rest a node sends 256 keepalive
  frames per second over 128 connections, and at most 6,400 heartbeat frames per second
  under load.

### D13. Forced checkpoints and snapshot pacing

- A group whose retained log passes its share of the node budget (R9), or whose slowest
  follower lags past it, checkpoints, and laggards install from it.
- Each node runs at most `snapshot_concurrency` (default 4) outgoing and 4 incoming
  transfers. A queued request waits without a timeout (D18).

### D14. The prototype

- `consensus/bench/multigroup`: N cores on one measured node, each with G groups on one
  shared journal (D11) or one journal per group (the alternative), against simulated
  peers on other processes with their own storage, so the measured node's barriers and
  latency are its own. Distinct peers per node vary over 2, 16, and at least 1,000
  simulated peers. It measures barriers per second per node, commit latency (p50, p99),
  CPU per committed command, memory per group, journal recovery time, reclamation with
  one lagging group, a node returning after downtime with lagging groups, a light group's
  p99 beside a heavy one on the same core, and strata's own commit and checkpoint
  barriers counted in the node's budget.

### D17. Memory and storage per group (R9)

- **Today:** 136,168 bytes per idle three-voter group, 131,376 of them the snapshot
  channel's buffers.
- **Target:** at most 8 KiB per idle group (Worker, Core, and peer state without the
  retained log); an active group adds its retained-log index (16 bytes per entry) and its
  send window (D15).
- The retained log moves into the journal (D11) behind an in-memory index of entry
  offsets; payloads are read back for replication and delivery through a bounded cache. A
  node-wide budget (default 4 GiB on disk, 64 MiB of cache) bounds it, and each group's
  share is its byte budget's share. Retention during a transfer then costs disk, not
  memory, so writes do not stall while a learner installs.
- Snapshot buffers are allocated per transfer, from the transfer pool, not per peer.
- Storage admission is one node pool on `ledger`'s capacity and permit core (PLAN), not
  one worst-case reservation per group.

## C4: pipelined replication and flow control

### D15. Pipelined appends (R11)

- The leader keeps up to `pipeline` Append RPCs and `window_bytes` in flight per peer,
  advancing `next` optimistically (§10.2.1). A failed or timed-out RPC returns the peer to
  one flight at its verified progress until a success.
- Responses match by RPC token and read probe; a probe counts for the flight that carried
  it; `matched` never moves back. Keepalives stay outside the window and never count for
  probes.
- **Defaults.** Membership groups: `pipeline = 4`, `window_bytes = 4 MiB`. Fixed
  configurations keep `pipeline = 1` unless their host raises it.
- **Follower batching.** While a batch is persisting, the follower's Worker keeps the
  group's next appends (up to the window), and on completion steps them as one
  `Receive` of contiguous appends from one leader, which the core turns into one storage
  batch. The throughput bound is then `window_bytes / barrier latency` per group: 4 MiB at
  10 ms is 400 MB/s.
- **Sender queues** are per group, bounded at `pipeline` frames and `window_bytes` bytes,
  replacing today's four frames per connection. The window's memory per node is the sum
  over active groups, capped by a node send budget (default 256 MiB); groups beyond it
  run at `pipeline = 1`.

### D16. Multiplexing and credit (R12)

- One connection per node pair and lane carries every group's traffic. All of a group's
  Raft messages use the control lane; snapshot chunks use the bulk lane. A group's
  appends never split across lanes.
- **Credit.** Each connection has a window, and each group on it a window of its own,
  granted by the receiver in window updates, as in HTTP/2 and QUIC (RFC 9113 §5.2, RFC
  9000 §4). The receiver keeps a queue per group, bounded by that group's window, so a
  group waiting on its barrier consumes only its own credit, never the connection's.
  Send-side queues are per group and served round-robin.
- **Authentication.** A connection authenticates the peer's node identity against its
  pin in the node directory (D1). Frames then name a group and incarnations; the receiver
  admits a frame only for a group it hosts and an incarnation it holds. The peer set is
  the union of the nodes in the local groups' memberships: connections open on demand
  and close when no group shares the peer, and directory changes apply without a
  restart.
- **Connections and memory per node** are `2P` and about `2P` TLS sessions' buffers
  (`L·P`, D12), plus the per-group windows of active groups (D15).

### D18. Snapshots (R10)

- A transfer keeps a window of chunks in flight (default 16 chunks of 64 KiB) within the
  group's credit on the bulk lane (D16), instead of one chunk per round trip: at 10 ms
  round trips that is 104 MB/s against 6.55 MB/s today.
- Timeouts measure inactivity: a transfer ends when no chunk is acknowledged for
  `snapshot_timeout` (60 s). A request queued behind the node's limit does not time out;
  its clock starts when it starts sending.
- **Catch-up condition.** A learner catches up when the image's bytes divided by the
  transfer rate is less than the leader's retained log on disk divided by its write
  rate. When it fails (the leader's retention passed the image's boundary before the
  install finished), the host takes a newer checkpoint and transfers only the files the
  receiver lacks: strata's segments are immutable, so a second attempt sends the new
  segments and the manifest. A third failure reports the learner as unable to catch up
  and the host stops adding it.
- One flow-control mechanism: chunks use the group's credit (D16); the snapshot channel's
  own per-chunk credit goes.

## Gate rows (R13, R14)

Each row runs in `consensus` (or `journal` for D11) at the item's commit on macOS, Linux,
and FreeBSD, and Detent builds and passes its suite at the coordinated commit (D2).

- **T1 Compatibility.** Stores written at `b7073bb` recover unchanged (fixtures generated
  there by a named deterministic schedule: a create, appends, a checkpoint, a vote); every
  new format and wire tag in "Format and wire changes" has its fixture and a
  previous-reader rejection test; every existing `consensus` test passes; Detent, at its
  commit with the pin moved, builds from a clean clone and passes its suite run serially
  (`LIBRARY_BOUNDARIES.md:199`).
- **T2 Safety (C1, C3).** A randomized simulation of 3–7 nodes:
  - Faults: message loss, reordering, duplication, partitions, crashes whose restarts
    keep or lose each pending batch at random, clock rates varying per node from 0.5×
    to 2×, and membership changes (adds, removes, promotions, replacements, self-removal
    of the leader, witnesses, learners), and repairs.
  - Oracles after every step: at most one leader per term; leader completeness at every
    election against a global record of committed entries; log matching; no committed
    entry lost or changed; one applied prefix on every node; one grant per voter per term
    across restarts; every grant and successful acknowledgement backed by the sender's
    durable state when sent.
  - Forced rare schedules, counted per seed: a configuration entry in a replaced suffix,
    a crash between promotion and removal, a removed leader with a read and a snapshot
    in flight, and a delayed and duplicated `TimeoutNow`.
  - Seeded faults the harness must detect within the seed budget, with the seeds to
    detection recorded: D3(a) off; D1's rollback off; a self-removed leader counting
    itself; sends before durability; an unclamped heartbeat commit; repair below
    `base_index`.
  - At least 10^7 steps over at least 10,000 seeds, each replayable from its seed. The
    harness is C5's first version.
- **T3 Liveness (C1).** Under the timing profile: after a partition heals, each schedule
  elects a leader and commits a new entry within 1 s at p99 over the seeds (2 s worst);
  a removed or partitioned node's campaigns never raise a healthy group's term; a
  transfer completes within one `election_max` (200 ms) when the target is reachable; a
  leader that cannot receive steps down within one `election_max` and a new leader
  commits within 1 s; a quiescent group's leader node failing yields a new leader within
  2 s (three missed keepalives plus an election); a node restart leaves the leaders of
  its healthy idle groups unchanged.
- **T4 Learners and witnesses (C1).** A learner catches up by append and by install and
  is promoted; a node that lost its disk rejoins as a new incarnation; a `2 + witness`
  group is founded as one, survives any single failure, and its witness leader hands
  off within one `election_max`; the witness at its limit with a data voter lagging
  stalls commits and truncates nothing; a witness replaced while a data voter lags; a
  learner installs a 10 GiB image at 10 ms of added round trip under 10 MB/s of writes.
- **T5 Repair (C3).** An application image damaged at each point is restored from a
  local checkpoint and from each member at every boundary in `[base_index, applied]`,
  and the application's applied sequence and digest afterwards equal an undamaged
  replica's; the node votes throughout; repair is refused while busy or with a delivery
  outstanding; a capture from before the repair is rejected; a leader transfers before
  repairing; a boundary below `base_index` is rejected.
- **T6 Multi-group (C2).** D14's prototype on the Linux gate VM, macOS, FreeBSD, and the
  bare-metal machine when reachable, under BENCHMARKS' trial rules:
  - At most one barrier per core per turn at 1, 10, 100, and 1,000 groups; a barrier is
    one durability call (`fdatasync`, `fsync`, or `F_FULLFSYNC`).
  - 0 barriers and at most `2P/k` frames per second per node at 1,000 quiescent groups.
  - A light group's commit p99 beside a heavy neighbor on one core within 2× its p99
    alone.
  - Memory per idle group at most 8 KiB, excluding retained-log entries.
  - Journal recovery at 1,000 groups within 10 s per 4 GiB retained.
  - Reclamation with one lagging group keeps the journal within D11's space bound.
  - The per-group journal alternative reported beside the shared one, with the decision.
- **T7 Pipelining and flow control (C4).** Replication throughput of one group at 0, 1,
  and 10 ms of added round trip (`netem`), on real storage at the follower: pipelined at
  least 4× `pipeline = 1` at 10 ms, and within 20% of the follower's barrier-bound rate;
  with 100 groups on one connection and one group's follower receive path slowed (its
  barrier delayed 10×), every other group's commit p99 within 1.5× the unslowed run;
  snapshot throughput at 10 ms at least 50 MB/s.
- **T8 Robustness.** One CPU-hour of fuzzing per new decoder: the join record, the
  initialization record with roles, `Configuration` entries, the checkpoint bundle
  version 3, the snapshot descriptor `RFTSNP02`, `PreVote`, `TimeoutNow`, coalesced
  frames, window updates, the `DTC2` frame header, and the shared journal's block, record,
  and group checkpoint; a fixture per format change; ASan, UBSan, and TSan over every
  test; line and branch coverage at TESTING's targets; a scale statement per new
  component (memory, files, and work at 1,000 groups per node); Goal 5's audit of the
  changed `completion`, `journal`, and `consensus` lands in `consensus`'s docs before
  Phase 3R adopts them.

## Coordination with Detent (R15)

- D2's `DTC2` wire and `DTPROF02` profile, D11's journal format, and D15, D16, and D18's
  transport changes change libraries Detent links. On 2026-10-13 the owner authorized this
  session to change those libraries provided Detent still builds and
  `LIBRARY_BOUNDARIES.md` is updated. Each change lands as its own versioned commit in the
  library; the coordinated commit moves Detent's pins and fixtures.
- No Detent session was running on 2026-10-14. Findings and changes that affect Detent
  are recorded in `completion/docs/PLATFORM_FINDINGS.md`, and this section lists each
  library commit and Detent's agreement or its commit as they land.
