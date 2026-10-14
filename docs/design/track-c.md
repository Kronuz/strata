# Track C design: consensus gaps before Phase 3R

PLAN, Track C: C1–C4 pass before Phase 3R starts; C5 (the deterministic simulation
harness with a linearizability checker) follows. This document specifies C1 (membership
changes and voter roles), C3 (repair at or below the commit cursor), C2 (multi-group
operation: design, prototype, and measurement), and C4 (pipelined replication and flow
control on the peer transport). The code lands in `Kronuz/consensus` (and `journal`
where a decision says so); each item's gate is tests and measurements there, reviewed
by the architect. REPLICATION's G1–G7 map to these items: G1 and G2 are C1, G3 is C3,
G4 is C2, G5 is C4; G6 (checkpoint bundles that reference strata manifests) is Phase
3R's, and G7 (Linux NVMe cost) is measured in C2's rows.

## Starting point

`kronuz::consensus::Core` (`include/consensus/core.h`, `consensus` `b7073bb`) is an
I/O-free Raft core for a fixed voter set:

- `FixedConfiguration` (cluster and configuration identities, the local id, 1–31
  voters) is checked at construction, written once by `storage.h`'s initialization
  record, and compared on every recovery; editing it fails recovery.
- One storage batch is outstanding at a time; dependent actions wait for its
  completion. Terms and votes persist before election traffic; commit advances only
  from distinct voters' durable progress at a current-term index.
- One Append RPC is in flight per peer (stop-and-wait). Reads are quorum probes after a
  committed current-term entry; there are no leases.
- Local checkpoints truncate the retained log; `InstallPrepared` installs a peer's
  image above the commit index (an image at or below it is rejected as `CaughtUp`).
- Messages from nodes outside the voter set are dropped. The transport's hello
  fingerprint covers the sorted voter set, so a different set fails admission.

Detent links this core for its authority (3 or 5 fixed voters). Every change here
keeps a fixed configuration's behavior, wire messages, and storage records as they are
(D2, T1), and Detent's tests run unchanged at each item's gate.

## C1: membership changes and voter roles

### D1. Membership lives in the log

- A new entry kind, `Configuration`, carries a `Membership`: members sorted by id, each
  a `Voter`, `Learner`, or `Witness`; at most `limits.voters` voters and witnesses and
  `limits.members` members in all.
- A node uses the newest `Configuration` entry in its log, committed or not, from the
  moment it is appended (Raft dissertation §4.1). Replacing a suffix that held one rolls
  the node back to the newest remaining entry, or to the base membership.
- `RecoveredState` carries the membership in force at `base_index`; checkpoints,
  installs, and snapshot keys carry it too, so a truncated log never loses it.

### D2. A fixed configuration is the initial membership

- `FixedConfiguration`'s voters become the initial membership: every member a `Voter`.
  With no `Configuration` entry ever appended, every check, message, and record is the
  one the core uses today.
- The `configuration` identity names the group, not a voter set. Storage keeps the
  initialization record and adds `Configuration` entries as an entry kind; a store
  without one recovers as before. The record format's version rises only for stores
  that hold one, and an older reader rejects those.
- The transport's fingerprint covers the group's identity and limits, not the voter
  list: the membership is the log's. A membership change never needs a new group or a
  re-export.

### D3. One voter at a time, after the leader's own commit

- A `ChangeMembership` proposal adds or removes at most one voter or witness, and any
  number of learners. Promoting a learner is adding a voter. A replacement is an add,
  then a remove.
- The leader admits it only when (a) it has committed an entry of its current term (the
  no-op), which closes single-server changes' known gap (Ongaro, raft-dev, 2015); (b) no
  `Configuration` entry sits above the commit index; and (c) a promoted learner's
  matched index is at least the commit index at admission.
- Joint consensus is not built: replacements are two steps, and one quorum rule keeps
  commit, vote, and read code single.
- The proposal's result is the configuration entry's commit, reported as for commands.

### D4. Quorums follow the current membership

- Commit, elections, and read probes count the voters and witnesses of the membership
  the node currently uses (D1). Learners are never counted.
- A leader that removes itself keeps replicating and does not count itself; once the
  removal commits it transfers leadership (D6), then steps down. A node not in its own
  membership never campaigns.

### D5. Who may talk to whom

- Append and install traffic is accepted from any authenticated peer of the group (the
  transport binds the group identity, D2), since a follower can lag the membership that
  made the sender leader. A message's term still decides.
