# The write buffer: one buffer for frozen views, live readers, and concurrent writers

Revision 0, 2026-10-21. State: designing (ready for preliminary review). Concurrency (lock-free
structures, lifetimes across threads, leadership handed between threads), so the architect
reviews this design before anything but the bake-off prototype is built (PLAN's review tiers,
the "Durability, concurrency, security surface" row). Decisions D1 to D14, gate rows T1 to T12,
owner questions Q1 to Q3. Code is cited at strata `main` `41fde48` (its `include/` equals
`75143f9`, which shared-reader cites), `design/shared-reader` at `a04671d` plus revision 2 (same
day), `design/durability-modes` at `95357d9`, and `explore/buffer-bytes` at `0a8c85a`. RocksDB
11.11 (`~/.scratch/rocksdb-11.11.fb`) and Lucene 10.4.0 (both Apache 2.0) were read for their
mechanisms; no code of theirs enters strata. Nothing was compiled or run for this design: every
number below is quoted from a cited report or marked as an estimate.

## The owner's direction (2026-10-21)

strata is a generic engine that products use differently. The use case that sets the bar
(Elysian on strata, Xapiand's successor, `docs/PLAN.md:83`): any client checks an index out of a
pool, reads or writes, and returns it at the end of its request, with no single pinned writer and
no refresh cadence. strata ships **one** write-buffer implementation, and it must give all three:

1. **Frozen views**: a cheap freeze and publication of the buffer, with no disk flush.
2. **Live reads**: readers on any thread see acknowledged writes, not the last refresh.
3. **Concurrent writers**: several writers on one index at once.

This design compares three candidates for that one implementation, recommends one, and makes the
bake-off's gate rows decide between the two finalists. The key-value (document) layout comes
first; D11 states what prospect's inverted layout needs.

## Goals (owner, 2026-10-21)

1. **As fast as possible in all three uses, beating every baseline.** On the key-value rows,
   RocksDB at 1, 2, 4, and 8 writers on one shared DB (≤ 1.0× its time, ≥ 1.0× its throughput, the
   `document` gate's shape, `docs/PLAN.md:616-626`); where the inverted layout applies, the faster
   of Lucene and Tantivy at the same writer counts. **The single-writer path is no slower than
   today's buffer** on `main` (T1, T5).
2. **Xapiand and Elysian use the multi-writer mode**, so a request never juggles refreshes and
   never waits on another request's writer.
3. **The fallback.** If several writers cost much more, or the bake-off shows Xapiand's load does not
   need them, Xapiand ships with one writer per index whose readers see live writes (D14). The
   bake-off measures exactly that choice, on Xapiand-shaped load (many small indexes, reads and
   writes mixed in each request) as well as on the key-value rows, against a threshold the owner
   applies (T11, Q3).

## What this builds on, and what it changes

| Document | What this design uses | What it changes |
|---|---|---|
| `docs/ARCHITECTURE.md` | Write path, two stages, stage 1 sequential and deterministic (`:75-119`); seqno under groups is (log index, position in batch) (`:67-73`); memory ownership and bounded deferred frees (`:188-192`); the host reactor never blocks (`:6`) | Item 1, "one writer per shard, owned by one core" (`:88-89`): a write lease passed between threads (D2); item 4's "in-memory segment" becomes lanes and frozen buffers (D3, D8); item 9, refresh as the visibility step (`:108-109`): visibility is the published position (D5). The lead edits it on landing |
| `docs/reviews/stage-1.md` (c), ruling (`:317-336`) | A total order at admission; a sequential, deterministic stage 1 | "One writer per shard ... A Lucene-style DWPT pool is rejected" becomes several writers whose stage 1 stays sequential in one leader (D4). The ruling's reason, deterministic sequential docids and versions, is kept whole; what it rejected, Lucene's global delete queue, stays rejected (D1, candidate B). The architect rules on the change |
| `docs/design/format-14-kv.md` D3 (`:266-328`), T6 (`:384`) | The arena record, the version map's 8-byte slots at most 7/8 full, the two-run sorted view, 494 B per buffered record today and 177 B measured for the arena | The arena becomes per-writer lanes (D3); the version map becomes single-writer, multi-reader (D4); overwrites stop marking the old record dead (D6); the two-run view becomes shared between readers (D9). T6's target stands |
| `docs/perf/buffer-bytes.md` (branch `explore/buffer-bytes`) | 290 B per record on `main`: 112 B value string, 94 B vector element, 42 B key map, 42 B docid map; 8-byte slots measured at -63 B per record | Its order (slots, then the arena) is this design's step 1 and 2 |
| `design/shared-reader` revision 2 | Views, hazard records, the publishable rule, copy-on-write live bits (D3), retention of held segments (D7), non-blocking close (D11), `publish_through` (D9) | Revision 2 (same day) takes the checkout model and states in its D12 what changes when this buffer lands: views gain the buffer, readers read at a published position, bits are applied at freeze |
| `design/durability-modes` D1 (`durability-modes.md:823-955`); Track C D11 (`track-c.md:460-626`) | A turn log per core, one barrier per turn; a group's batch is one record; per-group sequences are contiguous; apply is deterministic and replayable | One batch record per write group (D7); the turn log must accept a staged batch from any thread (D7, a change owed to Kronuz/journal) |
| `docs/design/xapiand-on-strata.md` D7, D13 (`:192-196`, `:224-236`) | Reads see every acknowledged write; the seam is `task::task<T>`; Xapiand's pool checkout (`:125`) | Checkout no longer waits for an exclusive writer (D2, D12); D7's refresh before a search becomes nothing for key-value reads and a coalesced freeze for search (D11) |
| `docs/perf/benchmark-coverage.md` | The KV harness (`bench/kv`), the threaded rows `readwhilewriting` and `fwdrangewhilewriting` (`:93-94`), the thread asymmetry (`:48-50`) | The bake-off rows T1 to T6 run one shared index across threads, as RocksDB shares one DB |
| `docs/perf/core-vs-rocksdb-0021.md` | The single-thread rows strata wins (26 of 26 non-durable, `:21-23`) | T5: the chosen buffer keeps every one of them |

## Today's buffer, and what each property needs from it

The document layout's buffer is a `borrowable_vector<buffered>` of whole documents (two
`std::string`s each), a docid map and a key map, both `hash_slots` (16-byte slots, at most half
full, removal by backward shift: `index.hh:199-275`, `:2969-2978`). Stage 1 (`evaluate`,
`index.hh:2081-2193`) looks a key up in the key map, then in every segment newest first through
the key filter and dictionary (`lookup_key`, `:1952-1964`); an add or replace pushes the document
(`:2181-2189`), points the key map at it (`:2192`), and marks the old copy dead in place: a
buffered copy's flag or a segment's live bit (`remove_copy`, `:2016-2032`). A rejection undoes
each step from a log (`:1968-2013`). `flush()` writes the buffer and clears the maps
(`:2311-2321`).

Every one of the three properties breaks on a mutation made in place:

| Property | What breaks today | Where |
|---|---|---|
| Live readers on other threads | The key map's backward-shift removal and growth move slots under a reader; the vector reallocates; a delete sets a live bit or a dead flag in place, so a reader sees the delete half of a replace before its add | `index.hh:223-239`, `:2016-2032` |
| Frozen views | A view must be the state between two transactions; the dead flags and live bits belong to the current state only | the same |
| Concurrent writers | Stage 1 reads and writes the maps, the undo log, `last_docid`, and the buffer: one thread at a time | `:2081-2193`, `:2967-2980` |

## D1. The candidates, and the recommendation

**(A) RocksDB's memtable: one concurrent skip list on an arena, ordered by a write group.**
Writers join a lock-free queue (`write_thread.cc:226`, `LinkOne`; `:401-445`,
`JoinBatchGroup`); the first becomes the group's leader, sizes the group (`:451-454`), assigns
sequence numbers (`db_impl_write.cc:1381-1387`), writes the WAL, then lets every member insert its
own batch into the memtable at once (`LaunchParallelMemTableWriters`, `write_thread.cc:680-717`);
the last to finish publishes the last sequence (`CompleteParallelMemTableWriter`, `:720-744`;
`SetLastSequence`, `db_impl_write.cc:1137`, `:1554`). The skip list takes a compare-and-swap per
level per insert (`inlineskiplist.h:1027-1045`, `CASNext` at `:393-396`), keeps each node's tower
in front of its key in one allocation (`:858-880`), and is read without locks
(`inlineskiplist.h:20-27`). A key's versions sit side by side, ordered by a sequence number packed
into the internal key (`memtable.cc:1116-1150`); a reader takes the newest at or below its
snapshot. A full memtable is switched for an empty one and queued as immutable
(`db_impl_write.cc:3118`, `:3395`). Concurrency is on by default
(`include/rocksdb/options.h:1449-1451`) except where a write reads state: in-place updates and
merge operands disable it (`db_impl_write.cc:1260-1284`).

What strata needs beyond it: RocksDB's puts are blind. strata's stage 1 reads before it writes
(a conditional version, a replace that keeps the docid, version + 1, `index.hh:2142-2179`), so a
key lookup sits on the sequential path, and a skip list lookup is slow: 1,745 ns per insert with
its lookup and 2,042 ns per get, against 433 and 244 ns for a hash over the arena
(`format-14-kv.md:299-304`, 1,000,000 records, one thread, measured there). So strata's (A) keeps a
hash version map beside the skip list for stage 1 and for point reads: **A'**, a hash for points
and a skip list for order.

**(B) Lucene's DocumentsWriterPerThread, each buffer readable while written.** Each writer
inverts into a private buffer it locks for the call (`DocumentsWriterPerThread.java:52`, `:139`,
`updateDocuments` at `:226-300`); there is no concurrency inside a buffer. Deletes across writers
go through one global queue that hands out sequence numbers, and each buffer applies the deletes
that precede its documents by docid at flush (`finishDocuments`, `:325-350`). Lucene has no
conditional writes: a delete by term is resolved later, never answered at the call. To give
strata's verdicts, version conflicts and kept docids, at the call, (B) needs one place that knows
every key's live copy across all writers: a shared version map, written in one order. With it, (B)
is (C) plus a private key map per buffer; without it, (B) cannot answer `expected_version`. Live
reads on (B) probe every writer's buffer on a miss, and a scan merges every buffer's order.

**(C) Hybrid: per-writer lanes, one shared version map, a sequential leader.** Each writer
appends its records to a private arena, its **lane**, with no atomics, as (B) does. One version
map (a hash, as format 14 measured) maps every buffered key to its newest record across lanes; only
the write group's leader writes it, any thread reads it, as (A)'s skip list is read. The leader
runs stage 1 for its group in order and fills each record's header in the writer's lane; the
records are already copied, so a group completes when its stage 1 does, with no parallel phase and
no barrier on the slowest member. Scans read sorted runs of record references built on demand and
shared between readers (D9).

**The comparison** (estimates, stated per axis; the bake-off measures every one):

| Axis | A' (hash + concurrent skip list) | B (per-writer buffers, readable) | C (lanes + one map) |
|---|---|---|---|
| Single writer, against format 14's arena | each insert also links a skip list node: 1,745 against 433 ns at a million records, measured for the skip list alone, so A' pays roughly the skip list's cost on top of C's (an estimate) | as C, plus a private map per buffer | the arena's insert and the hash's get (433 / 244 ns measured in isolation) plus two uncontended compare-and-swaps per transaction to join and leave the group (estimate) |
| 2, 4, 8 writers | stage 1 sequential in the leader; record copies and skip list links parallel; a group publishes when its slowest member finishes | stage 1 must still be global to give verdicts (above) | stage 1 sequential in the leader; record copies, hashing, and log encoding parallel (D3); a group publishes when stage 1 ends |
| Point read while writing | the hash, newest at or below the reader's position (D5) | one probe per writer's buffer on a miss | the hash, as A' |
| Scan while writing | the skip list, no build | a k-way merge of every buffer's order, each built on demand | shared sorted runs, built on demand, plus a private sort of the newest few hundred records (D9) |
| Memory per record, 100 B values | C's plus a tower, about 10.7 B at branching 4 (estimate) | C's plus a map slot per buffer | about 153 B: a 24-byte header written in place, key, value, and a 9.1 B map slot (estimate, D3); RocksDB 153 B measured (`format-14-kv.md:322-325`) |
| Freeze | O(1) in records | O(writers) | O(1) in records, plus the pending segment deletes (D8) |
| Delete and replace across writers | the leader's order (D6) | a global queue, resolved at flush | the leader's order (D6) |
| Turn-log order | one batch per group (D7) | per writer, needing a global sequence | one batch per group (D7) |
| Search's inverted layout | a concurrent skip list orders nothing a posting list needs; inversion would need lanes anyway | lanes are DWPTs: the natural fit | lanes are private inverters (DWPTs) under the shared map (D11) |

**Decision: (C), with A' as the bake-off's other finalist.** (B) is out on semantics: it cannot
answer strata's conditional writes at the call without (C)'s shared map, and with that map it is
(C) with an extra map per writer. A' and (C) share every piece but one: lanes, the version map, the
write group, positions, freeze, and retirement are common, and they differ only in how a scan
finds key order (D9). So the bake-off is one prototype with two ordered indexes. (C) is
recommended going in because:

1. **The many-index workload weighs single-writer cost.** A host's cores go to different indexes,
   and indexing is measured per index on one thread (`docs/PLAN.md:222-231`). (C)'s single writer
   pays format 14's measured arena plus two uncontended compare-and-swaps; A' pays a skip list link
   on every write, which the format-14 prototype measured at 4.0× the hash's insert.
2. **One mechanism for both layouts.** A lane is the layout's private stage-2 structure: records
   for the document layout, an inverter for search (D11). A concurrent skip list orders key-value
   records and nothing prospect inverts.
3. **No group barrier.** In (C) a group's records are complete when its leader's stage 1 ends; in
   A' the group publishes after its slowest skip list insert.

A' wins if scans beside writers dominate and (C)'s shared runs cannot keep their p99 within the
target (T3, T4). The rule that decides is with the gate rows.

## D2. The checkout model and the write lease

A client **checks out** an index, reads and writes during its request, and **checks it in** at the
end. Checkout is never exclusive and never waits: any number of clients hold an index at once.

- **The write lease** is index-level and has exactly one holder at a time: the leader of the
  current write group (D4). Every duty shared-reader assigns to "the owner thread" (copy-on-write
  counts, view publication and its registry scan, freeing views, merge publication, `retain()`,
  `reclaim()`, close) is the lease holder's, on whatever thread leads. Leadership passes by a
  release store into the next leader's state word and its acquire load
  (`ExitAsBatchGroupLeader`'s handoff, `write_thread.cc:751`), so each holder sees everything
  earlier holders wrote, and plain (non-atomic) fields stay correct.
- **Chores without writes.** A chore owed while no group runs (a finished merge to publish,
  retired views to free) is a writerless group: the caller links an empty writer and, as leader,
  runs the chore, as RocksDB's unbatched writers do (`write_thread.h:379-389`). Check-in links one
  when chores are owed, so an index nobody writes still publishes its merges and frees its views
  at the next check-in.
- **A checkout's lane.** A checkout that writes claims a lane (D3) by one compare-and-swap in the
  index's lane table, released at check-in. With every lane taken, a writer passes its records to
  the leader, which copies them into its own lane: slower, never refused.
- Debug builds assert the lease, not a thread id, wherever shared-reader asserts the owner.

## D3. Lanes and records

A lane is a single-writer arena: chunks of 4 KiB doubling to 1 MiB, charged to the node budget as
they are taken (format-14 D3, `format-14-kv.md:315-318`), plus a chunk table of up to 256
pointers that only grows. An index starts with no lane; a lane exists once a writer used it and is
freed with its buffer, so a single-writer index has one lane and a tiny index's buffer costs one
4 KiB chunk.

**Pre-stage, on the writer's thread, before it joins a group:** it copies each record of its
transaction into its lane (header left for the leader), hashes each key, resolves each key against
the current view's segments and frozen buffers (D4), and encodes the transaction's log payload.
This is the parallel part.

**A record** (key-value layout), 8-byte aligned:

```
[docid u64][version u64][position delta u32][prev ref u32]   written by the leader in stage 1
[flags u8: tombstone, superseded][key length varint][value length varint][key][value]
                                                                written by the writer, pre-stage
```

24 bytes are filled by the leader because their values are known only in order: docids, versions,
and positions. A **record reference** is 32 bits: a lane (3 bits), a chunk (8 bits), and an offset
in 8-byte units (17 bits, 1 MiB chunks). A reader resolves one through the lane table and the
chunk table, both of which only grow and publish an entry before any reference to it.

Memory at 16-byte keys and 100-byte values (estimate): 24 + 3 + 16 + 100 = 143, aligned to 144 B,
plus a 9.1 B map slot (8 bytes at 7/8 full), about 153 B against format 14's 177 B measured and
`main`'s 290 B. The docid map (another 9.1 B) exists only once a docid-addressed operation or a
keyless document reaches the buffer; the key-value harness's keyed puts never create it. A lane's
unused chunk tail is at most one chunk per lane, which T7 reports per open index.

## D4. The sequencer: stage 1 in the group's leader

1. A writer links itself to the index's writer queue by one compare-and-swap (RocksDB's
   `LinkOne`); the writer that finds the queue empty leads.
2. The leader takes the queue (up to a byte cap, small groups capped at their size plus one
   eighth of it, RocksDB's rule at `write_thread.cc:451-454`), reverses it to arrival order, and
   for each transaction in turn assigns its position and runs today's stage 1 against the version
   map: lookups, version checks, docid and version assignment, metadata. For each accepted write it
   fills the record's header in the writer's lane and stores the map slot with release.
3. It stages one log batch for the group (D7), wakes or resumes each follower with its verdict
   (D12), and hands leadership to the oldest writer that linked meanwhile.

**The version map** keeps format 14's 8-byte slots (a 32-bit record reference and 32 bits of the
key's hash, at most 7/8 full, `format-14-kv.md:319-321`; measured -63 B per record and 16% more
run-phase throughput on `main` in `buffer-bytes.md`), with three changes so readers can probe it
while the leader writes:

- Each slot is a `std::atomic<std::uint64_t>`: the leader fills the record, then stores the slot
  with release; a reader loads it with acquire, so a slot it sees names a complete record.
- **No removal during the buffer's life.** A delete appends a tombstone record and points the slot
  at it; a replace points the slot at the new record. So no backward shift moves a slot under a
  reader (today's `erase`, `index.hh:223-239`, leaves the buffer's map).
- **Growth** builds a table twice the size and publishes its pointer with release; the old table is
  retired at the current view generation and freed by the lease holder's registry scan once no
  reader can hold it (D10). A reader that loaded the old pointer finds every key whose position it
  can see, since the leader stops writing the old table only after the new one holds everything.

**A rejected transaction** restores the slots it changed from the undo log, as today, after
setting its records' position to "never", so a reader that loaded such a slot skips the record and
follows `prev`. The records stay in the lane until the buffer is freed (a reader may be reading
one); conflicts are rare, and T3 reports their bytes.

**Pre-resolution.** A key absent from the version map would cost the leader a filter probe and a
dictionary lookup per segment (`lookup_key`, `index.hh:1952-1964`). The writer resolves it before
joining, against the view of generation g it holds: absent, or (segment, ordinal, docid, version).
The leader uses that result only when the key is still absent from the active buffer and the
current generation is still g (no freeze, flush, merge, heal, or install since); otherwise it
resolves again. The result is the same as the leader's own lookup, so replay, which resolves
everything itself, decides identically (T9).

The sequential part per transaction is then one map probe, the header writes, and one slot store
per write. Docid assignment stays sequential and deterministic, which stage-1's ruling requires.

## D5. What readers see, and when

The index publishes one 64-bit **visible position** V: every transaction at or below V is visible
whole, and none above it. The leader stores V with release once its group's stage 1 has ended and
the group's acknowledgement condition holds (D7, Q1).

A reader loads V with acquire, then acquires a view (shared-reader D5) and reads at V. Loading V
first is SR-D15's order: the lease holder publishes any new view before it publishes a position
the view is needed for, so the view a reader acquires covers V.

- **get(key)**: probe the active buffer's map; take the slot's record and follow `prev` while the
  record's position exceeds V or is "never". A record found answers (a tombstone answers "absent");
  none found goes on to the view's frozen buffers, newest first (the same probe, on maps that no
  longer change), then its segments with the view's live bits (shared-reader D8). A key's newest
  record usually has a position at or below V; `prev` is followed only for writes published after
  the reader's load, so chains stay short in practice (T3 reports their length).
