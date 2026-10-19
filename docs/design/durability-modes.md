# Durability and replication modes, part A: the consistency spectrum and replication

- Status: **designing (ready for preliminary review)**, 2026-10-18. Part A of two. This file decides
  the acknowledgement and read levels, the commit split, replication by segments, recovery after
  correlated failures, the presets, and what each costs. Part B (memory residency and retention) is
  outlined at the end and designed after A's preliminary review (Q7).
- Request and scope: the owner, 2026-10-18 (`docs/status/durability-modes.md`): split "commit",
  relax persistence per index where replicas hold the data in memory, keep stronger consistency
  where an index needs it, find the indexing win, and let every system on strata choose its side.
- Amends `docs/REPLICATION.md` (guarantee, levels, write path, replication, Xapiand mapping) and
  one row of `docs/DISTRIBUTION.md`; extends Track C's D5, D10, D11, D12, and D18
  (`docs/design/track-c.md`); claims wire fields in the peer transport's D9 (`design/peer-transport`
  at `38a6c6f`); takes small-indexes' D3-A as its checkpoint batcher (`perf/small-indexes` at
  `7c23560`); asks format 13 for two reservations (`phase4/format13` at `bb1e90a`); changes the
  simulator's fault model and checker modes (`design/simulator` at `27cfb30`); reopens
  rocksdb-survey's Q3 (Q5).
- Read at: strata `main` `734949e` (merged here at `c6df449`); consensus `61149a2`; journal
  `cb66f3d`; completion `5251cf9` (strata's pin); Xapiand `master` `7e8cccb7d`, its MIT own code
  only (nothing under `src/xapian`, no `sortable_serialise`); the storage library `671eaac`
  (Xapiand's pin); `research/no-stalls` at `origin` `c72f0f1` (its local `b9dddfd`, unpushed, for
  section 5); Kronuz/cpp-libraries `8814481`; Detent `310e59c`.
- Measured here, as branch evidence for choices, never as gate rows (PLAN, "What waits"): T1's sync
  counts and T2's sizing on the development Mac. Raw files: `bench/durability/results/`; probes:
  `bench/durability/`; derived figures: `bench/durability/arith.py` and its output
  `results/arith.out`. Every other figure is the cited code's or document's, arithmetic, or a
  labeled estimate.

## Summary

- **Commit splits in three** (D1). An acknowledgement waits only for the core's shared journal
  barrier, visibility is refresh, and today's commit protocol becomes a background checkpoint
  batched across indexes. A tiny index's 10 device flushes on the acknowledgement path (measured,
  T1) become a share of one barrier per turn.
- **A new acknowledgement, `memory`** (D2, D3, D7): a majority of voters wrote the batch to their
  journals, and each syncs within `sync_interval`. It survives any number of process crashes and
  the OS crash or power loss of up to f voters; it loses at most the last `sync_interval` plus a
  barrier when more than f voters lose their OS at once. A second cursor, `commit_durable` (a
  majority synced), bounds checkpoints and `quorum` acknowledgements, so no recovery ever loses what
  a checkpoint or a `quorum` write holds.
- **Forgetful voters** (D5): a voter that may have lost acknowledged entries (an OS crash, a power
  loss, a failed barrier) keeps its identity, acknowledges appends, and votes only after it
  re-learns from a leader, as Viewstamped Replication's recovering replica does. A correlated loss
  of more than f voters then recovers by a per-index policy (D6) that never loses a durably
  committed entry and repairs any replica that applied what it lost.
- **Segment mode** (D8): followers append the log but install the leader's segments instead of
  re-indexing. Measured at 100,000 documents on one thread (T2), a follower spends 1.8 to 2.6 µs
  of CPU per document against 42.8 to 129.3 µs in document mode; a three-voter group spends 0.35
  to 0.37 of document mode's CPU (an estimate from measured parts). The leader ships its segments,
  merges included; a promoted follower replays its tail, bounded to one election timeout (D9).
- **One vocabulary** (D10): five acknowledgement levels (`all`, `quorum`, `memory`, `leader`,
  `none`), five read levels on Cosmos DB's spectrum (`linearizable`, `bounded`, `session`,
  `prefix`, `eventual`), and presets (`strict`, `replicated`, `relaxed`; Part B's `resident`;
  D12's `available`) that strato, shale, the log system, and the analytics system share.
- **Partition-tolerant writes** (D12) are designed now and built later, as host-level side logs
  merged by one ordinary command on the one log; leaderless replication is rejected (D13).
- **No stalls and live tuning** (D14, D15): every background task this design adds reports its
  debt to the one controller `research/no-stalls` designs, and each bound says whether it changes
  live, through Detent, or through the log.

| Preset | Acknowledged (default) | Replication | Default read | Loses, beyond the model | Failover gap (estimate) |
|---|---|---|---|---|---|
| `strict` | `quorum` | document | `session` | nothing (D11's media limit aside) | 2.4 s |
| `replicated` | `memory` | segment | `prefix` | `memory` writes of the last `sync_interval` plus a barrier, when more than f voters lose their OS at once | 2.4 s plus a replay of at most 1 s |
| `relaxed` | `leader` | segment | `prefix` | also the leader's writes not yet on a majority, at any failover | as `replicated` |
| `resident` (Part B) | `memory` | Part B | `prefix` | Part B | Part B |
| `available` (D12, later) | `memory`; `provisional` on a minority side | segment | `eventual` on a minority side | the older of two concurrent writes to a key, by its hybrid clock | none for the minority side's writes |

## What exists today (verified)

### Xapiand (MIT own code at `master` `7e8cccb7d`)

| Claim | Evidence | Correction to the lead's analysis |
|---|---|---|
| One cluster-wide Raft elects the master | `src/server/raft_delegate.h:53-123` (delegate of `cluster::Raft`) | Holds |
| Each shard's primary comes from a separate majority poll the Raft leader runs; it picks the eligible active node with the highest revision | `src/server/discovery.cc:653-716` (nodes answer with UUID and revision), `718-801` (the leader counts a majority of the shard's nodes; selection `776-787`) | The range is 653-801 |
| The Xapian database is opened `DB_NO_SYNC` whenever the WAL is built in; durability is the WAL's | `src/database/shard.cc:77-81`, `350`, `356` | Not in the lead's list; it matters for `?commit` |
| The WAL's volume syncs through the debounced fsyncher (`STORAGE_ASYNC_SYNC`) | `src/database/wal.cc:68`, `114`, `129`; `src/xapiand_fsyncher.h:29-51`; storage `storage.h:1090-1113` | Holds |
| A failed asynchronous sync is logged and dropped: nothing retries it or fences the shard | `src/xapiand_fsyncher.h:39-47` | New; an observation for Xapiand's backlog, outside this design |
| `DB_SYNCHRONOUS_WAL` makes the WAL *write* inline, not its sync: the volume stays `STORAGE_ASYNC_SYNC` | `wal.cc:1162-1178` (`execute` against `enqueue`); `wal.cc:676`, `695` | REPLICATION.md:165 maps it to `ack=local`; it is process-crash safety only, `memory` at rf = 1 (D10) |
| `?commit` commits Xapian (no sync), enqueues the WAL commit line, and triggers replication | `search_views.cc:2333-2337`; `shard.cc:1466-1468`, `750-858` (`814-820`, `823-827`) | REPLICATION.md:166 maps it to `ack=quorum` plus `refresh=true`; it gives visibility, not power-loss durability |
| Effective debouncer defaults are set in `opts.cc`, overriding `opts.h`'s initializers | `src/opts.cc:114-136`, `628-650`; `opts.h:114-133` | See the next table |
| Debouncer: the first call fires after `debounce`, later calls after `busy`, capped at a random force time; a throttle spaces fires | `debouncer.h:177-211`, `228-281` | |
| Replicas pull after `DB_UPDATED` with a random 0 to 3 s delay plus the trigger debounce | `discovery.cc:488-545` (`535`) | Not in the lead's list |
| Under the async hook, the storage v2 commit's two syncs around the header write are both deferred, so the data-before-header order is not kept | storage `storage.h:1116-1148` | An observation for Xapiand's backlog, outside this design |

| Debouncer | Debounce | Busy | Forced | Throttle |
|---|---:|---:|---:|---:|
| committer (autocommit) | 1,000 ms | 3,000 ms | 8,000 to 10,000 ms | 0 |
| fsyncher (WAL sync) | 500 ms | 800 ms | 2,500 to 3,500 ms | 1,000 ms |
| db_updater (`DB_UPDATED`) | 100 ms | 500 ms | 4,900 to 5,100 ms | 1,000 ms |
| trigger_replication (pull) | 100 ms | 500 ms | 4,900 to 5,100 ms | 1,000 ms |

What a client can lose today, derived from those constants (not measured):

| Failure | Idle (one write) | Sustained writes |
|---|---|---|
| Primary process crash, then failover to a replica | Writes not yet pulled: commit at 1 s, update at 1.1 s, pull at 1.2 to 4.2 s | Commits forced every 8 to 10 s: up to about 13 s |
| Primary power loss | The unsynced WAL tail: 0.5 s, plus the async writer queue | Up to 3.5 s plus the queue |

The lead's "about a second" is the idle commit delay alone.

### strata, consensus, and journal

- One Raft group per shard (`strata::group`, REPLICATION.md:26-54), with root and placement groups
  above (DISTRIBUTION.md:38-63): leadership is per shard, as in CockroachDB and TiKV.
- Document mode: every voter applies every batch (REPLICATION.md:84-86). The batch carries the
  prepared document (REPLICATION.md:61-63; ARCHITECTURE.md:85-91), so followers repeat stage 1,
  inversion, flush, and merges, never analysis.
- The consensus core holds one storage batch outstanding and acts on it only once `Persisted`
  completes it (`core.h:424-429`, `439-475`); followers persist their hard state, commit index
  included, with each batch (`core.h:785`, `914`); commit counts voters' durable matches
  (`core.h:1146-1161`); a follower refuses to replace an entry at or below its commit index
  (`core.h:868-871`); `Repair` restores an image at a committed boundary and redelivers from the
  voter's own log (`core.h:597-609`); an install at or below the commit index is refused as caught
  up (`core.h:484`).
- The turn log (`RFTW0001`, Track C D11) writes one turn and its barrier at a time
  (`turn_log.h:278-284`; `turn_driver.h:64-99`); each turn ends with a record counting its records
  (`turn_log.h:300-304`); recovery keeps the newest epoch's last turn only whole and calls damage
  before a later complete turn corruption (`turn_log.h:451-453`, `534`, `555`). Standalone
  durability is the same mechanism as a group of one (`workload-profiles.md:100-106`; rocksdb-survey
  K2).
- `index::install` replaces the whole content under fresh local names (`index.hh:775-842`);
  Track C's D18 retry already asks for an install of only the files a receiver lacks
  (`track-c.md:783-789`); the survey's Q7 asks for adopting an externally built segment through the
  log (`rocksdb-survey.md:365`; owner decision `:386-388`).
- Read copies are learners, up to every node (`placement-strategies.md:111-127`).

**Syncs to create one tiny index and commit it, measured (T1)** through a counting IO over
`PosixIO`, with the syscalls counted by an interposed library (macOS, APFS, development Mac;
`results/2026-10-18-t1-macos.txt`):

| Step | Durability calls | `F_FULLFSYNC` | `fsync` | Where |
|---|---:|---:|---:|---|
| `create` | 6 | 6 | 4 | `owner.lock` (completion `posix.h:348-358`); `clear_uncreated`'s directory sync (`index.hh:2281-2292`); the first manifest's metadata file, directory, manifest temporary, directory (`index.hh:2118`, `2153`, `2157`, `2161`) |
| First commit, one document | 4 | 4 | 3 | The segment (`sync_files`, `index.hh:2118`), the directory (`2153`), the manifest temporary (`2157`), the directory (`2161`) |
| Each later commit | 4 | 4 | 3 | The same |

Create plus the first commit: 10 durability calls, 10 device flushes and 7 plain `fsync`s on macOS,
the same for each of 10 indexes. On Linux the same 10 calls are 10 `fsync`s by `posix.h:121-129`,
`175-186`, and `443-453` (derived, not measured here: the gate VM is a shared measurement host).
small-indexes measured 10 on its host (`small-indexes.md:19-21`).

## Decisions

### D1. Commit becomes three steps

- **Durability** is the core's turn-log barrier, shared by every group on the core: one barrier per
  turn today, at most one per `sync_interval` in groups that acknowledge from memory (D3).
- **Visibility** is refresh: memory only, every second by default (ARCHITECTURE.md:105-106).
- **Checkpoint** is today's commit protocol (`index.hh:2077-2167`: the new files synced together,
  the directory, the manifest under a temporary name, the rename, the directory), moved off the
  acknowledgement path. It runs in the background, batched across indexes through one node-level
  commit group (small-indexes' D3-A in SI-R6's form, below), and is published by the group
  checkpoint record that lets the log truncate (`track-c.md:582-587`; condition 25).
