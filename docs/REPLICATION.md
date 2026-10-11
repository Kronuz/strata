# strata replication, durability, and consistency

This document defines the fault model, the guarantee the engine gives inside it, the
split of responsibilities between the engine and Xapiand, and the consistency modes.

## Guarantee

> Under the fault model, at most `f` of `2f + 1` voters fail at once by crash, power
> loss, or disk loss; `fsync` and `F_FULLFSYNC` are honored; and nodes are not
> Byzantine. Within it, a write acknowledged with `ack=quorum` or `ack=all` is never
> lost, and a replica never serves a log entry, manifest, or block whose checksum fails
> where it is verified: it fails closed and reports the shard unavailable.

Outside the model, loss or corruption is possible:

- More than `f` simultaneous failures.
- Disks or filesystems that acknowledge flushes they do not persist.
- Corruption CRC32C misses (about 2^-32 per corrupted block).
- Memory or CPU corruption before checksumming, as on hosts without ECC.
- Software bugs.
- mmap pages re-read after verification (the strict mode's verified block cache
  closes this).
- Writes acknowledged with `ack=local` or `ack=none`, which are outside the guarantee
  by definition.

## Ownership

```
 Xapiand        HTTP, schema, routing, membership hints (gossip)
 meta group     strict-consensus authority: placement, group membership
 ─────────────────────────────────────────────────────────────────────
 strata::group  one replication group per shard: consensus + journal + core
 ─────────────────────────────────────────────────────────────────────
 strata core    deterministic state machine: apply, checkpoint, install
```

- **Core.** Applying the same batches in the same order produces the same logical
  content. The core exposes `apply(batch, position)`, `applied_position()`,
  `visible_position()`, `checkpoint()`, and `install(checkpoint)`. It has no network
  or consensus code.
- **Group** (`strata::group`, its own target in the strata project). It composes
  `cluster::consensus::Core`, `kronuz::journal::Journal`, and the core. The journal
  replaces Xapiand's WAL. Committed batches apply to the core in log order.
- **Meta group.** Placement and group membership are decided by a strict-consensus
  authority group, not by gossip. Gossip supplies liveness hints only; the legacy
  `cluster::Raft` is volatile and does not decide membership.
- **Xapiand.** It routes requests, maps its schema, and runs gossip. It never touches
  the log or segment files. Xapiand's per-shard `ELECT_PRIMARY` election retires when
  groups land, and group leadership becomes the single source of truth.

Log truncation depends on checkpoints, checkpoints on manifest retention, and replay on
deterministic apply. All three are engine internals, so they live in one project.

## Determinism contract

The leader resolves all nondeterminism before proposing: generated ids, timestamps,
script effects, and schema resolution (including schemas read from other indexes). A
command carries the prepared document, the expected version, and the schema version.
Apply rejects a schema-version mismatch, and the leader re-prepares. Apply is a pure
function of state and batch. Tests compare a logical-content digest across replicas
after every simulated schedule, because divergent replicas pass every checksum.

## Write path

```
 client ─▶ leader: prepare → admit → group commit → journal append + barrier
                                       │ replicate
                                       ▼
                    followers: journal append + barrier → ack
                                       │ majority → committed
                                       ▼
          every voter: apply(batch) → refresh → visible
```

The seqno of an operation is the pair (log index, position in batch), ordered
lexicographically. Group commit puts concurrent writes into one log entry, so one
durability barrier on each voter covers many documents.

Replication is `document` mode: every voter applies every committed batch. It costs
indexing CPU on every voter and gives deterministic logical content and immediate
failover. Physical segment files differ between replicas.

## Checkpoints and repair

- A journal checkpoint bundle references a strata manifest by applied log position,
  term, and manifest CRC32C. Strata owns its segment files and their retention; the
  journal never copies them.
- Checkpoints are triggered by retained-log bytes and elapsed time, never by refresh.
  The checkpoint freeze is budgeted against the election timeout.
- A new or far-behind voter installs a checkpoint: snapshot transfer streams a
  segment pack in 64 KiB chunks from a pinned manifest.
- A voter whose segment files fail a checksum reinstalls a checkpoint at or below its
  commit position and replays its own log from there.
- Recovery replays the journal from the applied position recorded in the referenced
  manifest.

## Replica count

Raft tolerates `f` failures with `2f + 1` voters, so the engine supports:

| Configuration | Data copies | Voters | Tolerates |
|---|---:|---:|---|
| `rf = 1` | 1 | 1 | No failure |
| `rf = 3` | 3 | 3 | One failure |
| `rf = 5` | 5 | 5 | Two failures |
| `rf = 2 + witness` | 2 | 3 | One failure; the witness stores the log, never segments; it leads only to ship its log to a data voter and hand leadership off |

