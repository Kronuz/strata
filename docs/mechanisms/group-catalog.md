> Status: built (quest group-catalog). Decision: D169 in Xapiand's campaign plan. Contracts
> `strata:group` (`group::config`, the borrowed index) and the catalog's host hooks.

# A replication group on its catalog slot's index

The catalog (`include/strata/core/catalog.hh`, `catalog.md`) owns every open index of a partition
by value, and strata locks an index's directory, so one directory opens once. A replication group
(`include/strata/group/group.hh`, `replication-group.md`) therefore never opens its own index: it
borrows the one in its catalog slot, and the catalog keeps the index's whole lifetime.

```
 catalog slot:  io ──▶ index ──▶ H { group }        open:  resolve, open_index, open_payload
                 ▲       ▲          │ borrows both   close: close_payload, index.close(), retire
                 └───────┴──────────┘                       (H, then the index, then the io)
```

## The borrow

- **The constructor takes both, by reference:** `group(loop, config, directory, index, app,
  chore)`. The file set shares the directory under its `app.` prefix; strata's names are the
  index's. `config` keeps only what consensus needs (id, options, identity, the last close's
  record).
- **The host opens the index:** `group::open_index(directory, options, uuid)` is the catalog's
  `open_index` for a group's slot: the durable index, else a deferred create that writes nothing
  until the first checkpoint's commit (D112). `catalog::opening::io` hands `open_payload` the
  slot's directory, which the group borrows for the slot's life.
- **Only open() joins the loop's table, on the loop.** The constructor touches nothing of the
  loop's, so a host constructs the group on its own thread (the catalog's lane) while the loop
  looks up other groups' notices. A second group with the same id is refused there, before
  anything starts.
- **Recover reads the index it is lent:** P_H is `index.applied()`, the durable head after an
  open, or past it when an earlier group over the same open index closed. The bundle must not be
  past it (P_b <= P_H, else fenced); entries above P_b re-run their file effects and strata skips
  them at or below P_H, as in D16.
- **Close leaves the index to the host:** it checkpoints until the bundle covers the commit index,
  closes the consensus row, leaves the loop's table on the loop, refuses chore items, waits for
  the chore to go idle, then closes the file set. It never closes the index or the directory. On
  return nothing of the group touches either, so the host may close the index and destroy the
  group on any thread. A second close returns at once; a fenced group's close tears down the same
  way, publishes nothing and throws `group_fenced` (its log and last bundle stand).
- **A failed open tears down as close does:** a recover that fences (P_b > P_H), a create or a
  Driver error fences the group, closes its row, leaves the loop's table on the loop and waits for
  the chore to go idle before open() throws. The catalog then destroys H on its lane, as its
  `run_open` does on any `open_payload` throw; a later close() returns at once.

## The hooks

- **`commit` stays a strata commit** for a group's index (the default `index.commit()`). The
  catalog runs it inside `chore_scope`, holding the index's write lease. A bare commit only raises
  P_H; P_b stays, and D16's recovery holds: reads open once applied reaches P_H, and replay rewrites
  the file bytes above P_b that strata's documents may name.
- **A commit hook never waits on the group.** The group's chore takes the write lease in
  `apply()` and `commit()` (`lease_scope`), which waits while the chore bit is held (`owner.hh`).
  A commit hook that sync-waits on `group.checkpoint()` therefore waits on itself, on any lane.
- **`close_payload` runs the group's close:** every catalog close (sweep, bound, shutdown,
  replace) calls `close_payload(H&, Index&)` just before `index.close()`, after the chore's lease
  is released, so the group's checkpoint takes the lease itself. Its throw is counted
  (`payload_close_failures`), names the key at shutdown, and the index closes regardless: H no
  longer uses it. A close the index refuses afterwards calls it again, which the group's second
  close answers at once.
- **`open_blocking()` and `close_blocking()`** bridge the catalog's synchronous hooks to the
  group's coroutines: they spawn `open()`/`close()` on the loop and block the calling thread
  until the work's frame has ended and the loop holds no reference to its error, so the caller
  may destroy the group as soon as the call returns. They refuse, before starting anything, to run on the loop or the group's chore (a thread inside it,
  or one pumping it inline), or under the index's write lease, each of which the call would wait
  on. A refused open leaves a group the host may destroy; a refused close leaves it open, so a
  host that destroys it then breaks the destructor's contract. The catalog calls both on its lane
  outside the lease, where no refusal arises while the loop and the chore run off its lanes
  (below).
- **The group's chore writes with no pin of its own.** Follower and `ack::none` applies and
  Driver-started checkpoints call the borrowed index while no request pins the slot; the slot
  stays the group's because H holds it open until `close_payload`, which every close runs before
  `index.close()`. The catalog's debug check that a call holds a pin (CT-R15) counts these calls
  (`owner_break`, aborting only under `abort_on_owner_break`); a demoted slot's apply promotes the
  index itself (`map_owed`), outside the catalog's wake accounting.

## The lanes

A blocking hook occupies a catalog lane thread until the group's work ends, so that work must run
elsewhere:

- The group's chore runs on a lane apart from the catalog's transitions. On the catalog's own lane
  the open's recovery item and the close's capture queue behind the hook that waits for them.
- The loop runs on its reactor thread, never a catalog lane: the Driver's checkpoint and close
  resume there.
- While a group's close checkpoints (file syncs, a strata commit, a journal turn), the partition's
  other transitions on that catalog lane wait behind it.

`test/group/group_catalog_test.cc` runs the loop, the group's chore, the catalog's lane and the
test on four threads, one case per ctest:

- `borrow`: a group opened by `open_payload` recovers and applies; its own close leaves the pinned
  index open, readable and committable, and a new group over it recovers and applies; the
  catalog's shutdown closes the idle group through `close_payload` with a bundle at everything
  applied, which a restarted process recovers.
- `failed-open`: a recover that fences fails two pins while another group writes on the loop; each
  failed group left the loop's table before its throw.
- `attach`: five groups open on the lane while another writes; none is in the loop's table before
  its open.
- `sweep`, `bound`, `replace`: the sweep's eviction, the wake bound's close (eight failed wakes of
  a demoted slot) and a fenced index's replace each close the group through `close_payload`, and
  a reopen recovers the bundle (the replace's from the log alone, its checkpoint unable to commit).
- `bare-commit-crash`: the catalog's bare commit makes P_H durable over unsynced file-set bytes;
  power goes off and, over 16 crash seeds, the reopen at P_b 0 replays every value's bytes.

Every step has a deadline, so a close that waits on itself fails the test instead of hanging it.
TSan runs every case.