- **scan(from, to, n)**: D9.
- A view (shared-reader revision 2, D12) holds the active buffer, the frozen buffers, the
  segments with their frozen bits, and the metadata. A new view object is published only when one
  of those changes (a freeze, a flush's install, a merge, `heal()`, `install()`, metadata or
  schema); a write that changes only buffered records publishes only V. So the registry scan runs
  per freeze or install, never per write group.

This is RocksDB's visibility: a snapshot is a sequence number, and a write is visible once
`SetLastSequence` passes it. It replaces shared-reader D2's "readers see refreshed state" once
this buffer lands; until then shared-reader's step 1 ships as approved.

## D6. Deletes and replaces across writers

**Who wins: the position order.** Two writers that replace one key are ordered by the leader that
puts them in a group, in arrival order, or by the groups' order. The later position wins; a
conditional write is checked against the state after every earlier position, published or not.
That is today's single-writer semantics with arrival as the order, and replay in log order gives
the same verdicts (T9).

- **In the buffer**, nothing is marked dead in place. The new record (or tombstone) points `prev`
  at the old one and takes the slot; a reader at an earlier V still finds the old version through
  `prev`. A record nothing points to and no reader can see is skipped at flush, which writes only
  each key's newest record at the freeze position.
- **In a segment**, the live bit is not set in place either. The newer record in the buffer
  shadows the segment's copy for every key-value read, and the leader appends (segment id,
  ordinal) to the buffer's **pending segment deletes**, an array it alone appends to. The freeze
  applies them to the segments' copy-on-write live bits (D8; shared-reader D3), so a view's bits
  are exact as of its freeze. The writer's own stage 1 stops at the buffer's tombstone or newer
  record, so it never meets the stale segment copy.
