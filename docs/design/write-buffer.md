# The write buffer: one buffer for frozen views, live readers, and concurrent writers

Revision 4, 2026-10-26. State: designing (revision 4 ready for review: it answers the re-check of
revision 3, WB-20 to WB-24 and C8 to C11, "Changes in revision 4", at the end; C8's rule is the
code reviewer's to confirm at file:line, and step 1's item 4 may proceed under C9). Revisions 3 and
4 cite step 1's code (`phase4/write-buffer` at `f21a10d`, its ledger's "For the designer and the
architect") and the architect's model of revision 3 (`.scratch/rev3/docid_model.py` in this
worktree, cited as "the model").
Concurrency (lock-free structures, lifetimes across threads, leadership handed between threads),
so the architect reviews this design before anything but the bake-off prototype is built (PLAN's
review tiers, the "Durability, concurrency, security surface" row). The preliminary review
(`docs/reviews/write-buffer-design.md`) rejected revision 0 on WB-1 to WB-6; revision 1 answers
WB-1 to WB-15 and C1 to C5 ("Changes in revision 1"); the re-check (`baf669d`) approved it with
conditions and raised WB-16 to WB-19, which revision 2 answers with the scan structure the owner
accepted on 2026-10-26 ("Changes in revision 2", at the end). Decisions D1 to D14, gate rows
T1 to T12, owner questions Q1 to Q3 (answered) and Q4 (new, from WB-5). Code is cited at strata
`main` `41fde48`, `design/shared-reader` revision 2 at `b1568a7`, `design/durability-modes` at
`95357d9`, `explore/buffer-bytes` at `0a8c85a`, and the step-0 prototype on `perf/lanebuf-rebased`
at `5a41d53` (`bench/kv/proto/lanebuf*.hh`; its report `docs/perf/write-buffer-bakeoff.md`, cited
below as "bake-off"). RocksDB 11.11 (`~/.scratch/rocksdb-11.11.fb`)
and Lucene 10.4.0 (both Apache 2.0) were read for their mechanisms; no code of theirs enters
strata. Nothing was compiled or run for this design: every number below is quoted from a cited
report, probe, or the lead's raw bake-off lines, or marked as an estimate.

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
   today's buffer** on `main` (T5).
2. **Xapiand and Elysian use the multi-writer mode**, so a request never juggles refreshes and
   never waits on another request's writer.
3. **The fallback.** If several writers cost much more, or the bake-off shows Xapiand's load does not
   need them, Xapiand ships with one writer per index whose readers see live writes (D14). The
   bake-off measures exactly that choice, on the path Xapiand's indexes run (D7, WB-5) and on the
   key-value rows, against the owner's threshold (Q3).

**Where multi-writer parallelism lives (WB-5, stated first because it scopes everything below).**
Every journaled index is a consensus group, standalone included (`durability-modes.md:780-781`), and
every preset keeps a synced journal (`:951`); Xapiand's default preset is `replicated` at `memory`
(`:2060`). On a journaled index the log orders writes and **one applier** runs stage 1 after the
log, at `commit_memory` (durability-modes D7, `:1685-1720`). So for Elysian's indexes:

- "A request never waits on another request's writer" holds with no write group at all: requests
  propose concurrently and each awaits its own entry's verdict.
- What runs in parallel is everything before the log (building and analyzing documents,
  encoding payloads, pre-resolving keys against a view) and, on the leader replica, the lanes:
  records pre-staged by each proposer so the applier fills headers instead of copying, and for
  search, inversion after the verdict on each proposer's thread (D11). Stage 1 itself is serial in
  the applier, as it is in the write group's leader.
- The write group (D4) is the sequencer only for indexes without a strata journal: 7a, where
  Xapiand keeps its own WAL (`xapiand-on-strata.md:6`), embedders that need no durability, and the
  benchmarks against RocksDB with its WAL off.

D7 tabulates the paths; T11a, on the applier path, is the row that decides D14 for Xapiand; Q4 asks
the owner to confirm that scope.

## What this builds on, and what it changes

