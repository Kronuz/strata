# Durability and replication modes per index

- Status: **draft, paused at the usage limit (2026-10-18)**. Not ready for preliminary
  review: the measurement (T2), the instrumented fsync count (T1), the prior-art citations,
  and the peer-transport and simulator cross-checks are owed (`docs/status/durability-modes.md`).
- Amends: `docs/REPLICATION.md` (ack table, write path, Xapiand mapping); extends Track C's
  D5 and D11 (`docs/design/track-c.md`); relates to `perf/small-indexes`' D3 (below).
- Owner's request (2026-10-18): split "commit", relax persistence per index where replicas
  hold the data in memory, keep stronger consistency for indexes that need it, and find the
  indexing win relaxed models allow.

## What exists today (verified)

### Xapiand (MIT own code at `master` `7e8cccb7d`)

| Claim | Evidence | Correction to the lead's analysis |
|---|---|---|
| One cluster-wide Raft elects the master | `src/server/raft_delegate.h:53-123` (delegate of `cluster::Raft`) | Holds |
| Each shard's primary comes from a separate majority poll run by the Raft leader; it picks the eligible active node with the highest revision | `src/server/discovery.cc:653-716` (nodes answer with UUID and revision), `718-801` (the leader counts a majority of the shard's nodes; selection `776-787`) | The range is 653-801 |
| The Xapian database is opened `DB_NO_SYNC` whenever the WAL is built in; durability is the WAL's | `src/database/shard.cc:77-81`, `350`, `356` | Not in the lead's list; it matters for `?commit` |
| The WAL's volume syncs through the debounced fsyncher (`STORAGE_ASYNC_SYNC`) | `src/database/wal.cc:68`, `114`, `129`; `src/xapiand_fsyncher.h:29-51`; storage `storage.h:1091-1113` | Holds |
| `DB_SYNCHRONOUS_WAL` makes the WAL *write* inline, not its sync: the volume stays `STORAGE_ASYNC_SYNC` | `wal.cc:1162-1178` (`execute` against `enqueue`); `wal.cc:676`, `695` | REPLICATION.md:165 maps it to `ack=local`; it is process-crash safety only |
| `?commit` commits Xapian (no sync), enqueues the WAL commit line, and triggers replication | `search_views.cc:2333-2337`; `shard.cc:1466-1468`, `750-858` (`814-820`, `823-827`) | REPLICATION.md:166 maps it to `ack=quorum` plus `refresh=true`; it gives visibility, not power-loss durability |
| Effective debouncer defaults are set in `opts.cc`, overriding `opts.h`'s initializers | `src/opts.cc:114-136`, `628-650`; `opts.h:114-133` | See the next table |
| Debouncer: the first call fires after `debounce`, later calls after `busy`, capped at a random force time; a throttle spaces fires | scheduler `debouncer.h:177-211`, `228-281` | |
| Replicas pull after `DB_UPDATED` with a random 0 to 3 s delay plus the trigger debounce | `discovery.cc:488-545` (`535`) | Not in the lead's list |
| Under the async hook, the storage v2 commit's two syncs around the header write are both deferred, so the data-before-header order is not kept | `storage.h:1116-1148` | An observation for Xapiand's backlog, outside this design |

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

### strata (`main` `755a918`)

- One Raft group per shard (`strata::group`, REPLICATION.md:26-54); root and placement groups
  (DISTRIBUTION.md:38-63). Leadership is per shard group, as in CockroachDB and TiKV.
- Document mode: every voter applies every batch (REPLICATION.md:84-86). The batch carries the
  **prepared** document (REPLICATION.md:61-63; ARCHITECTURE.md:85-91), so followers repeat
  stage 1, inversion, flush, and merges, never analysis. The win segment mode can claim is
  therefore (rf-1)/rf of the core's apply, flush, and merge CPU, not of parsing and analysis.
- The turn log (Track C D11) gives one barrier per core per turn, one turn in flight
  (`track-c.md:497-509`); standalone durability is the same mechanism as a group of one
  (`workload-profiles.md:100-106`; `rocksdb-survey.md` K2, `:207`).
- `index::install` replaces the whole content under fresh local names (`index.hh:623-704`);
  D18's catch-up retry already asks for an install of only the files a receiver lacks
  (`track-c.md:783-789`); the RocksDB survey's Q7 asks for adopting an externally built
  segment through the log (`rocksdb-survey.md:367`, owner decision `:386-388`).