Two-voter groups are not supported: they tolerate no failure and block writes while
either copy is down. Xapiand's `num_replicas = 1` (two copies) maps to
`rf = 2 + witness`.

## Consistency modes

### Write acknowledgement (`ack`)

| Mode | Acknowledged when | Guarantee |
|---|---|---|
| `quorum` (default) | A majority of voters persisted the batch | Survives any `f` failures. With `rf = 1`, it is a local durable write |
| `all` | Every voter persisted the batch | Same, and every replica is current. Times out while any voter is down |
| `local` | The leader persisted the batch | Acknowledged before commit: lost if leadership changes before the batch commits |
| `none` | The leader admitted the batch | Acknowledged before persistence: lost on leader crash or leadership change |

### Read consistency (`read`)

| Mode | Served by | Sees |
|---|---|---|
| `nearest` (default) | Any replica | Its latest refreshed snapshot |
| `session` | A replica whose visible position is at least the client's token | The client's own writes |
| `linearizable` | The leader, after a quorum read probe and a refresh covering the captured commit position | Every write acknowledged before the read started. Get-by-id is served from the live version map; searches pay a forced refresh |

Every write returns its seqno as the token for `session` reads.

### Visibility (`refresh`)

| Mode | Behavior |
|---|---|
| `false` (default) | Visible after the next periodic refresh (1 s) |
| `wait_for` | The write returns once a refresh makes it visible |
| `true` | The write forces a refresh before returning |

### Versioned writes

Expected-version checks are evaluated at apply, so every voter reaches the same
verdict, which the leader reports after commit.

## Cost

A journal append measures four barriers (journal sync, temporary manifest sync,
rename, directory sync), about 62 ms p50 per durable group on the development Mac.
With the `quorum` default, a single client issuing sequential writes is bound by this
latency; concurrent clients share it through group commit. Linux NVMe measurements
and a two-barrier frontier design are part of Track C.

## Xapiand mapping

| Xapiand today | Group equivalent |
|---|---|
| Async WAL default (`DB_NO_SYNC` plus async writer) | `ack=none` at `rf=1` |
| `DB_SYNCHRONOUS_WAL` | `ack=local` at `rf=1`, which equals `quorum` there |
| `?commit` | `ack=quorum` plus `refresh=true` |
| Autocommit debounce (1 s) | Periodic refresh (visibility) and the checkpoint policy (truncation) |
| `DB_DISABLE_WAL` for RESTORE | `ack=none`, or a bulk-import command whose segments the leader builds |
| `DB_DISABLE_WAL` for replica seeding | Checkpoint install |
| Async pull on `DB_UPDATED` | Leader push; learners catch up |
| Live glass file copy with retries | Install from a pinned manifest |
| `ELECT_PRIMARY` | Group leadership |

## Corruption defense

- CRC32C everywhere: journal batches, segment files, manifests, live-docs files.
- Verification points are listed in [ARCHITECTURE.md](ARCHITECTURE.md#manifest-and-commit);
  a scrubber verifies whole files on a schedule.
- A checksum failure fences the replica's shard, which repairs through a checkpoint
  reinstall.
- Publication follows write, fsync, rename, and directory fsync (`F_FULLFSYNC` on
  macOS). Recovery never falls back to an older generation silently.

## Consensus library gaps

`cluster::consensus` and `kronuz::journal` have the right shape (an I/O-free
deterministic Raft core with persistence barriers, a fail-closed journal, injected
filesystem tests) and are qualified for a fixed 3–5 voter authority. Required
additions, tracked as Track C in [PLAN.md](PLAN.md):

1. G1: membership changes and voter roles (learners, an optional witness). Without
   it, a node that lost its disk cannot rejoin.
2. G2: the witness role and the configurations in [Replica count](#replica-count).
3. G3: repair at or below the commit cursor.
4. G4: multi-group operation: one journal per core multiplexing its groups, group
   commit across groups, coalesced heartbeats, quiescent idle groups, forced
   checkpoints for laggards.
5. G5: a stream transport with flow control and TLS: Detent's authority peer
   transport (TLS over TCP, control and bulk lanes), which moves into `consensus`.
6. G6: checkpoint bundles that reference strata manifests.
7. G7: cost: Linux NVMe measurement, fewer barriers per append, and defaults retuned
   for bulk ingest (command size, retained log, uncommitted entries).

G1, G3, G4, and G6 close before Phase 3R starts.
