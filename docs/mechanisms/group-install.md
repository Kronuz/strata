> Status: built (quest group-heal, task `install`), bound to consensus's Driver install surface
> (consensus `docs/DRIVER.md`, Install; Detent's `consensus-driver-install`), which strata pins
> once it lands. Design: Xapiand `docs/design/cluster-release.md` D16 (install moves the file
> set), D7, F1; contracts `strata:group-install`, `strata:checkpoint-bundle`.

# Install

A replica behind the leader's checkpoint cannot catch up from the log: consensus compacted it.
It installs instead: the leader's bundle, a strata checkpoint at or past it, and each file of the
application's file set to the bundle's length, in one transfer. Then consensus delivers every
entry above the bundle, as after a recovery. `include/strata/group/install.hh` holds it.

```
 leader                                   learner
 bundle P_b ──consensus image──────────▶  image = bundle
 make_pin: strata checkpoint G ≥ P_b       fetch_pin ◀── "P_b/pin"
           files, lengths, CRC32C          stage: is.* ia.* ◀── "P_b/s|a/offset/name" (≤ 64 KiB)
 serve ◀──────── fetch lane ────────────   marker ip (stage complete)
                                           consensus persists the bundle (its install)
                                           activate: strata.install(is.*, G), ia.* → app.*, sweep
                                           recovery from the bundle, then the log above P_b
```

## The pin

`make_pin`, on the leader's chore between two entries, pins strata's checkpoint generation G
(`index::checkpoint()`), which strata keeps with its files until `release_pin`. strata committed at
or past the bundle's P_b before it published, so G's applied position is at or past P_b: the
learner's strata replays nothing at or below it, and the file set replays from P_b, as in
recovery (D16). The pin lists each file with its length and the CRC32C of its bytes to that
length:

- strata's manifest log that holds G, to its length now, and G's segments;
- the set's files the bundle names, to the bundle's lengths (a sealed file's CRC32C is the
  bundle's; a live one's is read once). Apply writes a live file only past its length at P_b,
  so that prefix does not change while the pin lives.

The pin's bytes: magic and version, G, the bundle, the two lists (name, length, CRC32C), and a
CRC32C over all of it. `decode_pin` refuses a strata name that is not a manifest or segment
name, a set name the bundle does not hold at that length and CRC32C, a duplicate, and a damaged
pin. A strata pin lives in the manifest, so it survives the leader's process; no install does,
so a reopened leader releases every pin its manifest keeps (`held_pins`, see the binding).

## Serving

The fetch lane (consensus `docs/mechanisms/peer-fetch.md`) maps a name to bytes. `serve` answers
two names below the group's own routing: `P_b/pin`, and `P_b/k/offset/name` with `k` `s` for
strata or `a` for the set, up to 64 KiB of the file from the offset, never past its pinned
length. Anything else, or another pin's position, is not-found. The reader runs on the transport's
loop and reads only pinned prefixes, which nothing rewrites.

## Staging

`stage`, on the learner, writes every file of the pin under a staging name in the replica's
directory: `is.` and strata's name, `ia.` and the set's name (a set name with it stays within
NAME_MAX). strata reads only its own names and the set's recovery removes only `app.` ones, so a
stage leaves the replica as it was. For each file it keeps what it holds: a staged file from an
earlier try, or else the set file under its own name, up to the pinned length. It fetches the
rest from the end of what it keeps, 64 KiB at a time and a window of fetches at once, writing
each window's answers in order and none after a failed one. A file's kept and fetched bytes must
match its CRC32C; kept bytes that do not (a damaged file, another leader's file of the same name)
are fetched whole once more. Every file is synced, then the marker `ip` (the pin, through a
temporary and a rename) says the stage is complete.

A fetch that ends with an error (`timed_out` past the lane's deadline, `io_error`,
`connection_aborted`, `not_connected`, a busy peer), a not-found, a chunk of the wrong size or a
fetched file that fails its CRC32C throws `install_failed`. What was written stays, so the retry
fetches only what the learner lacks or holds short. A new stage first removes an earlier marker
and every staging name its pin does not name.

## Activation and recovery

consensus persists the install (the bundle becomes the replica's checkpoint) only after the
stage completes; then `activate` makes the replica the pin's:

1. strata installs generation G from the staged files (`index::install` through a view of the
   `is.` names): the old content or the new across a crash. A deferred index (a fresh learner's,
   which never committed) commits its empty state first: install writes its segment copies
   before its manifest, and a directory holding segments with no manifest log is damage to
   strata's open, not an interrupted create. The staged strata files go next, the manifest log
   first, so a repeat does not install again.
2. Each staged set file replaces the replica's own.
3. The marker goes, then every staging name.

Recovery runs `resume` first: a complete stage whose bundle is the one consensus restored was
persisted, so activation finishes; a stage at or below the restored bundle is stale and swept;
one above it waits for consensus's retry. The group's recovery then runs as always: P_b ≤ P_H,
the set truncated to the bundle's lengths, its state. Every step repeats safely, so a crash
anywhere leaves the old replica (before consensus persists) or the complete new one (after).

## The binding

`group.hh` carries the transfer on consensus's Driver (a group opened with
`GroupOptions::snapshots`). consensus sends the bundle itself, the image, over the host's transfer
frames (the loop's `send_transfer`); the pinned files go over the fetch lane (the loop's `fetch`,
answered by the leader's `group::serve`).

```
 leader (Sink, chore)                       learner (Sink, chore)
 SnapshotNeeded ─▶ chore: make_pin(latest)
   image ═══ SendTransfer frames ═════════▶ ValidateImage(image = bundle, source)
 serve ◀═══════ fetch lane ═══════════════  fetch_pin, chore held: stage ─▶ validated
                                            consensus persists the install
                                            ActivateImage ─▶ chore: adopt(bundle) ─▶ activated
 SnapshotReleased ─▶ chore: release_pin     Committed past the boundary
```

- **The leader** pins at `SnapshotNeeded`, on the chore between entries, the bundle at the key's
  boundary when it is its latest one (restored, installed or published); another boundary pins
  nothing. One pin per bundle, kept until a later bundle supersedes it and no flight holds it:
  consensus sends the image as the flight starts, so the learner's pin fetch can outrun the
  chore, find none and fail validation, and the leader's next flight then finds the pin. A pin
  released at its flight's end would lose that race every time. A publish or an activation
  queues the release of superseded pins; strata's release commits, and short of space it stalls
  the chore as an apply does and repeats after `resume()`, so a full disk never fences the
  leader. A checkpoint's `unlink_retired` stops below the lowest live pin, so the set's files a
  pin names stay until it goes: the latest bundle's pin keeps them one checkpoint longer. A
  close releases what is left on the chore once its consensus row is closed (the index is the
  catalog's and stays open, D169); pins a crash or a fenced close left in strata's manifest go
  after the next open: recovery reads them (`held_pins`) and queues their release on the chore,
  where short of space it stalls as above, so a full disk never fails the open.
- **`serve`** answers the fetch lane on any thread: a pin's own name or its files' chunks, nothing
  for a pin released or once closed. The directory is borrowed, so the teardown waits for every
  serve in flight before close returns it to the host.
- **The learner** at `ValidateImage` decodes the image as a bundle at the boundary, fetches the
  pin from the source incarnation, then holds the chore for the stage (apply waits; the stage is
  the chore's only work), stages, and answers `validated`, or `validation_failed` with what it
  staged kept. consensus persists the install only after `validated`.
- **Activation** at `ActivateImage` is a chore item behind what was queued below the boundary:
  `adopt`, the step recovery runs after opening strata, finishes the stage (`resume`) and loads
  the set and the state from the bundle; then `activated`, and consensus delivers past the
  boundary. A failure fences the replica (`activation_failed`). A crash between consensus's
  persist and `activated` recovers through the same `adopt`.
- **A close** waits for the learner's stage and activation tasks as for the chore: its teardown
  ends once neither runs, so the host may close the index and destroy the group on return.

## Repair

A replica whose bytes fail their checks reinstalls its leader's image through the same path
(lead's decision 2026-11-21; consensus `docs/mechanisms/install-request.md`).
`include/strata/group/repair.hh` holds the marker and the checks; `group.hh` binds them.

```
 damage found                       fenced locally                 repaired
 open: corrupt_index ──┐            reads, writes, checkpoints
 recovery: set check ──┼─▶ rd ──▶   answer replica_damaged   ──▶   Driver::reinstall ─▶ image
 apply, read ──────────┘  (durable) ranges acknowledged unapplied  stage, activate (CRC32C) ─▶ rd goes
```

- **Damage** is strata's `corrupt_index` or `corrupt_segment` and the set's `file_set_damage` (a
  missing, short or, sealed, CRC32C-failing file), met at the index's open (`group::open_index`),
  at recovery's `files_.recover`, at apply or at a read. A segment maps at its first read
  (cold-tier D2), so its footer and sections are checked there, at the read, apply or merge
  that first touches it; a host that sets `eager_segments` finds it at the open. A leader's pin
  checks each segment's footer (checksum, magic, the manifest's table CRC) before it lists the
  segment, so a source never ships a segment it has not read: a mismatch is its damage. A live set file
  carries no CRC32C, so its damage is not seen (F24: no scrubber).
- **The marker** `rd` (its text the reason, through a temporary and a rename, the directory
  synced) goes first: a crash anywhere from here reopens the replica damaged. A damaged index's
  names then go (`quarantine_strata`) and a deferred index takes their place, which install's
  activation commits before it installs (F2). A marker that cannot be written fences as before.
- **Fenced locally.** consensus's Core is not fenced (`application_failed` would end the
  incarnation). The group refuses reads, proposals and checkpoints with `replica_damaged`,
  answers every waiter so, pins and serves nothing, and runs only the install's own chore items.
  A delivered range is acknowledged unapplied: consensus refuses the request while a range is
  outstanding, and it re-delivers past the image's boundary from this replica's log. The
  acknowledgement moves consensus's applied index only; votes and commits count the log, which
  the replica holds, and the group's own applied index stays, so nothing is served from state
  it lacks.
- **The request.** `Driver::reinstall` on the loop, one at a time, repeated each election
  timeout while damaged (`Current`: the leader ships one image per base; `NotLeader`, `Busy`). A
  damaged leader (`LeaderMustTransfer`) hands leadership to the follower the host names
  (`applied_past`), else to a voter of its membership, first.
- **The image** stages as any install: kept bytes that fail the pin's CRC32C are fetched whole,
  so only damaged or missing files cross the lane. The activation reads every staged file back
  against its CRC32C before it moves anything; one that fails marks the replica damaged, sweeps
  the stage, and the activation still answers `activated` (consensus holds the image as its
  base; failing it would fence the Core), so the replica asks again. A complete activation
  removes the marker, durably, before it sweeps the stage; the group's recovery from the bundle
  then ends the local fence. A crash after the marker goes recovers through `resume` as any
  activation does.

## What it costs

A fresh learner moves every pinned byte once over the lane: G's segments, its manifest log, and
the set's files to P_b. The leader reads each live file and segment once more at the pin for its
CRC32C. The learner writes strata's files twice (staged, then strata's own copy under its names)
and the set's once, and needs room for both until the sweep. The activation reads every staged byte once more for
its CRC32C. A repair moves what an install of the leader's base moves, less the set's files the
replica holds whole; an idle group carries two flags and a reason string for it.

## What it relies on

- strata's `install` is old-or-new across a crash and leaves the target unfenced on a failure.
- The source keeps the pin's files: strata keeps a pinned generation's segments and log; a set
  file the bundle names and a later bundle retires must not be unlinked while a pin names it.
- consensus delivers the bundle as its install image and persists it only after the learner's
  semantic validation (the stage) succeeds.

## Proof

`test/group/voters_test.cc` drives the binding on consensus's Driver, transfer frames and
fetches on its seeded network. Messages reorder; a pair's transfer frames keep their order, as on
the Bulk connection, and a lost one stalls the flight one `snapshot_timeout` (3.6 s there, 30
election timeouts as at consensus's defaults), after which the leader starts it over:
`install_gap` (a replica cut off while every other checkpoints
past it installs and its digest equals the replay), the ungated schedules (group-core's seeds 8,
12, 16, 18, 23, 24, 27 and seeds 101-140 at 3 voters, crashes and power cuts included, settle by
install), and `grow_and_heal` (at 3 and 5 voters, a joiner added past every checkpoint installs,
is promoted beside a voter it replaces, and consensus's core removes that voter by itself (D3),
a leader change between each step; the removed voter never learns its removal's commit, so the
test retires it before settling; the second round replaces the leader). `test/group/install_suite.sh BUILD [regex]` builds
and runs ctest's group tests, against a consensus checkout when CONSENSUS_DIR is set.
`release_short_of_space` fills the leader's disk after a checkpoint supersedes the install's
pin: the release stalls the leader, unfenced, and it settles once resumed (mutant
`release-fences` fences it instead). Mutant `pin-per-flight` releases a pin at its flight's end:
`install_gap` does not settle. `test/group/mutants.py install-silent` drops `SnapshotNeeded`: the ungated schedules do not
settle. `reopen_short_of_space` restarts a leader holding its bundle's pin on a full disk: it
opens, the pin's release stalls, and once resumed the pin goes and the cluster settles (mutant
`release-at-open` releases at recovery and fails the open). `deferred_learner_crash` cuts the
power of a learner whose index is deferred at every mutation of its directory from its rejoin
through the settled install: each reboot settles with equal digests and no staging name (mutant
`install-onto-deferred`, which installs without the first commit, leaves a directory strata
refuses to open).

`test/group/repair_test.cc` (ctest's `group_repair`; `group_repair_test [case]`): `segment`,
`manifest`, `file` (a stopped follower's damaged segment footer, manifest log or sealed set file:
it reopens damaged, a damaged index replaced by a deferred one, and reinstalls once; a damaged
segment is found by the first read that maps it, which answers `replica_damaged`, and at the
open under `eager_segments`, ctest's `group_repair_eager`), `apply`
and `read` (damage met while running; the read answers `replica_damaged`), `refusal` (while
damaged, commits counting its log, it answers every read, checkpoint and write
`replica_damaged`, serves no install and keeps its applied index while consensus's moves on;
the only healthy replica that applied those commits is then lost and they survive), `staged` (a
staged live set file flipped before the activation is refused, and a later image repairs),
`leader` (a damaged leader hands off, then reinstalls), `source` (a leader's segments damaged
and never read since its reboot: a learner's request makes it pin them, the footer check marks
it damaged, it hands off, and the learner and it install from the next leader), `deposed` (a leader that loses its
leadership mid-flight names no install from then on, and none does once settled: every flight's
release ends its peer's entry, review F3), and `crash` (a power cut at every
mutation of the directory disk, then the host disk, from the damaged reboot through the settled
repair, for a damaged manifest log on a deferred index and a damaged set file: each reboot is damaged
or repaired, never unfenced over damaged bytes). `test/group/repair_mutants.py` holds one mutant
per mechanism, each killed by the case it names; `test/group/repair_suite.sh` builds and runs
the group tests.

`test/group/install_test.cc` (ctest's `group_install`; `group_install_test [case]`):
`values` (the pin, the serve's bounds, a fresh learner fetching every byte once in
ceil(length / 64 KiB) chunks, the digest after catch-up equal to the leader's and a replay's),
`retry` (fetches ending `timed_out` and `io_error` mid-window, then a retry that fetches exactly
the bytes not staged), `held` (an old replica keeps its files and fetches the rest), `damaged` (a
flipped byte in a held sealed volume: that file fetched whole), `crash` (a process crash and a
power loss at every mutating operation from the stage through the activating recovery, persisted
or not, then the activating recovery crashed at each of its own: old replica or complete new
one), `deferred` (the same crashes on a fresh learner whose index is deferred: the empty replica
or the complete new one), `threads` (fetches answered on four threads; TSan). `test/group/install_mutants.py` holds
one mutant per guard, each killed by the case it names.