- **A checkpoint never records an applied position above the group's durable commit**
  (`commit_durable`, D7). The checkpoint record's own barrier then also covers the voter's log
  through that position. So no recovery after a correlated loss (D6) can drop an entry a
  checkpoint holds.
- **Per tiny index** (derived from T1's counts): the acknowledgement path pays 1/N of a shared
  barrier, N being the groups in the turn; a checkpoint's 4 durability calls run in the background,
  and on macOS 4 device flushes per device per batch once completion has a cross-IO directory
  barrier (SI-R10); create's 6 fold into the first checkpoint, since the journal records the
  creation (small-indexes' D3-C in SI-R11's safe form). Idle indexes rarely checkpoint: the turn
  log copies their live records forward instead (`track-c.md:523-535`).
- Standalone (rf = 1) is the same mechanism, a group of one.

### D2. A new acknowledgement, `memory`

- **Acknowledged when** a majority of voters, the leader included, wrote the batch to their
  journals (the write returned, so each kernel holds it) and the leader applied it, so the reply
  carries the verdict (D7). Each voter's barrier follows within `sync_interval` (D3).
- **Survives** any number of process crashes (the kernel's page cache keeps what was written, D5),
  and the OS crash or power loss of up to f of 2f + 1 voters at once (D5 keeps a voter that lost it
  out of elections until it re-learns).
- **Lost** only when more than f voters lose their OS or power within `sync_interval` plus a
  barrier of acknowledging it, and the recovery (D6) elects a leader whose quorum held no copy.
  Every voter that wrote the batch has synced it by `sync_interval` plus its barrier latency after
  the acknowledgement; from then on it is as safe as `quorum`.
- Precedents with the same guarantee: PostgreSQL's `synchronous_commit = remote_write` ("data
  preservation if a standby instance of PostgreSQL crashes, but not if the standby suffers an
  operating-system-level crash"), MongoDB's majority writes with
  `writeConcernMajorityJournalDefault: false` ("could possibly roll back in the event of a
  transient loss ... of a majority of nodes"), and Elasticsearch's `index.translog.durability:
  async` (fsync every `sync_interval`, 5 s by default, dynamic per index).

### D3. The bounded barrier

- Per core: a barrier at most every `sync_interval` covering every turn written since the last, and
  an immediate one whenever a staged record needs it: hard state (D4), a `quorum` or `all` write,
  a configuration entry, a checkpoint record, or a segment switch.
- Several turns then sit between barriers, which the turn log's one-turn-in-flight rule forbids
  today (`turn_log.h:282-283`; condition 7): an ordinary power loss could tear an earlier turn and
  keep a later one, which recovery reads as corruption. The rule becomes **one write in flight**:
  turn k + 1 is written after turn k's write returns, never before, so turns land in file order.
- **Each end-of-turn record gains the turn its writer last saw synced** (8 bytes beside today's
  4-byte count, `turn_log.h:303-304`). Recovery takes S, the highest such value among complete
  turns of the newest epoch. Turns through S keep today's rules: damage there is corruption and
  costs the groups the block manifests name. After S it keeps the longest whole prefix and drops
  the rest, complete or not: none of those turns was synced, so this voter may have acknowledged
  what it drops and recovers forgetful (D5).
- Damage inside turns after the last recorded S reads as their torn tail, D11's stated limit
  (`track-c.md:560-562`) widened from one turn to those turns. A strict record never sits there
  unsynced for long: it forces a barrier, and the next turn records it synced.
- **Format `RFTW0002`**: the 12-byte end-of-turn payload, a 16-byte kernel boot identity in the open
  record (D5), and a clean-close record (kind `0x12`, synced at shutdown). A turn driver with a
  `sync_interval` writes it; one without keeps writing `RFTW0001`, and both are read. Detent's
  named-file backend is untouched.
- **Default `sync_interval`: 100 ms** (Q2). The turn driver takes the smallest bound among its
  groups' presets and the node's knob (D15).
- **If Track C's T6 keeps the per-group journal instead** (SYS-8), D3 is the same rule on that
  backend: appends are written without the four ordering points, and the frontier update (journal
  sync, temporary manifest, rename, directory) runs at most every `sync_interval` or at once when a
  record needs it. Its recovery already discards bytes past the manifest's frontier
  (`track-c.md:115-117`), so the window is the bytes past it, and it needs no format change; only
  the boot identity and the clean-close marker (D5) are new there. Nothing else in this design
  depends on the backend.

### D4. Hard state and configuration stay durable before anything depends on them

- A term or a vote is synced before the vote reply or any message at the new term (dissertation
  §3.8: persisting the term and vote "is necessary to prevent the server from voting twice in the
  same term"). A configuration entry is synced before its node acts on it: a membership is in
  force from the moment it is durable (`track-c.md:216-218`), and an `acks` change (D7) must not be
  forgotten.
- A commit-index change rides the bounded barrier: §3.8 lets the commit index "safely be
  reinitialized to zero on a restart". `StorageBatch` gains an urgency flag (barrier now, or within
  `sync_interval`); a batch whose hard state changes only the commit index takes the second.

### D5. Forgetful voters

A voter is **forgetful** after an unclean restart of a group whose newest configuration entry in
its durable log allows acknowledgements from memory (D7), and after any failed barrier:

- **Unclean** means the kernel boot identity changed and the journal holds no clean-close record.
  The boot identity is a new completion primitive (additive): Linux's
  `/proc/sys/kernel/random/boot_id`, macOS's `kern.bootsessionuuid` (read on the development Mac),
  and FreeBSD's `kern.boot_id` (r359053, 2020). The open record stores it (D3).
- **Same boot identity** (a process crash, the kernel alive): the page cache holds every turn whose
  write returned. Recovery syncs each segment holding turns after S before it writes the open
  record, and the voter resumes, not forgetful: nothing acknowledged is lost.
- **A failed barrier** fences the core (`core.h:476`); Linux may drop the unsynced pages
  (small-indexes SI-R8), so the voter restarts forgetful and never retries the sync as success.

While forgetful, a voter keeps its incarnation, hard state, and log, acknowledges appends (each
acknowledgement is prefix-complete: the consistency check, `core.h:854-857`, matched its log to
the leader's through the entry), and is counted for commit. It grants no vote or pre-vote, acts on
no `TimeoutNow`, and never campaigns (D6's probes aside), until it **re-learns**: its log matches a
leader of its durable term or later through that leader's last index at their first contact
(`leader_last`, a new append field, D16). The re-learned prefix needs no sync of its own: a vote
reads the log in memory, which holds at least the durable log, and a second unclean restart makes
the voter forgetful again.

A forgetful voter's replies say so (`forgetful`, D16), and the leader then lowers its matched index
for that voter to what the voter holds and resends from there. Today's core never does: "Durable
peers cannot lose a matched prefix on restart; never rewind below verified progress"
(`core.h:957-969`; Track C's D15, "`matched` never moves back", `track-c.md:730-731`), so a voter
that lost acknowledged entries would never receive them again.
Lowering a matched index never lowers a commit already counted, and Track C's promotion check (its
D3) and the witness's bound (`core.h:911-913`) then see the lower figure, as they should.

**Why commits at the memory majority stay safe while at most f voters are forgetful.** Let e be
written by a majority Q_e in term T and counted committed there. Take a later term U whose leader
won a quorum Q_U of voters that are not forgetful. Majorities intersect, so some x is in both.
Either x has had no unclean restart since writing e, and holds e (no leader of a term between T and
U lacks e, by induction on terms, so none truncated it); or x restarted forgetful and re-learned
from a leader of a term below U, which held e by induction, through that leader's last index, so x
holds e again. Since x voted for U's leader, that leader's log is at least as up to date as x's and
holds e (dissertation §3.6.3). The base case is T's own leader, which holds its entries while it
leads. Terms survive every restart (D4), so no voter votes twice. Excluding a forgetful voter from
Q_U costs nothing in the argument: the intersection is non-empty for any two majorities, so it
always holds some other voter.

Precedents: Viewstamped Replication Revisited §4.3 ("When a replica recovers after a crash it
cannot participate in request processing and view changes until it has a state at least as recent
as when it failed"); here the voter keeps acknowledging appends, which Raft's consistency check makes
safe, and stays out of elections only. Kafka's KIP-966 removes a broker that shut down uncleanly
(detected by its clean-shutdown file and broker epoch) from the eligible leaders; Kafka's own
design notes that a replica rejoining the ISR "must fully re-sync again even if it lost unflushed
data in its crash".

D5 also closes D11's stated limit on votes (`REPLICATION.md:183-189`: a vote in a damaged newest
turn is unproven) for groups that allow memory acknowledgements: such a voter restarts forgetful,
votes nothing until it re-learns, and re-learning takes it to its leader's term, past any term whose
vote it may have lost. Extending the rule to `strict` groups would close the limit everywhere, at the
price of D6's wait after every correlated power loss; that is not proposed.

**Residual.** A writeback error between the crash and the reopen that the kernel never reports:
Linux 4.17 and later deliver an unseen error to the next `fsync` once (errseq), but an inode
evicted under memory pressure loses it (PostgreSQL's "Fsync Errors" wiki and the errseq-v4.17
pull). That case is outside the model, as a disk that acknowledges unpersisted flushes is
(REPLICATION.md:17).

### D6. Recovery after a correlated loss

When more than f voters are forgetful at once, no election quorum of non-forgetful voters exists,
and a forgetful voter granting anyone before then could elect a log that lacks a write another voter
still holds. A per-index policy (`unclean_recovery`, a replicated setting, D15) decides when
forgetful voters start granting:

- **`balanced`** (default, Q3). A forgetful voter sends pre-vote requests when its election timer
  fires with no leader, as probes: they carry its log's last index and term and change no term.
  Its recovery condition holds once it has seen, since its restart, a pre-vote or vote request from
  every voter of the membership in force, or once `recovery_timeout` (default 5 minutes, KIP-966's)
  has passed without a leader. From then on it grants a pre-vote or vote only to a candidate whose
  log is at least as up to date (term, then index, §3.6.1) as every log it has seen in those
  requests, its own included, and it campaigns only if its own log is such a log. With every voter
  heard, the winner's log is at least as up to date as every voter's, so it holds every committed
  entry any voter holds, synced or not; after the timeout, every committed entry the reachable
  voters hold. Non-forgetful voters campaign and grant as usual throughout.
- **`aggressive`**: as `balanced`, with `recovery_timeout` of one election timeout. A forgetful
  voter's timer can then end before it hears a voter that holds a `memory` write, so `aggressive`
  can lose one even within the model; it is `relaxed`'s default and never `replicated`'s.
- **`manual`**: never on its own; an operator's command through the strata tool (PLAN, "Recovery
  tooling") lets one election run under `balanced`'s rule.

Each vote reply says whether its voter was forgetful (`forgetful`, D16). A leader whose quorum held
any forgetful grant, its own included, writes a `Recovery` entry first (a new entry kind: its term
and its last index at election), then its no-op, and every append it sends names the term of the
newest `Recovery` entry in its log (`recovery_term`, D16).

A follower replaces a conflicting entry above its `commit_memory` as Raft always does. It replaces
one at or below its `commit_memory` only when the entry is above its `commit_durable` and older than
the append's `recovery_term`, and never one at or below `commit_durable`. Anything else still fails
closed (`core.h:868-871`), so a conflict outside a recovery stays a detected bug. After such a
replacement its `commit_memory` falls to the divergence point, and if its applied index passes that
point, it serves no read until it repairs: `Repair` at its newest checkpoint, which D1 keeps at or
below `commit_durable` and so below the divergence point, then redelivery from its own log (Track
C's D10, `core.h:597-609`). Replicas never diverge.

- **Never lost:** a durably committed entry (synced on a majority M), so every `quorum` and `all`
  acknowledgement. Any recovery quorum intersects M in a voter whose durable log holds the entry, and
  that voter grants only a log at least as up to date as its own.
- **Lost:** `memory` writes no voter of the new leader's quorum held (under `balanced` with every
  voter heard, only those no voter held at all), and `leader` writes never committed. Readers may
  have seen the `memory` ones, since visibility follows `commit_memory` (D7). A session token naming
  a lost write reports it lost (D10).

Precedents: Kafka's `unclean.leader.election.enable` (false by default since 0.11) and KIP-966's
`unclean.recovery.strategy` (Aggressive, Balanced, None; "the one with the highest leader epoch
plus the longest log end offset will be the new leader"); RAMCloud, which after a correlated power
failure waits for backups to reboot unless "an operator can request that the system continue with
some loss of data"; and §3.8: "If a majority of the cluster loses its persistent state ... a
system administrator would need to admit the possibility of data loss." Here the loss is bounded
by `sync_interval` and the policy is per index.

### D7. Two commit cursors

| Cursor | Counts | Drives |
|---|---|---|
| `commit_memory` | A majority wrote the entry (the leader's own write included, as §10.2.1 counts the leader's disk write in parallel) | Apply and its verdicts, `memory` acknowledgements, refresh, and the index a linearizable read captures |
| `commit_durable` | A majority synced it | `quorum` acknowledgements (`all`: every voter synced), the checkpoint ceiling (D1), and the floor below which D6 never truncates |

- **Groups that forbid memory acknowledgements** (`acks: synced` in the configuration entry,
  D16) answer appends only after their barrier, so the two cursors are equal: today's Raft, and
  Detent's fixed groups unchanged. A group switches through a configuration entry, which takes
  effect when committed; switching to `synced` syncs everything before it (D4).
- **Apply follows `commit_memory`** so a `memory` reply carries its verdict: expected-version checks
  and schema checks are evaluated at apply and reported by the leader (REPLICATION.md:146-149). The
  rejected alternative, apply at `commit_durable`, needs no rewind after a correlated loss, but
  every reply with a verdict would then wait for the next barrier, up to `sync_interval` (100 ms),
  slower than `quorum` for a single client. Part B's memory-resident preset needs apply at the
  memory cursor anyway, since its segments are searchable at once.
- **`leader`** acknowledges at the leader's own write, before commit, so its reply names the
  batch's log position and no verdict; `none` acknowledges at admission. Conditional writes need
  `memory` or stronger.
- **Core events**: `Written{token}` (new, additive) before `Persisted{token}`. In a group that allows
  memory acknowledgements the core starts the next storage batch at `Written`, not `Persisted`
  (one write in flight, D3), and releases actions that need durability (a vote grant, a term
  change) only at `Persisted`. A host that never reports `Written` gets today's behavior.
- **Wire**: the append's `commit` carries `commit_memory`, clamped to the verified prefix as today
  (`core.h:914`); new fields carry `commit_durable`, `leader_last`, `sync_through` (the highest
  index a `quorum` or `all` write needs synced now), and `recovery_term` (D6); the append reply's
  `matched` is the written index, a new `synced` field the synced one, and `forgetful` D5's flag;
  coalesced heartbeats and replies carry the same (D16).

### D8. Segment mode

- **Followers append, never index.** The log carries the prepared batch in both modes. A
  segment-mode follower appends and acknowledges it (counted for both cursors), acknowledges its
  deliveries without applying their document batches, as Track C's damaged host does
  (`track-c.md:425-431`), applies only strata's control commands (`Image`, settings), and keeps the
  leader's segment files instead.
- **The leader announces each shippable state in the log.** After a refresh publishes a flush or a
  merge, the leader proposes an `Image` command (a strata command, no new consensus kind) naming its
  state at its applied position p: the segments (identity, size, CRC-32C, SHA-256 from
  Kronuz/sha256), the live-docs generations, and the metadata reference. Document-mode replicas
  skip it at apply, so the digest is unchanged. A segment-mode follower fetches the files it lacks,
  verifies each whole against the sizes and checksums the entry names, and installs the image once
  its files are present and its entry is committed (p is then below `commit_memory`): searchable at
  its next refresh, durable as a checkpoint only once the entry's index is at or below
  `commit_durable` (D1).
- **One mechanism moves segment files**: a pull protocol in strata's host frames on the bulk lane
  (D16), fetching by identity from the leader, or from any member that holds the file once its
  SHA-256 matches the committed `Image` entry (the threat model below), with the requester pacing
  its own fetches across groups. It also carries Track C's D18 catch-up retry
  (the files a receiver lacks), repair's file fetches (Track C's D10), and Q7's bulk adoption;
  consensus's snapshot transfer keeps carrying the image itself (for strata, the manifest), which
  is all Detent sends through it. An install is `index::install` made incremental: today's rewrites
  every segment under a fresh local name (`index.hh:775-842`); the incremental form keeps files the
  receiver already holds and writes only the rest.
- **Not the consensus install path.** An install at or below the commit index is refused
  (`core.h:484`), and the snapshot transfer's handshakes and `snapshot_concurrency` of 4 per node
  (`track-c.md:671-674`) suit whole images, not one image per refresh per group at 1,000 groups.
- **Segment identity never collides across producers**: the producing term in the identity, a
  format-13 reservation ("physical segment identity for segment-mode replication" is on format 13's
  freeze-readiness list, `docs/status/format13.md` on its branch). Files are then physically the
  leader's: scrubs compare CRCs, repair fetches one file, and OpenSearch's renamed-segment collisions
  after promotion (#6369) cannot happen.
- **Promotion.** A segment-mode follower that wins an election rewinds its application to its
  installed position p and replays p + 1 to `commit_memory` from its own log before it applies new
  entries. Checked against consensus's repair path (`core.h:597-609`):
  1. `Repair` refuses a leader of more than one voter (`core.h:600`). Transferring helps no one in
     segment mode, where every follower is equally behind, so `Repair` gains a `promotion` flag
     the core accepts from a leader when its host declares segment mode; document mode keeps the
     refusal.
  2. `Repair` needs its boundary at or above the log's base (`core.h:602`), while a checkpoint is
     only bounded by the delivery cursor (`core.h:562`), which runs ahead of a segment-mode
     follower's installed state. The core gains a checkpoint ceiling the host sets (additive), and
     a segment-mode follower checkpoints only its durably installed image.
  3. `Repair` waits for no delivery outstanding and no persistence (`core.h:599`): after the new
     leader's no-op, one delivery, which the segment-mode host acknowledges at once.
  4. `Repair` advances the application epoch (`core.h:606`), so no capture from before the rewind
     becomes a base.
  5. A position inside an entry rewinds to the entry before it; strata skips the transactions it
     already applied (`index.hh:418-428`).
  During the replay, new writes are appended and committed, and their replies wait for their
  verdicts; reads wait for the applied cursor (`core.h:1176`). D9 bounds the replay.
- **Merges are shipped** (Q4): a follower receives each merge's output. The no-stalls survey's copy
  merges (I5, about 20× cheaper per merged byte, `docs/research/indexing.md` 5.3 via no-stalls
  §2.1) would make replaying merges cheaper than shipping their output; Q4 is revisited then.
- **Mixed versions**: the leader writes only segment formats every member reads; a member that
  cannot read an installed segment applies the batches itself (Track D8).
- **The win, measured on one thread at 100,000 documents** (T2's sizing;
  `results/2026-10-18-t2-macos.txt`, `results/arith.out`):

  | Configuration | Document-mode follower | Segment-mode follower | Group at rf = 3 | Group at rf = 5 | Bytes per follower |
  |---|---:|---:|---:|---:|---|
  | One flush (256 MiB buffer), 1 segment | 42.80 µs/doc | 1.81 µs/doc (23.7×) | 0.370× | 0.240× | 1,179 B/doc of log; 1,614 with segments (+37%) |
  | 16 MiB buffer, 37 segment files, 4 at the end | 129.30 µs/doc | 2.61 µs/doc (49.5×) | 0.350× | 0.218× | 1,179 B/doc of log; 2,394 with segments (+103%) |

  A document-mode follower's figure is apply, flush, merge, and commit (41.3 to 42.3 µs and 124.9
  to 130.1 µs over three runs) plus its log append (1.02 to 1.10 µs); a segment-mode follower's is
  the append plus receiving, verifying, and writing the leader's segment files (0.72 to 0.77 µs and
  1.48 to 1.62 µs). The group columns are estimates from these parts and the leader's one encoding
  of each batch (1.77 µs/doc); they leave out TLS on the extra bytes and the network stack.
  OpenSearch reports segment replication at 40% higher throughput in its first benchmarks and, at
  9 replicas, p50 CPU falling from 69.857% to 8.833%.

### D9. The failover gap, bounded

The gap from a leader's failure to the next acknowledged write, at the default profile (heartbeat
100 ms, elections drawn from 1,000 to 2,000 ms, `track-c.md:376-380`; arithmetic in `arith.py`):

| Part | Document mode | Segment mode |
|---|---|---|
| Detection | The election timer: at most `election_max`, 2,000 ms; a quiescent group adds three missed 500 ms keepalives (`track-c.md:647-650`) | The same |
| Election | A pre-vote and a vote round of two barriers and a round trip each, 161 ms at b = 80 ms (`track-c.md:836-838`) | The same; a forgetful voter waits per D6 |
| First commit | The no-op: a round trip and a barrier (`quorum`) or a write (`memory`) | The same |
| Replay | None | At most `election_min`, 1,000 ms, by the rule below |
| Total (estimate) | About 2.4 s; 3.9 s for a quiescent group | About 3.4 s; 4.9 s quiescent |

- **The replay rule.** The leader proposes an `Image` before a voter's install lag passes
  `election_min` divided by its measured apply cost per document: about 24,000 documents at T2's one
  flush and 7,800 at its 16 MiB buffer. Shipping that often makes more, smaller segments (T2's 16 MiB
  configuration costs the leader 3× the CPU per document), so the rule trades merge work for the
  gap, per index.
- A segment-mode follower whose install lag passes the bound adds the excess to its election timer,
  so the most current follower usually wins; that affects liveness only.
- During the replay, writes are accepted and committed, and their replies wait for their verdicts:
  the gap is latency, not refusal.
- **Leases** (optional, off by default): a leader serves linearizable reads without a probe until
  its heartbeat's start plus `election_min` divided by the clock-drift bound (§6.4.1), and expires
  its lease before transferring. The old lease ends before any follower's timer can fire, so a new
  leader serves lease reads at once and leases add nothing to the gap. CockroachDB's leader leases
  (25.1) report recovery "within seconds".
- After a correlated loss of more than f voters, D6's policy adds its wait: until every voter is
  back under `balanced`, at most `recovery_timeout`.
- `relaxed` adds a loss, not a gap: writes the old leader acknowledged and no majority held are
  truncated at the failover.

### D10. One spectrum, one vocabulary

**Acknowledgement levels** (`ack`), strongest first. REPLICATION.md's `local` retires: at rf = 1 it
is `quorum`, and at rf > 1 a write synced only on the leader is truncated at failover like a
`leader` write (Q6).

| Level | Acknowledged when | Survives | Lost when (window) |
|---|---|---|---|
| `all` | Every voter synced it | Any f failures; every replica current | Never within the model; times out while any voter is down |
| `quorum` | A majority synced it (`commit_durable`) | Any f failures of any kind | Never within the model |
| `memory` | A majority wrote it (`commit_memory`), with its verdict | Any process crashes; the OS crash or power loss of up to f voters | More than f voters lose their OS within `sync_interval` plus a barrier, and D6's quorum held no copy |
| `leader` | The leader wrote it; no verdict | Follower failures; a leader process crash it returns from within an election timeout | Any leadership change before a majority wrote it; the leader's OS crash before its barrier |
| `none` | The leader admitted it; no verdict | Follower failures | Any leader failure before its write; then as `leader` |

**Read levels** (`read`), strongest first, on Cosmos DB's five-level spectrum:

| Level | Served by | Sees |
|---|---|---|
| `linearizable` | The leader, after a quorum read probe (or within its lease) and a refresh covering the captured `commit_memory` | Every write acknowledged at `memory` or stronger before the read started. Get-by-id reads the live version map; searches pay a forced refresh |
| `bounded(K, T)` | Any replica that received a group message from the leader within T (its own clock) and whose visible position is at most K entries behind the commit index that message carried; otherwise it asks the leader for its commit index (one round trip, a follower read index) or forwards | At most K entries and T behind, plus the one-way delay of that message. A quiescent group sends no heartbeats (`track-c.md:635-637`), so its replicas take the round trip after T. Cosmos DB throttles writes to hold its bound; strata never does (D14) |
| `session` | A replica whose visible position is at least the client's token and whose log holds the token's term at its index | The client's own writes and monotonic reads. A token is (term, index, position in batch): REPLICATION's seqno plus the term, so a token whose entry D6 lost reports `lost` instead of matching a new entry at the same index |
| `prefix` | Any replica's latest refreshed snapshot (today's `nearest`, renamed) | The state after some prefix of the one log, which every replica applies in order: Cosmos DB's consistent prefix, never out-of-order writes |
| `eventual` | A replica on a partition's minority side under D12 | Its last prefix plus its own side log's writes; converges after the merge |

**Presets**, each a named set of defaults; every bound is also a knob (D15):

| Preset | Default ack (floor) | Memory acks | Replication | Default read | `sync_interval` | Recovery | Use |
|---|---|---|---|---|---|---|---|
| `strict` | `quorum` (`quorum`) | No | Document | `session` | None needed: every barrier is immediate | Not needed | Writes that must never be lost: authorities (root, placement), Detent's groups, billing-like indexes |
| `replicated` | `memory` (`none`) | Yes | Segment | `prefix` | 100 ms | `balanced` | Most of Xapiand's indexes; strato's default (Q1); shale's default |
| `relaxed` | `leader` (`none`) | Yes | Segment | `prefix` | 100 ms | `aggressive` | Highest single-client ingest; logs and metrics; Kafka `acks=1` users |
| `resident` | `memory` | Yes | Part B | `prefix` | Part B | Part B | Scuba-like analytics (Part B) |
| `available` | `memory`; `provisional` on a minority side | Yes | Segment | `eventual` on a minority side | 100 ms | `balanced` | Writes that must never be refused (D12, later) |

- **Per request**, a write or read may ask for a stronger level at any time and a weaker one down to
  the index's floor (`minimum_ack`, `minimum_read`); a request below the floor is refused, naming
  the floor, and never silently changed. A `quorum` write in a `replicated` index sets
  `sync_through`, so its barriers run at once.
- **The stack speaks this vocabulary**: strato (Xapiand's HTTP API, `ack`, `read`, `refresh`),
  shale (the replicated key-value store), the log system on journal and consensus (Kafka's `acks=all`
  with `min.insync.replicas` is `quorum`, `acks=1` is `leader`, `acks=0` is `none`), the analytics
  system (`resident`, `prefix`), and Detent's authority (`strict`, fixed groups).

| Elsewhere | Here |
|---|---|
| PostgreSQL `synchronous_commit`: `on` (standbys flushed), `remote_write` (standbys' OS has it), `local`, `off` (before the local flush) | `quorum`, `memory`, the retired `local`, `none` |
| MongoDB `w: majority, j: true`; `j: false` majority; `w: 1`; `w: 0` | `quorum`; `memory`; `leader`; `none` |
| Elasticsearch `translog.durability` `request`; `async` | `all` (it waits for every in-sync copy); `memory` over every copy rather than a majority |
| Cosmos DB strong, bounded staleness, session, consistent prefix, eventual | `linearizable`, `bounded`, `session`, `prefix`, `eventual` |
| CockroachDB follower reads (`with_max_staleness`, exact staleness of at least 4.2 s) | `bounded` |
| Xapiand async WAL; `DB_SYNCHRONOUS_WAL`; `?commit` | `none` at rf = 1; `memory` at rf = 1; `refresh=true` |

### D11. Rejected: UDP multicast for data

Bandwidth is not the cost; re-indexing CPU is (D8). Multicast is unavailable on most cloud and
Kubernetes networks, and UDP would need ordering, retransmission, fragmentation, and DTLS. The peer
transport (TLS over TCP) stays.

### D12. Partition-tolerant writes: designed now, built later

- **Behavior.** In an index that opts in (`available`), a replica that cannot reach a leader within
  an election timeout accepts writes into a **side log**: a local journal of operations, each
  stamped with a hybrid logical clock (HLC; Kulkarni et al., OPODIS 2014) and its origin
  incarnation, acknowledged `provisional` at the replica's own durability. Its reads at `eventual`
  see its last prefix plus its side log.
- **Merge on healing.** Once the replica reaches a leader, it submits its side log as one `Merge`
  command, an ordinary command in the one log. Apply resolves each operation deterministically
  against the current state: last writer wins by HLC (ties by origin incarnation) for whole
  documents, and conflict-free merges for typed fields, counters as deltas and sets as
  observed-remove sets (Shapiro et al., 2011). Conditional writes on a minority side are re-validated
  at the merge, where they may be rejected; the merge reports each operation's final verdict.
- **The conflict window** runs from the partition's start to the merge's commit. Two concurrent
  writes to one key keep the later HLC; the other is a lost update, reported. Counters and sets lose
  nothing.
- **What the contract needs now**, so the log and apply leave room for it:
  1. The leader stamps each prepared write with its HLC; the prepare step already resolves
     timestamps (REPLICATION.md:61-63).
  2. The determinism contract: apply's conflict rule is a function of state, operation, and HLC only.
  3. A per-version HLC, reserved in format 13 ("HLC timestamps per version for partition-tolerant
     merges" is on its freeze-readiness list).
  4. The digest's document record includes the HLC only in indexes that enable the preset, so every
     other index's digest is unchanged.
- **Cost if never used**: one setting; an 8-byte HLC per prepared batch in the log; no segment bytes
  (the reserved field stays absent); no apply work.
- **Raft is untouched**: side logs live outside the log until merged, the minority replicas stay
  followers, and a merge is one command. It is not a second replication mechanism.

### D13. Rejected: leaderless replication

Dynamo-style replication (DeCandia et al., SOSP 2007; Cassandra; ScyllaDB's vnodes) as a second
mechanism beside consensus per shard is rejected:

- **Two repair paths**: read repair, hinted handoff, and Merkle-tree anti-entropy, beside the log's
  catch-up, install, and repair.
- **Two failure models**: sloppy quorums beside majorities, so every guarantee in D10 needs a second
  statement.
- **Two test matrices**: the simulator, the checker, and every gate row twice.
- **No total order**: no consistent prefix, no deterministic apply, and no digest to compare
  replicas by, which strata's corruption defense relies on (REPLICATION.md:64-66).
- What it buys, writes during partitions, D12 gives on the one log. ScyllaDB has moved the other
  way: strongly consistent topology on Raft and tablets with one leader per tablet (ScyllaDB 6.0),
  with Raft-based strongly consistent tables announced beside its Paxos lightweight transactions.

### D14. No stalls: what this design adds to background work

Every task below is background work judged at sustained ingest (PLAN, "Background work is measured
under load", PLAN.md:123-128), scheduled on the shared pool, and reported as a debt to the one
controller `research/no-stalls` designs (its I1 proportional admission and I4 one budget across
indexes, `docs/research/no-stalls.md` §5 at `b9dddfd`). This design adds no controller of its own.

| Work | Its debt | Its deadline | The cliff today, and the rule |
|---|---|---|---|
| Checkpoints (D1) | Retained log bytes over the group's share | The share (Track C's D17) | At the retained-log limit, proposals are refused (`core.h:725-730`). Admission slows in proportion to the debt before the limit, and the limit, a load-bearing cap of disk per node (D17's 16 GiB), names its mechanism |
| Barriers (D3) | Unsynced bytes per core | `sync_interval` | A slow device widens the `memory` window instead of stalling: past `max_unsynced` (default 64 MiB per core: the bytes an OS crash can cost a core beyond the interval; a pacing threshold that refuses nothing, with no counterpart in Lucene, Tantivy, or Xapian, which acknowledge only after a commit's syncs) or two intervals late, the node reports it and admission slows |
| `leader` writes not yet committed | Uncommitted entries | `uncommitted_entries` (1,024, `core.h:722`) | Refused at the cap today; `leader` acknowledgements slow with the replication lag first |
| Image shipping and installs (D8) | Install lag per follower | D9's replay bound | Shipping is asynchronous and never blocks a write; a follower past the bound is reported, and the leader flushes earlier |
| Copy-forward (Track C D11) | Segments pinned by idle groups | Two segments per core of slack | Unchanged: bounded slices per turn (`track-c.md:523-535`) |

### D15. Live tuning

Detent's integer knobs are process-wide, with no per-group overrides in v1 (Detent's README), so
node-wide defaults and ceilings live in Detent and per-index values in strata's replicated index
settings, which apply at a log position on every replica.

| Bound | Where | Live | Why it is safe |
|---|---|---|---|
| `sync_interval` | Detent (node), capped by each preset | Yes, at the next barrier | It changes only the `memory` window, which D2 states per interval |
| `max_unsynced` | Detent (node) | Yes | A reporting and pacing threshold |
| Refresh interval | Index setting, with a Detent default | Yes | Visibility only |
| Checkpoint thresholds (bytes, time) | Detent (node) | Yes | Within D17's share either way |
| `recovery_timeout` | Detent (node) | Yes | Liveness only |
| `unclean_recovery`, read and ack floors, defaults | Index setting (replicated) | Yes | Every replica decides by the same log position |
| `acks: synced` or `written` | The group's configuration entry | Yes, through the log | In force when committed; the switch to `synced` syncs everything before it (D4) |
| Replication mode | Index setting (replicated) | Yes, through the log, and heavy | Document to segment: followers stop applying and take the leader's next image whole, since their files differ. Segment to document: followers replay from their installed position, which D9 bounds |
| Lease reads | Index setting | Yes | Off takes effect at once; on, after one election timeout |
| Turn-log geometry, segment identity format, journal version | Build or format | No | Formats change through their reviews and activation (Track D8) |

### D16. Formats and wire

Every field below has a version, a fixture, and a previous-reader rejection test (T12). The wire
fields are version 2's bodies, so they land before the coordinated `DTC2` commit, since "a kind
added later is version 3" (`track-c.md:177-180`; `peer-transport.md:663`); otherwise they wait for
version 3, and the peer transport's D7 keeps the feature off in a group while any member's session
runs the older version (`peer-transport.md:523`).

| Change | Where | Size and rule | Owner, when |
|---|---|---|---|
| `VoteResponse` (kind 2) + `forgetful` | consensus wire | 56 to 64 bytes; kinds 1, 6, and 7 are unchanged, since a forgetful voter decides by its own recovery condition (D6) | This design into peer transport's D9 (`peer-transport.md:612`), before the coordinated commit |
| `AppendRequest` (kind 3) + `commit_durable`, `leader_last`, `sync_through`, `recovery_term` | consensus wire | 104 to 136 bytes without entries; `control_frame_bound` becomes `136 + 32·rpc_entries + rpc_bytes` (`codec.h:10-15`); "kind 3 is control when its length is 136" | The same; changes `peer-transport.md:602` |
| `AppendResponse` (kind 4) + `synced`, `forgetful` | consensus wire | 96 to 112 bytes, 112 to 128 with its compacted boundary (`peer-transport.md:614`) | The same |
| Coalesced heartbeat and reply records (kinds 9, 10) | consensus wire | Heartbeat record + `commit_durable`, `leader_last`: 48 to 64 bytes; reply record + `synced`, `forgetful`: 48 to 64 bytes (`peer-transport.md:618`) | Track C's C2 bodies, before the coordinated commit |
| strata::group's host frames: a segment fetch (control) and its chunks (data) | Host kinds | strata::group takes host control kinds 256 to 271 and host data kinds 512 to 527 (`peer-transport.md:627-628`), the rest left to the application; the frames name their group inside, since host kinds are node-level | strata, Phase 3R; recorded in peer transport's D9 |
| `Membership` + optional `acks` (`synced`, `written`) | consensus log format | Absent reads as `synced`, so old configurations and Detent's keep their bytes; applies by position, as limits do (R16) | consensus, with C1's format changes; activation after every node runs it (peer transport's D7) |
| `EntryKind::Recovery` | consensus log format | Payload: term and last index, 16 bytes; older readers refuse the kind (`codec.h:78`, `170`) | consensus |
| `Written` event, `Repair{promotion}`, the checkpoint ceiling, `StorageBatch`'s urgency | consensus API | Additive; absent, today's behavior | consensus |
| `RFTW0002` | journal format | End-of-turn payload 4 to 12 bytes; the open record's boot identity (16 bytes); clean-close record `0x12`; `RFTW0001` still read | journal |
| Kernel boot identity; a cross-IO directory barrier (SI-R10) | completion API | Additive | completion |
| `Image` and `Merge` commands; the session token's term | strata log commands and API | Versioned with strata's command encoding; `Image` names each file's size, CRC-32C, and SHA-256 | strata |
| Producing term in segment identity; per-version HLC | strata format | Reservations in format 13, so both arrive without a format 14 | Format 13's freeze-readiness check |

## Threat model

What this design adds to the peer transport's (`peer-transport.md:205-228`): the segment fetch, the
new message fields, and the forgetful and recovery signals. Members are trusted as Raft trusts them
(REPLICATION.md:8-12: "nodes are not Byzantine"); the rows below say where that matters.

| Actor | Capability | Stopped by | Residual |
|---|---|---|---|
| A node with a pinned key that holds no member of the group | Fetch an index's segment files | Each fetch names its group and the requester's incarnation; the server serves only an incarnation a committed membership binds to the requesting node (the peer transport's D5), else drops and counts | None beyond its handshake slots |
| A compromised member serving files | Serve forged bytes to followers that fetch from it | A file fetched from anyone but the leader must match the SHA-256 in the committed `Image` entry, so it corrupts no one but itself; CRC-32C covers corruption from the leader | The leader is trusted, as Raft trusts it. Hashing every shipped byte once on the leader and once on the follower is a cost T2's procedure measures; until then it is an estimate |
| A compromised member lying in replies | Claim `synced` above what it synced; claim `forgetful` | Nothing in consensus: a voter that lies about durability breaks Raft's assumption | A false `synced` can make `commit_durable`, and so a `quorum` acknowledgement, count a copy that is not on disk; a false `forgetful` costs only its own resends. Both are the compromised-voter row of the peer transport's threat model (`peer-transport.md:213`) |
| A compromised leader | Write a `Recovery` entry or a false `recovery_term` | Followers still refuse to replace anything at or below the `commit_durable` they have learned, which only grows (D6), so no entry they know is durably committed goes | It can roll back `memory` writes above `commit_durable`, as a compromised leader can already stall or fence its group |
| A member that floods fetches | Exhaust the server's disk bandwidth or credit | Host control frames are capped at 4 KiB and 8 pending per connection, host data at its credit (`peer-transport.md:627-628`, D11); the server serves at most a fixed number of fetches per requesting node at once | Its share of the bulk lane |
| Local access on a node | Fake a boot identity or a clean-close record | Whoever writes a node's journal is that node (the peer transport's local-file row) | A false clean close keeps a voter that lost entries in elections, as a deleted journal would |

## The fault model per preset

| Failure | `strict` | `replicated` | `relaxed` |
|---|---|---|---|
| Process crashes, any number, kernels alive | Nothing lost | Nothing lost (D5's same-boot rule) | Writes on the leader alone, if another voter is elected before it returns |
| OS crash or power loss of at most f voters | Nothing lost | Nothing lost (D5) | The leader's unreplicated writes, if it is among them; under `aggressive`, also `memory` writes held only by a voter not yet heard (D6) |
| OS crash or power loss of more than f voters at once | Nothing lost, but D11's stated limit (damage in the newest turns) | `memory` writes of the last `sync_interval` plus a barrier that no recovery quorum held (D6); never `quorum` writes | As `replicated`, plus the leader's unreplicated writes |
| Disk loss of at most f voters | Nothing lost: a new incarnation re-replicates (`track-c.md:321-323`) | The same | The leader's unreplicated writes, if its disk |
| A partition | The minority side refuses writes; `prefix` and `bounded` reads on any side; `linearizable` on the majority side | The same | The old leader acknowledges until CheckQuorum steps it down (at most `election_max`); those writes are truncated at the heal |
| A leadership change with no failure | Nothing lost | Nothing lost | Writes on the old leader alone |

## How small-indexes' D3 relates

- **D3-A** (batched syncs over today's files) is the checkpoint batcher, in the form SI-R6
  recommends: no host API, `commit()` stays the one call, and a node-level commit group shares the
  syncs of commits already waiting. SI-R9's condition holds: it is the engine's one sync batcher
  below `commit()`, and D1's background checkpoints use it. Its failure cases carry over, with SI-R7
  and SI-R8's corrections; what changes is that a failed checkpoint no longer fails an acknowledged
  write, which the journal holds.
- **D3-B** (a shared journal) is D1 and D3 on Track C's turn log, so it is replaced here.
- **D3-C** (fewer syncs at create) composes, in SI-R11's safe forms: the journal's creation record
  makes the deferral safe.

## Changes to other documents

Each is the owning document's to make; the lead applies the ones on `main`.

| Document | Change |
|---|---|
| `REPLICATION.md:8-24` | The guarantee gains `memory`'s line (D2), and the outside-the-model list gains "more than f voters losing their OS within `sync_interval`" for `memory` writes |
| `REPLICATION.md:68-86` | The write path shows the two cursors (D7) and segment mode beside document mode (D8) |
| `REPLICATION.md:117-149` | The acknowledgement and read tables become D10's; `local` retires and `nearest` becomes `prefix` (Q6); tokens gain the term |
| `REPLICATION.md:151-158` | Cost: the bounded barrier (D3) |
| `REPLICATION.md:160-172` | Xapiand mapping: `DB_SYNCHRONOUS_WAL` is `memory` at rf = 1; the async WAL is `none` at rf = 1; `?commit` is `refresh=true` |
| `REPLICATION.md:183-189` | D11's limit widened to the turns after the last recorded synced turn (D3) |
| `DISTRIBUTION.md:111` | "Replaced by quorum-acknowledged journals" becomes "by journals acknowledged at the index's level (D10)" |
| `PLAN.md:552-553` | Phase 3R lists document-mode replication; segment mode and `memory` join its scope, or follow it (the lead's call) |
| `track-c.md` D5 (`:301-327`) | Forgetful voters keep their incarnation; losing durable hard state or the log still means a new incarnation |
| `track-c.md` D10 (`:419-456`) | `Repair{promotion}` (D8); D6's truncation path and repair |
| `track-c.md` D11 (`:460-625`), conditions 7 and 24, T9 | One write in flight; the end-of-turn synced field; `RFTW0002`; the clean-close record; T9's multi-turn rows |
| `track-c.md` D12 (`:627-667`), D18 (`:783-789`) | The records' new fields; the catch-up retry's files through strata's fetch |
| `track-c.md` D15 (`:730-731`) | "`matched` never moves back" gains its exception: a reply flagged `forgetful` lowers it (D5) |
| `track-c.md` format table (`:182-200`) | D16's rows |
| `peer-transport.md` D9 (`:587-672`) | Kinds 2, 3, 4, 9, and 10 at D16's sizes; strata::group's host kind ranges |
| `simulator.md` D5, D6, D9 | A process crash that keeps the page cache, apart from an OS crash or power loss that drops unsynced writes, with the boot identity; a forgetful voter counts against f until it re-learns; the checker's `memory` mode is definite within the model and unknown beyond it, `leader` unknown |
| `small-indexes.md` D3 | As the section above |
| `format-13.md` | The two reservations of D16 |
| `rocksdb-survey.md:318`, `:386-388` | `memory` at rf = 1 is the process-crash-only setting, so RocksDB's WAL-without-sync rows compare against it (Q5) |
| `workload-profiles.md:100-106` | Standalone durability offers `memory` and `quorum` (Q5) |

## Prior art

| Source | What it shows | Taken here |
|---|---|---|
| Ongaro's dissertation (2014), §3.8 (`basicraft/consensus.tex` at `0c10ea1`) | Terms, votes, and entries persist before they count; the commit index may be lost; "If a majority of the cluster loses its persistent state ... a system administrator would need to admit the possibility of data loss" | D4; D6's bounded, per-index version of that admission |
| The same, §6.4.1, §10.2.1, §11.6 (`clients/clients.tex`, `performance/performance.tex`, `related/primarybackup.tex`) | Leases of `election timeout / clock drift bound`, with clients tracking the index they saw; the leader's disk write in parallel; a primary-copy primary "reflects uncommitted entries" and must roll back after losing leadership | D9's leases and D10's tokens; D7's leader counting itself; D8 applies only committed entries, so nothing rolls back outside D6 |
| Liskov and Cowling, "Viewstamped Replication Revisited" (MIT-CSAIL-TR-2012-021), §4.3, §5.1 | A recovering replica takes no part until its state is as recent as when it failed; "The log can be pushed to disk in the background"; failure independence, UPSes, or non-volatile memory cover power failures | D5's re-learning, kept out of elections only; D3's background barrier |
| Apache Kafka's design ("Replicated logs" and "Unclean leader election", as hosted at docs.confluent.io/kafka/design/replication) | No fsync requirement; a rejoining replica "must fully re-sync again even if it lost unflushed data"; `unclean.leader.election.enable` false since 0.11 | D5; D6's policy; D10's mapping of `acks` |
| KIP-966, Eligible Leader Replicas (Accepted; KAFKA-15332) | Unclean shutdown detected by a clean-shutdown file and broker epoch; such a broker leaves the eligible set; unclean recovery Aggressive, Balanced (default), None, after 5 minutes, electing "the highest leader epoch plus the longest log end offset" | D5's detection; D6's three policies and timeout |
| OpenSearch segment replication (docs.opensearch.org; issues #2212, #3988, #6369, #9027) | Replicas copy segments and keep a translog; 40% throughput and lower CPU reported, a loss at 9 replicas; no `wait_for` refresh; gets go to the primary; a promoted replica re-indexes its translog's tail; renamed-segment collisions; the oldest version promoted in mixed clusters | D8's mode, promotion, segment identity, and mixed-version rule |
| CockroachDB (follower reads, leader leases, `kv.raft_log.disable_synchronization_unsafe`) and TiKV (`raftstore.sync-log`) | Bounded-staleness follower reads; leader leases; an unsynced Raft log only for tests, "risks data loss or data corruption"; TiKV removed `sync-log` in 5.0 and forces it on | `bounded`; leases; acknowledgements from memory kept apart from the durable cursor, so Raft's safety never rests on an unsynced acknowledgement except through D5's re-learning |
| Azure Cosmos DB, consistency levels (learn.microsoft.com, 2025-09-03) | Five read levels on one replication protocol; bounded staleness by K versions or T time, at least 10 writes or 5 s in one region, 100,000 or 300 s across regions, with writes throttled past the bound; overrides apply to reads | D10's read levels; bounded reads that never throttle writes (D14) |
| PostgreSQL (`synchronous_commit`; asynchronous commit) | `remote_write` survives a standby's process crash, not its OS crash; asynchronous commit risks three `wal_writer_delay`s (200 ms by default) of data, never consistency | D2's guarantee; D10's mapping |
| MongoDB write concern | `writeConcernMajorityJournalDefault: false` acknowledges from memory and may roll back after a transient loss of a majority | D2; D6's rollback, here a repair |
| Elasticsearch `index.translog.durability` | `async` fsyncs every `sync_interval` (5 s by default, at least 100 ms), dynamic per index | D3; D15 |
| Ongaro et al., "Fast Crash Recovery in RAMCloud" (SOSP 2011) | Backups buffer the log in DRAM and answer at once, which needs batteries or capacitors to be as durable as disk; after a correlated power failure, wait for backups or accept loss | D2's explicit window, without special hardware; D6 |
| Abraham et al., "Scuba" (VLDB 2013); Goel et al., "Fast Database Restarts at Facebook" (SIGMOD 2014) | In memory, pruned by age and space every 15 minutes, best-effort partial results; asynchronous disk writes, "losing a tiny amount of data ... acceptable"; shared memory across clean restarts only (2 to 3 minutes against 2.5 to 3 hours from disk) | Part B |
| ScyllaDB and Cassandra time-window compaction | Windows compacted within, never across; whole expired SSTables dropped; one TTL per table; 20 to 30 windows | Part B |
| DeCandia et al., "Dynamo" (SOSP 2007); ScyllaDB 6.0 | Hinted handoff, read repair, and Merkle trees; Raft topology and tablets | D13 |
| Kulkarni et al., HLC (OPODIS 2014); Shapiro et al., CRDTs (SSS 2011) | Causal timestamps close to physical time; conflict-free types | D12 |
| Linux errseq (4.13; errseq-v4.17); PostgreSQL's "Fsync Errors" wiki | An unseen writeback error is reported once to the next `fsync`; an evicted inode loses it | D5's residual |
| Boot identities: Linux `boot_id`; macOS `kern.bootsessionuuid`; FreeBSD `kern.boot_id` (r359053) | A per-boot identifier on every platform strata targets | D5's detection |

## Gate rows

strata's rows run in strata; consensus's and journal's in their repositories at the item's commit,
on Linux, macOS, and FreeBSD. Performance rows (T2, T8, T13, T14) are the lead's, on `main`.

| Row | Target | Procedure |
|---|---|---|
| T1 Syncs per tiny index | Under D1, creating an index and acknowledging its first write issue no sync of their own on the acknowledgement path: the write waits for its turn's barrier, at most one device flush per core per turn whatever the group count; a checkpoint costs at most 4 durability calls per index and, on macOS, at most 4 `F_FULLFSYNC`s per device per batch. Today (measured, macOS): 10 calls and 10 device flushes for create plus the first commit | `bench/durability/count_syncs` with its counting IO at 1, 10, and 1,000 indexes; syscalls by `syncspy` on macOS, `strace -c -f -e trace=fsync,fdatasync,sync_file_range` on Linux, `truss -c` on FreeBSD. Counts, no hold |
| T2 Segment-mode CPU | At 100,000 documents of the 1M corpus's first lines on one thread: a segment-mode follower's CPU per document at most 0.1× a document-mode follower's, in both of T2's buffer configurations; a group's at most 0.45× document mode's at rf = 3 and 0.30× at rf = 5, TLS and the network stack included. This design's sizing, without them: 0.042× and 0.020× per follower; 0.370× and 0.350× at rf = 3; 0.240× and 0.218× at rf = 5 | `bench/durability/sizing`'s phases driven through strata::group's real apply and install paths, with the `Image` files' SHA-256 on the leader and on the follower; five interleaved trials under a light hold, as BENCHMARKS' rules state |
| T3 The `memory` window | Within the model (at most f voters per group losing their OS or power in any `sync_interval` plus a barrier, any process crashes, partitions, and clock skew): zero `memory`, `quorum`, or `all` writes lost. Beyond it: zero `quorum` or `all` writes lost, and every lost `memory` write acknowledged within `sync_interval` plus a barrier before the loss. The beyond-model profile must report loss, or its oracle is blind | The simulator, tier C, with D5's fault classes: 10^7 steps over 10,000 seeds per profile; the checker's `memory` mode |
| T4 No divergence | After every schedule, D6's recoveries included, each replica's digest equals the leader's at equal applied positions, and a replica whose applied index passed a divergence point repaired before serving a read | The simulator and Track C's T2 oracles; the digest per replica after each step |
| T5 Linearizability per level | Histories of `linearizable` reads and `memory` or stronger writes are linearizable while at most f voters are forgetful at once; `leader` and `none` outcomes are unknown; `session` and `bounded` reads meet their necessary conditions | The simulator's checker (D9 of its design) with the `memory` and `leader` modes |
| T6 Forgetful voters | A forgetful voter grants no vote or pre-vote, acts on no `TimeoutNow`, and never campaigns before it re-learns or its policy allows; under `balanced` it grants only a log at least as up to date as every log it has seen; each mutant is caught within the seed budget, its first seed recorded: a forgetful vote; re-learning through `commit_memory` instead of `leader_last`; a leader that ignores `forgetful` and never rewinds a matched index (the voter must re-learn within the run); a D6 truncation at `commit_durable`; a checkpoint above `commit_durable` | consensus's `simulation_test` with the forgetful fault class; `tools/mutants.sh` |
| T7 Process restarts | Under one boot identity, 1,000 `SIGKILL`s of random voters at random points under writes: every write acknowledged at `leader` or stronger survives, and no voter restarts forgetful | Three processes on one host, the kill loop and a checker of acknowledged writes, replacing `harness/wal_recovery_check.sh` (PLAN's Phase 7 gate) |
| T8 Failover gap | Killing the leader (process and power) at sustained ingest: the next acknowledged write within D9's total at p99 over 100 kills per preset (2.4 s and 3.4 s at the default profile), and the replay within `election_min` at the leader's measured apply rate | A three-process loopback cluster, and the simulator with the measured barrier profiles |
| T9 Journal with turns between barriers | Power lost at every point with any subset of the unsynced turns' 512-byte sectors persisting, with recycled segments (condition 10): recovery keeps exactly the longest whole prefix after the last recorded synced turn; no synced record is lost but within D3's stated limit; damage at or before it costs only the groups named; `RFTW0001` journals recover unchanged; a same-boot reopen syncs the tail first | journal's T9 harness, extended |
| T10 Overrides and floors | Every combination of preset, requested `ack`, and requested `read`: stronger honored, weaker below the floor refused naming the floor, never silently changed | strata::group tests |
| T11 Bounded and session reads | A `bounded(K, T)` read never serves state more than K entries behind, or older than T by the replica's clock, than the commit it last learned; a `session` read honors read-your-writes and monotonic reads; a token whose entry D6 lost reports `lost` | The simulator's checker; strata::group tests |
| T12 Formats and wire | Every row of D16 has its fixture and previous-reader rejection test; the kinds' sizes as D16 states; a `Membership` without `acks` reads as `synced`; Detent builds and passes its suite at each additive step's commit | With Track C's T1 and the peer transport's T3 |
| T13 Live tuning | Under sustained ingest, changing `sync_interval`, refresh, checkpoint thresholds, floors, and `max_unsynced` takes effect within one interval, with no restart and the foreground write p99 within 1.5× of steady state across the change; switching `acks` and replication mode through the log keeps every acknowledged write and the digest | Three processes under ingest, Detent's file source |
| T14 Background under load | 10 minutes of sustained ingest at 1,000 groups per node: retained log per group within its share, install lag within D9's bound, unsynced bytes within `max_unsynced`, and the foreground write p99 within the bound `research/no-stalls` sets for background work | With no-stalls' harness, on the gate VM |

## Open questions for the owner

**Q1. Xapiand's default preset.**
1. `replicated`: `memory` acknowledgements and segment mode.
2. `strict`: `quorum` and document mode.
3. `relaxed`: `leader` acknowledgements, closest to today's asynchronous pull.

Recommendation: 1. It loses less than today in every failure of the table (Xapiand's own losses,
above, run to seconds even on a process crash), keeps every replica's CPU at a twentieth of
document mode's or less (T2), and leaves `strict` one setting away for indexes that need it.

**Q2. The default `sync_interval`.**
1. 100 ms.
2. 500 ms, Xapiand's fsyncher debounce today.
3. 10 ms, near a barrier's latency on NVMe.

Recommendation: 1. It is a fifth of Xapiand's window today, ten times fewer barriers than 10 ms,
and below every refresh interval, so the window stays invisible behind visibility.

**Q3. The default recovery after a correlated loss.**
1. `balanced`: elect once every voter is heard, or after 5 minutes.
2. `aggressive`: after one election timeout.
3. `manual`: an operator decides.

Recommendation: 1 for `replicated` and `available`, `aggressive` for `relaxed`. Under `balanced` a
correlated power loss costs a group its election as soon as every voter is back, losing only what no
voter held, and up to 5 minutes when a voter stays down; KIP-966 chose the same default.

**Q4. Merges in segment mode.**
1. Shipped: followers receive each merge's output.
2. Replayed: followers merge the same segments themselves, deterministic output checked by CRC.

Recommendation: 1 now. Shipping cost 1.48 to 1.62 µs per document in T2 against 124.9 to 130.1 µs
for the leader's work; revisit with copy merges (no-stalls I5), which would make replaying cheaper
than the extra 103% of bytes per follower.

**Q5. `memory` at rf = 1 (standalone).**
1. Offer it: the turn log written, its barrier within `sync_interval`, which is RocksDB's default
   WAL setting and supersedes rocksdb-survey's Q3 ruling ("no new durability tier").
2. Keep standalone at `quorum` and `none` only, as Q3 decided.

Recommendation: 1. The owner's request of 2026-10-18 asks for relaxed persistence per index; at
rf = 1 it is process-crash safety only, which the documentation states.

**Q6. One vocabulary.**
1. Adopt D10's five acknowledgement and five read levels, retiring `local` and renaming `nearest`
   to `prefix`.
2. Keep REPLICATION's names and add the new ones beside them.

Recommendation: 1: one name per level across strata, strato, shale, and the log system, and the
read levels users already know from Cosmos DB.

**Q7. The split.**
1. Part A now, reviewed alone; Part B (memory residency and retention) designed after A's
   preliminary review.
2. One design for both.

Recommendation: 1, which this file follows. A changes consensus, the journal, and the wire, which
freeze; B builds on A's acknowledgement and recovery rules and on format 13's reservations, and
asks questions of its own.

## First steps

Nothing below starts before this design's preliminary review (a format, wire, and durability
change), and nothing lands before its full review clears.

| Step | Where | Starts | Waits for |
|---|---|---|---|
| 1. `RFTW0002`: one write in flight, the end-of-turn synced field, the boot identity in the open record, the clean-close record, recovery's rule, with T9's rows (D3) | `Kronuz/journal`, branch `durability-barrier` | After the preliminary review's D3 ruling | journal's code review; Detent unaffected (named-file backend) |
| 2. The kernel boot identity and a cross-IO directory barrier (D5; SI-R10) | `Kronuz/completion` | After the preliminary review | Additive, with Detent's suite at its commit |
| 3. `Written`, the two cursors, forgetful voters, the `Recovery` entry, `Repair{promotion}`, the checkpoint ceiling, `Membership::acks`, with T6 (D2, D4 to D8) | `Kronuz/consensus`, branch `durability-modes` | After the preliminary review | Steps 1 and 2; the full design review before it lands |
| 4. D16's wire fields in version 2's bodies | `Kronuz/consensus`, with the peer transport's step 4 | With that step | The peer transport's re-check and Detent's agreement (its Q4) |
| 5. strata::group: D1's checkpoints on small-indexes' D3-A commit group, apply at `commit_memory`, `Image`, the fetch protocol, incremental install, D10's levels and floors, with T10 and T11 | strata, Phase 3R's branch | Phase 3R's start (C1 to C4) | Format 13's reservations; small-indexes' D3 rulings |
| 6. D5's fault classes and the checker's modes (T3 to T5) | `design/simulator` | With the simulator's S2b and S3 | Nothing else |
| 7. T1 on Linux and FreeBSD | The lead | Now | Nothing |

## Part B: memory residency and retention (outline, designed after A)

- **`resident`**: replicated in memory, searchable at once, a disk tier behind. To decide: whether
  the acknowledgement is A's `memory` (written to the kernel) or process memory only; segments built
  in RAM in the on-disk format, so one byte layout serves both; the disk tier's lag bound and what
  `commit_durable` means without a synced log (the disk tier's position); restarts from peers (A's
  install), from the disk tier plus the log, or from shared memory after a clean shutdown only, with
  a valid bit, as Scuba does ("We do not use shared memory to recover from a crash").
- **Retention**, two levels to compare and split: time-windowed segments in strata (the `log`
  profile's time order, a merge policy that never merges across windows, dropping a window as whole
  segments, per-document TTL as a filter until its window drops), and time-based shards above it
  (rollover in the shard manager, Track D4). The likely split: strata owns windows and whole-segment
  drops, which need each segment's time bounds readable without opening it (a format-13
  reservation); rollover belongs to the shard manager, not strata.
- **What A leaves for B**: D5's forgetful rule, which covers a member restarting without its
  memory; D6's recovery; apply at `commit_memory` with `Repair`; the checkpoint ceiling; D10's
  vocabulary; and the rule that a retention drop is a log command stamped with the leader's time, so
  every replica drops the same windows at the same position.

## Status

Part A designed in full; nothing is built beyond the two probes. Measured on this branch (the
development Mac, as evidence for D1 and D8's choices): T1's counts with no hold, T2's sizing under
a light hold. Every other figure is the cited code's or document's, arithmetic, or a labeled
estimate.

What the preliminary review should look at first:

1. D5's safety argument: commits at the memory majority with forgetful voters counted for commit but
   kept out of elections, re-learning through `leader_last` with no sync of its own, and the leader
   lowering a forgetful voter's matched index, which today's core forbids (`core.h:957-969`).
2. D6's truncation above `commit_durable` and repair below the divergence point, the one place the
   design lets a follower replace an applied entry.
3. D3's recovery rule after the last recorded synced turn, and its widened torn-tail limit.
4. D7's choice of apply at `commit_memory` over apply at `commit_durable`.
5. D8's file movement through strata's host frames rather than consensus's snapshot transfer, and
   its claim to be the one mechanism for D18's retry, repair's fetches, and Q7.
6. D16's sizes against the peer transport's D9, and the order against the coordinated commit.
7. For the security reviewer: the threat model's fetch rows, since the segment fetch is a new
   network path that serves index contents, and the cost of hashing what it ships.