- **Docid-addressed operations** (`replace_docid`, `delete_docid`) resolve the docid through the
  docid map (D3), whose slots work like the key map's, then shadow by key as above. A keyless
  document's delete has no key to shadow: its segment bit waits for the freeze, and a live reader
  can still see the document until then. Key-value reads never meet one (they are keyed); search
  reads at a freeze (D11), where the bit is set. Step 1's tests state it.
- **Metadata and schema** are ordered with the documents by position and change the view (D5).

The invariant format 14 relies on, one live copy per key (`format-14-kv.md:283-286`), holds per
view at every freeze; between freezes it holds at each position through shadowing.

## D7. Turn-log order and durability

- **One batch per group.** The leader concatenates the group's pre-encoded transactions into one
  batch record and stages it in the index's turn log, as Track C's Worker stages one record per
  batch (`track-c.md:574-583`). A transaction's position is (log index, place in the batch), the
  seqno ARCHITECTURE defines under groups (`ARCHITECTURE.md:67-73`). Groups follow one another, so
  the index's per-group sequence stays contiguous (`track-c.md:518-519`).
- **Staging from any thread.** The turn log is per core (`track-c.md:462-463`) and its driver
  gathers the batches staged since the last turn. A leader on another thread must stage into the
  index's home core's log: either the driver accepts stages through a lock-free list (the shape
  `LinkOne` has), or the leader posts the batch to the home core's executor. The first adds no
  hop; it is a change to Kronuz/journal, which Detent links, so it goes to durability-modes and
  journal's owner (Detent's agent is told first). Neither blocks the bake-off, which runs without
  a log.