| Document | What this design uses | What it changes |
|---|---|---|
| `docs/ARCHITECTURE.md` | Write path, two stages, stage 1 sequential and deterministic (`:75-119`); seqno under groups is (log index, position in batch) (`:67-73`); memory ownership and bounded deferred frees (`:188-192`); the host reactor never blocks (`:6`) | Item 1, "one writer per shard, owned by one core" (`:88-89`): a write lease passed between threads (D2); item 4's "in-memory segment" becomes lanes and frozen buffers (D3, D8); item 9, refresh as the visibility step (`:108-109`): visibility is the read position (D5). The lead edits items 1, 4, and 9 on landing (C5) |
| `docs/reviews/stage-1.md` (c), ruling (`:317-337`) | A total order at admission; a sequential, deterministic stage 1 | Several writers whose stage 1 stays sequential in one leader or applier; the architect accepted the change (WB-13). Lucene's global delete queue stays rejected |
| `docs/design/format-14-kv.md` D3 (`:266-328`), T6 (`:384`) | The arena record, 8-byte map slots at most 7/8 full, the two-run sorted view, 494 B per buffered record today and 176.7 B measured for the arena layout (`format-14-kv/report.md:84`) | The arena becomes per-writer lanes (D3); the version map becomes single-writer, multi-reader (D4); overwrites stop marking the old record dead (D6); the two-run view becomes a binary counter of prefixed runs, built on the pool when a scan asks and installed under the lease (D9). T6's target stands; D3 restates the estimate (WB-7) |
| `docs/perf/buffer-bytes.md` (branch `explore/buffer-bytes`) | 290 B per record on `main`; 8-byte slots measured at -63 B per record | Step 1 finishes its slot change first |
| `design/shared-reader` revision 2 (`b1568a7`) | Views, hazard records, SR-D15's store order, copy-on-write live bits (D3), retention of held segments (D7), non-blocking close (D11), `publish_through` (D9), the write lease (D4), D12 | D5 adds the read position `max(V, view.position)` (WB-1); D6 keeps copy-on-write bits on segments and shadows frozen key-value buffers instead (WB-16); D10 bumps the view generation when a map table or run set is replaced |
| `design/durability-modes` D1, D7, D10 (`durability-modes.md:780-781`, `:951-953`, `:1685-1720`, `:2031-2080`) | Every journaled index is a group; apply at `commit_memory` in one applier; acknowledgement levels with a default and a floor per preset; checkpoints of a past generation at or below `commit_durable` | D7's path table; the four visibility rules of Q1 (D7, WB-6); a checkpoint covers flushed state only (D8) |
| Track C D11 (`track-c.md:460-626`); Kronuz/journal (`turn_driver.h:57-79`, `turn_log.h:288-294`) | A turn log per core with a single-threaded driver; per-group sequences checked contiguous | A per-core inbox on strata's side drains staged batches into `stage()` (D7, WB-11); journal unchanged |
| `docs/design/xapiand-on-strata.md` D7, D13 (`:192-196`, `:224-236`) | Reads see every acknowledged write; the seam is `task::task<T>`; Xapiand's pool checkout (`:125`) | Checkout never waits (D2, D12); D7's refresh before a search becomes nothing for key-value reads and a coalesced freeze for search (D11) |
| `docs/perf/benchmark-coverage.md` | The KV harness, the threaded rows (`:93-94`), the thread asymmetry (`:48-50`) | The bake-off rows run one shared index across threads |
| `docs/perf/core-vs-rocksdb-0021.md` | The single-thread rows strata wins (`:21-23`) | T5 keeps every one |

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
`JoinBatchGroup`); the first becomes the group's leader, sizes the group (`:449-454`), assigns
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
merge operands disable it (`db_impl_write.cc:1258-1284`).

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
the lease holder (the write group's leader, or the applier on a journaled index, D7) writes it,
any thread reads it, as (A)'s skip list is read. The sequencer runs stage 1 in order and fills
each record's header in the writer's lane; the records are already copied, so a group completes
when its stage 1 does, with no parallel phase and no barrier on the slowest member. Scans read
sorted runs of record references built off the read path (D9).

**Per-key locks, weighed (WB-15).** A live version map with a lock per key lets stage 1 run in
parallel for distinct keys, which removes the sequencer as a ceiling. It makes the order of two
writes the order their locks were taken, so docid assignment, `last_docid`, and the verdicts would
follow lock order, and replay would need that order recorded and reproduced exactly. strata's
docids are exposed (`_docid`) and must be the log's deterministic order (stage-1 ruling (c)), and on
a journaled index the log orders writes before any lock is taken. (C) keeps one order, the log's or
the queue's, and pays a serial stage 1 for it; the Risks state the ceiling.

**The comparison** (estimates, stated per axis; the bake-off measures every one):

| Axis | A' (hash + concurrent skip list) | B (per-writer buffers, readable) | C (lanes + one map) |
|---|---|---|---|
| Single writer, against format 14's arena | each insert also links a skip list node: 1,745 against 433 ns at a million records, measured for the skip list alone, so A' pays roughly the skip list's cost on top of C's (an estimate) | as C, plus a private map per buffer | the arena's insert and the hash's get (433 / 244 ns measured in isolation) plus two uncontended compare-and-swaps per transaction to join and leave the group (estimate) |
| 2, 4, 8 writers | stage 1 serial; record copies and skip list links parallel; a group publishes when its slowest member finishes | stage 1 must still be global to give verdicts (above) | stage 1 serial; record copies, hashing, encoding, and pre-resolution parallel (D3, D4) |
| Point read while writing | the hash, newest at or below the read position (D5) | one probe per writer's buffer on a miss | the hash, as A' |
| Scan while writing | the skip list, no build | a k-way merge of every buffer's order, each built on demand | runs built off the read path plus a private sort of the tail (D9) |
| Memory per record, 100 B values | C's plus a tower, about 10.7 B at branching 4 (estimate) | C's plus a map slot per buffer | about 165 B estimated, 177-181 B by format 14's measured layout (D3, WB-7); RocksDB 153 B measured |
| Freeze | O(1) in records | O(writers) | O(1) in records, plus the pending deletes (D8) |
| Delete and replace across writers | the sequencer's order (D6) | a global queue, resolved at flush | the sequencer's order (D6) |
| Order with a journal | the log's (D7) | per writer, needing a global sequence | the log's (D7) |
| Search's inverted layout | a concurrent skip list orders nothing a posting list needs; inversion would need lanes anyway | lanes are DWPTs: the natural fit | lanes are private inverters (DWPTs) under the shared map (D11) |

**Decision: (C), with A' as the bake-off's other finalist.** (B) is out on semantics: it cannot
answer strata's conditional writes at the call without (C)'s shared map, and with that map it is
(C) with an extra map per writer. A' and (C) share every piece but one: lanes, the version map, the
sequencer, positions, freeze, and retirement are common, and they differ only in how a scan finds
key order (D9). So the bake-off is one prototype with two ordered indexes. (C) is recommended going
in because:

1. **The many-index workload weighs single-writer cost.** A host's cores go to different indexes,
   and indexing is measured per index on one thread (`docs/PLAN.md:222-231`). (C)'s single writer
   pays format 14's measured arena plus two uncontended compare-and-swaps; A' pays a skip list link
   on every write, which the format-14 prototype measured at 4.0× the hash's insert.
2. **One mechanism for both layouts.** A lane is the layout's private stage-2 structure: records
   for the document layout, an inverter for search (D11). A concurrent skip list orders key-value
   records and nothing prospect inverts.
3. **No group barrier.** In (C) a group's records are complete when its stage 1 ends; in A' the
   group publishes after its slowest skip list insert.

A' wins if scans beside writers dominate and (C)'s runs cannot keep their tail latency within the
target (T3). The rule that decides is with the gate rows.

## D2. The checkout model and the write lease

A client **checks out** an index, reads and writes during its request, and **checks it in** at the
end. Checkout is never exclusive and never waits: any number of clients hold an index at once.

- **The write lease** is index-level and has exactly one holder at a time: the leader of the
  current write group on a journal-less index (D4), the applier on a journaled one (D7). Every duty
  shared-reader assigns to the owner (copy-on-write counts, view publication and its registry
  scan, freeing views, merge publication, `retain()`, `reclaim()`, close) and every store of the
  visible position (D7) is the lease holder's, on whatever thread holds it. Leadership passes by a
  release store into the next leader's state word and its acquire load
  (`ExitAsBatchGroupLeader`'s handoff, `write_thread.cc:751`), so each holder sees everything
  earlier holders wrote, and plain (non-atomic) fields stay correct.
- **Chores without writes.** A chore owed while no group runs (a finished merge to publish,
  retired views to free, an acknowledgement to publish, D7) is a writerless group: the caller links
  an empty writer and, as leader, runs the chore, as RocksDB's unbatched writers do
  (`write_thread.h:379-389`). Check-in links one when chores are owed, so an index nobody writes
  still publishes its merges and frees its views at the next check-in.
- **Lanes are claimed per pre-stage (WB-10)**, not per checkout: a writer claims a lane of the
  active buffer when it starts pre-staging a transaction and releases it once that transaction's
  verdict is delivered (D3). A request that writes once and then reads or suspends holds no lane.
- Debug builds assert the lease, not a thread id, wherever shared-reader asserts the owner.

## D3. Lanes and records

**A lane belongs to one buffer (WB-3).** It is a single-writer arena: chunks of 4 KiB doubling to
1 MiB, charged to the node budget as they are taken (format-14 D3, `format-14-kv.md:315-318`). A
buffer starts with no lane. **Lanes sit on a push-only list (WB-19):** a pre-stage claims a free
lane by a compare-and-swap on its busy flag, or pushes a new lane with a compare-and-swap on the
list head; nothing is ever removed or replaced, readers never touch the list, and the lanes are
freed with the buffer (`lanebuf.hh:93-106`). So a writer adds a lane without the lease, a
single-writer buffer has one lane, and a tiny index's buffer costs one 4 KiB chunk. A lane is
exclusive to one pre-stage from its claim to its verdict: nobody appends to it while the sequencer
writes headers into it.

**Pre-stage, on the writer's thread, before it joins a group or proposes:** it acquires a view
guard, claims a lane of that view's active buffer, copies each record of its transaction into the
lane (header left for the sequencer), hashes each key, pre-resolves each key against the view's
frozen buffers and segments (D4), and encodes the transaction's log payload. This is the parallel
part. The guard keeps the buffer alive until the writer links (D8).

**A record** (key-value layout), 8-byte aligned:

```
offset  0  [docid u64][version u64][position delta u32][prev ref u32]   the sequencer, stage 1
offset 24  [flags u8: tombstone, docid prev][3 B padding]               the record's producer
offset 28  [docid prev ref u32], only when its flag is set               reserved by the producer,
                                                                         filled in stage 1
then       [key length varint][value length varint][key][value]          the record's producer
```

**The flags precede what they announce (WB-21).** The flags byte sits at a fixed offset, 24, so a
reader finds it without knowing the record's shape; the docid prev field, when the flag is set, sits
at 28, and the lengths follow it (at 28 without the field, at 32 with it).

**Two predecessors.** `prev` names the record the key's slot held before (a keyless
record's: the docid's). A `replace_docid` that changes the key, or that writes a docid deleted
earlier in the buffer, also has a docid predecessor that is not its key predecessor, and `prev`
cannot name both. So a `replace_docid` record carries a **docid prev** field. **Whoever produces a
record reserves the field and sets its flag from the operation's type (WB-21):** the writer at
pre-stage; the sequencer when it copies a sealed lane's records into its own lane (D8, WB-3) and
for every record under D14's fallback; the applier when it copies records from a log entry where no
pre-staged lane exists (D7: followers, and the leader for proposals from other replicas); and
replay. The sequencer fills it in stage 1 with the record the docid's slot held before when that
differs from the key's, and with no reference otherwise. No other operation needs it: an add or a
replace by key writes its key's live docid or a fresh one, and a tombstone writes its copy's own
docid under its copy's key, so their docid predecessor is their key predecessor or none. Every
`replace_docid` builds the docid map before its stage 1 (step 1's `writable_buffer`,
`index.hh:2867-2876` at `f21a10d`), so the records a buffer holds before its map exists (D3) need
no field. The cost falls on `replace_docid` records only: the field's 4 B and 3 B of padding, so at
16-byte keys and 100-byte values the record grows from 144 to 152 B after alignment (an estimate).
D5 states the walk that reads it.

**No field is written after its record is published (WB-9).** The sequencer writes the header,
then stores the map slot with release; nothing writes the record again. Revision 0's "never"
position and "superseded" flag are gone: a rejected record's slot is restored (D4), which hides it
from the map (the review's `vm_*_no_never` passes), and rejected records never enter the position
index (D9), so nothing enumerates them.

**A record reference** is 32 bits: a chunk index (15 bits) into the buffer's chunk directory and
an offset in 8-byte units (17 bits, 1 MiB chunks). Lanes are not in the reference (WB-10): a lane
taking a new chunk claims the next index by one `fetch_add` (once per chunk) and stores the chunk's
pointer with release before any reference to it is published.

**The chunk directory is never replaced (WB-19).** Writers claim chunks outside any group, so the
directory cannot grow by replacement under the lease. It is fixed at the buffer's creation: 8 inline
entries, then 12 blocks of 8 << k entries (k = 0 to 11), 8 + 8 × (2^12 − 1) = 32,768 entries. A
block is allocated by the first writer whose index falls in it and installed by compare-and-swap (a
loser frees its copy); a reader loads the block pointer and the entry with acquire. Blocks are freed
with the buffer, so the directory needs no retirement, and an idle buffer pays its 20-pointer root
(160 B) (`lanebuf_core.hh:116-181`). **The limit is load-bearing:** 32,768 chunks per buffer (at
least 128 MiB at the smallest chunk, 32 GiB at 1 MiB); a buffer whose directory fills freezes (D8),
which is the mechanism. The lane count has no cap of its own: lanes grow with concurrent pre-stages,
as Lucene's DWPT pool grows with indexing threads.

**Memory (WB-7, restated).** At 16-byte keys and 100-byte values the record is 24 + 1 + 2 + 16 + 100
= 143 B, aligned to 144 B. The map slot's cost is the table's real occupancy, not 8 bytes at 7/8: a
doubling table averages far less full, 16.8 B per record at 1,000,000 records (2,097,152 slots) and
15.7 B by `kv_proto_bufmem`'s resident-growth method (the review's `bytes.out`). The position index
adds 12 B in revision 2 (D9), 4 B before. Format 14's prototype of the same layout (inline records,
a 24-byte header, 8-byte slots, two runs) measured 161.5 B accounted and **176.7 B resident**
(`format-14-kv/report.md:84`). The step-0 prototype measured **181.7 B accounted and 193.5 B
resident** per record with the 12-byte position index, against 167.8 and 185.1 B with the 4-byte one
(bake-off `:430-431`). RocksDB measured 153 B. T6's target stays at 153 B, and a miss is reported as
one. What closes the gap, each to be measured: a 4-byte docid offset from the buffer's base, with an
escape for a replace that keeps an older docid (about -4 B, estimate); a 4-byte version with an
escape (about -4 B); a table pre-sized from the index's previous flush, which keeps occupancy near
7/8 (about -6 B, estimate). The docid map (another slot) exists only once a docid-addressed
operation or a keyless document reaches the buffer; keyed puts never create it.

## D4. The sequencer: stage 1 in one place at a time

On a journal-less index the sequencer is the write group:

1. A writer links itself to the index's writer queue by one compare-and-swap (RocksDB's
   `LinkOne`); the writer that finds the queue empty leads.
2. The leader takes the queue up to a byte cap: a group whose first write is small grows to that
   write's size plus one eighth of the maximum group size (RocksDB's rule, `write_thread.cc:449-454`,
   WB-15), reverses it to arrival order, and for each transaction in turn assigns its position and
   runs today's stage 1 against the version map: lookups, version checks, docid and version
   assignment, metadata. A writer whose lane is in a sealed buffer has its records copied into the
   leader's lane of the active buffer first (D8). For each accepted write the leader fills the
   record's header, stores the map slot with release, and appends the record to the buffer's
   position index (D9).
3. It ends the group (D7's rules decide when V moves and verdicts are delivered) and hands
   leadership to the oldest writer that linked meanwhile.

On a journaled index the applier does step 2 for each committed entry in log order (D7); a
pre-staged lane exists only on the replica where the proposal was made.

**The version map** keeps format 14's 8-byte slots (a 32-bit record reference and 32 bits of the
key's hash, grown at 7/8; measured -63 B per record and 16% more run-phase throughput on `main`,
`buffer-bytes.md`), with three changes so readers can probe it while the sequencer writes. The
review's probe confirms the protocol: 4,261,195 native reads across 15 growths with 0 wrong and 0
torn, and TSan clean over 529,752 reads (`write-buffer-design.md:48-52`).

- Each slot is a `std::atomic<std::uint64_t>`: the sequencer fills the record, then stores the slot
  with release; a reader loads it with acquire, so a slot it sees names a complete record.
- **No removal during the buffer's life.** A delete appends a tombstone record and points the slot
  at it; a replace points the slot at the new record. So no backward shift moves a slot under a
  reader (today's `erase`, `index.hh:223-239`, leaves the buffer's map).
