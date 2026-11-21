> Status: records, keys and placement built (quest placement, task `root`); the scan is task
> `scan` (`placement/scan.hh`), repair is task `repair`. Design: Xapiand
> `docs/design/cluster-release.md` D5 and D7, `docs/design/cluster-membership.md` M1 and M5;
> contract `strata:placement-records`. The bytes are pre-release (D30): open until the first
> release, reviewed by `format-freeze`.

# The placement root

The root authority is one strict-consensus `strata::group` (`docs/mechanisms/replication-group.md`)
whose application is `strata::placement::root` (`include/strata/placement/root.hh`). Its state is
a strata index of the document layout: every record is a key and a stored value, so its checkpoint
is a strata manifest, its install is group-install's, and its memory is strata's caches. The
application state in the bundle is one format byte.

## Keys

One strata index holds every record. A key is a tag byte and the record's name:

| Key | Record | Value |
|---|---|---|
| `D` | the directory | next node id, the application's attributes |
| `G` | the group counter | next group id |
| `L` + id, 8 B big-endian | a node's load | groups (memberships, witnesses included), preferred leaderships |
| `N` + id, 8 B big-endian | a node | name, addresses, certificate pin, state (`active`, `leaving`, `retired`), directory version, attributes |
| `P` + path | a namespace, one per index | `rf`, settings version, per shard: group id, incarnation, members (node, role) |
| `S` | the cluster settings | `replace_after` in ms (repair's, `placement-repair.md`); put, never erased |

- **Paths.** A path is 1 to `max_path` bytes (strata's key limit, `format::max_term_bytes`, less
  the tag) of `/`-separated names, none empty, no NUL, no leading or trailing `/`. Its key stores
  each `/` as byte 0: `twitter` and `twitter/user` are `Ptwitter` and `Ptwitter\0user`, two
  records. Names starting with `.` are records like any other; whether a listing shows them is the
  scan's rule.
- **Order.** Keys sort as bytes and names hold no NUL, so namespace keys sort in path order, name
  by name: `twitter` < `twitter/user` < `twitter/user/a` < `twitter-a`. Every one is in
  `[namespaces_begin(), namespaces_end())` = `["P", "Q")`, and the subtree under `p` (its
  descendants, not `p` itself) is the contiguous range `[subtree_begin(p), subtree_end(p))` =
  `["P" p' "\0", "P" p' "\1")`, where `p'` is `p` with each `/` as byte 0, right after `p`'s own
  key: a scan reads each record once. A key holding a raw `/` names no path. Node and load keys
  sort by id.
- **What the scan relies on**: `namespace_key(path)`, `path_of(key)`, `subtree_begin(p)`,
  `subtree_end(p)` and the ranges above (`records.hh`); a scan reads
  `basic_index::scan(from, to, n, f)` and seeks by starting a new scan at the next key it can
  match.

## Records

A value is a format byte (1), then LEB128 integers in their shortest form and byte strings as a
length and bytes. Decoders throw `corrupt_record` on a truncated, overlong or trailing input, a
count past the bytes left (checked before anything is allocated), an unknown enum, and on these
rules: next ids are at least 1; a load's leaderships are at most its groups; a namespace has
`rf >= 1` and at least one shard, its group ids nonzero and distinct, incarnations at least 1, each
shard at least one member, its nodes nonzero and distinct, the first a voter.

- **The directory** is M1's cluster record as strata sees it: the node counter, with Xapiand's
  fields (CA, name) in `attributes`. Its strata version is the directory version (M5): every node
  change rewrites it, and the node record carries the version its change wrote, so a node behind
  the version a keepalive carries fetches the node records above its own.
- **Node ids and group ids** come from the counters and are never reused: a command may not erase
  `D` or `G`.
- **Members** are the shard's group as placed: the first is the preferred leader the root spreads
  leaders by (`transfer_leadership`); a current leader is never recorded (D5).

## Commands

A root command is a list of record writes, each a put or an erase with an optional expected
strata version (0: absent), applied as one strata transaction, all or none. Apply decodes it and
checks every put's value with its key's decoder; a command that fails is answered `invalid` and
writes nothing. The leader builds every command (D106's prepare, `strata/group/submit.hh`) from the
records it reads plus its own commands placed and not yet applied, expecting each record's version
as it read it; a command that loses a race is refused `version_conflict` and prepared again.

| Operation | Writes |
|---|---|
| add node | `D` (next id + 1), `N`+id (expected absent), `L`+id zero (expected absent) |
| update node | `N`+id, `D` rewritten (the directory version moves) |
| remove node | refused while its load holds a group; erases `N`+id and `L`+id, rewrites `D` |
| create namespace | refused if the path exists; `P`+path (expected absent), `G` (next + shards), each chosen node's `L` |
| replace namespace | `P`+path at its version, the `L` of every node whose memberships change |
| delete namespace | `P`+path erased at its version, its members' `L` decremented |

## Placement

`placement.hh` places one shard at a time on the active nodes, from their loads, and counts each
choice before the next shard:

1. The preferred leader: the node with the fewest preferred leaderships (then fewest groups, then
   lowest id).
2. The other data members: the `min(rf, nodes) - 1` other nodes with the fewest groups (then fewest
   leaderships, then lowest id).
3. Two data members are never a lasting two-voter group (D7): with a third node, a witness joins on
   the node with the fewest groups among the rest; on two nodes the second member is a learner.

Every data member but a learner is a voter. With fewer active nodes than `rf` the shard runs short.
Choosing the leader first keeps every node's leaderships within one of each other under
creations, so at most one above the mean; the other members take the fewest groups.

The leader choice is the trade: the leader slot ignores groups, so with mixed `rf` group counts
drift apart (a model of 5,000 random creation sequences on up to 12 nodes: leaderships at most
0.92 above the mean, groups at most 2 apart at one `rf` and 10 apart with `rf` mixed from 1 to 5),
while choosing all members by groups first lets leaderships reach 2 above the mean at one `rf` and
over 100 with `rf` mixed. Deletes and joins move the mean under a node's leaderships; spreading them
again is repair's leader balance (`transfer_leadership`), not placement's.

## Proof

- `test/placement/root_records_test.cc` (ctest `placement_root_records`): each record and a
  command against fixed bytes both ways, every strict prefix and a trailing byte refused, each
  decoder rule refused by a record that breaks it alone (a count of 2^62 refused before any
  allocation), path validity, and over random paths: key order equals path order, and a path's
  subtree range holds exactly its descendants, right after its own key.
- `test/placement/root_placement_test.cc` (ctest `placement_root_placement`): 2,000 seeded
  creation sequences on 1 to 12 nodes, every shard with `min(rf, nodes)` distinct data members,
  D7's roles, a voter first, the loads equal to the shards placed, and every node's leaderships at
  most one above the mean. `root_placement_model.py` (ctest `placement_root_model`) checks the
  choices for 1,000 scenarios with random loads, repeated or zero ids and `rf 0` against a model
  of the rule written apart.
- `test/placement/root_group_test.cc` (ctest `placement_root_group`): on a lone voter, nodes and
  namespaces created, read, updated and deleted through the writer, `twitter` and `twitter/user`
  distinct, a second create and a busy node's removal refused, a leaving node skipped, five
  concurrent creates placed once each with distinct group ids, a stale command refused on its
  expected version, a malformed one `invalid`, and the records equal a replay of the log. At 3 and 5
  voters, 20 seeds each lose every minority in turn (power cut, never back) at a seeded event while
  creates and deletes run, half also cutting and restarting a survivor: every acknowledged record is
  on every survivor, and their records equal each other's and a replay.
- `test/placement/root_fuzz.cc`: the decoders under libFuzzer (`STRATA_FUZZ`); whatever decodes
  encodes back to the same bytes. Its corpus replays as ctest `fuzz_placement_root_replay`.
- `test/placement/root_mutants.py`: one mutant per guard, each killed by one of those tests.
