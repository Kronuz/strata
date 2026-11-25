# A group's open and its refusals

`strata::group::open()` (`include/strata/group/group.hh`) joins a replica to its consensus group
and recovers it over the index its host lends (the recovery steps are in
[replication-group.md](replication-group.md), Recovery). Three positions decide whether the
replica on disk is one consistent pair of journal and index:

- **P_b**, the recovered bundle's position: the last checkpoint consensus published.
- **P_H**, strata's durable head (`index.applied()` once opened): the last entry strata committed.
- **E**, the recovered log's end (`Driver::state(id).last`): read on the loop right after
  `driver.recover` (or `driver.create`) returns, so after the log's tail is recovered and before
  the group can elect and append.

A sound pair has **P_b <= P_H <= E**. A bundle publishes only after strata committed through it,
and consensus delivers an entry only once the durable commit index covers it, which is persisted
after the entry. Each break of that order refuses the open.

## The three refusals

| Refusal | Order broken | How it arises | Checked | Leaves on disk |
|---|---|---|---|---|
| **Journal older** | P_H > E | the journal put back from an earlier copy, a copy of a running data root, a plain-mode power cut on macOS that lost the log's tail | first in `adopt`, before install's `resume`, the quarantine's release and the file set's recover | every index and volume file byte-equal; consensus closes the group's row in the journal |
| **Journal missing** | P_H > E, E the created log's | the journal holds no group (lost or replaced) over an index with state: `open` creates the group, and its new log ends below P_H | the same check | the same; the created group's row is closed |
| **Index older** | P_b > P_H | the index's directory put back from an earlier copy, under a newer journal | in `adopt` after `resume`, before the file set's recover | the index's files as `resume` left them (it finishes or sweeps an install stage only); the file set untouched |

A journal whose log is damaged is refused earlier still: journal's Store loses a bounded group
inside `driver.recover`, before `adopt` (replication-group.md, A damaged turn log).

The refusals throw `group_fenced`, naming both positions:

```
strata::group: strata's durable head P_H = 9 is past the recovered log's end E = 4: the journal is older than the index
strata::group: the bundle is past strata's durable head (P_b > P_H)
```

The open's failure path then fences the group locally, closes its Driver row and leaves the
loop's table, so the same process can open the id again once the right journal or index is back.
Nothing is repaired: replaying from E would place new entries at positions strata already holds,
and strata would skip them as replay.

## What a sound open reads

| Path at open | P_b | P_H | E |
|---|---|---|---|
| clean stop (the close checkpoints through the commit index) | the close's bundle | P_b | at least P_b |
| crash with a tail | the last bundle | at least P_b (a commit whose publish was lost) | at least P_H |
| closed record (a reopen in process, or after a restart) | the close's bundle | at least P_b | the record's last |
| checkpoint plus a tail | the checkpoint's | at least P_b | at least P_H |
| a complete install stage that consensus persisted | the boundary B | the old index's, below B (consensus installs only above its commit index); B once activated | at least B |
| a damaged index (quarantined, opened deferred) | any | 0 | any |

| a restart after an install's activation, before the log passed B | B | B: the leader pins strata at the bundle itself (docs/design/journal-binding.md D5), and the learner's `check_stage` refuses a staged head past it | at least B |

An install's activation (`activate_image`) is not checked: the image replaces the index, so the
old P_H says nothing about the new pair; the new one is the bundle's boundary (D5,
`test/group/install_bundle_test.cc`). Tests: `test/group/journal_behind_test.cc` (clean-stop,
crash, missing, sound) and `test/group/lifetime_test.cc` (failed-open, P_b > P_H).
