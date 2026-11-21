> Status: the transfer is built and tested on its own (quest group-heal, task `install`);
> its binding to the group's chore and consensus's install is not. Design: Xapiand
> `docs/design/cluster-release.md` D16 (install moves the file set), D7, F1; contracts
> `strata:group-install`, `strata:checkpoint-bundle`.

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
and `release_pins` drops them all when the leader reopens.

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
   `is.` names): the old content or the new across a crash. The staged strata files go next,
   the manifest log first, so a repeat does not install again.
2. Each staged set file replaces the replica's own.
3. The marker goes, then every staging name.

Recovery runs `resume` first: a complete stage whose bundle is the one consensus restored was
persisted, so activation finishes; a stage at or below the restored bundle is stale and swept;
one above it waits for consensus's retry. The group's recovery then runs as always: P_b ≤ P_H,
the set truncated to the bundle's lengths, its state. Every step repeats safely, so a crash
anywhere leaves the old replica (before consensus persists) or the complete new one (after).

## What it costs

A fresh learner moves every pinned byte once over the lane: G's segments, its manifest log, and
the set's files to P_b. The leader reads each live file and segment once more at the pin for its
CRC32C. The learner writes strata's files twice (staged, then strata's own copy under its names)
and the set's once, and needs room for both until the sweep.

## What it relies on

- strata's `install` is old-or-new across a crash and leaves the target unfenced on a failure.
- The source keeps the pin's files: strata keeps a pinned generation's segments and log; a set
  file the bundle names and a later bundle retires must not be unlinked while a pin names it.
- consensus delivers the bundle as its install image and persists it only after the learner's
  semantic validation (the stage) succeeds.

## Proof

`test/group/install_test.cc` (until registered, `test/group/install_build.sh BUILD [case]`):
`values` (the pin, the serve's bounds, a fresh learner fetching every byte once in
ceil(length / 64 KiB) chunks, the digest after catch-up equal to the leader's and a replay's),
`retry` (fetches ending `timed_out` and `io_error` mid-window, then a retry that fetches exactly
the bytes not staged), `held` (an old replica keeps its files and fetches the rest), `damaged` (a
flipped byte in a held sealed volume: that file fetched whole), `crash` (a process crash and a
power loss at every mutating operation from the stage through the activating recovery, persisted
or not, then the activating recovery crashed at each of its own: old replica or complete new
one), `threads` (fetches answered on four threads; TSan). `test/group/install_mutants.py` holds
one mutant per guard, each killed by the case it names.
