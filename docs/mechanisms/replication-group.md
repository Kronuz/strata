> Status: lone voter built (quest group-core, step 1); multi-voter, `read_applied`, the idle cost
> and the catalog's ownership follow. Design: Xapiand `docs/design/cluster-release.md` revision 2
> (D3, D4, D11, D12, D16, D18), contracts `strata:group` and `strata:checkpoint-bundle`.

# The replication group

`strata::group` (`include/strata/group/`) binds one consensus group to one strata index, an
application file set and an application. Consensus's `Driver` (consensus `docs/DRIVER.md`) runs the
log; strata holds the documents; the file set holds what strata does not (Xapiand's blob volumes
and upload staging); the application turns each committed command into file effects, a strata
transaction and its own state.

```
 propose ──▶ Driver (loop) ──Committed──▶ dispatch ──▶ group queue ──▶ chore: per entry
                  ▲                                                     1 file effects (positional)
                  └──────── applied(through), post to the loop ◀────── 2 strata write (retryable)
                                                                        3 application state
                                                                        4 applied position
                                                                        5 the quorum waiter
```

## How it works

- **One `loop` per reactor loop** owns the `Driver` and its Sink (`dispatch`), which only routes
  each notice to its group: a `Committed` range is enqueued on the group's chore, a `Fenced`
  fences it. The Sink never applies (D18), so one slow group delays only itself.
- **The chore** is a scheduler the host supplies (a work group in production). It runs one queue
  item per resume and yields between them: a committed range one entry per resume, a capture, a
  checkpoint's syncs and commit, an unlink, a recovery. Everything that must fall between two
  entries is a queue item, so it needs no lock with apply. When a range is applied the chore posts
  `applied(through)` to the loop.
- **Apply, per entry**, reads the entry's starting application state only:
  1. File effects (`application::effects`) write at offsets the starting state names, never at a
     file's end, so a retry rewrites the same bytes. ENOSPC stalls the group with its phase kept.
  2. The strata write at position `(index, 1)`. `no_space` stalls; `flush_owed`, `drain_owed` and
     `map_owed` run their chore and retry at once; a stage-1 throw retries after a yield, and the
     third at one entry fences. `skipped` is replay's alone: above strata's durable head it fences.
  3. The application state (`application::advance`), whatever strata answered: replay skips strata
     at or below its head, where the verdict is unknown, and the next locator depends on the state.
  4. The applied position, published after strata's write is visible.
  5. The `quorum` waiter at that index: the verdict when the term matches, `superseded` otherwise.
- **A stalled group** (`no_space`, ENOSPC) applies nothing until the host calls `resume()`;
  its `quorum` answers wait with it. A stalled group arms a loop timer for one election
  timeout and re-arms it each timeout while the stall lasts, whatever its role; at each, a
  leader asks the loop's `applied_past` hook (the host's view of its followers) for one applied
  past the stalled entry and `transfer_leadership`s to it. So a replica stalled as a follower
  and later elected hands off within one timeout of leading. Without the hook, or as a lone
  voter, it hands off to no one.
- **`propose`** at `ack::none` answers at `ProposalPlaced`; at `ack::quorum` it answers when this
  replica applies the placement, with apply's verdict (Q10's lead default, `answer.hh`). A proposal
  whose placement is not yet known registers late, so apply keeps the outcomes of entries above the
  oldest unregistered proposal's starting point, and drops them once none is outstanding.
