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

- **Paths.** A path is 1 to `max_path` bytes (strata's key limit, `format::max_term_bytes`, less
  the tag) of `/`-separated names, none empty, no NUL, no leading or trailing `/`. `twitter` and
  `twitter/user` are two keys, `Ptwitter` and `Ptwitter/user`. Names starting with `.` are records
  like any other; whether a listing shows them is the scan's rule.
- **Order.** Keys sort as bytes, so namespace keys sort as their paths: every one is in
  `[namespaces_begin(), namespaces_end())` = `["P", "Q")`, and the subtree under `p` (its
  descendants, not `p` itself) is the contiguous range `["P" p "/", "P" p "0")`, since `0` follows
  `/`. A sibling such as `p-x` sorts between `p` and `p/...`, so a matcher that wants `p` and its
  subtree seeks twice or reads past the siblings between. Node and load keys sort by id.
- **What the scan relies on**: `namespace_key(path)`, `path_of(key)` and the range above
  (`records.hh`); a scan reads `basic_index::scan(from, to, n, f)` and seeks by starting a new scan
  at the next key it can match.

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