- **Visibility after acknowledgement (Q1).** V passes a group's last position when the group is
  applied and acknowledged at the index's level: at once for `none` and `leader`, at the turn's
  barrier for `memory` and standalone durable writes, at the quorum for `quorum`
  (durability-modes D10). The turn driver's completion stores V; turns complete in order, so V only
  grows. A reader therefore never sees a write a crash could take back. Stage 1 of later groups
  does see earlier, unacknowledged groups: that is the log's order, and a failed barrier restarts
  the process (durability-modes D5).
- **Replicated groups.** Under consensus, apply runs on committed entries in log order, one
  applier per group; clients propose concurrently, and the proposal batching is the write group.
  The applier is the lease holder and uses one lane; what stays parallel is each client's
  pre-stage (encoding and pre-resolution).
- **Determinism.** Lanes change where records sit in memory, never what stage 1 decides: verdicts,
  docids, and versions are a function of the log's order. Flushed key-value segments are
  key-ordered (format-14 D1a) and so are the same bytes whatever the lanes. Search's per-lane
  flushes are not (D11).

## D8. Freeze

**A freeze is a write-group operation** run by the leader at a group boundary, at position F:

1. Seal the active buffer: it becomes a **frozen buffer**, whose maps, lanes, and records nobody
   writes again. A new active buffer is created at the next write.