- Read copies are learners, up to every node (`placement-strategies.md:111-127`).

**Fsyncs to create and commit one small index today**, derived from the code at completion
`5251cf9` (Linux; it matches the 10 small-indexes measured):

| Step | Syncs | Where |
|---|---:|---|
| `owner.lock` created | 1 | completion `posix.h:348-358` |
| `clear_uncreated` directory sync | 1 | `index.hh:1967-1980` |
| First commit: metadata file, directory, manifest temporary, directory | 4 | `index.hh:1759-1851` |
| Commit with one segment: segment, directory, manifest temporary, directory | 4 | same; retention removes nothing (`index.hh:173`) |
| Total | 10 | |

On macOS each directory sync adds an `F_FULLFSYNC` (`posix.h:443-452`). T1 counts these with
an instrumented IO; the table above is a reading, not a count.

## Decisions (outline; full text owed)

- **D1. Commit becomes three steps.** Durability is the core's turn-log barrier, shared by
  every index on the core. Visibility is refresh, memory only (ARCHITECTURE.md:105-106).
  Checkpoint is today's commit protocol (segments, live docs, metadata, manifest, directory
  syncs), moved off the acknowledgement path, in the background, batched across indexes, and
  published by the group checkpoint record that lets the log truncate (`track-c.md:582-587`).
  A checkpoint never records an applied position beyond its node's journal barrier.
  Derived count per tiny index under D1: 1/N of a shared barrier on the acknowledgement path
  (N indexes in the turn); the checkpoint's 4 syncs in the background, batched (small-indexes
  D3-A); create's 6 folded into the first checkpoint, since the journal records the creation
  (small-indexes D3-C).
- **D2. A new acknowledgement, `memory`:** a majority of voters, the leader included, wrote
  the batch to their journals (`write` returned, so the kernel holds it), and each voter's
  barrier follows within `sync_interval`. It survives every process crash and a minority's
  power loss; a correlated OS crash or power loss of a majority loses at most the writes
  acknowledged within `sync_interval` before it.
- **D3. The bounded barrier.** Per core, one barrier at most every `sync_interval`, covering
  every turn written since the last, plus an immediate one whenever a turn needs it (hard
  state, `quorum` and `all` writes, configuration entries, a segment switch, a checkpoint).
  Several turns then sit between barriers, which D11's one-turn-in-flight rule
  (`track-c.md:506-509`) forbids today: each end-of-turn record gains the turn its writer last
  saw synced, recovery keeps the longest whole prefix after it and calls damage at or before
  it corruption. A versioned, additive journal change (Detent's named-file backend is
  untouched). Default proposed: 100 ms (Q2).
- **D4. Terms, votes, and configuration entries stay durable** before any message that
  depends on them: a voter that forgot a vote could vote twice in one term (dissertation
  §3.8, to cite), and a mode change acknowledged from memory could hide that a voter acked
  from memory.
- **D5 (extends Track C D5). Recovering voters.** A restart under the same boot id syncs the
  recovered tail at open and resumes: nothing acknowledged is lost. After an unclean shutdown
  (a changed boot id and no clean-close record, or a failed barrier), a voter of a group that
  allows `memory` keeps its incarnation and hard state, counts for commit, and grants no vote
  or pre-vote and never campaigns until its log matches a leader of its durable term or later
  through that leader's last index at first contact (a new `DTC2` append field). Election
  quorums then exclude every forgetful voter, so each intersects the non-forgetful part of
  every commit quorum. No membership change per rebooted node, unlike a new incarnation.
- **D6. Unclean recovery, the stated loss.** When recovering voters are a majority and no
  leader answers within an election timeout, they grant by durable-log comparison (pre-vote
  replies carry the flag). The new leader's first entry records the recovery and the rolled
  back index; a follower whose applied state passes the divergence point repairs from a
  checkpoint at or below it (D10's path), so replicas never diverge. Automatic or gated is Q3.
- **D7. Two commit cursors.** The leader tracks `commit_memory` (majority written) and
  `commit_durable` (majority synced); followers report their synced index in append replies
  and coalesced heartbeat replies (a `DTC2` field). Apply follows `commit_memory` in groups
  that allow it; `quorum` and `all` writes wait for `commit_durable`.
- **D8. Segment mode.** The log carries the prepared batch in both modes. Followers append it
  and acknowledge deliveries without applying them (as D10's damaged host does,
  `track-c.md:425-431`), and install the leader's manifests incrementally: only the files
  they lack, each verified whole by id, size, and CRC, over D18's transfer on the bulk lane.
  A follower's checkpoint is its installed manifest. On promotion it rewinds its application
  to its installed position and replays the tail through D10's redelivery (exempt from the
  transfer-first rule in this mode). Install, catch-up install, repair, Q7's bulk adoption,
  and segment shipping are one mechanism. Segment ids never collide across producers (the
  producing term in the id's high bits: a format-13 item). Physical files then equal the
  leader's: the digest is unchanged (logical, layout-independent, as calibration 2026-10-18
  requires), repair can fetch one file, and scrubs can compare CRCs. In a mixed-version
  window the leader writes only formats every member reads, and a member that cannot read
  an installed segment applies the batches itself (Track D8).