- **Growth** builds a table twice the size, fills it, then publishes its pointer with release; the
  old table is retired and the view generation bumped (D10), not the component epoch (WB-18). A
  reader that loaded the old pointer finds every key whose position it can see, since the
  sequencer stops writing the old table only after the new one holds everything.

**A rejected transaction** restores the slots it changed from the undo log, as today, and rewinds
the position index to its count before the transaction, which nobody has seen (the count is
published at the group's end). Its records stay in the lane until the buffer is freed, unreferenced;
a reader that loaded a slot before its restore reads a complete record whose position is above its
read position (D5) and follows `prev`. T3 reports the bytes.

**Pre-resolution.** A key absent from the version map would cost the sequencer a filter probe and a
dictionary lookup per segment (`lookup_key`, `index.hh:1952-1964`). The writer resolves it before
joining, against the view it holds, and records that view's **component epoch** e: absent, or
(component, ordinal, docid, version). The sequencer uses that result only when all three hold:

1. The key is still absent from the active buffer.
2. The current view's component epoch is still e.
3. **The active buffer holds no record of the pre-resolved copy's docid (WB-20)**: when the active
   buffer has a docid map and the result is a copy, the sequencer probes the map for the copy's
   docid, and any record found makes it resolve again.

Otherwise it resolves again. Rule 3 exists because a `delete_docid` or a key-changing
`replace_docid` of a segment copy writes no record under the copy's key (D6) and moves no
component epoch, so rules 1 and 2 alone would accept a copy already dead at the sequencer's
position (the model's `preresolution` variant: 104 of 300 runs reach a wrong verdict or docid, a
deleted document returning under its old docid among them; `preresolution_fixed`: 0 of 300). An
absent result needs no probe: a write that makes the key present writes a record under the key. The
probe costs one docid-map lookup, only when the map exists. (C8 allows one equivalent: the result
records the active buffer's count of docid-only supersessions, D6, and the sequencer compares it.)
With the three rules the result is the same as the sequencer's own lookup, so replay, which
resolves everything itself, decides identically (T9). On a journaled index the proposer's result
rides with its pre-staged lane on the leader replica; followers and replay resolve themselves.

**Two counters, not one (WB-18).** A view carries a generation, which moves at every publication and
keys retirement (D10), and a component epoch, which moves only when what pre-resolution reads
changes: a freeze, a flush's install, a merge's install, `heal()`, `install()`, or a change of live
bits, which happens only inside those (the prototype has no `heal()` or `install()`). A replaced map
table or run set changes nothing pre-resolution reads (frozen buffers' maps, segments, their bits),
so it moves the generation alone and invalidates nothing in flight (`lanebuf.hh:108-115`, `:683`,
`:802-812`). In the prototype's T1 rows (10^6 writes, 1 to 8 writers, no scans) every pre-resolution
was used and none redone (the lead's raw lines,
`docs/perf/write-buffer-bakeoff-data/scan-tail-5-guard-t1.jsonl`).

The sequential part per transaction is then one map probe, the header writes, one slot store and
one position-index append per write. Docid assignment stays sequential and deterministic, which
stage-1's ruling requires.

## D5. What readers see, and when

The index publishes one 64-bit **visible position** V, the contiguous acknowledged prefix (D7):
every transaction at or below V is visible whole, and none above it. Only the lease holder stores
V, with release.

**A view has a position too (WB-1):** the position all of its positionless state reflects, its
segments' live bits and the segments a flush, merge, or install put there: a freeze's F, or the
group boundary an install ran at. A reader loads V with acquire, acquires a view (shared-reader
D5), and **reads at r = max(V, view.position)**. Records in buffers are filtered at r; the view's
bits and segments are exactly the state at view.position. A view is published only once V has
reached its position (D7, rule 4), so r never exceeds the acknowledged prefix, and r never
decreases for a reader. The review's model returns the transaction whole under this rule where
reading at V tore it (`view_model.out`: "freeze, read at max(V, view.position): sum=100 OK").

- **get(key)**: probe the active buffer's map; take the slot's record and follow `prev` while the
  record's position exceeds r. A record found answers (a tombstone answers "absent"); none found
  goes on to the view's frozen buffers, newest first, each probed the same way with no live bits
  to check (shadowing decides, D6, WB-16), then its segments with the view's live bits
  (shared-reader D8) (`lanebuf.hh:204-224`) and the docid check: a segment copy is superseded
  when the view's active buffer holds a record of its docid at or below r; a reader at r below the
  active buffer's first docid-only supersession skips the check (D6, WB-22). `prev`
  is followed only for writes published after the reader's load: the review's probe saw chains of
  at most 1 (`write-buffer-design.md:52`).
- **The docid walk at r**, for the docid check and any read by docid: probe the buffer's docid
  map through the **const probe (WB-23)**, which loads the map's table pointer with acquire and
  never builds: no table means the buffer holds no docid-only supersession (D6), so the check
  passes. While the record's position exceeds r, go to its docid prev when the field holds a
  reference, else to `prev` (D3). A record of another docid, or none, ends the walk: the docid has
  no record in this buffer at r. So a reader at a position older than a key-changing
  `replace_docid` sees the docid's document as it was, under its old key, and a reader by key sees
  each key's own chain, as before. Item 4 offers other threads get by key and scan, which run the
  walk for the docid check only; a read by docid from other threads is not in item 4, since a
  buffer without a docid map cannot answer one, and the step that offers it decides how that map
  gets built, under the lease (D8).
- **scan(from, to, n)**: D9.
- A view (shared-reader revision 2, D12) holds the active buffer, the frozen buffers, the segments
  with their live bits, and the metadata. A new view object is published when one of those changes
  (a freeze, a flush's install, a merge, `heal()`, `install()`, metadata or schema) and when a map
  table or run set is replaced (D10); a write that changes only buffered records advances V alone.
  Besides its position a view carries a generation (retirement, D10) and a component epoch
  (pre-resolution, D4, WB-18).

This is RocksDB's visibility (a snapshot is a sequence number, visible once `SetLastSequence`
passes it) with one addition RocksDB does not need: positionless state, strata's live bits, is
published as a view with its own position.

## D6. Deletes and replaces across writers

**Who wins: the sequencer's order.** On a journal-less index two writers that replace one key are
ordered by the leader that puts them in a group, in arrival order, or by the groups' order; on a
journaled index by the log. The later position wins; a conditional write is checked against the
state after every earlier position. Replay in log order gives the same verdicts (T9).

- **In the active buffer**, nothing is marked dead in place. The new record (or tombstone) points
  `prev` at the old one and takes the slot; a reader at an earlier r still finds the old version
  through `prev`.
- **Frozen key-value buffers carry no live bits; shadowing supersedes their copies (WB-16).** A get
  reaches a frozen buffer's record through its map, which yields a record reference, not a
  position-index slot, so bits indexed by slot could not be checked on that path. None are kept: a
  replace or delete whose live copy is in a frozen buffer appends nothing, and its newer record or
  tombstone, in a newer buffer, is what every get and scan meets first (components newest first,
  D5, D9). A reader at r below the newer record's position correctly sees the older copy; one at
  or above it never reaches the copy. The flush's install drops superseded copies (below).
  Search's frozen lanes keep bits, since shadowing hides no postings (D11).
- **Segments carry copy-on-write live bits**, indexed by ordinal through shared-reader D3's
  accessor. A replace or delete whose live copy is in a segment appends (segment id, ordinal) to
  the active buffer's **pending deletes**, a list only the sequencer appends to; the newer record in
  the buffer shadows the copy for key-value reads meanwhile, and the sequencer's own stage 1 stops
  at it. The freeze applies the pending deletes to the segments' bits (D8), so a frozen buffer has
  nothing pending (`lanebuf.hh:716`, `:822-850`).
- **Installs carry pending deletes (WB-2).** A lease operation that replaces a component remaps the
  pending deletes that name it in the active buffer (a frozen buffer has none, above) before the
  new view is published:
  - a merge's install maps each (source, ordinal) through `plan.ordinals` to (output, ordinal), as
    `install_merge` maps the deletes set since scheduling (`index.hh:1776-1795`); one that maps to
    no output document fences, as a carried delete does today;
  - a flush's install, which has no frozen-buffer bits to carry (WB-16), clears in the new
    segment's bits every key that a newer buffer's map (the active buffer's or a newer frozen
    buffer's) holds, and every keyless document whose docid a newer buffer's docid map holds; it
    runs at a group boundary and its view's position is that boundary, so every reader of the view
    reads at or above each newer record that killed a copy (D5) (`lanebuf.hh:899-920`);
  - `heal()` keeps the segment id and ordinals, so its pending deletes stand;
  - `install()` replaces everything and discards the buffers, their pending deletes with them.
  The review's model resurrected a deleted key without this (`view_model.out`, "RESURRECTED");
  T8 schedules installs with deletes pending on their sources, with a mutant that drops them.
- **Docid-addressed operations** (`replace_docid`, `delete_docid`) resolve the docid through the
  docid map (D3), whose slots work like the key map's. A keyed copy in a buffer has its key in its
  record, so a delete or a key-changing replace appends a tombstone for that key, which shadows the
  copy by key as above (step 1's `evaluate`, `index.hh:2956`, `:2979` at `f21a10d`); a keyed copy
  in a segment is shadowed by docid instead (below). A **keyless buffered document** deleted by
  docid gets a docid tombstone: a record with the tombstone flag and no key, which takes the docid
  map's slot (WB-15); readers by docid see it at r, and the flush skips the document. A keyless
  document in a frozen buffer is deleted the same way, by a docid tombstone in the active buffer,
  which shadows it by docid as a keyed record shadows by key, and its flush's install drops it
  (WB-16). One in a segment is deleted through the pending list, visible at the next freeze;
  key-value reads never meet one, and search reads at a freeze (D11).
- **A keyed segment copy addressed by docid is shadowed by docid.** A segment's key
  dictionary maps a key to its ordinal and nothing maps back (`keys.hh:208-219`; step 1's
  `key_of_copy`, `index.hh:2747-2752`), so stage 1 cannot name the key of a keyed segment copy that
  a `delete_docid` or a key-changing `replace_docid` supersedes: its tombstone or new record carries
  the copy's docid but not its key, and the copy waits in the pending deletes for the freeze. So
  **every reader, the lease holder included, treats a segment copy as superseded at r when its
  view's active buffer holds a record of the copy's docid at or below r**, found by the docid walk
  (D5). Why the active buffer alone suffices: a freeze applies its buffer's pending deletes to the
  segments' bits in the view that first lists that buffer as frozen (D8), and a flush's install
  clears the copies a newer buffer's docid map holds (above), so no frozen buffer in a view holds a
  supersession its segments' bits lack (the model's `check_all_buffers` changes nothing). Merges
  keep docids (`merge.hh:136`), so the check needs nothing from a merge's install; the remap of the
  pending deletes still serves the freeze. A buffer without a docid map holds no such supersession,
  since every docid-addressed operation builds the map before its stage 1 (`index.hh:2867-2876`).
  Step 1's set of pending (segment, ordinal) pairs (`index.hh:1567-1581`, `:1833`) stays stage 1's
  index of what the freeze applies; reads use the docid check.
- **Only the lease holder builds a docid map (WB-23).** Readers probe through D5's const probe,
  which never builds. A map is built in stage 1 (the active buffer's, before a docid-addressed
  operation or a keyless document, in `writable_buffer`; a frozen buffer's, the first time stage 1
  resolves a docid through it, in `locate_docid`, `index.hh:2714`, D8's one exception) and in a
  flush's install. A build publishes the table, then fills it (`version_map::reserve`, then
  `store_docid`, `write_buffer.hh:85-90`, `index.hh:2681-2692`); a reader that probes it while it
  fills can miss only records stored before the map existed, which are keyed puts and key
  tombstones that readers meet by key, so a partial map hides no docid-only supersession. Step 1's
  build from a const lookup through a plain `mutable bool` (`find_docid_in` calling `build_docids`,
  `index.hh:2675-2685`, `:1827`) changes to this in item 4.
- **The first docid-only supersession (WB-22).** Each buffer keeps the position of its first
  **docid-only supersession**: an accepted transaction whose `delete_docid` or `replace_docid`
  finds its segment copy by docid alone, not by the key it writes. It starts at "none"; the lease
  holder stores it with release before V passes that position, and a rejected transaction restores
  it. A reader at r below the view's active buffer's value skips the docid check: no record at or
  below r supersedes a segment copy by docid alone, and a supersession that writes the copy's key
  is met by key first. The model's `skip_below_first_docid_only`: 0 wrong in 6,259,626 reads.
- **What the docid check costs (WB-22).** It runs on every segment copy that a get, or a scan's
  merge, selects as a key's newest at r, emitted or not, while the view's active buffer has a docid
  map and r is at or above its first docid-only supersession. Per copy considered:
  1. Reading the copy's docid, which get paths do not read today (`get_segments`,
     `index.hh:1575-1585`): one bit-array load in a key-ordered (permuted) segment, up to 127 LEB128
     decodes in a docid-ordered (monotone) one, the search layout whose gets read live (Q2)
     (`docids.hh:166-175`, blocks of 128, `format.hh:119`).
  2. One docid-map probe.
  3. On a hit, the record's docid and position, a load into its lane.

  The worst case is a scan through a range that docid deletes emptied: until the freeze turns those
  deletes into bits, it reads and probes every dead copy in the range, O(range) rather than O(n)
  for n results. T2's and T3's variants price it (gate rows). If T3's shows the O(range) cost, a
  freeze triggered by the count of docid-only supersessions bounds it, since a freeze turns them
  into bits; adding it is the lead's call on that measurement.

  The options weighed (costs are estimates; nothing was measured):

  | Option | Reader rules | Cost | Verdict |
  |---|---|---|---|
  | (a) Resolve the key at stage 1 from the segment, so the tombstone carries it | unchanged, as revision 2 assumed | A document-layout segment's ordinal is its key's rank (`reader.hh:366`, `:605`), but the key dictionary has no select by rank: blocks are found by key through an FST or walked by their length headers (Kronuz/termdict `termdict.hh:23-33` at `8b1e964`). So a per-block first-ordinal table (a key-dictionary format change) or an O(blocks) walk on the serial path per resolution. A docid-ordered segment (the search layout, `reader.hh:597`), whose gets also read live (Q2), has no rank to use and would need an ordinal-to-key table, 4 B per document on disk: a second mechanism per layout | rejected |
  | (b) A pending-delete bitmap per segment | Bits without positions are unsound: a reader at r below the delete would see it, and a rejected transaction's bit before its restore. Sound only as the view's own bits, set at the group's end and published as a view at that boundary (D5's positionless state, D7 rule 4), which adds no rule | A whole-bitmap clone per touched segment per group that deletes by docid (`deleted_bits.hh:52-60`: a clone copies every word), 1.25 MB for a 10^7-document segment, plus a view publication and its registry scan, with V held until the view is out; a stream of docid deletes pays it every group | rejected |
  | (c) Carry the delete in the version map: the active buffer's docid map shadows segment copies by docid | **Extended**: a segment copy found by key also passes the docid check; the docid walk (D5) | Nothing while the active buffer has no docid map (keyed puts never create one, D3) or r is below its first docid-only supersession; otherwise, per segment copy considered (emitted or not), a docid read from the segment (one load, or up to 127 LEB128 decodes in a docid-ordered segment) and one docid-map probe, O(range) for a scan across copies deleted by docid until the freeze (above). Needs the docid prev field (D3) to be exact at r. The flush's install already clears by docid this way (`index.hh:3151-3156`) | **chosen** |

  (c) is the one mechanism already in D6 for keyless documents and in the flush's install, applied
  to keyed segment copies; with the skip it costs nothing on the keyed workloads the gate rows run,
  nor on buffers whose docid map exists only for keyless documents or key-preserving docid writes;
  it adds no format and keeps the freeze and the installs as they are.
- **Metadata and schema** are ordered with the documents by position and change the view (D5).

The invariant format 14 relies on, one live copy per key (`format-14-kv.md:283-286`), holds per
view at every freeze and through every install; between freezes it holds at each position through
shadowing.

**Buffers are newer than segments (WB-24).** In every view, every record in the active and frozen
buffers is newer than every copy in the view's segments. Shadowing by key and the docid check both
rely on it: a buffered record of a key or docid hides every segment copy of it. Flushes keep it,
since the oldest frozen buffer installs first (`index.hh:3118-3134`); merges combine segments only;
`heal()` keeps the segments' content; `install()` discards the buffers. Every install path keeps it,
step 3's segment-mode follower install by D7's rule.

## D7. Paths, order, and acknowledgement

**Which path runs (WB-5).**

| Index | Orders writes | Runs stage 1 | What lanes buy there | The write group |
|---|---|---|---|---|
| Journal-less (7a under Xapiand's WAL; embedders without durability; the WAL-off benchmark rows) | the write group's arrival order | the group's leader | parallel record copies, hashing, and pre-resolution; no group barrier | the sequencer |
| Journaled, rf = 1, any preset (standalone durable; a single-node Elysian) | the log (a group of one) | the applier, at `commit_memory` | on this replica: the applier fills headers instead of copying; pre-resolution's result rides along; for search, inversion on the proposer's thread (D11) | none; proposals are concurrent |
| Journaled, rf > 1, document-mode followers | the log | each replica's applier | the leader replica as above; followers copy from entries into one lane | none |
| Journaled, rf > 1, segment-mode followers (`replicated`, `relaxed`) | the log | the leader's applier; followers install its segments and apply only a lagging tail | the leader replica as above | none |

**A segment-mode follower's install keeps buffers newer than segments (WB-24).** A follower that
installs the leader's segments through position p leaves no buffered record at or below p in the
view that lists them: it discards, as `install()` does, every buffer holding such a record, and
applies to its buffers only entries above p. Otherwise a buffered record older than an installed
copy would shadow it, by key or by docid (D6's invariant). Step 3 builds this rule; T8's step-3
schedule installs segments over a follower's buffered tail and reads by both keys and by docid.

On a journaled index, "a request never waits on another request's writer" holds through concurrent
proposals alone: each request awaits its own entry. What lanes add on that path is the work they
move off the applier, which is serial: the record copy (small for 100-byte values, larger at
Xapiand's kilobyte records), resolution, and for search the inversion, the dominant cost. T11a
measures that path; Q4 confirms the scope.

**One batch per group (journal-less) or per proposal (journaled).** A transaction's position is
(log index, place in the batch), the seqno ARCHITECTURE defines under groups (`ARCHITECTURE.md:67-73`).

**Staging from any thread (WB-11).** The turn log's driver is single-threaded: `stage()` pushes
onto a plain vector (`turn_driver.h:57-60`) that `begin()` drains (`:66-79`). A leader or proposer
on another thread pushes its batch onto a lock-free inbox per core, on strata's side (the
`LinkOne` shape), which the core's turn loop drains into `stage()` before `begin()`. No hop, and no
change to Kronuz/journal; Detent's agent gets a note (C2). Per-group sequences stay contiguous
(`turn_log.h:288-294`) because one sequencer at a time stages for an index.

**Acknowledgement and visibility: the four rules (WB-6, the owner's Q1).** Writes carry
acknowledgement levels with a default and a floor per preset (`durability-modes.md:2033-2062`).

1. **V is the contiguous acknowledged prefix, with one owner.** V passes a position only when it
   and every position before it are acknowledged at their own levels: a `none` write behind a
   pending `memory` write waits for it. Acknowledgement events (the turn's barrier, `commit_memory`,
   `commit_durable`) arrive on the turn driver's or the consensus core's thread, which records them
   in a monotonic atomic and links a writerless group if none runs; only the lease holder computes
   and stores V.
2. **Every verdict waits for its predecessors.** A verdict for position p, a rejection or a version
   conflict included, is delivered only once V ≥ p, since it observes earlier writes a crash could
   still remove.
3. **V is stored before the writers resume**, so a request that reads after its own acknowledged
   write sees it.
4. **Positionless state waits for acknowledgement too.** A freeze or install at position F
   publishes its view only once the acknowledged prefix reaches F, under the lease (the
   acknowledgement's writerless group publishes it, then stores V). V never passes the newest
   published view's cover: positions in a buffer no published view holds wait for its view.

On a journaled index at `memory`, apply already runs at acknowledgement, so rules 1 to 3 cost
nothing extra; a `quorum` write holds V, and the visibility of later writes, until
`commit_durable`. On a journal-less index V moves at the group's end. T8 runs with a log under mixed
levels and crashes before barriers, and checks that no reader and no verdict observes a write the
crash removed.

**Determinism.** Lanes change where records sit in memory, never what stage 1 decides: verdicts,
docids, and versions are a function of the log's order. Flushed key-value segments are key-ordered
(format-14 D1a) and so are the same bytes whatever the lanes. Search's per-lane flushes are not
(D11).

## D8. Freeze

**A freeze is a lease operation** at a group boundary (or between two applied entries), at
position F:

1. Seal the active buffer: it becomes a **frozen buffer**. Its maps, chunk directory, position
   index, and records are never written again, except a search lane's live bits through
   copy-on-write (D11) and one exception (WB-23): a buffer frozen without a docid map gets one
   built once, by the lease holder, the first time it resolves a docid through that buffer (stage
   1's `locate_docid`, `index.hh:2714`), never by a reader (D6). A buffer frozen without a map
   holds no docid-only supersession, so readers lose nothing before the build. A new active buffer
   is created at the next write.
2. Apply the pending deletes to the components' live bits (D6).
3. Publish the view once the acknowledged prefix reaches F (D7, rule 4), then V.

**Writers that pre-staged into the sealed buffer (WB-3).** A writer pre-stages into a lane of the
buffer that was active when it started; a freeze can come before it links, at one writer too (a
search's coalesced freeze, a node-budget freeze). At linking (or when the applier takes its entry)
the sequencer sees that the writer's buffer is sealed and copies the writer's records into its own
lane of the active buffer before stage 1. A late append into the sealed lane's tail is harmless:
nothing reads a sealed buffer except through its map and position index, which never reference
it. The writer's view guard keeps the sealed buffer alive until it links. The copy costs only
around freezes; T8 freezes while writers pre-stage.

The cost is O(1) in buffered records, plus one bit per pending delete and one copy-on-write clone
per component that took one, plus the view's allocation (O(components)). No sort, no copy, no disk.
T6 measures it.

**A frozen buffer is an in-memory segment** to every reader: probed through its map for a get,
ordered through its runs for a scan, checked against its live bits, and dropped from the next view
when its flush installs (D6 carries the deletes). The flush, on a pool thread, sorts the frozen
buffer's live newest records by key and writes a segment as today's flush does. Freezes happen:

- when the buffer reaches its budget share or the node budget asks (today's `needs_flush`,
  `index.hh:1247`), or its chunk directory fills (D3): a freeze, then a flush;
- when a search must see writes past the last freeze (D11);
- when a frozen-buffer count cap is reached (default 4, an estimate T6 tunes): the oldest flush
  first.

**Checkpoints and log reclamation (WB-15).** A checkpoint covers flushed state only: its position is
at most the first position of the oldest unflushed frozen buffer (or of the active buffer when none
is frozen), and below `commit_durable` (durability-modes D1). A commit that must cover everything
freezes and flushes first, as today's `commit()` flushes. The log keeps every entry above the
checkpoint, so replay rebuilds the buffers.

## D9. Key order for scans

**Decided (owner, 2026-10-26): (C).** The bake-off chose (C) (bake-off `:270-275`, `:279-285`):
A' met T3 at every write rate (0.56 to 0.78× RocksDB's p99.9) but lost T1 to (C) by 4 to 7× and
missed it against RocksDB (0.72 to 0.86×), so its code is deleted (one implementation). The owner
accepted (C)'s remaining T3 miss at full write speed; T3's target stands, and the optimizations
below are queued against it. The structure (`lanebuf.hh:27-48`, `:937-1083`, `:1395-1452`,
`:1515-1575`; `lanebuf_core.hh:40-62`, `:292-375`):

- **The position index (WB-4), prefixed.** Each buffer has one append-only array of 12-byte
  entries, one per accepted write in position order: the key's first 8 bytes (big-endian, zero
  padded) and the 4-byte record reference. The sequencer appends; the count is published with
  release at the group's end, before V moves. It grows by geometric blocks (64 << k entries) that
  are never replaced, so it needs no retirement. Positions never decrease along it, so a reader
  finds its visible end at r by binary search. Run builds, a scan's private tail, and the flush
  enumerate records through it only, never through a lane.
- **Runs: a binary counter of prefixed entries.** A buffer's runs are immutable sorted levels of
  the same 12-byte entries (key order; among one key's records, the newest position first),
  biggest first, with the position-index count they cover. A level merges into the one before it
  while that one is at most 8 times its size (ratio 8), so merges amortize to O(log n) per entry
  and a scan merges few levels.
- **Fresh and compaction jobs, applied as splices.** Two pool jobs build the runs. A fresh job
  sorts the uncovered backlog and merges levels of up to 16,384 entries; a compaction job merges
  the bigger levels. Each works on a copy of the levels it was posted with and returns a splice:
  the consecutive levels it consumed, the level it produced, and for a fresh job the new cover.
  **Only the lease holder applies a splice**, to whatever set is installed by then (a splice whose
  levels are gone is dropped), installs the new run set with release, and retires the old one
  (D10). A long compaction never holds back a fresh publication, and retirement stays the lease
  holder's.
- **Builds only when a scan asks (WB-17).** A scan whose private tail passes 64 entries sets the
  buffer's build flag. The lease holder posts a fresh job at a group's end only when the flag is
  set, and a scan on an index nobody writes links a writerless group to get one (D2). Writes alone
  build nothing, as format 14 built its view only on demand (`format-14-kv.md:306-313`). The flush
  sorts its frozen buffer's position index on the pool itself and needs no run.
- **A scan at r** merges, for the active and each frozen buffer, its levels and a private tail (the
  position-index entries past the cover, at or below r, at or after `from`), and the segments. The
  tail is not sorted whole: the 32 smallest entries are selected and sorted, the rest only if the
  scan runs past them. The merge compares the 8-byte prefixes first: one pass finds the smallest
  prefix and the sources holding it, only those step, and a record is read only on a prefix tie.
  Among one key's records it takes the newest at or below r. Every segment record it selects as a
  key's newest passes D6's docid check, emitted or not, so a range emptied by docid deletes costs
  O(range) until the freeze; the check costs nothing while the view's active buffer has no docid
  map or r is below its first docid-only supersession (D6, WB-22).

**Measured costs** (the lead's runs, macOS x86_64 only; Linux, FreeBSD, and arm64 are owed):

| Row | Result | Where |
|---|---|---|
| T3 at full write speed, 1 to 8 writers, 4 readers | p99.9 40 to 63 µs, 1.5 to 1.8× RocksDB's (target 1.0×: missed, accepted); p99 31 to 41 µs, 1.3 to 2.1× | bake-off `:406-419` |
| T3 at 2 MB/s | p99.9 0.33 to 0.39× RocksDB's: met | `:406-419` |
| T1, 1 to 8 writers | 1 to 3% slower, inside the A/A spread (the sequencer writes 12 bytes per record into the position index, not 4) | `:420-437` |
| T2 get p99 | unchanged | `:429` |
| Memory per record (T6) | +8.3% accounted (167.8 to 181.7 B), +4.5% resident (185.1 to 193.5 B): the 12-byte position index in blocks that overallocate up to 2× | `:430-431`, `:439` |
| Memory per index (T7) | +528 B after one write (the first block holds 64 entries) | `:432` |
| Runs of a scanned buffer | 12 B per record | the lead's raw lines (`runs_per_rec`, `scan-tail-4-ratio.jsonl`) |
| Freeze at 10^6 buffered, 10% replaces | 9% faster | `:433` |
| View publications | 19 per 10^6 writes on an index nobody scans (T1 rows, no run builds); 19,700 to 22,900 per second (medians, ratio 8) while scanned beside 1 to 8 writers at full speed, one per installed splice | raw lines (`views`, `run_builds`: `scan-tail-5-guard-t1.jsonl`, `scan-tail-4-ratio.jsonl`) |

**Queued (owner, 2026-10-26), after the buffer is built.** The full-speed gap is bound by source
count and cache misses (about 11 sources per scan; 45% of a scan builds its sources, 53% merges
them, bake-off `:457-459`):

1. A sampled index over each big level, so a seek touches fewer cache lines per source.
2. The visibility position in the entry (16 bytes), so a source needs no record read to filter at r.

Each costs memory and lands only on a measurement. A 4-byte prefix in the position index would
take back half of its 8 added bytes at the price of more tie reads; it is unmeasured, and T6's
153 B target is the lead's call.

## D10. Retirement: what readers hold, and who frees it

A reader holds a view (shared-reader D5's hazard record); the view holds its buffers and segments
by `shared_ptr` copies the lease holder makes and drops. Within a buffer, three kinds of objects are
replaced while readers may hold the old copy: a grown table of the key map or of the docid map
(WB-23), a replaced run set (D9), and the buffer itself once flushed. A docid map's first table
replaces nothing; its growths, during a first build or later, retire the old table as a key map's
do. Step 1 frees a grown table at once (`write_buffer.hh:104-120` at `f21a10d`), which item 4
changes. **Only the lease holder replaces any of them.** What writers grow outside the lease, the
chunk directory and the lane list, is never replaced (D3, WB-19), and the position index's blocks
are never replaced either (D9). Each replacement is retired into the index's retire list stamped
with the current view generation g, and **the replacement publishes a new view** (same components,
generation g + 1). The lease holder frees a retired object during its registry scan once no record
names a view of generation g or older.

Why that suffices: a reader can load the old pointer only while it holds a view of generation at
most g, since the lease holder's release store of the new pointer comes before its publication of
g + 1, so a reader that acquires g + 1 or later loads the new pointer. Bumping the generation at
each replacement is what frees the old copies at the next scan instead of at the next freeze (the
review measured another 16.8 B per record held until a freeze otherwise, WB-7). The cost is a view
allocation (O(components)) per map-table doubling and per installed splice (WB-17, restated): 19
per 10^6 writes on an index nobody scans, and about 20,000 per second on one scanned beside writers
at full speed (D9's table). The registry scan is not per publication: the lease holder scans at a
group's end once 8 groups have passed since the last scan or more than 32 objects wait (the
prototype's constants, `lanebuf.hh:311`). No collector, no thread, bounded work, as
ARCHITECTURE's memory rule asks (`:188-192`).

The step-0 prototype protects readers with this registry (shared-reader's `hazard.hh` with SR-D18's
fix), not with a process-wide epoch domain, which shared-reader D5 rejects (WB-8).

## D11. Search's inverted layout

- **Lanes are private inverters.** A search lane holds its writer's inverter (today's
  `active_buffer`, `index.hh:1296-1314`) and stored records. Stage 1 (keys, docids, versions)
  stays the sequencer's, so a replace by Xapiand's `_id` term is decided once, in order. The
  writer inverts its accepted documents on its own thread after the verdict: inversion runs in
  parallel, as Lucene's DWPTs invert (`DocumentsWriterPerThread.java:226-300`), with strata's
  verdicts kept. On a journaled index that is the proposer's thread on the leader replica; a
  follower in document mode inverts in its applier. Rejected documents never reach an inverter,
  since inversion follows the verdict (WB-12).
- **Buffer deletes across lanes (WB-12).** A replace decided for one lane leaves the old document's
  postings in another lane's inverter, and shadowing by key hides no postings. The sequencer
  appends (lane, docid) to the pending deletes, and the freeze applies them to each frozen lane's
  live bits, as it does for segments (D6). Step 4's done criteria carry it (C3).
- **Search reads at a freeze** (the owner's Q2). Postings in a growing inverter are not readable
  by another thread without a concurrent term hash and posting slices, which neither candidate
  gives. A search reads a view's frozen lanes, and shared-reader D9's `publish_through(p)` becomes a
  coalesced freeze through p: the first search that needs p freezes, and the searches queued behind
  it find the view past p. A freeze of a search buffer waits for each lane to finish inverting what
  it holds at or below F, suspending, never blocking (D12); on a durable index it also waits for
  acknowledgement (D7, rule 4). That wait is the barrier on the slowest member that (C) avoids for
  key-value; T12 reports it.
- **Frozen lanes must be readable.** Either prospect reads an inverter as an in-memory segment, or
  the freeze encodes each lane into segment bytes held in memory (`open_segment::owned`,
  `index.hh:1261`), written to a file later. The second needs no second reader path and is
  shared-reader Q1's option (c); prospect measures both at T12's NRT row.
- **One segment per lane at flush.** Per-lane flushes split documents by writer, not by docid range
  as D18's parallel flush does (`index.hh:2389-2391`), so a document-mode replica that replays the
  log in one lane writes different segments with the same digest. durability-modes D18's sampled
  rebuild then needs each flush's lane partition in the log or the flush concatenated in docid
  order; that is prospect's step and that design's call.

## D12. The async seam

- A write is a task (`xapiand-on-strata.md:224-236`): pre-stage on the caller's thread, then link
  (journal-less) or propose (journaled), and await the verdict. Followers suspend and are resumed
  on their own executors after V passes their position (D7, rules 2 and 3); plain threads (the
  benchmarks) spin briefly, then wait on the state word (`std::atomic::wait`), as RocksDB's
  `AwaitState` spins, yields, then blocks (`write_thread.cc:64-80`).
- **The sequencer's longest step (WB-14).** A group's stage 1 is bounded by its byte cap. Two steps
  are not: growing a map table (at 2,097,152 slots, 16 MiB read and 32 MiB written, milliseconds
  against ARCHITECTURE's 100-250 µs slice, `:163-185`), a docid map's first build (WB-23: one record
  read and one slot store per record of the buffer, for up to five buffers, the active one and the
  frozen ones at D8's default cap, the first time a docid-addressed operation arrives; the same
  class as a growth), and a freeze's bit clones. On a reactor thread, the sequencer posts a growth
  past 65,536 slots, a first build past 65,536 records, or a freeze with clones past 1 MiB
  (estimates T1 tunes), to the pool and awaits it as a task; writers behind it suspend, they never
  block a thread. Smaller ones run inline. Today's buffer has the same growth pause on its owner
  thread; the difference is that it is now stated and moved. T1 and T2 report the longest group.
- Checkout never blocks (D2), which retires the stage-1 review's deadlock of a blocking pool
  checkout on a reactor thread (`reviews/stage-1.md:943`).
- A view guard is held across suspension as shared-reader D9 says.

## D13. Costs, stated as estimates until the gate rows measure them

| What | (C) | (A') |
|---|---|---|
| Per transaction, one writer, journal-less | format 14's arena insert, one slot store and one position-index append per write, two uncontended compare-and-swaps (join, leave), one release store of V | as (C), plus a tower and its links: up to a compare-and-swap per level |
| Serial per transaction (leader or applier) | one map probe, the header writes, one slot store, one append per write | the same, plus the links on a journaled index (the applier links) |
| Per record, 100 B values | 181.7 B accounted, 193.5 B resident, measured in the prototype with the 12-byte position index (D3, D9) | about 175-192 B (the tower) |
| Point read | map probe with acquire loads, `prev` rarely | the same |
| Docid check (D6, WB-22) | nothing without a docid map in the active buffer or below its first docid-only supersession; else per segment copy considered, emitted or not: a docid read (one load in a key-ordered segment, up to 127 LEB128 decodes in a docid-ordered one) and one docid-map probe; a scan across copies deleted by docid pays O(range) until the freeze | the same |
| Scan | prefixed runs built on the pool when a scan asks, a selected private tail, a prefix-first merge (D9) | a skip list walk |
| Freeze | O(1) in records, plus pending deletes and clones | the same |
| Per open index | nothing until a write: then one lane chunk (4 KiB), the chunk directory's 160 B root, a 16-slot map, and the position index's first block (768 B) | the same |

## D14. The fallback: one writer, live readers

The fallback is the same implementation with the concurrency between writers taken out, not a
second buffer: one lane, direct arena offsets instead of chunk-table references, no pre-staging
by other threads (the sequencer copies every record itself), and no pre-resolution. On a
journal-less index an exclusive write lease per transaction replaces the write group's queue; on a
journaled index the applier copies and inverts everything itself. Everything readers see is
unchanged: the map readable while written, V, the read position, `prev`, tombstones, freeze,
views. Readers still see live writes on any thread with no refresh cadence.

What multi-writer costs over it: on journal-less indexes, the queue's compare-and-swaps and
handoffs; everywhere, the chunk directory's translation on every reference a reader follows, the
lanes' chunk tails, pre-resolution's epoch check, and the leader-copy at freezes. What it
buys: on journal-less indexes, a request never waits for another request's write; everywhere,
record copying, hashing, resolution, and for search inversion run on each writer's thread instead
of in the serial sequencer. On a journaled index requests never wait on each other in either mode
(D7), so the question there is only whether moving that work off the applier pays. T11a decides it
on Xapiand's path, T11b on journal-less indexes; the losing code is deleted on the path where it
loses, and if it loses on both, it is deleted (one implementation).

## Gate rows

Performance rows are measured by the lead on `main` or, for the bake-off, on the prototype's
branch as evidence for choosing (never as a gate result), under a strict hold, in minutes-long
runs. Every row reports the honest magnitude, losses included, beside an **A/A spread** of the same
configuration run twice; a difference inside the spread counts as zero (WB-8).

| Row | Target | Procedure |
|---|---|---|
| **T1** Write throughput, 1, 2, 4, 8 writers on one index, journal-less | At 1, 2, 4, and 8 writers: throughput ≥ 1.0× RocksDB's on one shared DB with `allow_concurrent_memtable_write` (its default), WAL off. The 1-writer column against `main`'s buffer is informational: the prototype's lean loop and the engine's `apply` differ in scope; T5 is the check of "no slower than today" (WB-8) | `bench/kv/proto` `lanebuf` (step 0): `fillrandom` and `overwrite`, 100 B and 1 KiB values, writers sharing one buffer, keys resolved against real segments (below); `main`'s buffer timed by `kv_proto_bufmem`'s loop (`bufmem.cc:53-75`) for the informational column; RocksDB through `bench/kv` with T threads on one DB, WAL off. Reports ops/s, CPU per op, the leader's share of time, group sizes, and the longest group (WB-14). The fallback at W writers is reported too |
| **T2** Point reads while writing | Reader p99 ≤ 1.0× RocksDB's at 1, 4, 8 readers beside 1, 2, 4, 8 writers | The prototype's `readwhilewriting`: readers `get` uniformly over loaded keys while writers run at full speed and at 2 MB/s (db_bench's `MB_WRITE_PER_SEC`); reports p50, p99, p99.9, the maximum, `prev` chain lengths, and the longest group. A variant with 10% of the writes `delete_docid` or key-changing `replace_docid` of segment copies prices D6's docid check, reporting segment copies checked per get; it runs on the engine after step 1's item 4, since the prototype has no docid operations, unless the prototype gains them first (WB-22) |
| **T3** Scans while writing (decided (C) against A', D9) | Seek and 10 nexts p99.9 ≤ 1.0× RocksDB's beside 1, 2, 4, 8 writers | The prototype's `fwdrangewhilewriting`. Reports p50, p99, p99.9, the maximum, (C)'s run builds per second with their time distribution, view publications per second (WB-17), private tail sizes, and rejected-record bytes (WB-8). A variant (WB-22) scans ranges where most segment copies are deleted by docid in the active buffer, reporting p99.9 and segment copies examined per emitted result; it runs where T2's variant runs |
| **T4** Single-writer shape | Every column of format 14's buffer table (`format-14-kv.md:299-304`: resident bytes, insert, get, seek and 10, YCSB E, flush sort) within 5% of the arena variant for (C). A' has no T4 target of its own; only decision rule 1's 10% clause weighs its insert (WB-8) | `lanebuf --writers 1` beside `kv_proto_buffer` (format 14's prototype), 1,000,000 records of 100 B and 200,000 of 1 KiB, three interleaved rounds, light hold |
| **T5** The 0021 rows | On `main` after step 2 lands: every fits1 and twox1 row of `core-vs-rocksdb-0021.md` within its trial spread or better; none lost. This is the check of the single-writer goal | `python3 bench/kv/run.py rows OUT fits1 5`, then `twox1` |
| **T6** Freeze cost and buffer memory | Freeze at 1,000,000 buffered records within 2× of the freeze at 1,000 (O(1) in records), plus a linear term in pending deletes; resident growth ≤ 153 B per record at 100 B values (format-14 T6) at 1 and 8 writers, a miss reported with its breakdown (record, slot at measured occupancy, position index, lane tails) | `lanebuf --freeze-sweep` at 10^3 to 10^6 records with 0% and 10% replaces of component copies; `kv_proto_bufmem`'s method for bytes, three trials |
| **T7** Many indexes | In the prototype (decides): resident bytes per buffer after one write, over 1,000 buffers, at most format 14's arena buffer plus 1 KiB. On the engine after step 1 (gate): the `many` row unchanged | `lanebuf --many 1000` with the counting allocator; later `python3 bench/kv/run.py rows OUT many 3` |
| **T8** Concurrency stress under sanitizers | Zero TSan, ASan, UBSan reports; every read equals a model at its read position; no transaction seen in part; the read position never decreases per reader | 1 to 8 writers and 1 to 16 readers on one index; writers apply puts, replaces, conditional writes (some rejected), deletes by key and docid (keyless included), metadata; freezes, flushes, merges, `heal()`, `install()`, grown maps, chunk-directory blocks, and run splices at random. Schedules required by the review: a freeze and an install between a reader's load of V and its acquire (WB-1); merges and flushes installing with deletes pending on their sources (WB-2); freezes while writers pre-stage (WB-3); more writers than lanes, the leader-copy path included (WB-10); a log under mixed acknowledgement levels with crashes before barriers, checking that no reader and no verdict observes a removed write (WB-6); a keyed delete, and a docid delete of a keyless document, of a frozen-buffer copy, read after a second freeze and across the first buffer's flush install (WB-16); pre-stages that claim chunks across a directory block boundary at once (WB-19); a `delete_docid` and a key-changing `replace_docid` of a keyed segment copy, then a `replace_docid` that writes the deleted docid again, each read by both keys from another thread at positions on both sides, before the freeze, after it, and across a merge install that carries the pending delete (D3, D5, D6); a key pre-staged against a segment copy that a `delete_docid` or key-changing `replace_docid` supersedes before the writer links (WB-20, step 2); a docid map's first build and its growth in the active buffer while readers run the docid check, and a frozen buffer's build under the lease while a reader holds its pre-freeze view (WB-23); segments installed over a segment-mode follower's buffered tail, read by both keys and by docid (WB-24, step 3). The model is the sequential replay of the logged order (T9). At least 10^7 reader operations and 10^5 transactions per platform (Linux x86_64, macOS arm64, FreeBSD) |
| **T9** Determinism | The verdicts, docids, versions, digest, and flushed key-value segment bytes of a concurrent run equal those of a single-threaded replay of its log order | Log the order in T8; replay on one thread, one lane, no pre-resolution; compare. Mutants, each killed by a named test: reading at V instead of r (T8's WB-1 schedule); an install that drops pending deletes (T8's WB-2 schedule, then T9's digest); a sealed buffer's records used without the leader-copy (T8 under ASan); pre-resolution used across a changed component epoch (T8's model, T9); a slot stored before its header (T10); a map entry erased in place (T8); V published before stage 1 ends (T8); a verdict delivered before V reaches it (T8's crash schedule); a retired table freed at the wrong generation (T8 under ASan); a reader skipping D6's docid check, and a docid walk that follows `prev` where the docid prev field holds a reference (T8's D6 schedule; the model kills both: `no_docid_check`, `walk_prev_only`); pre-resolution used without D4's docid check (WB-20: T8's step-2 schedule, then T9's verdicts and digest, since replay resolves everything itself); a docid-map table freed at once instead of retired (WB-23: T8's first-build schedule under ASan) |
| **T10** Ordering litmus | Zero torn records in at least 10^7 iterations; the mutant with the slot stored relaxed fails it under TSan and on macOS arm64. On x86_64 a relaxed store compiles to the release store's `mov`, so the native x86_64 run cannot kill it (the review's `vm_native_relaxed_slot`: 4,920,228 reads, 0 wrong); it runs there as a check of the correct build only (WB-9) | The sequencer fills a header and stores the slot; a reader probes and checks the header against the slot, as shared-reader T8 does for the hazard handshake |
| **T11a** Xapiand's path: the applier, multi-writer against the fallback (decides D14 for Xapiand) | Both ≥ 1.0× RocksDB's request throughput and ≤ 1.0× its request p99 on the same load with its WAL on; the multi-writer mode is kept on this path only if it clears Q3's bar against the fallback | `lanebuf --requests --applier` (step 0): R = 1, 4, 8, 16 proposing threads; each request picks an index (1,000 small indexes of 1,000 to 10,000 records, 1 KiB values; uniform, and Zipfian s = 0.99 so hot indexes see concurrent requests), checks it out, runs 4 operations (1 write and 3 gets; and 3 writes and 1 get), checks in. Writes pre-stage into lanes (multi-writer) or not (fallback) and enter an ordered per-index queue; one applier per index applies in order, the log modeled by a per-turn delay, run at 0 and at a stated delay, the same for both arms. Most keys live in segments built by strata's writer with the real key filter and dictionary, at the segment counts the merge policy leaves at these sizes (reported). RocksDB: one DB with 1,000 column families, R threads, the same stream, WAL on; the row states that its column families share one write queue and WAL path. Reports requests/s, request p50, p99, p99.9, the applier's busy fraction, CPU per request, resident bytes per open index, A/A spreads. The engine confirms it at step 3 with the turn log before the losing code is deleted |
| **T11b** Journal-less indexes, multi-writer against the fallback (decides D14 there) | As T11a, against RocksDB with its WAL off | T11a's load through the write group instead of the applier |
| **T12** Inverted layout (step 4, prospect) | Indexing throughput at 1, 2, 4, 8 writers ≥ 1.0× the faster of Lucene and Tantivy at the same thread counts; freeze-to-visible p99 ≤ 1.0× Lucene's reopen p99, reporting the wait on the slowest lane | prospect's indexing bench at those writer counts (`benchmark-coverage.md`, Suite 2, "Indexing GB/hour"); shared-reader's T7 NRT shape with `publish_through` as a freeze (D11) |

**The bake-off's decision rules (WB-8).** The lead writes them into
`docs/perf/write-buffer-bakeoff.md` before the first timed run (C1).

1. **A' against (C).** A finalist that misses T1, T2, T4 (C only), or T7 is out. If both miss, the
   one closer to its target on the row it misses wins (the ratio to target, the worse row counted
   when both miss several), and every miss is reported. If both pass, (C) is chosen unless A'
   beats (C) on T3's p99.9 by more than 20% at 4 writers while (C) misses T3's target; A' is then
   chosen only if its single-writer insert (T4's column) stays within 10% of (C)'s, since the
   many-index rule weighs one writer per index first. Differences inside the A/A spread count as
   zero.
2. **Multi-writer against the fallback (D14)**, on the winner of 1: T11a against Q3's bar decides
   for journaled indexes, Xapiand's and Elysian's; T11b decides for journal-less ones. Where the
   multi-writer mode misses, that path ships the fallback.

The lead records both outcomes in `docs/perf/write-buffer-bakeoff.md`; the losing code is deleted
(one implementation).

## Questions for the owner

**Q1 (answered 2026-10-21): readers see a write only once it is acknowledged at the index's
durability level.** D7's four rules implement it (WB-6). Linearizable get-by-id reads the live
version map, as durability-modes already says (`durability-modes.md:2049`).

**Q2 (answered 2026-10-21): search reads at a freeze, coalesced through `publish_through`; gets and
scans read live.** D11; a durable index's search freeze also waits for acknowledgement (D7, rule 4).

**Q3 (answered 2026-10-21, stricter than proposed): Xapiand keeps multi-writer only if it gives at
least 1.5× the requests per second or 0.5× the request p99 on the Xapiand-shaped load, at most 3%
slower with one writer, and at most 5% more CPU per request; otherwise one writer with live
readers.** The bar applies to T11a, on the path Xapiand's indexes run (WB-5), with each difference
read against the A/A spread (WB-8).

**Q4 (new, from WB-5). Elysian's indexes are journaled, so their writes are ordered by the log and
applied by one applier; the write group runs only on journal-less indexes. Where should the
multi-writer decision bind?**

1. Accept the split (recommended): on journaled indexes, multi-writer means concurrent proposals
   plus pre-staged lanes on the leader replica, and T11a against Q3's bar decides whether lanes pay
   there; the write group serves journal-less indexes (7a under Xapiand's WAL, embedders, the
   WAL-off benchmark rows), decided by T11b and T1. Requests never wait on each other on either
   path. The honest consequence: for Elysian's key-value indexes lanes save mostly the record copy,
   so T11a may well choose the fallback there; for search they move inversion off the applier,
   which T12 measures at step 4.
2. Drop the write group: journal-less indexes serialize stage 1 on the lease (pre-staging still
   parallel), one sequencing path fewer. The RocksDB rows at 2 to 8 writers with the WAL off then
   run with writers taking turns on the lease, which T1 would likely lose (an expectation, not a
   measurement).
3. Run stage 1 before the log on journaled indexes too, as RocksDB's leader does. Rejected:
   durability-modes applies committed entries, at `commit_memory`, in log order on every replica
   (D7, `durability-modes.md:1699-1703`); a leader that applied entries before they were committed
   would hold state a failover discards, and would need a rewind on losing leadership that no
   design provides.

## First buildable step

**Step 0, the bake-off prototype (continuing now under C1; benchmark code outside the engine, in
`bench/kv/proto` only)**, on `perf/lanebuf`, a CMake target outside `all` under
`STRATA_KV_PROTO`, as format 14's prototypes are (`format-14-kv.md:63-71`):

1. The common pieces: the write group (link, lead, the group cap, handoff, spin then
   `atomic::wait`) and an applier mode (an ordered per-index queue and one applier); lanes owned by
   one buffer and claimed per pre-stage; the chunk-table reference scheme; the version map (atomic
   slots, no erase, growth published after filling, retirement with a generation bump); records
   with sequencer-filled headers and no field written after publication (WB-9); the position index
   (WB-4); V as the contiguous acknowledged prefix with one owner (WB-6); views with positions and
   reads at `max(V, view.position)` (WB-1); freeze with the leader-copy of sealed lanes (WB-3);
   pending deletes carried through installs (WB-2); readers protected by the hazard registry
   (`hazard.hh` with SR-D18's fix, WB-8).
2. The two ordered indexes: (C)'s runs built on the pool and installed by the lease holder, and
   A's tower in the record with concurrent links and V after the last link.
3. D14's fallback from the same source (a compile-time switch in the prototype only).
4. Segments built by strata's writer, so pre-resolution and the sequencer resolve keys through the
   real key filter and dictionary (WB-8).
5. A driver: T1 to T4, T6, and T7's prototype form; T11a and T11b's request loops; T8's model check
   and T10's litmus at small counts under TSan.
6. `run.py` entries so the lead runs the timed rows interleaved under a hold, each with its A/A.

The developer dry-runs it on tiny inputs (non-empty results, the model check passing) and makes
functional claims only; the lead measures.

**Done criteria for step 0** (for the lead to set; contributors tick, never edit):

- K1 (C), A', and the fallback built from one source: the ordered index and the writer
  concurrency are the only differences; the journal-less and applier modes share every buffer
  piece.
- K2 T8's model check and T10's litmus pass under TSan at small counts; T9's and T10's mutants are
  each killed where T9 and T10 say.
- K3 Dry runs on tiny inputs give non-empty results for every workload and engine.
- K4 Nothing outside `bench/kv/proto` changes.
- K5 T8's model check includes the WB-1, WB-2, and WB-3 schedules (C1).

**Step 1 (after the architect's re-check), the core's buffer, single-writer first**, on
`phase4/write-buffer`:

1. `hash_slots` to 8-byte slots (`buffer-bytes.md`'s experiment, finished: tests, the docid-map
   comparison, sanitizers).
2. Lanes and records replacing `buffered` and `borrowable_vector` (`index.hh:2971`); the position
   index; the flush reading frozen buffers; the version map without in-place removal; tombstones
   (key and docid), `prev`, pending deletes carried through installs, freeze.
3. The write lease, with one writer: every owner duty moved under it (D2); checkout and check-in.
4. V, view positions, and the four rules (without a log: V at the group's end); live `get` and
   (C)'s scan (D9) from other threads; shared-reader's views gaining the buffer (its D12). The
   docid prev field in D3's byte layout, reserved by every record producer (WB-21); the docid walk
   through the const probe (D5); the docid check on segment copies in every reader's get and scan,
   with the first-docid-only skip (D6, WB-22); docid maps built only under the lease, at the two
   stage-1 sites and the flush's install, with D8's exception for frozen buffers (WB-23). Item 4
   also ends two deferrals step 1 recorded: a grown map table, key or docid, is freed at once until
   D10's generation-stamped retirement arrives with views that hold buffers (WB-23), and the
   position index keeps 4-byte entries until D9's 12-byte prefixed index, runs, and tail arrive
   with (C)'s scan. D12's pool rule covers a first docid-map build (WB-23). Its done criteria
   include C9's, proposed in the ledger.
5. Tests: T8 and T9 at one writer with every mutant of T9; the core's suites unchanged. WB-14's
   growth bound and WB-15's checkpoint bound are done criteria here (C4).

**Step 2**, several writers: pre-stage, pre-resolution with D4's three rules (WB-20, C8), lane
claims, the write group; T8 and T9 at 8 writers, with T8's pre-resolution schedule and T9's
mutant. **Step 3**, the journal: the applier path (proposals with pre-staged lanes), the per-core
inbox (WB-11), V at acknowledgement under mixed levels, the segment-mode follower's install rule
(D7, WB-24, C11); T11a on the engine. **Step 4**, prospect:
search lanes, buffer deletes across lanes, the freeze's wait reported (D11, C3).

## Risks

- **The sequencer is the ceiling.** Stage 1 is serial, in the leader or the applier, so write
  throughput on one index saturates at its rate however many writers join. Pre-resolution and
  pre-staging move everything else off it; T1 reports the sequencer's share of time, and
  ARCHITECTURE item 12's partitioned writers remain the answer past it.
- **Lanes may not pay on Xapiand's path.** On journaled key-value indexes they save mostly a record
  copy (D7); T11a may pick the fallback there, which Q4 makes explicit rather than a surprise.
- **Scans beside full-speed writers** miss T3 by 1.5 to 1.8× on p99.9 (macOS x86_64), a miss the
  owner accepted; D9's two queued optimizations target it, and T3 on the other platforms is owed.
- **Hot keys overwritten many times** grow `prev` chains and buffer bytes (every version is a
  record), as in RocksDB's memtable; flush writes only the newest. T2 reports chain lengths.
- **Memory.** The layout measured 176.7 B in format 14's prototype and 181.7 B accounted in step
  0's with the 12-byte position index (D9); T6's 153 B needs D3's squeezes, and may miss. The miss
  is reported with its breakdown.
- **Search's per-lane segments** change byte identity across document-mode replicas (D11); segment
  mode installs the leader's bytes and is unaffected.

## Changes in revision 1

| Finding | Change |
|---|---|
| WB-1 | D5: views carry a position; readers read at `max(V, view.position)`; T8's schedule and the read-at-V mutant (T9) |
| WB-2 | D6: frozen buffers and segments are components with copy-on-write bits; merge, flush, `heal()`, and `install()` carry or drop pending deletes, stated per install; T8's schedule and mutant |
| WB-3 | D3, D8: a lane belongs to one buffer; the sequencer copies a sealed lane's records into its own lane; late appends are unreachable; the guard keeps the sealed buffer alive; T8's schedule |
| WB-4 | D9: the per-buffer position index (4 B per record), the only enumeration path; runs installed only by the lease holder, built on the pool |
| WB-5 | Goals and D7: the path table; multi-writer on journaled indexes is concurrent proposals plus pre-staged lanes, stage 1 in one applier; D14 and T11a/T11b scoped per path; Q4 |
| WB-6 | D7: the four rules (V the contiguous acknowledged prefix with one owner; verdicts after their predecessors; V before resumption; positionless state published at acknowledgement, under the lease); T8's crash schedule |
| WB-7 | D3: memory restated at the slot's measured occupancy (15.7-16.8 B) and format 14's 176.7 B; the squeezes named; D10: replacements bump the generation, so old tables free at the next scan; the chunk table starts at 8 entries |
| WB-8 | Gate rows: both-miss rule, T7's prototype form, no A' T4 target, real segments, A/A spreads, T3 at p99.9 and maximum, T1's 1-writer column informational, the hazard registry in the prototype, T11's column-family asymmetry stated |
| WB-9 | D3: no field written after publication ("never" and "superseded" removed); T10's killers are TSan and macOS arm64 |
| WB-10 | D2, D3: lanes claimed per pre-stage; the reference drops the lane field; the chunk-table limit is the mechanism; T8 runs more writers than lanes |
| WB-11 | D7: a per-core inbox on strata's side; journal unchanged (C2) |
| WB-12 | D11: buffer deletes across lanes; rejected documents never inverted; the freeze's wait reported in T12 (C3) |
| WB-13 | Recorded in the builds-on table |
| WB-14 | D12: the sequencer's longest steps stated and moved to the pool past a size; T1 and T2 report the longest group (C4) |
| WB-15 | D8: checkpoints cover flushed state only; D6: docid tombstones for keyless buffered documents; D4: RocksDB's group cap restated; D1: per-key locks weighed |
| C1 | First buildable step: step 0's added pieces and K5 |
| C2 to C5 | D7 (inbox note to Detent), D11 (step 4's criteria), step 1's criteria, the builds-on table (ARCHITECTURE items 1, 4, 9) |
| Owner's Q1 to Q3 | Recorded as answered |

## Changes in revision 2

| Finding | Change |
|---|---|
| WB-16 | D6: frozen key-value buffers carry no live bits; a newer record or tombstone shadows their copies, by key or by docid; pending deletes name segments only; a flush's install clears the copies newer buffers hold; search's frozen lanes keep bits (D11). D5's get checks no bits on frozen buffers. T8's schedule |
| WB-17 | D9: builds are posted only when a scan's tail passes 64 entries; writes alone build nothing; D10's publication count restated from the prototype's counts; T3 reports publications per second |
| WB-18 | D4: pre-resolution is keyed on a component epoch, moved only by freezes, installs, merges, `heal()`, and `install()`; the generation keys retirement alone (D5, D10); T9's mutant restated |
| WB-19 | D3: a chunk directory fixed at creation (8 inline entries and 12 blocks, 32,768 entries, blocks installed by compare-and-swap) and a push-only lane list, neither ever replaced; D10 no longer retires chunk tables; T8's schedule |
| D9 (owner, 2026-10-26) | (C) chosen; the run pair replaced by the accepted structure: a 12-byte prefixed position index, a binary counter of prefixed runs (ratio 8), fresh and compaction jobs applied by the lease holder as splices, a select-32 tail, a prefix-first merge; measured costs (T3 miss accepted at 1.5 to 1.8×, +8.3% bytes per record); two optimizations queued. D3, D13, and the Risks restate memory to match |

## Changes in revision 3

Two gaps from step 1's implementation (`docs/status/write-buffer.md` on `phase4/write-buffer` at
`f21a10d`, "For the designer and the architect"). Both extend the reader rules of D5 and D6, so the
architect re-checks them before step 1's item 4; items 1 to 3 read only as the lease holder and
are unaffected.

| Gap | Change |
|---|---|
| A keyed segment document deleted by docid: stage 1 cannot name its key, so revision 2's shadowing by key never hid it from readers on other threads until the freeze | D6: every reader treats a segment copy as superseded at r when its view's active buffer holds a record of the copy's docid at or below r; the docid map is built only under the lease. Options (a), resolving the key at stage 1, and (b), a bitmap per segment, weighed with their costs and rejected. D5 and D9 add the check to get and scan; T2's variant prices it; T8's schedule and two T9 mutants test it. **Reader protocol: extended** |
| One `prev` per record: a key-changing `replace_docid` has a key predecessor and a docid predecessor | D3: a docid prev field, reserved in `replace_docid` records only and filled when the two predecessors differ. D5: the docid walk at r follows it, else `prev`, and stops at another docid, so a reader at an older position sees the docid's document under its old key; reads by key are unchanged. **Reader protocol: extended** |
| Two deferrals step 1 lists | Recorded in step 1's item 4, unchanged: a grown map table freed at once (D10's retirement comes with item 4) and a 4-byte position index entry (D9's prefixed index comes with item 4's scan) |

## Changes in revision 4

The architect's re-check of revision 3 (`docs/reviews/write-buffer-design.md`, "Re-check of
revision 3", WB-20 to WB-24, C8 to C11). Each fix is the one the review's model validated or
specified; none adds protocol beyond it.

| Finding | Change |
|---|---|
| WB-20 (C8) | D4: pre-resolution's third rule: the sequencer resolves again when the active buffer's docid map holds a record of the pre-resolved copy's docid (the model's `preresolution_fixed`, 0 of 300 runs failing); the review's equivalent counter named as allowed. T8's step-2 schedule and T9's mutant "pre-resolution used without the docid check"; step 2 carries both |
| WB-21 | D3: the flags byte at a fixed offset, 24, before the docid prev field it announces (28) and the lengths; every record producer (the writer, the sequencer's leader-copy and D14's fallback, the applier from log entries, replay) reserves the field from the operation's type |
| WB-22 (C10) | D6: the docid check's cost restated per segment copy considered, emitted or not, including the docid read (up to 127 LEB128 decodes in a docid-ordered segment) and a scan's O(range) worst case across copies deleted by docid; each buffer's first docid-only supersession, below which readers skip the check (the model's `skip_below_first_docid_only`, 0 wrong); D5, D9, and D13 restated. T3's variant added; T2's variant runs on the engine after item 4 unless the prototype gains docid operations |
| WB-23 (C9) | D6 and D5: only the lease holder builds a docid map, at the two stage-1 sites and the flush's install; readers use a const probe on the table pointer's acquire load that never builds. D8: a frozen buffer's first docid-map build is the one exception to "never written again". D10: docid-map tables retire like key-map tables. D12: a first build falls under the pool rule. T8's first-build and frozen-build schedules, T9's freed-at-once mutant under ASan |
| WB-24 (C11) | D6: every record in a view's buffers is newer than every copy in its segments, the invariant shadowing and the docid check rest on. D7: a segment-mode follower's install leaves no buffered record at or below the installed segments' cover. T8's step-3 schedule; step 3 carries the rule |