2. Apply the pending segment deletes to the segments' live bits through shared-reader D3's
   copy-on-write accessor.
3. Publish a view whose frozen buffers include the sealed one, then V (D5).

The cost is O(1) in buffered records, plus one bit per pending segment delete and one
copy-on-write clone per segment that took one, plus the view's allocation (O(segments)). No sort,
no copy, no disk. T6 measures it.

**A frozen buffer is an in-memory segment** to every reader: probed through its map for a get,
ordered through its runs for a scan (built once, by the first scan or by the flush), and dropped
from the next view when its flush installs. The flush, on a pool thread, sorts the frozen
buffer's newest records by key and writes a segment as today's flush does; installing it is a
lease operation, like a merge's install. Freezes happen:

- when the buffer reaches its budget share or the node budget asks (today's `needs_flush`,
  `index.hh:1247`): a freeze, then a flush;
- when a search must see writes past the last freeze (D11);
- when a frozen-buffer count cap is reached (default 4, an estimate T6 tunes): the oldest flush
  first.

Key-value reads never need a freeze; they read the live buffer at V.

## D9. Key order for scans: the bake-off's question

- **(C) shared sorted runs.** Format 14's two-run view (a main run and a delta of at most 4,096
  records, `format-14-kv.md:306-313`) becomes an immutable pair of arrays of record references in
  key order, each covering every record up to a position, published by compare-and-swap on the
  buffer's run pointer. A scan at V merges the runs, a private sort of the records past the delta's
  cover and at or below V, the frozen buffers' runs, and the segments; among one key's records it
  takes the newest at or below V. A scanner whose private tail passes 256 records (an estimate T4
  tunes) builds a new delta first; a delta past 4,096 merges into a new main run. Two scanners
  building at once both finish; one installs, the other frees its copy. Writers never build runs;
  readers who scan pay, as format 14's design pays on demand. Replaced pairs retire like tables
  (D10).
- **(A') a concurrent skip list.** Each record carries its tower in front of its header, allocated
  in the lane by the writer (RocksDB's layout, `inlineskiplist.h:858-880`, height chosen by the
  writer), and the writer links it after its verdict with a compare-and-swap per level
  (`inlineskiplist.h:1027-1045`); V waits for the group's last link, as RocksDB's
  `CompleteParallelMemTableWriter` makes it wait. Scans walk the list at V; flushes walk it with no
  sort.

Everything else in the buffer is the same in both.

## D10. Retirement: what readers hold, and who frees it

A reader holds a view (shared-reader D5's hazard record); the view holds its buffers (active and
frozen) and segments by `shared_ptr` copies the lease holder makes and drops. Within a buffer,
three things are replaced while readers may hold the old copy: a grown map table, a replaced run
pair (C), and the buffer itself once flushed. Each is retired into the index's retire list
stamped with the current view generation g, and the lease holder frees it during its registry scan
once no record names a view of generation g or older. Why that suffices: a reader can load the old
pointer only while it holds a view published no later than the replacement, whose generation is
at most g; a reader holding a later view loaded the pointer after the release store that replaced
it. The list is bounded by the buffer's growth (at most one table per doubling and one run pair per
delta merge) and freed on the lease holder's thread, as ARCHITECTURE's memory rule asks
(`:188-192`): no collector, no thread, bounded work. With no reader holding anything, the scan
frees it all at the next chore.

