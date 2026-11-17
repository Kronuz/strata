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
  its `quorum` answers wait with it.
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
- **A replica behind the leader's checkpoint** catches up only by install (group-install). Until
  that lands the group answers consensus's `SnapshotNeeded` by naming the peer in
  `installs_needed()`; the peer stays behind.
- **A fence or a close refuses every `quorum` waiter** from then on, linked or not yet: a fence
  answers `fenced`, a close `closed` for a placement this replica never applied (a leader cut off
  from its followers closes with one). journal's `group_closed` maps to `closed` too.
- **Lifetime**: a group is destroyed with its chore idle, after `close()` or once fenced and the
  chore drained (a stopped process). Its `applied(through)` posts hold the loop and the id, not
  the group.
- **The file set** (`file_set.hh`) is one flat directory. Apply writes, seals (rotation: the file
  is immutable from then, its CRC32C named) and retires; it never unlinks. A retired file is
  unlinked only after a bundle that omits it publishes.

## The checkpoint

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

1. Open strata at P_H (create it if an interrupted create left none); fail closed if P_b > P_H.
2. Truncate each named file to its length (missing or short fails closed); delete every other
   name; sync the directory.
3. Load the application state. Core then delivers every committed entry above P_b: file effects
   re-run, strata skips at or below P_H.
4. `readable()` stays false until applied reaches P_H, since strata's documents in (P_b, P_H] may
   name bytes the replay has not rewritten yet.

A crash inside recovery repeats it: truncation and deletion converge.

## What it relies on

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
their third placement. A replica cut off while every other one checkpoints past it is named in
`installs_needed()` and stays behind (no install yet).

`test/group/mutants.py` holds the mutants, each killed by one of those tests (`--target=`).