- Vote and pre-vote requests are answered for any authenticated peer of the group; a
  candidate counts only grants from its own membership's voters.
- A joining node starts from a join record: the group's identities and its own id, no
  membership. It neither votes nor campaigns until a `Configuration` entry in its log
  names it a voter, and it learns the log by append or install.
- A node whose durable hard state or log was lost never rejoins under its old id, since
  it could vote twice in one term: it joins as a new id (a new incarnation), and the old
  id is removed.

### D6. Leadership transfer

- `TransferLeadership{target}`: the leader stops admitting proposals, brings the target
  to its last index, and sends `TimeoutNow`. The target campaigns at once, with a flag
  that bypasses D7's guard. The transfer ends when the leader sees a higher term, or
  aborts after one election timeout, when proposals resume.
- Used by a self-removing leader (D4), a witness leader (D9), and the host (draining a
  node).

### D7. Pre-vote and the leader-contact guard

- A node whose election timer fires first sends `PreVote` at its next term without
  changing its term or ballot. A peer grants it when the candidate's log is at least as
  up to date and the peer has not heard from a current leader within `election_min`.
  Only a pre-vote quorum starts a real campaign (dissertation §9.6).
- A peer that has heard from a current leader within `election_min` also ignores a
  `VoteRequest` without the transfer flag (§4.2.3). A removed or partitioned node then
  cannot raise a healthy group's term.
- Both guards affect availability only: votes still require the log comparison and the
  durable ballot.

### D8. Learners

- Learners receive appends and snapshots, never vote, are never counted, and never
  campaign. The leader reports each learner's matched index (`progress(peer)`), so the
  host decides when to promote; D3's catch-up check is the core's floor.

### D9. Witnesses

- A witness is a voter whose host keeps the log and hard state and never applies
  commands to an index. Its checkpoints carry a boundary and the membership, no image.
- A witness that wins an election accepts no proposals or reads and transfers
  leadership (D6) to the most caught-up data voter at once.
- A witness truncates its log only through an index every data voter has matched, as
  the leader reports it in its appends. When the only data voter that holds the newest
  entries is down and the other data voter lags past what the witness retained, the
  group waits for it: with two data copies, that copy is the only current one.

## C3: repair at or below the commit cursor

### D10. Application repair

- A voter whose application state fails a checksum (a strata segment, live-docs file,
  or manifest) keeps its log and hard state and keeps voting. Its host restores an image
  at a boundary `b` with `base_index ≤ b ≤ commit`: a pinned local checkpoint, or a
  peer's image through `InstallPrepared` with the new `repair` flag, which the core
  accepts at or below its commit index when its log has the boundary's term.
- `Repair{b}` rewinds the applied cursor to `b`, and the core delivers `b+1 … commit`
  again from its own retained log. Reads wait, as now, for the applied cursor to reach
  their captured index.
- A boundary below `base_index` is rejected: the entries after it are gone, so the host
  installs a peer's image at or above `base_index` instead.
- Loss of the log or hard state is not repair: the node rejoins as a new id (D5).

## C2: multi-group operation

### D11. One journal per core, one barrier per turn

- Each owner core runs one journal that holds the storage batches of every group on
  that core, each record tagged with its group. A core's turn gathers the batches its
  groups staged, writes them, and issues one durability barrier; each group then gets
  its `Persisted` completion. Group commit across groups bounds barriers per core per
  turn at one, whatever the group count.
- A group's records stay ordered within the shared journal; recovery splits the journal
  by group, and each group's semantic recovery is the one it has today. Checkpoints and
  log truncation are per group; the shared journal reclaims a segment once no group
  needs it.

### D12. Coalesced heartbeats and quiescence

- Heartbeats coalesce per node pair: one frame per tick carries `(group, term, commit)`
  for every non-quiescent group the sender leads with a follower on the receiver, and
  one reply carries the followers' acknowledgements.
- A group is quiescent when its leader has no uncommitted entry, no pending read, and
  every voter matched its last index and commit. A quiescent leader sends no heartbeat;
  its followers stop their election timers. Any proposal, read, or message wakes the
  group; a follower also wakes it when gossip membership (DISTRIBUTION) reports the
  leader's node down, and then campaigns through pre-vote (D7).

### D13. Forced checkpoints and snapshot pacing

- A group whose retained log passes its byte or entry limit, or whose slowest follower
  lags past it, checkpoints, and laggards install from it.