## D11. Search's inverted layout

- **Lanes are private inverters.** A search lane holds its writer's inverter (today's
  `active_buffer`, `index.hh:1296-1314`) and stored records. Stage 1 (keys, docids, versions)
  stays the shared leader's, so a replace by Xapiand's `_id` term is decided once, in order. The
  writer inverts its accepted documents on its own thread after the verdict: inversion runs in
  parallel, as Lucene's DWPTs invert (`DocumentsWriterPerThread.java:226-300`), with strata's
  verdicts kept.
- **Search reads at a freeze.** Postings in a growing inverter are not readable by another thread
  without a concurrent term hash and posting slices, which neither candidate gives. A search reads
  a view's frozen lanes, and D9's `publish_through(p)` (shared-reader) becomes a coalesced freeze
  through p: the first search that needs p freezes, and the searches queued behind it find the
  view past p. A freeze of a search buffer waits for each lane to finish inverting what it holds at
  or below F (at most one transaction per lane), suspending, never blocking (D12).
- **Frozen lanes must be readable.** Either prospect reads an inverter as an in-memory segment, or
  the freeze encodes each lane into segment bytes held in memory (`open_segment::owned`,
  `index.hh:1261`), written to a file later. The second needs no second reader path and is
  shared-reader Q1's option (c); prospect measures both at T7's NRT row.
- **One segment per lane at flush.** Per-lane flushes split documents by writer, not by docid range
  as D18's parallel flush does (`index.hh:2389-2391`), so a document-mode replica that replays the
  log in one lane writes different segments with the same digest. durability-modes D18's sampled
  rebuild then needs each flush's lane partition in the log or the flush concatenated in docid
  order; that is prospect's step and that design's call (Q2).

Live search between freezes (Q2) would need a reader-safe inverter: a research item, not this
design.

## D12. The async seam

- A write is a task (`xapiand-on-strata.md:224-236`): pre-stage on the caller's thread, link, then
  either lead or suspend. A leader's work is bounded CPU, stage 1 for at most the group's byte cap
  and no I/O (the barrier completes asynchronously and delivers the verdicts), so a leader on a
  reactor thread stays within ARCHITECTURE's slice (`:163-185`). Followers in a task suspend and
  are resumed on their own executors; plain threads (the benchmarks) spin briefly, then wait on the
  state word (`std::atomic::wait`), as RocksDB's `AwaitState` spins, yields, then blocks
  (`write_thread.cc:64-80`).
- Checkout never blocks (D2), which retires the stage-1 review's deadlock of a blocking pool
  checkout on a reactor thread (`reviews/stage-1.md:943`).
- A view guard is held across suspension as shared-reader D9 says.

## D13. Costs, stated as estimates until the gate rows measure them