- **D9. The failover tail is bounded.** The leader refreshes and ships when a voter's install
  lag passes what the new leader indexes within one election timeout at its measured rate,
  so write unavailability after a failover stays within about two election timeouts. Reads
  on segment-mode followers are staler by refresh, transfer, and install; real-time get and
  session reads past the installed position go to the leader.
- **D10. Presets per index**, with per-request overrides (stronger always allowed; an index
  may refuse weaker):

  | Preset | Ack | Replication | Use |
  |---|---|---|---|
  | `strict` | `quorum` (majority synced) | document | Indexes whose data must never be lost |
  | `replicated` (cluster default) | `memory` with `sync_interval` | segment | Most of Xapiand's indexes |
  | `relaxed` | the leader's write; replicas follow asynchronously | segment | Xapiand-like, highest ingest |

  At rf=1, `replicated` acknowledges after the turn's write with the barrier within
  `sync_interval`: Xapiand's `DB_SYNCHRONOUS_WAL` as it was meant to work.
- **D11. Rejected: UDP multicast for data.** Bandwidth is not the cost (re-indexing CPU is);
  multicast is unavailable on most cloud and Kubernetes networks; UDP would need ordering,
  retransmission, fragmentation, and DTLS. The peer transport (TLS over TCP) stays.

## How small-indexes' D3 relates

- D3-A (batched syncs over today's files, `prepare_commit()` and `commit_all()`) is the
  stepping stone and stays: under D1 it is the checkpoint batcher. Its failure cases (a failed
  shared sync fences the batch, per-index ENOSPC fails one index, a crash between phases)
  carry over; what changes is that a failed checkpoint no longer fails an acknowledged write,
  which the journal already holds.
- D3-B (a shared journal) is this design's D1 and D3 on Track C's turn log: replaced by it.
- D3-C (fewer syncs at create) composes: the journal's creation record makes the deferral safe.

## Gate rows (targets and procedures owed)

T1 syncs per tiny index today and under D1 (instrumented IO, a count, no hold); T2 CPU per
document, leader apply, flush, and merge against a follower's log append plus install, at
100,000 documents (light hold; deferred at the coordinator's request); T3 power loss of a
majority inside the bound loses only the window (simulator); T4 no divergence after rejoin;
T5 linearizability under `strict`; T6 recovering voters never vote; T7 a process restart
loses nothing; T8 the failover tail within D9's bound; T9 journal recovery with many turns
between barriers, any sector subset persisting (faultfs); T10 per-request overrides.

## Open questions (recommendations owed in full)

- Q1. Xapiand's default preset: `replicated` (recommended), against `relaxed` (closest to
  today's async WAL).
- Q2. `sync_interval`: 100 ms (recommended) against Xapiand's 500 ms.
- Q3. Unclean recovery: automatic for `replicated` and `relaxed`, never needed under `strict`
  (recommended), against operator-gated (Kafka's `unclean.leader.election.enable`, KIP-966).
- Q4. Merges in segment mode: shipped (recommended first) against replayed from the log
  with deterministic output checked by CRC.

## Prior art (to fetch and cite)

Kafka acks, `min.insync.replicas`, flush settings, and its design note on rejoining replicas;
KIP-966; Raft dissertation §3.8 and §11; OpenSearch segment replication; CockroachDB and TiKV
multi-raft; VR Revisited's recovery protocol; RAMCloud's buffered logging; PostgreSQL's
`synchronous_commit = remote_write` and asynchronous commit; MongoDB's `j: false` with
`writeConcernMajorityJournalDefault`; Elasticsearch's `index.translog.durability`.