- Each node runs at most `snapshot_concurrency` outgoing and incoming transfers, so a
  node returning after downtime with many lagging groups catches up in bounded slices.

### D14. The prototype

- `consensus/bench/multigroup`: N cores of one node, each with G groups on one shared
  journal (D11), three simulated nodes connected in-process, coalesced heartbeats and
  quiescence (D12), and a configurable proposal load. It measures barriers per second
  per node, commit latency (p50, p99), and CPU per committed command, at G·N from 1 to
  1,000 groups.

## C4: pipelined replication and flow control

### D15. Pipelined appends

- The leader keeps up to `limits.pipeline` Append RPCs (and `limits.window_bytes`
  bytes) in flight per peer, advancing `next` optimistically (dissertation §10.2.1). A
  failed or timed-out RPC returns the peer to one flight at its verified progress until
  a success, so the stop-and-wait recovery logic stays as is.
- Responses still match by RPC token and read probe; a probe counts for the flight that
  carried it. Out-of-order responses never move `matched` backwards.
- `pipeline = 1` is today's behavior and the default for a fixed configuration.

### D16. Transport flow control and group multiplexing

- One connection per node pair and lane carries every group's traffic (the control lane
  for votes, appends below a size, heartbeats; the bulk lane for snapshots and large
  appends). Frames carry a group id.
- The receiver grants byte credit per connection from its inbound budget (the
  transport's `ByteBudget`) in window updates; the sender sends within the credit and
  queues per group, round-robin, so one group's backlog cannot starve another. TLS stays
  as the transport has it.

## Gate rows

Each row runs in `consensus` (or `journal` for D11's record format) at the item's
commit; Detent builds against that commit and its tests pass unchanged (T1).

- **T1 Compatibility**: every existing `consensus` test passes; a fixed configuration
  writes byte-identical storage records and wire messages to `b7073bb`'s (fixtures);
  Detent's build and tests pass.
- **T2 Membership safety (C1)**: a randomized simulation of 3–7 nodes with message loss,
  reordering, duplication, partitions, crashes and restarts at every persistence point,
  and membership changes (adds, removes, promotions, replacements, self-removal of the
  leader, witnesses), checking after every step: at most one leader per term; log
  matching; no committed entry lost or changed; the applied sequence one prefix on every
  node; every committed membership a single-voter step from the previous one. At least
  10^6 steps over at least 1,000 seeds, each seed replayable.
- **T3 Membership liveness (C1)**: after healing, each schedule elects a leader and
  commits a new entry within a bound of election timeouts; a removed node's campaigns
  never raise a healthy group's term (D7); a transfer completes within one election
  timeout when the target is reachable.
- **T4 Learners and witnesses (C1)**: a learner catches up by append and by install and
  is promoted; a node that lost its disk rejoins under a new id; a group of two data
  voters and a witness survives either data voter's loss and the witness's, and a
  witness leader hands off at once.
- **T5 Repair (C3)**: an application image damaged at any point is restored from a local
  checkpoint and from a peer at every boundary in `[base_index, commit]`, the
  application state afterwards equals an undamaged replica's, and the node votes
  throughout; a boundary below `base_index` is rejected.
- **T6 Multi-group (C2)**: D14's prototype on the Linux gate VM and the bare-metal
  machine: barriers per second per node at 1, 10, 100, and 1,000 groups, idle and under
  load; commit latency and CPU per command against one group; at 1,000 idle groups, the
  heartbeat traffic and CPU per node with quiescence on and off.
- **T7 Pipelining and flow control (C4)**: replication throughput of one group at 0,
  1, and 10 ms of added round-trip delay (Linux `netem`), pipeline 1 against the
  default; with 100 groups on one connection and one group's follower slowed, every
  other group's commit latency within a stated bound of the unslowed run.
- **T8 Robustness**: the new decoders (membership payloads, coalesced frames, window
  updates, the multi-group journal record) fuzzed one CPU-hour each; ASan, UBSan, and
  TSan over every test; line and branch coverage of the changed files at TESTING's
  targets.

## Open decisions

- D11 changes `journal`'s record framing (a group tag). If `journal` should stay
  single-stream, the alternative is one journal per group with barriers batched by the
  core's I/O driver; T6 measures the one built.
- The membership-change API for Xapiand (who proposes adds and removes: the placement
  group's leader, from gossip and operator input) is Phase 3R's and DISTRIBUTION's.
