# Media faults: unreliable reads, bad sectors, and lying writes

Phase 3's rows test power loss, ENOSPC, failed writes and syncs, and files damaged at
rest. This note adds storage that misbehaves while running: reads that fail or return
wrong bytes, transiently or for good, and writes the device acknowledges but loses. At
100,000 nodes these are routine events. Field studies of 1.5 million disks found latent
sector errors on 3.45% of disks within 32 months (Bairavasundaram et al., SIGMETRICS
2007) and silent checksum mismatches on 0.86% of nearline disks within 17 months
(Bairavasundaram et al., FAST 2008).

## Fault model

| Fault | What the device does | In the model |
|---|---|---|
| Transient read error | A read fails with EIO, the next succeeds | Yes |
| Latent sector error | Every read of a range fails with EIO | Yes |
| Transient corruption | A read returns wrong bytes (bus, cable, controller, DMA), the next is right | Yes |
| Persistent corruption | Bit rot: every read returns the same wrong bytes | Yes |
| Torn write, power loss | Already in faultfs | Yes |
| Failed `fsync` | Already in faultfs; it fences, since a retry can report success for lost data (PostgreSQL's 2018 "fsyncgate") | Yes |
| Misdirected write | A write lands at another file's location | Detected, not repaired locally (below) |
| Lost write | The device acknowledges a write and a sync, then never persists it | Out of the model for the consensus journal (below) |
| Memory or CPU corruption | Before a checksum is computed or after it is checked | Out of the model (REPLICATION, Guarantee) |

## Other engines

- **Lucene** ends every file with a CRC32 footer. It verifies small files whole at open
  and every file whole when it merges or replicates them (`checkIntegrity`), and
  `CheckIndex` verifies everything on demand. Ordinary reads are not checked.
  Elasticsearch turns a `CorruptIndexException` into a marker file, fails the shard, and
  rebuilds it from a replica.
- **Tantivy** gives each file a checksummed footer and validates it on request, not on
  read.
- **Xapian**, as its documentation describes, has no per-block checksums; corruption
  surfaces when structural checks fail, as `DatabaseCorruptError`.
- **RocksDB** verifies a block checksum on every read by default, and on a mismatch
  stops background work and reports the error.
- **etcd** compares state hashes across members periodically to find a member whose data
  diverged.
- **ZFS and Btrfs** checksum every block end to end and repair a bad copy from a
  redundant one.
- **The research.** Ganesan et al. (FAST 2017) injected single-block read errors and
  corruption into ZooKeeper, Kafka, Cassandra, MongoDB, CockroachDB, and others. Most
  crashed, propagated the corruption, or lost data instead of using their redundancy.
  Alagappan et al. (FAST 2018, protocol-aware recovery, "CTRL") show that Raft and ZAB
  logs need corruption handling of their own: truncating a corrupted entry as if it were
  a torn tail can lose a committed entry.

## What strata does

- **Every byte is checked before it is used.** Manifests, live-docs files, and metadata
  carry a trailing CRC-32C; a segment's footer and table of contents are checked at open;
  its terms, postings, positions, and stored records are checked per region on first
  read, and its lengths on first use. A file's identity (segment id and index UUID in its
  header, its size, CRC, and generation in the manifest) is checked too, so a stale or
  misdirected copy of another file fails like damage.
- **One retry, then damage.** A read that fails, or whose bytes fail their check, is read
  again once (`index::read_checked`). If the second read passes, the fault was transient
  and the index continues; `media_retries()` counts it. If it fails, the file is damaged:
  the open fails with `corrupt_index` when the read error is EIO; other errors propagate
  as themselves.
- **Healing after the open.** Regions checked on first read can meet a transient fault
  after the open. A search then throws `corrupt_segment`; the host calls `heal()`, which
  verifies each committed segment in memory and reads a failing one again, verified whole,
  in its place. It returns the segments whose file is damaged too.
- **Never a wrong answer.** A query over damaged bytes throws; it never returns a result
  computed from them.
- **Repair.** A damaged shard is fenced and repaired: in a group, from a member's image
  (Track C's D10, repair at or below the commit cursor); standalone, from a retained
  predecessor that does not name the damaged file (`recover --to`), else it stays fenced
  for the operator.
- **Term dictionary blocks** are checked through the region callback termdict calls; its
  `termdict::corrupt` is reported as `corrupt_segment` like every other region.

## Tests

`media_test` (faultfs media faults: EIO or one flipped bit over a byte range, the next N
reads or every read):

- 782 offsets across every file of a three-segment index with deletes and metadata, each
  with a transient flip and a transient EIO: every open and digest equals the undamaged
  digest (529 healed by the open's retry, 104 by `heal()`). Without the retry, a
  transient EIO on the head manifest fails the open.
- The same offsets with sticky faults: 529 fail the open, 104 are reported damaged by
  `heal()`, and 149 land in bytes nothing reads (older manifests, padding). The digest
  never differs from the undamaged one.
- A merged segment damaged for good: the head fails, and `recover --to` the retained
  predecessor opens with the same digest.

## Next

- **Scrubbing.** A background pass verifies every file whole on a schedule (each file
  within a week, at a bounded rate) and runs `heal()` on what fails, so latent errors are
  found while the replicas still hold good copies. Its rate is budgeted with merges and
  reclamation.
- **Cross-replica digests.** Replicas compare `digest()` at a log position periodically,
  as etcd compares hashes, which also catches divergence that no checksum sees.
- **Device health.** Transient retries and damage are counted per device; a device past a
  threshold drains, a placement decision (Track D).
- **The consensus journal.** A lost write is indistinguishable from a torn tail without
  an independent record. Track C's shared journal (D11) treats a bad block followed by
  valid blocks as corruption and costs only the groups with records in it; protocol-aware
  recovery (fetching a damaged committed entry from the leader, not voting until then)
  would let those groups keep their incarnation. That is a Track C question.
- **Mappings.** With mmap, a page read again after eviction is not checked again; the
  verified block cache (ARCHITECTURE, I/O) is the mode for hosts that require every read
  checked.