| What | (C) | (A') |
|---|---|---|
| Per transaction, one writer | format 14's arena insert, one map slot store per write, two uncontended compare-and-swaps (join, leave), one release store of V | as (C), plus a tower and its links: up to a compare-and-swap per level |
| Sequential per transaction, many writers | one map probe, the header writes, one slot store per write | the same |
| Per record | about 153 B at 100 B values (D3) | about 164 B (the tower) |
| Point read | map probe with acquire loads, `prev` rarely | the same |
| Scan | shared runs, built on demand; a private sort of at most the tail threshold | a skip list walk |
| Freeze | O(1) in records, plus pending segment deletes | the same |
| Per open index | nothing until a write: then one lane chunk (4 KiB) and a 16-slot map | the same |

## D14. The fallback: one writer, live readers

The fallback is the same implementation with the concurrency between writers taken out, not a
second buffer: one lane, direct arena offsets instead of lane references, an exclusive write lease
taken per transaction instead of the write group's queue, and no pre-resolution. Everything readers
see is unchanged: the version map readable while written, V, `prev`, tombstones, freeze, views. So
readers still see live writes on any thread with no refresh cadence; what changes is that two
requests writing one index take turns on the lease. The lease is awaited as a task, never by
blocking a reactor thread (D12).

What the multi-writer mode costs over it, per transaction: the queue's two compare-and-swaps and
the leader handoff, the lane table's translation on every reference a reader follows, pre-resolution's
generation check, and the lanes' extra chunk tails. What it buys: a request never waits for another
request's write, and record copying, hashing, encoding, and resolution run on each writer's thread.
Which matters more on Xapiand's load is T11's question; the step-0 prototype builds both, and the
loser is deleted (one implementation).

## Gate rows

Performance rows are measured by the lead on `main` or, for the bake-off, on the prototype's
branch as evidence for choosing (never as a gate result), under a strict hold, in minutes-long
runs. Every row reports the honest magnitude, losses included.

| Row | Target | Procedure |
|---|---|---|
| **T1** Write throughput, 1, 2, 4, 8 writers on one index | At 1 writer: (C), A', and D14's fallback no slower than `main`'s buffer (≤ 1.0× its time), and (C) within 3% of the fallback. At 1, 2, 4, and 8 writers: throughput ≥ 1.0× RocksDB's on one shared DB with `allow_concurrent_memtable_write` (its default) | `bench/kv/proto` `lanebuf` (step 0): `fillrandom` and `overwrite`, 100 B and 1 KiB values, writers sharing one buffer, no log; `main`'s buffer timed by `kv_proto_bufmem`'s loop (the real `basic_index::apply`, nothing flushed, `bufmem.cc:53-75`) for the 1-writer baseline; RocksDB through `bench/kv` with T threads on one DB, WAL off. Reports ops/s, CPU per op, the leader's share of time, group sizes. The fallback at W writers is reported too (its writers take turns) |
| **T2** Point reads while writing | Reader p99 ≤ 1.0× RocksDB's at 1, 4, 8 readers beside 1, 2, 4, 8 writers | The prototype's `readwhilewriting`: readers `get` uniformly over loaded keys while writers run at full speed and at 2 MB/s (db_bench's `MB_WRITE_PER_SEC`); reports p50, p99, p99.9, and `prev` chain lengths |
| **T3** Scans while writing (decides A' against C) | Seek and 10 nexts p99 ≤ 1.0× RocksDB's beside 1, 2, 4, 8 writers | The prototype's `fwdrangewhilewriting`; (C) reports run builds per second and their cost, the private tail sizes, and rejected-record bytes |
| **T4** Single-writer shape | Every column of format 14's buffer table (`format-14-kv.md:299-304`: resident bytes, insert, get, seek and 10, YCSB E, flush sort) within 5% of the arena variant for (C); A' reported against it | `lanebuf --writers 1` beside `kv_proto_buffer` (format 14's prototype), 1,000,000 records of 100 B and 200,000 of 1 KiB, three interleaved rounds, light hold |
| **T5** The 0021 rows | On `main` after step 2 lands: every fits1 and twox1 row of `core-vs-rocksdb-0021.md` within its trial spread or better; none lost | `python3 bench/kv/run.py rows OUT fits1 5`, then `twox1` |
| **T6** Freeze cost and buffer memory | Freeze at 1,000,000 buffered records within 2× of the freeze at 1,000 (O(1) in records), plus a linear term in pending segment deletes; resident growth ≤ 153 B per record at 100 B values (format-14 T6) at 1 and 8 writers | `lanebuf --freeze-sweep` at 10^3 to 10^6 records with 0% and 10% replaces of flushed keys; `kv_proto_bufmem`'s method for bytes, three trials |
| **T7** Many indexes | Per open index with one write, resident bytes at most format 14's arena buffer plus 1 KiB; `many` row unchanged | `python3 bench/kv/run.py rows OUT many 3`; the counting allocator over 1,000 indexes |
| **T8** Concurrency stress under sanitizers | Zero TSan, ASan, UBSan reports; every read equals a model at its V; no transaction seen in part; V never decreases per reader | 1 to 8 writers and 1 to 16 readers on one index; writers apply puts, replaces, conditional writes (some rejected), deletes by key and docid, metadata; freezes, flushes, merges, grown maps, and run builds at random; readers get, scan, and hold views across pauses, releasing some on other threads. The model is the sequential replay of the logged order (T9). At least 10^7 reader operations and 10^5 transactions per platform (Linux x86_64, macOS arm64, FreeBSD) |
| **T9** Determinism | The verdicts, docids, versions, digest, and flushed key-value segment bytes of a concurrent run equal those of a single-threaded replay of its log order | Log the group order in T8; replay on one thread, one lane, no pre-resolution; compare. Mutants, each killed by a named test: pre-resolution used across a changed generation (T9); a slot stored before its header (T10); the undo without "never" (T8); a map entry erased in place (T8); V published before stage 1 ends (T8); a retired table freed at the wrong generation (T8 under ASan) |
| **T10** Ordering litmus | Zero torn records in at least 10^7 iterations natively on x86_64 and macOS arm64; a mutant with the slot stored relaxed fails it | The leader fills a header and stores the slot; a reader probes and checks the header against the slot, as shared-reader T8 does for the hazard handshake |
| **T11** Xapiand-shaped load: multi-writer against the fallback (decides D14) | Both ≥ 1.0× RocksDB's request throughput and ≤ 1.0× its request p99 on the same load; the multi-writer mode is kept only if it clears Q3's threshold against the fallback | `lanebuf --requests` (step 0): 1,000 small indexes (1,000 to 10,000 records each, 1 KiB values, Xapiand's stored-record size class), request threads R = 1, 4, 8, 16 each looping: pick an index (uniform, and Zipfian s = 0.99 so hot indexes see concurrent requests), check out, run a request of 4 operations mixed as 1 write and 3 gets (Xapiand's index-then-read) and as 3 writes and 1 get (bulk updates), check in. Multi-writer (C) against D14's fallback, same binary. RocksDB: one DB with 1,000 column families, R threads, the same request stream, WAL off. Reports requests/s, request p50, p99, p99.9, time spent waiting for the lease (fallback) or as a follower (multi-writer), CPU per request, and resident bytes per open index. The durable variant (turn log, one barrier per turn) follows step 3 |
| **T12** Inverted layout (step 4, prospect) | Indexing throughput at 1, 2, 4, 8 writers ≥ 1.0× the faster of Lucene and Tantivy at the same thread counts; NRT freeze-to-visible p99 ≤ 1.0× Lucene's reopen p99 | prospect's indexing bench at those writer counts (`benchmark-coverage.md`, Suite 2, "Indexing GB/hour"); shared-reader's T7 NRT shape with `publish_through` as a freeze (D11) |

**The bake-off's decision rules.** Two decisions, in order.

1. **A' against (C).** A finalist that misses T1, T2, T4, or T7 is out. If both pass, (C) is
   chosen unless A' beats (C) on T3's p99 by more than 20% at 4 writers while (C) misses T3's
   target; A' is then chosen only if its single-writer insert (T4) stays within 10% of (C)'s, since
   the many-index rule weighs one writer per index first.
2. **Multi-writer against the fallback (D14)**, on the winner of 1: T11 against Q3's threshold. If
   the multi-writer mode misses it, the fallback ships, and Xapiand serializes writes per index on
   the lease while its reads stay live.

The lead records both outcomes in `docs/perf/write-buffer-bakeoff.md`; the losing code is deleted
(one implementation).

## Questions for the owner

**Q1. When do live readers see a write: once applied, or once acknowledged?**

1. Once acknowledged at the index's level (recommended): V passes a write at its turn's barrier
   for durable writes, at once for `none` and `leader`. No reader sees a write a crash could take
   back; a durable write is visible after its barrier, as a synced RocksDB write is visible after
   its WAL sync. The cost is visibility one barrier later.
2. Once applied: visible before the barrier. Readers may act on writes that a power loss then
   removes, which no preset in durability-modes allows a client to observe.
3. Per index, defaulting to 1.

**Q2. Must search read live writes between freezes?**

1. No (recommended): search reads at a freeze; `publish_through` makes one coalesced freeze serve
   every search waiting on a position; frozen lanes encode to in-memory segment bytes first. This
   gives Xapiand D7's guarantee at a freeze's cost instead of a refresh's.
2. Yes: prospect needs an inverter readable while written (a concurrent term hash, posting slices
   published by position). A research item, unmeasured, and a second reader path in prospect.

**Q3. The threshold for keeping several writers (T11, D14).**

1. Keep the multi-writer mode when, on T11's Zipfian load at R = 8, it gives at least 1.2× the
   fallback's requests per second **or** at most 0.7× its request p99, while costing at most 3% at
   one writer on T1 and at most 5% CPU per request on T11's uniform load (recommended). Hot indexes
   are where requests collide; a uniform load measures what everyone else pays.
2. Keep it whenever it costs at most 3% at one writer, whatever T11 shows: Xapiand gets the dream
   mode unless it is measurably expensive.
3. Ship the fallback unless the multi-writer mode at least halves T11's request p99: only a large
   win justifies the leader, lanes, and pre-resolution.

## First buildable step

**Step 0, the bake-off prototype (may start now; it is benchmark code outside the engine and
lands in `bench/kv/proto` only).** `bench/kv/proto/lanebuf.hh` and `lanebuf.cc`, a CMake target
outside `all` under `STRATA_KV_PROTO`, as format 14's prototypes are (`format-14-kv.md:63-71`):

1. The common pieces: the write group (link, lead, group cap, handoff, spin then
   `atomic::wait`), lanes with the 32-bit reference scheme, the single-writer, multi-reader version
   map (atomic slots, no erase, growth with generation-stamped retirement), records with
   leader-filled headers, V, tombstones and `prev`, freeze, and a minimal view (generation, frozen
   buffers) with a hazard record per reader.
2. The two ordered indexes: (C)'s shared run pair with the tail threshold, and A's tower in the
   record with concurrent links and V after the last link.
3. D14's fallback from the same source: one lane, direct offsets, an exclusive lease per
   transaction, no pre-resolution (a compile-time switch in the prototype only; the engine keeps
   whichever wins).
4. A driver with W writers and R readers on one buffer: T1 to T4 and T6's workloads, using
   `kvgen.hh`'s keys and values; T11's request loop over many buffers; T8's model check and T10's
   litmus at small counts, under TSan.
5. `run.py` entries so the lead runs T1 to T4, T6, and T11 interleaved under a hold.

The developer dry-runs it on tiny inputs (non-empty results, the model check passing) and makes
functional claims only; the lead measures.

**Step 1 (after the architect's review), the core's buffer, single-writer first**, on
`phase4/write-buffer`:

1. `hash_slots` to 8-byte slots at 7/8 (`buffer-bytes.md`'s experiment, finished: tests, the
   docid-map comparison, sanitizers).
2. Lanes and records replacing `buffered` and `borrowable_vector` (`index.hh:2971`), the flush
   reading frozen buffers; the version map without in-place removal; tombstones, `prev`, pending
   segment deletes, freeze.
3. The write group and the write lease, with one writer: every owner-thread duty moved under the
   lease (D2); checkout and check-in.
4. Positions and V; live `get` and the winner's scan from other threads; shared-reader's views
   gaining the buffer (its D12).
5. Tests: T8 and T9 at one writer, every mutant of T9; the core's existing suites unchanged.

**Step 2**, several writers: pre-stage, pre-resolution, lane claims, group sizing; T8 and T9 at 8
writers. **Step 3**, the turn log: one batch per group, V at acknowledgement (Q1), staging from
any thread (with journal's change). **Step 4**, prospect: search lanes (D11).

**Done criteria for step 0** (for the lead to set; contributors tick, never edit):

- K1 (C), A', and the fallback built from one source: the ordered index and the writer
  concurrency are the only differences.
- K2 T8's model check and T10's litmus pass under TSan at small counts; the T9 and T10 mutants are
  each killed.
- K3 Dry runs on tiny inputs give non-empty results for every workload and engine.
- K4 Nothing outside `bench/kv/proto` changes.

## Risks

- **The leader is the ceiling.** Stage 1 is sequential, so write throughput on one index saturates
  at the leader's rate however many writers join. Pre-resolution and pre-staging move everything
  else off it; T1 reports the leader's share of time, and ARCHITECTURE item 12's partitioned
  writers remain the answer past it.
- **Shared runs under heavy scan-and-write.** Readers pay the run builds; if T3 shows builds
  dominating, A' wins, by the stated rule.
- **Hot keys overwritten many times** grow `prev` chains and buffer bytes (every version is a
  record), as in RocksDB's memtable; flush writes only the newest. T2 reports chain lengths.
- **Search's per-lane segments** change byte identity across document-mode replicas (D11, Q2's
  neighbor); segment mode installs the leader's bytes and is unaffected.
- **The journal change** (staging from any thread) touches a library Detent links; coordinate
  before step 3.