- **The write path** (`submit.hh`, Q10 (b), D106): a `writer` prepares a request on the chore
  once this replica applied the `Driver::read` index (the ready point, past its term's first
  entry), from the applied index plus this leader's own commands of its term not applied yet,
  proposes it at `quorum`, and prepares it again while `after` (`answer.hh`) says so: a refusal
  of expectations the leader chose (a merge's version), at most three placements, then a
  retryable 503. `invalid` and a client's own condition answer as they are, `superseded` too. A
  replica that is not the leader forwards the original request once, carrying its placement
  count; a read refused before the ready point answers 503 with nothing placed.
- **A replica behind the leader's checkpoint** catches up only by install
  ([group-install.md](group-install.md): the pin, the staged transfer, activation and its
  recovery, and their binding to consensus's Driver). The leader pins at `SnapshotNeeded` and
  names the peer in `installs_needed()` until the install ends; the learner stages at
  `ValidateImage` and becomes the bundle at `ActivateImage`, then applies past it.
- **A witness** (`witness.hh`, Q3 (a), consensus D9) is a voting member with the log and no
  application state, in a group of two data voters and itself (rf = 2 + witness). It is its own
  `member` on the loop: no index, file set or chore. consensus acknowledges a witness's own ranges,
  refuses its proposals and reads, and hands off a leadership it wins to the most caught-up live
  data voter; it wins one when it holds committed entries a returning data voter lacks. Its Sink
  answers what reaches it before it is a witness without applying it: a range below its
  promotion is acknowledged, an image while it joins is validated with nothing staged (consensus
  keeps it as the base of the log), and it pins nothing for a peer's install. It joins by the
  install's steps, a learner promoted to witness beside the voter it replaces, and leaves by
  removal; it never becomes a data member.
- **A fence or a close refuses every `quorum` waiter** from then on, linked or not yet: a fence
  answers `fenced`, a close `closed` for a placement this replica never applied (a leader cut off
  from its followers closes with one). journal's `group_closed` maps to `closed` too.
- **A close's teardown** (its last chore item) marks the group closed: every chore item after it
  is refused, so a `checkpoint()` or `read_applied()` racing the close answers a value or
  `closed`, and `closed` once the close finished. The close holds the checkpoints' serializer
  from its first checkpoint through the teardown. At rest it keeps consensus's closed state
  (`closed_record()`, RFTCLOS1); handed to a reopen's `config.closed`, the reopened replica
  resumes its term with no election (consensus `docs/mechanisms/quiescence.md`). Keeping it
  across a restart is the host's choice: without it a reopen elects.
- **Lifetime**: a group is destroyed with its chore idle, after `close()` or once fenced and the
  chore drained (a stopped process). Its `applied(through)` posts hold the loop and the id, not
  the group.
- **An idle open group** holds no thread, timer, coroutine frame or IO of its own: its chore
  item ends when the queue empties, its file set opens a file per write and closes it, and an
  idle minute resumes nothing. Its descriptors are its index's two: the group holds one
  directory, the index's, which the host hands it, and a close closes the index and lets the
  directory go, so a closed group holds none. Its chore queue is linked through its items, and
  the config it opened with is freed once `open()` returns: an idle group's heap past its index
  and its consensus row is the object past the index and its file set's entries. A closed group
  holds no descriptor or frame: its heap is its object and consensus's closed row (the Driver's
  and the journal's record of the group); destroyed, it leaves only that row.
- **The file set** (`file_set.hh`) lives in the index's directory under the prefix `app.`:
  strata reads and removes only its own names (`manifest-*`, `seg-*`), and the set's recovery
  deletes only names under its prefix. Apply writes, seals (rotation: the file is immutable from
  then, its CRC32C named) and retires; it never unlinks. A retired file is unlinked only after a
  bundle that omits it publishes.

## The checkpoint

Checkpoints run one at a time (an async lock in the group): consensus refuses a second while
one is pending, so a close racing a checkpoint would fail.

1. **Capture**, a chore item: P_b is the applied position, the live files' lengths and the
   application state are read there. If strata's position is below P_b (the last entries were
   chunks or NoOps), an empty transaction at `(P_b, 1)` moves it (the mark); it stalls on
   `no_space` like an entry.
2. **Sync** each named file (an immutable one once) and the directory. A failure fences the
   replica; it reopens through recovery, since a retried sync can succeed with the bytes lost.
3. **Commit strata**, so its durable head P_H is at or past P_b.
4. **Publish** the bundle through `Driver::checkpoint`; consensus may compact at or below P_b.
   A bundle plus the log's bound (`maximum_size(limits)`) larger than the admission's replacement
   pool would wait forever for its reservation, so the checkpoint fails with `length_error`
   first: at consensus's default limits (a 64 MiB log) the host sizes the pool to them.
5. **Unlink** what was retired at or below P_b, then sync the directory.

The bundle (`bundle.hh`, contract `strata:checkpoint-bundle`) is D16's table: magic and version,
P_b and its term, the files (name, length at P_b, CRC32C once immutable), the application state,
and a CRC32C over all of it. It names no manifest CRC32C: install's pin carries that.

## Recovery

`open()` opens the Driver's group and, when the host's table holds it, `recover`s it: the
`Restore` callback only decodes the bundle on the loop. Recovery is the chore's first item, ahead
of every range Core delivers meanwhile:

A failed open fences the group and closes its Driver row, so the same process opens the id
again with a new group object.

1. Open strata at P_H; fail closed if P_b > P_H. With no index there, create it deferred (D112):
   nothing is written until the first checkpoint commits it, and a crash before that finds no
   index again and recreates it from the log.
2. Truncate each named file to its length (missing or short fails closed), and check each sealed
   one against its CRC32C (a mismatch fails closed); delete every other name under the set's
   prefix; sync the directory.
3. Load the application state. Core then delivers every committed entry above P_b: file effects
   re-run, strata skips at or below P_H.
4. `readable()` stays false until applied reaches P_H, since strata's documents in (P_b, P_H] may
   name bytes the replay has not rewritten yet.

A crash inside recovery repeats it: truncation and deletion converge.

## A damaged turn log

When a block of the turn log is damaged, `Host::open` keeps a group whose records after the damage
are contiguous, with its bound (the highest sequence the damage could hold): `GroupStatus::bounded`,
stored in the group's table entry (journal's `lost-bound.md`). Strata follows it in two places:

- **An index on the log** (`attach_log`) continues a bounded group when its head holds it through
  the bound (a head at the bound holds that record's whole batch), and replays the records after it.
  A head below the bound may lack records the damage took: the index calls `lose(g)` and the open
  throws `log_lost`.
- **A replicated group** (`group::open`, the witness's too) opens a bounded group through
  `driver.recover`. Journal's Store holds a group through its latest checkpoint record, none of
  which follows the bound, so it loses the group durably and throws `Corruption`; the failed open
  fences the group and closes its row, and a restarted host holds it lost. `driver.create` would
  refuse the table's entry and leave the group bounded.

The journal changes between the pins (9e11713, 9c7e31c) that strata follows: `Host::delete_group`
and `GroupState::deleted` (reported as `none`); `GroupStatus::bounded` with `Host::bound` and
`Host::lose`; `create_group`/`reopen_group` commit with the loop's next turn with records, so each
is durable before the group's first record (`turn_sealer`'s join order holds); a group's table
calls run on its loop, which `turn_sealer` adopts under its turn mutex (`host-ranges.md`). Renamed
or removed names (`park` to `evict`, the table shards' file names) are not called or read by strata.

## What it relies on

- **A witness's compaction** (consensus, not exposed): a witness checkpoints only through what
  every data voter matched (`Core::witness_bound`), which the Driver's `GroupState` does not
  give, so a witness takes no checkpoint and its retained log grows until consensus's log limits
  refuse appends. The Driver compacting a witness's log itself, as it acknowledges its ranges,
  or exposing the bound, closes it.

- **strata-nospace-position** (not landed): a `no_space` answer leaves strata's position
  unmoved, so the retry applies the transaction again. At strata 491a1bdc the reservation path
  keeps it, the allowance path moves it (`index.hh:878-880`); the group fences on the `skipped`
  that follows, and the lone-voter test exercises the reservation path only.
- strata's `invalid` for a size cap returns before the position moves at 491a1bdc; the capture's
  mark covers an entry whose write did not move it.

## Proof

`test/group/lone_voter_test.cc` (ctest `group_crash_lone_voter`, quest A1): one script crashed at every
crash point and its recovery crashed again at every point, over faultfs for strata, the file set
and the journal host (`test/group/host_io.hh`), with `HeldCompletion` holding the journal's IO. The
oracle: no applied entry lost or changed, every acknowledged command in the recovered log, reads
open, and the digest equal to a replay from empty through the application on a fresh index and an
in-memory file set. The script ends with a waiter across a close and a reopen in one process.

`test/group/voters_test.cc` (ctest `group_voters`, A2): three and five voters in one process,
each with its own host, strata index and file set over faultfs, and the network in seeded order.
Each schedule writes through the leader at `quorum`, checkpoints, and cuts one node's power at a
seeded event; half the schedules also spend one replica's storage budget and make another's
file-set push fail partway with ENOSPC, then free both. Once the cluster settles: every
acknowledged command at its placement in every log, every node applied the same entries, and
every digest equals the others' and the replay. It also holds a leader closing with an
uncommitted write (the waiter answers `closed`, the group reopens and the cluster converges), the
`ack` levels with the followers' appends held, and a checkpoint at consensus's default limits.
The write path (A3): two merges to one id, a client's conditional write and an invalid document
submitted while the leader is cut off and another elected: every answer matches the replay's
verdict at its placement, no merge answers 409, the invalid one 400 once, nothing needs a second
placement, a deposed proposer forwards; with a predictor that misses, merges answer 503 after
their third placement. An election that replaces a cut-off leader's placed request answers it
`superseded`, never forwarded or placed again; a follower forwards a request once, to the leader,
which answers it, or to a replica that does not lead, which answers 503; a leader stalled on
`no_space` hands leadership to a follower applied past it, which answers writes while the old
leader's waits, and so does a follower stalled on `no_space` and then elected by transfer
(`stalled_follower_elected`). A replica cut off while every other one checkpoints past it is named in
`installs_needed()` and installs (`install_gap`); the ungated schedules, whose checkpoints leave a
replica behind, settle by install and pass the oracle over the log merged from every replica's
applied entries; a learner joins past every checkpoint, installs, and is promoted to replace a
voter that consensus's core then removes, across leader changes (`grow_and_heal`).

`test/group/witness_test.cc` (ctest `group_witness`, group-heal A4), on the voters' world
(`test/group/world.hh`): two data voters and a founding witness write on through a power cut of
each member, which rejoins (`loss`); a data voter cut while the others commit returns as the other
is cut, so only the witness holds what it lacks: the witness leads, refuses a proposal and a read,
replicates, and hands leadership to it (`carry`); a witness joins three voters as a learner that
installs, is promoted beside the voter it replaces, which is removed, and the group survives each
member's loss (`join`). Once settled, every acknowledged command is in the log, every data
member's digest equals the replay, the witness applied through the commit index with its Sink
handed no range once it is a witness, and a data voter leads.

`test/group/lifetime_test.cc` (ctest `group_lifetime`, A5 and A10): two groups on one loop with
their chores on threads; one group's apply blocks at an entry's strata write while the other
proposes, commits, answers `quorum`, reads and checkpoints, and the host sees the held group's
applied position below that entry (A4); the other group then takes a burst of writes against reads,
checkpoints and loop-side observers (run under TSan). On inline schedulers, a checkpoint, a close
and `read_applied` start at every event of a run, with apply ahead of commits and behind them:
the bundle equals a replay through its P_b, a close's last bundle is at applied and a restarted
process recovers from it alone with every acknowledged entry, and a read sees a replay through
the position it ran at, at or past the commit index when it started. A close also starts at every
event with a checkpoint and a read at each event after it until one past its end: each answers
a value or `closed`, `closed` once the close finished. A lone voter closed and reopened in one
process, then after a restart (A5, A7): a closed group is closed in the host's table with no live
turn-log record, and reopened with its closed state it leads in its closed term. `reopen-turn`
(ctest `group_lifetime_reopen_turn`) also requires a reopen and the write issued with it to share
one turn, as many barriers as a write to an open group (the turn log's `stats().barriers`).
`failed-open`:
a reopen over a directory that lost strata's index fails closed (P_b > P_H) and leaves no row;
the same process then reopens the group over its directory and reads every acknowledged entry.

`test/group/footprint_test.cc` (ctest `group_footprint`, A6): lone voters on real directories,
counting descriptors, heap, threads, timers, coroutine frames and an idle minute's resumes per
group against a bare index holding the same document and a bare consensus row with one write
applied, and a close's syncs; closed and destroyed groups per closed group, against a bare row
checkpointed and closed. The frame pool is off there, so a freed frame leaves the count.
`test/group/footprint_owners.py` charges a stopped run's live heap to its owners at a named phase,
or past another (macOS, `malloc_history`). `test/group/file_set_test.cc` (ctest `group_file_set`):
strata's create, commits, retention, crash and open leave the set's names alone, and the set's
recovery leaves strata's.

`test/group/mutants.py` holds the mutants, each killed by one of those tests (`--target=`).
