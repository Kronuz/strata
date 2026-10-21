# The shared reader: snapshots beside the writer

Revision 2, 2026-10-21. State: designing (ready for preliminary review). Concurrency (lifetimes
across threads), so the architect reviews this design before anything is built (PLAN's review
tiers, the "Durability, concurrency, security surface" row). The preliminary review
(`docs/reviews/shared-reader-design.md`) approved revision 0 with conditions; revision 1 answered
SR-D1 to SR-D14, and the re-check approved it with conditions (C6). Revision 2 answers SR-D15 to
SR-D17, records the owner's answers to Q1 and Q2, takes the checkout model (D4, "the owner" is
the write-lease holder on any thread), and adds D12, what changes when the write buffer
(`docs/design/write-buffer.md`, branch `design/write-modes`) lands ("Changes in revision 2", at
the end). The re-check said SR-D15 and SR-D16 need no further architect re-check; the checkout
model and D12 are new concurrency text, so the architect reviews them with the write buffer.
Decisions D1 to D12, gate rows T1 to T9, owner questions Q1 and Q2 (both answered). Code is cited
at strata `main` `75143f9` (`41fde48`'s `include/` is identical), prospect `main` `59f36fe`, and
`design/durability-modes` at `95357d9`. RocksDB 11.11 and Lucene at `9983b7c` (both Apache 2.0)
were read for their mechanisms; no code of theirs enters strata.

The owner pulled this forward from Phase 3R (2026-10-21, `docs/perf/benchmark-coverage.md:245`).
It unblocks the threaded key-value rows (RocksDB's `readwhilewriting` and `fwdrangewhilewriting`,
`benchmark-coverage.md:93-94`), the NRT reopen row (`:144`), and the 7a concurrency row
(`docs/design/xapiand-on-strata.md:290`, T10), because Xapiand searches a shard while it indexes.

## Summary

A reader on any thread acquires a **view**: an immutable record of the index as of its last
flush (segments, frozen live-docs bits, metadata, statistics, the applied position). The owner,
whichever thread holds the index's write lease (D4), builds a view at every publishable point that
changed something (D1) and publishes it through one atomic pointer.
A reader protects the view with a **hazard record**, a word it claims in its owner's registry by
one compare-exchange from null to the view, so an acquire writes only a word no other reader
holds: no shared refcount, no lock, no per-(thread, index) slot. The owner frees a retired view,
and with it any segment mapping only that view held, after it scans its registry and finds no
record naming the view. That scan runs under the write lease at the owner's own publish, commit,
reclaim, and close, so no thread collects. Retention treats a segment a live view holds like a pending
merge's source: never queued, so never truncated under a mapping.

Readers see refreshed state only; the unflushed buffer stays the owner's (Lucene's NRT model, and
what ARCHITECTURE item 9 already says refresh does). A host that must show its acknowledged writes
to a search asks the owner for a publication at or past a position, and one refresh serves every
search waiting on it (D9). The lease holder's `get` and `scan` keep reading the buffer, so the writer
still reads its own writes.

## What this builds on, and what it changes

| Document | What this design uses | What it changes |
|---|---|---|
| `docs/ARCHITECTURE.md` | The host reactor never blocks on strata (`:6`); refresh publishes a snapshot, near real time, 1 s default (`:108-109`); memory ownership: a concurrent structure that defers a free bounds the deferred memory and the work, and does it on its owner's thread (`:188-192`); overload shed at a cap (`:180`) | "Read path" (`:156`): "the per-core handle" becomes "a view" (D5); the lead edits it on landing (SR-D13, C5) |
| `docs/design/phase-3.md` | Retention at default 0 predecessors (`:157-171`, D4/I14); a running merge's sources stay referenced (`:172-173`); truncate only after the last mapping is released (`:197-198`); `visible_position()` (`:436`); T6's "TSan over ... snapshot publication" (`:524`) | The per-core snapshot handles published by message and the owner's per-segment core count (`:178-185`) give way to D5 and D6. The architect accepted the change against R7 (SR-D13); the lead edits `:178-185` on landing |
| `docs/reviews/phase-3-design.md` R7 (`:707-717`); `docs/reviews/rocksdb-survey.md` R1 (`:43-56`) | The requirements | R7: met as SR-D13 states; R1: answered once step 1 lands |
| `docs/design/core-split.md` | The core's `get` and `scan` (`:80-85`) | "The pulled cursor waits for snapshot handles" (`:84-85`): a view's scan is that cursor's home |
| `include/strata/core/index.hh` | `get` (`:954-981`), `scan`/`scan_keys` (`:990-1236`), `segments()` (`:755-767`), `retain()` (`:2592-2655`), `reclaim()` (`:636-682`), `publish_merges` and `install_merge` (`:1699-1819`), the destructor and moves (`:497-499`) | D1 (the publishable rule), D3 (copy-on-write live bits), D4 (publication; merge jobs drop their sources on the owner), D6 (commit's order), D7 (`retain()`), D8 (one read implementation), D11 (close, destruction, move-assignment), and the comment at `:1003-1006` (below) |
| prospect `docs/ARCHITECTURE.md:110-111`; `include/prospect/searcher.hh:26-30` | A search reads a span of `segment_snapshot` (segment, deleted bits, dead count) | Its snapshot comes from a view (step 2); "published to each core by message" becomes D5 |
| prospect `docs/gates/phase-3-ratchets.md:361`, `docs/testing/parity.md:58` | "reads while a writer commits need a shared-ownership reader" | Closed by T1 and T2 once prospect's step 2 lands |
| `design/durability-modes` D1 (DM-R3, DM-R23, DM-R43) | A checkpoint records a past refresh generation, "live-docs files encoded from that refresh's deleted bitmaps (which its reader snapshot holds)" (`durability-modes.md:834-843`); `retain()` keeps the durable head, the recorded generation, and the current state (`:844-875`) | Nothing in its rulings. A view is that reader snapshot; `retain()` gains a fourth set, the segments live views hold (D7). Whether its batch entry holds a view is that design's call (D4) |
| `docs/design/xapiand-on-strata.md` D7, D13, T10 | Strata "calls `refresh()` ... before a search when the shard has unpublished writes" (`:192-195`); seam operations return `task::task<T>` (`:224-236`); T10's TSan row (`:290`; its `index.hh:685` is a stale pin) | D9 names D7's mechanism, coalesced; the lead carries it and its cost row to that design (C2) |
| `docs/design/write-buffer.md` (branch `design/write-modes`, revision 0) | The write lease (its D2), the visible position (D5), freeze (D8), retirement by generation (D10) | D4 takes the lease; D12 lists what changes when the buffer lands |
| `docs/perf/benchmark-coverage.md` | Rows 93, 94, 144 and step 6 (`:198`); the thread asymmetry (`:48-50`) | The threaded rows become T5 to T7; a threaded strata run shares one index across threads, as RocksDB shares one DB |

The comment at `index.hh:1003-1006` says "get has no such state and is safe". It is safe beside an
owner-thread scan, not beside a writer on another thread: a flush appends to `segs_`
(`index.hh:2364`, `2471`), a merge's install erases from it (`:1807`), a delete sets a bit in a
segment's `deleted` vector in place (`:2028`), and a rejected transaction clears it again
(`:1997`). Step 1 corrects the comment.

## What a reader on another thread races with today

| State | Owner-thread writer | Where |
|---|---|---|
| The segment list `segs_` | flush appends; merge install erases and inserts; commit drops all-dead segments; `heal()` and `install()` replace entries | `index.hh:2364`, `2471`, `1807-1813`, `2482`, `740`, `925` |
| Live-docs bits `open_segment::deleted` | every delete of a flushed document, in place; rollback | `:2028`, `:1997` |
| Other `open_segment` fields | `damaged`; `unsynced`, `ref.live*`, and `deletes_dirty` at commit; `deletes_dirty` on delete and rollback | `:726`, `:1749`, `:2521-2527`, `:1999`, `:2030` |
| The buffer (`buffer_`, `active_`, `pending_docs_`), its key map `key_slots_`, and the scan's sorted view `view_` | every apply; flush clears them; a scan rebuilds `view_` on demand | `:2971-2978`, `:1096-1131` |
| Metadata `meta_` and the manifest `m_` | apply, commit | `:2979`, `:2966` |

What is already safe for concurrent readers: a segment's bytes and its decoded structures are
immutable after open (`open_segment::seg`, `index.hh:1262`); the verified-region state is one
atomic per region, and two threads verifying one region at once is harmless
(`residency/residency.hh:155-157`); a get's decode buffer is per thread (`index.hh:279-312`);
prospect's mutable search state lives on per-query cursors (`prospect/reader.hh:1338`,
`1369-1370`, as the review found).

## Decisions

### D1. A view is an immutable generation, published only when the state is publishable

A `basic_view<Layout>` holds, for one generation:

- per segment: a `std::shared_ptr<const open_segment>`, of which the view dereferences only `seg`
  and its mapping (or owned bytes), which never change after construction; the segment's id and
  dead count, copied at publication; and its frozen live-docs bits (null when none is deleted,
  D3). The owner keeps writing the object's other fields (the table above), so a view never reads
  them (SR-D10);
- the metadata, as a shared, immutable map, kept until the owner changes it;
- the fields readers need from the manifest: `last_docid`, schema version, key prefix,
  configured structures, and the scoring statistics prospect computes from the segments
  (`searcher.hh:94-101`);
- its generation (a counter that increases at every publication) and its position, the applied
  position the view reflects, which is phase-3's `visible_position()` (`phase-3.md:436`).

A view is built whole by the owner, then published, and never written again. A reader touches no
member of `basic_index`.

**One rule guards every publication point (SR-D1).** The state is *publishable* when no
transaction was applied since the last flush. Between flushes it is not: a replace sets the old
copy's bit in place (`index.hh:2028`) while the new copy waits in the buffer, so a view built then
would show the replace as a delete. Every flush runs at a transaction boundary (apply flushes
before stage 1, `index.hh:527`; the other flushes are `refresh()` and `commit()`, `:567-571`,
`:624-632`), so a publishable state is one some prefix of the transactions produced, and a
transaction is visible whole or not at all. The publication points, each through the one
predicate:

| Point | Publishable there? | When it is not |
|---|---|---|
| End of `refresh()`, `commit()` | Always: the buffer was just flushed | |
| End of `install()` | Always: the buffer is discarded and the content replaced | |
| A merge installed by `wait_merges()`, `merge()`, `force_merge()`, `maybe_merge()` | Only if nothing was applied since the last flush | It publishes at the next flush point |
| `heal()` replacing a segment object | Only if publishable | The next flush point carries the new object |
| The first `reader()` | Only if publishable | `reader()` calls `refresh()` first, so the handle never starts empty |

**A point publishes when anything changed since the last view (SR-D16)**: the applied position
moved (a delete of a flushed document and a metadata or schema transaction move it and buffer
nothing), a merge or `heal()` replaced segments, or `install()` ran. Nothing keys on documents
flushed. A point where none of these happened leaves the current view in place, which is what
SR-D8 asked. "Publishable" is tested after the flush call: the buffer holds no document.

### D2. Readers see refreshed state; the buffer stays the owner's

A view holds no buffered document. A write becomes visible to other threads at the next flush
point, which the host paces with `refresh()` (ARCHITECTURE item 9, 1 s by default) or asks for on
demand (D9). This is Lucene's NRT model: `IndexWriter.getReader` flushes the in-memory buffer
before it opens the reader (`IndexWriter.java:508-581`, "flush at getReader", `flushAllThreads`).
RocksDB differs: its readers search the mutable memtable through a concurrent skip list
(`memtable/inlineskiplist.h:10-36`). Q1 asked whether shared readers need that; the owner chose
(a) for step 1, and the write buffer then gives every reader live writes (D12).

The lease holder's reads are unchanged: `basic_index::get` and `scan` still read the buffer first
(`index.hh:965-981`, `1134-1236`). Linearizable get-by-id (`workload-profiles.md:7`) and a read of
one's own unrefreshed write stay on the owner, under D13's seam a task posted to the owner's
executor.

### D3. Live-docs bits are copied on write

`open_segment::deleted` becomes a copy-on-write bitmap: one allocation holding a count, the size,
and the words, as the vector is one allocation today, so a readerless index allocates no more
often and pays 8 bytes per segment for the count (SR-D12). Publication shares each segment's
current bits with the view; the owner's next delete in that segment (`index.hh:2028`) clones them
first when they are shared and writes the clone, and the view keeps the old ones. Rollback
(`:1997`) then edits the clone, never a view's bits. Every writer of `deleted` goes through one
mutable accessor (`:2028`, `:1997`, `:1802`, `:2953`, and `heal()`'s move at `:736`). This is
Lucene's `PendingDeletes.getMutableBits` (`PendingDeletes.java:63-80`: "Copy on write ... must now
make a private clone").

The count is plain, not atomic: only the write-lease holder takes and drops it, and the lease's
handoff orders one holder's changes before the next's (D4), so "shared" is exactly "count above
one". A debug build asserts the lease at each change.

The cost is one copy of `doc_count / 8` bytes per segment that takes a delete after a publication,
once per publication interval, and the old copy lives as long as a view holds it. The merge's own
copy of its sources' bits at scheduling (`index.hh:1420`) can share the frozen bits instead; step 1
leaves that copy as it is.

### D4. Publication is opt-in, owner-only, one pointer

**The owner is the write-lease holder (the checkout model, owner 2026-10-21).** A client checks an
index out of a pool, reads and writes during its request, and checks it in; no thread is pinned
to an index. Exactly one holder at a time has the index's write lease: today the caller of a
writer method, and with the write buffer the leader of the current write group
(`write-buffer.md` D2). Every "owner" duty in this design (the copy-on-write count of D3,
publication, the registry scan and frees of D6, merge publication, `retain()`, `reclaim()`,
close) runs under the lease, on whatever thread holds it. The lease passes from holder to holder
by a release store and an acquire load, so each holder sees what the previous wrote, and the
plain count of D3 stays exact. Debug builds assert the lease where revision 1 asserted the owner
thread. Step 1 ships the lease with today's single writer: a host that calls writer methods from
several threads serializes them through it.

- A hazard registry (D5) comes in through `index_options::readers`, beside `budget`, `storage`,
  and `background` (`index.hh:370-387`). With the lease on any thread, a registry per thread no
  longer follows ownership, so the recommended configuration is **one registry per pool
  partition**: the set of indexes a host's pool partition serves (Xapiand's shards on one core's
  pool, say), shared by those indexes. A lease holder then scans only records its partition's
  readers claim, and a leak of guards sheds reads in one partition, not the node (SR-D8). An index
  without a registry cannot make readers and pays nothing beyond a null pointer.
- `basic_index::reader()`, called under the write lease, returns a `basic_reader` handle, usable from
  any thread. The handle and the index share a heap-allocated state (the current view's atomic
  pointer, the published position and generation, the registry), so moving the index
  (`index.hh:497`) moves nothing a reader points at. Copying a handle is a shared read-modify-write
  on that state's count, so a host keeps one handle per shard and lends it by reference (SR-D12);
  acquires never touch the count.
- The handle answers `published_position()` and `generation()` without an acquire. The owner
  stores the view's pointer first, then the position and generation with release;
  `published_position()` and `generation()` load with acquire (SR-D15). A reader that reads a
  position p there and then acquires a view gets the view published with p or a newer one, by
  coherence on the pointer, so a check of the position before acquiring is a safe fast path.
- The current view is a `std::atomic<const view*>`; the owner keeps the owning pointer and a
  short list of retired views. The owner publishes with a sequentially consistent store, retires
  the old view, then frees what D6 allows.
- Every `shared_ptr` a view holds is copied when the owner builds it and dropped when the owner
  frees it, both under the lease. For that to hold of segments, the owner also clears a merge job's `sources` and its
  copies of their bits when it installs, discards, or abandons the job (`index.hh:1704-1709`,
  `1761-1819`, `1823-1838`); today the pool's closure, which captures the job
  (`index.hh:1438`, `pool_executor.hh:66-72`), can hold the last reference and destroy a source
  on a pool thread (SR-D5). A debug build asserts that an `open_segment` is destroyed under the
  write lease. No reader ever changes a count.

A view is also what `durability-modes` D1 calls the refresh generation's reader snapshot: its
segments and frozen bits are what a checkpoint of that generation encodes
(`durability-modes.md:838-843`). An owner may keep a past view out of retirement while a
checkpoint batch reads it; that design decides whether its batch entry does.

### D5. Acquire and release: claim a record by compare-exchange

A `reader_registry` holds H hazard records, each an `std::atomic<const void*>` on its own
128-byte line (Apple's M-series cache line; 64 bytes elsewhere). A record is free when it holds
null and claimed while it names a view (SR-D3).

```cpp
// Any thread. Returns a guard on the current view; throws index_closed or too_many_readers.
v = state->current.load(seq_cst);               // null after close(): index_closed
i = hint;                                        // thread-local, a start index only
for (;;) {                                       // test, then test-and-set (SR-D17)
	if (!rec[i].load(relaxed) && rec[i].compare_exchange_strong(null, v, seq_cst)) break;
	if (++i == H) i = 0;
	if (probed all H) throw too_many_readers;
}
hint = i;
for (;;) {
	w = state->current.load(seq_cst);
	if (w == v) return view_guard{&rec[i], v};
	if (!w) { rec[i].store(null, release); throw index_closed; }
	rec[i].store(w, seq_cst);                     // still ours: it never held null
	v = w;
}
// Release, from any thread: rec[i].store(null, release).
```

The owner's side, after it publishes a new view and retires the old: load every record of its
registry (seq_cst) and free each retired view no record names. In the single total order of
sequentially consistent operations, a reader whose validation read `v` wrote `v` into its record
(the claim or the retry's store) before the owner stored the newer view, so before the owner's
scan loaded that record: the scan sees `v`, or sees the reader's later release, whose release
store the scan's load synchronizes with before the free. A reader whose write came after the
scan's load validates against the newer pointer and retries. This is Michael's hazard pointers
(IEEE TPDS 15(6), 2004), the mechanism C++26 standardizes as `std::hazard_pointer` (P2530). The
review's probes confirm it: 0 free-while-read outcomes in 2,000,000 iterations at seq_cst against
30 at release/acquire on x86_64, and a clean TSan stress with cross-thread releases
(`shared-reader-design.md:39-47`). Address reuse is harmless: a reader that validates a recycled
address holds the current view, which is protected.

What the claim costs: one compare-exchange, the same full barrier the seq_cst store is (`lock
cmpxchg` against `xchg` on x86_64, `casal` against `stlr` on arm64), on a line this thread usually
just released, starting at its hint. The probe reads a record relaxed and compare-exchanges only
one that reads null (SR-D17), so passing a claimed record costs a shared read of its line, never
a locked read-modify-write that takes another reader's line exclusive. The relaxed read only
chooses where to try; the compare-exchange is the claim, so the correctness argument above is
unchanged. Release is one store from whichever thread holds the guard,
so a task resumed on another thread (D9) releases without a lock, a pool, or a cache. No thread
parks records, no thread exit or registry lifetime rule applies beyond "the registry outlives its
indexes and guards", and several registries coexist. `too_many_readers` means H records are in
use, and it is the overload error ARCHITECTURE sheds at a cap (`:180`), never a wait.

The protocol uses sequentially consistent operations and no standalone fence, because
ThreadSanitizer does not model `std::atomic_thread_fence`. TSan alone does not catch a weakened
order, though (it does not reorder stores; the review's weakened build ran clean under it), so
T8's native litmus test is the check for ordering, and T2's seeded schedule for interleavings
(SR-D4).

The registry lives in strata core (`strata/core/hazard.hh`): no library in the Kronuz catalog
provides hazard pointers or epochs (`atomic-shared-ptr` wraps `std::atomic<std::shared_ptr>`). It
moves to its own repository when a second consumer needs it.

Rejected, with the reason each loses:

1. `std::atomic<std::shared_ptr<view>>` (Kronuz/atomic-shared-ptr on libc++). Every acquire and
   release is a read-modify-write on the one control block every reader thread shares, plus the
   library's lock, which libc++ takes from a global pool. That is the shared refcount R7 rules
   out, contended on the hottest path. Lucene's `ReferenceManager.acquire` is this design
   (`ReferenceManager.java:91-110`, `tryIncRef`), and T3 measures it as a comparison arm.
2. RocksDB's thread-local `SuperVersion` cache: a `Swap` to `kSVInUse` on acquire and a
   `CompareAndSwap` on return, with the installer scraping every thread's slot to `kSVObsolete`
   (`db/column_family.cc:1388-1434`, `1491`). Each slot is per (thread, column family): for
   millions of indexes per host, T threads times the open indexes readers ever touched, each slot
   holding a refcount on a possibly stale version.
3. Per-thread caches of free records over a shared pool. Idle threads park records the node then
   lacks, and a guard released on another thread sends every claim through the pool's mutex
   (the review's `parking` and `drift` probes, SR-D3).
4. Epoch-based reclamation (one epoch word per thread). One stalled reader stops every index on
   the node from freeing any view, which is deferred reclamation of unbounded work (PLAN Goal 5,
   `docs/PLAN.md:112-119`). A hazard record holds exactly one view.
5. phase-3's per-core handles published by message (`phase-3.md:178-185`). Readers would have to
   be executors with a message loop, which Xapiand's offload pool under D13's `sync_wait` bridge
   and the benchmarks' plain threads are not; each publication would cost a message per subscribed
   core and each (core, index) pair a slot.

### D6. The owner frees views; the deferred memory is bounded

Retired views are freed only under the write lease, after a scan of its registry: at every
publication, in `commit()`, at `reclaim()`, and in `close()`. Freeing a view drops its
`shared_ptr`s, and a segment the index dropped and no other view or merge holds is destroyed
then: its mapping is unmapped there, under the write lease.

**Commit's order (SR-D6):** flush, publish, scan and free, `retain()`, `reclaim()`. Publishing
before `retain()` lets the view this commit retires, which holds the sources of the merges the
commit installed, be freed first, so with a handle and no guard out those sources are queued in
the same commit, as today. `reclaim()` that freed a view runs the data-file half of `retain()`
(the in-memory diff that queues files no kept manifest and no live view names) before it frees
files, so an idle index returns a released view's space at its next `reclaim()`, not its next
commit.

Bounds: after a scan, the retired views an index keeps are those some record names, at most H. A
view a reader releases after the scan waits for the owner's next call; an idle index frees it at
its next owner call or at `close()`. Nothing is freed by a reader, a timer, or a collector
thread. The scan reads H records, a cost per publication, never per acquire; with one registry
per pool partition (D4) it reads only that partition's records. As an estimate of the unsplit worst case
the review gives: 10^4 indexes refreshing at 1 Hz on one node-wide registry of H = 4,096 is
4 × 10^7 record loads per second; split per pool partition, each partition's loads scale with
its own indexes' publications and its own H. T3's aggregate row measures it.

**A reader that never releases (SR-D7).** The count stays bounded, the bytes do not: one guard
pins a whole generation, so a `force_merge` under it keeps the index's old segments on disk until
release, and storage admission may refuse writes with `no_space` meanwhile; it never truncates a
held file. A host finds such a guard through:

- per index, `views_held()`: the views not yet freed, and the bytes of the segments only they
  hold;
- per registry, the high-water mark of claimed records, and for each claimed record the time the
  owner's scans first saw it naming its current view. The owner stamps that time in its own
  table during the scan, so the acquire path reads no clock; it is exact to one scan.

### D7. What keeps files and mappings alive

A view holds its segments' `open_segment`s and so their mappings. `retain()` adds the segment
names of every view not yet freed (current and retired) to its referenced set, beside the pending
merges' sources (`index.hh:2621-2624`). A file a view maps is therefore never queued for
`reclaim()`, which keeps phase-3's rule that a file is truncated only after its last mapping is
released (`phase-3.md:197-198`) at `retained_predecessors` 0. Unlinking a mapped file would be
safe; truncating it faults the reader with SIGBUS, so the rule is load-bearing. faultfs aborts on
a truncate below a live mapping (`faultfs.hh:341-344`), which T1 uses as its oracle with mapping
on, beside one run on the POSIX IO, both with a 4 KiB `reclaim_step` so every queued file is
truncated.

The files a view holds stay counted by storage admission until the view is freed and a later
`retain()` queues them, as a merge's sources do; at most H views per registry can hold any.
RocksDB pins files the same way through `Version` refs (`db/version_set.h:1078-1081`,
`db/version_edit.h:274`), and Lucene through the deleter's refs on an NRT reader's segments
(`IndexWriter.java:6012`, `incRefDeleter`; `StandardDirectoryReader.java:149`).

With durability-modes, `retain()` keeps four sets: the durable head (DM-R23), the recorded
generation (DM-R43), the current state, and the live views. They compose by union; none of their
rules changes.

### D8. Reads on a view, and one read implementation

`view_guard` (and the view it points at) offers the const reads:

- `get(key, f)` and `get(key)`: the segments newest first, each through the key range, the
  filter, and the dictionary with the view's frozen bits, the loop `index.hh:972-980` runs today;
- `scan(from[, to][, n], f)`: the k-way merge of `scan_keys` (`index.hh:1134-1236`) over the
  view's segments, with no buffer runs and no `scanning_` count, so any number of threads scan one
  view at once and `f` may call the writer, which cannot change the view;
- `segments()`: the `segment_view`s (`index.hh:755-761`), whose `deleted` points at frozen bits
  and whose id and dead count are the view's copies; prospect builds its `segment_snapshot` span
  from them (`searcher.hh:26-30`) and its statistics from the view (`searcher.hh:94-101`);
- `metadata()`, `last_docid()`, `schema_version()`, `generation()`, `position()`, and `digest()`,
  which over a view equals the index's digest at the moment of publication (the buffer is empty
  then), the check T2 uses.

The index's own `get` and `scan` call the same segment code with the buffer as one more source;
one implementation serves both (PLAN's rule, one implementation per mechanism). A cursor that
survives writes, the scan "with snapshot handles" `index.hh:1000-1001` promises, is a scan over a
view.

### D9. Views and the async seam

A `view_guard` is a movable value, not bound to a thread. A `task::task<T>` holds it in its frame
across suspension (residency, segment and window boundaries, ARCHITECTURE `:173-174`) and may
resume and release it on another thread; destroying a cancelled task's frame releases it. Acquire
never suspends and never blocks: it loops only when the owner published between its load and its
validation, or while it probes for a free record.

**Refresh before a search, coalesced (SR-D9).** xapiand-on-strata D7 already says strata refreshes
"before a search when the shard has unpublished writes" (`xapiand-on-strata.md:192-195`). Under
D13 the pattern is:

```
search task (pool thread)                     owner
  p = last acknowledged position
  if handle.published_position() < p:
      await owner.publish_through(p)  ──────▶ publish_through(p):
                                                if published_position() >= p: done
                                                else refresh(); done
  guard = handle.acquire()  (sees p or later)
```

`publish_through(p)` is an owner call (under the lease) that refreshes only when the current view
is behind `p`. The fast path's check is safe because the position is stored after the view and
loaded with acquire (D4, SR-D15); the guard's own `position()` is at or past the one read. The owner runs posted calls in order, so the first behind-`p` request flushes and publishes,
and every request queued behind it finds the view at or past its position and returns without a
flush: one refresh serves every search waiting on it. The task's completion orders the
publication before the acquire. A host that wants larger batches delays the first request by a
minimum refresh interval; that is the host's knob, not strata's.

Without coalescing, interleaved indexing and search would flush once per search; T5 adds a
refresh-on-demand variant beside the 1 s cadence, and the lead carries a cost row (refreshes and
segments per second under 7a's mixed load) to xapiand-on-strata (C2).

**Sizing H (SR-D8).** A Xapiand query holds one guard per local shard it searches
(xapiand-on-strata D7, "several local shards are one snapshot", `:195-196`). So H per partition
registry covers the owner's concurrent queries times the shards each searches, plus the
checkpoint batches that hold views (D4). ARCHITECTURE caps only demoted tasks (`:179-180`), so
the host states its query concurrency; T4 reports the bytes per record.

### D10. Costs, stated as estimates until T3 and T4 measure them

| What | Cost (estimate) | Measured by |
|---|---|---|
| Acquire and release | one seq_cst load, one seq_cst compare-exchange on a record line, one seq_cst load to validate, and one release store; no read-modify-write on a line another reader holds | T3, including release on another thread |
| A read on a view | the lease holder's read cost; the view adds one indirection per segment | T3 |
| Publication (owner) | one view allocation, about 150 bytes plus 48 per segment; one bitmap clone per segment that took a delete since the last publication; a scan of the owner's H records | T3 |
| An index without a registry | one null pointer in `basic_index`; 8 bytes per segment for D3's count, in the bitmap's existing allocation | T4 |
| An index with a reader handle | the shared state (about 80 bytes) and its current view; one retired view per guard in flight on it, at most H | T4 |
| A registry | H records times 128 bytes; H = 256 is 32 KiB | T4 |

For the many-indexes workload (millions of indexes per host, tens of thousands open), the cost is
per index only where a reader exists, and per pool partition, not per (thread, index), everywhere
else.

### D11. Close, destruction, move-assignment, install, heal

- **`close()` does not block (SR-D11, Q2).** It stores null into the current pointer, so new
  acquires throw `index_closed`, scans the registry, and frees every view no record names. If
  views remain protected, it keeps them, the files they hold, and the directory lock (so a reopen
  cannot truncate those files as orphans), and reports `closing()`. A closing index accepts only
  `try_finish_close()`, `closing()`, and `views_held()`; every other call throws as a fenced index
  does today (`index.hh:516`, `live()`). `try_finish_close()` frees the rest once released and then
  releases the lock (the re-check's note on SR-D11). Past an optional
  deadline, `try_finish_close()` throws `readers_active` naming the records' claim times (D6), so
  a host learns of a leak. Waiting inside `close()` is rejected: it blocks the owner's thread,
  which ARCHITECTURE forbids for a host reactor (`:6`), and deadlocks when a release needs that
  thread, as a task suspended on the owner's own executor while it holds a guard.
- **Destruction and move-assignment follow `close()` (SR-D2).** `~basic_index()` and assigning
  onto an index that has views run `close()`'s first half. A finished close is the host's duty; if
  a view is still protected at destruction, a debug build aborts, and a release build leaks that
  view, its segments, and the directory lock, never freeing them, so no reader faults and no reopen
  in this process truncates their files. Background-merges R4 made destruction wait for merges
  (`reviews/background-merges.md:178`); a guard can outlive any wait the owner could afford, so
  destruction does not wait for readers.
- A moved index keeps its shared state (D4); handles follow it.
- `install()` publishes the installed content; earlier views keep the old segments until freed,
  and `retain()` skips their files meanwhile (D7).
- `heal()` replaces a segment object only when it failed `verify_all()` or its consistency check
  (`index.hh:711-745`). Readers on older views keep the failing object and meet `corrupt_segment`,
  a typed error from memory-safe decoders, until they release; the next publishable point carries
  the re-read object (SR-D10, D1).

### D12. Views with the write buffer

`docs/design/write-buffer.md` (branch `design/write-modes`, revision 0, same day) is the owner's one
buffer for frozen views, live readers, and concurrent writers. When it lands, this design changes
in these places and nowhere else; step 1 ships as written before it.

| Here | Becomes | Write buffer |
|---|---|---|
| D1, a view is the state at a flush; the publishable rule | A view holds the active buffer and the frozen buffers beside the segments; positions and views are published only between write groups, which are transaction boundaries, so no view holds part of a transaction | D5, D8 |
| D1, SR-D16's "anything changed" | Only a change of components (a freeze, a flush's install, a merge, `heal()`, `install()`, metadata or schema) publishes a new view object; a write that changes only buffered records advances the visible position alone, so the registry scan runs per freeze or install, never per write | D5 |
| D2, readers see refreshed state | Readers load the visible position V with acquire, acquire a view, and read at V: key-value reads see every acknowledged write with no refresh; search reads at the last freeze | D5, D11 |
| D3, bits set by the owner's deletes | Deletes of flushed documents are shadowed by the buffer's newer record and applied to the copy-on-write bits at the freeze, never in place between freezes | D6, D8 |
| D4, SR-D15's store order | Unchanged, and V is the published position: the lease holder publishes any view a position needs before the position | D5 |
| D4, the owner | The write-group leader holds the lease; chores without writes run as an empty group | D2 |
| D6, what the scan frees | Also map tables and sorted runs the buffer replaced, each retired at a generation and freed when no record names a view at or below it | D10 |
| D9, `publish_through(p)` | For key-value reads, V ≥ p, with nothing to do; for search, a coalesced freeze through p instead of a refresh | D11 |
| Q1 | Answered by the buffer: readers see live writes | |

## Gate rows

Performance rows are measured by the lead on `main`, under a strict hold, in minutes-long runs
(BENCHMARKS tiers); the branch makes functional claims only.

| Row | Target | Procedure |
|---|---|---|
| **T1** stress under sanitizers | Zero reports under TSan, and under ASan and UBSan, on Linux x86_64, macOS arm64, and FreeBSD; retired views per index never above H (an assertion); no truncation under a mapping | `shared_reader_stress` (step 1): 1 writer and N = 1, 4, 16 reader threads on one index of each layout (`document_layout` in strata, prospect's search layout in prospect, step 2). The writer applies puts, replaces, deletes by key and by docid, rejected transactions, and metadata, calls `refresh()` every k transactions (k random in 1 to 64), commits every 10 refreshes, runs background merges on `pool_executor` with `auto_merge`, `force_merge`, `heal()`, a checkpoint and `install()`, and closes at the end, then destroys the index and move-assigns onto one, each with guards outstanding (D11's rules). Readers loop acquire, then a get, a 10-key scan, or (prospect) a top-10 search; some hold guards across a random pause and release them on another thread. `reclaim_step` 4 KiB, on faultfs with mapping on (its truncate abort, `faultfs.hh:341-344`, is the oracle) and once on the POSIX IO. At least 10^7 reader operations, 10^4 publications, 10^2 merges installed, per platform; the seeds and counts are printed |
| **T2** differential | Every read equals the model at the view's generation; no transaction seen in part; generations never decrease per reader | The model is a `std::map` of key to (value, version), snapshotted by the writer at each publication under the view's generation. (a) Deterministic: on `inline_executor`, one thread runs a seeded schedule whose steps are the protocol's atomic steps (the reader's load, claim, validate, retry store, release; the owner's publish, retire, scan, free), interleaved with owner calls, 1,000 seeds, replayable; the schedule creates the first handle and calls `heal()` while a replace is pending (SR-D1); it runs D9's fast path (read `published_position()`, then acquire) between the owner's view and position stores (SR-D15); and it applies a delete-only transaction on a flushed document and a metadata-only transaction, each followed by `refresh()`, which the model must see at that generation (SR-D16). Handoffs of the write lease between two threads are steps of the schedule. (b) Threaded: T1's readers log (generation, operation, result), checked against the model after the run. Transactions move value between key pairs whose sum is invariant, so a torn view shows as a wrong sum. `view.digest()` equals `index.digest()` taken at that publication. Mutants, each with the test that kills it: validation skipped (T2a, TSan in T1); the scan before the publish store (T2a); a retired view freed without the scan (T2a, ASan in T1); the bitmap clone skipped, on delete and on the rollback path (T2a, T2b); views left out of `retain()` (T1's faultfs abort); the publishable predicate removed (T2a's pending replace); weakened orders (T8); the position stored before the view pointer (T2a's fast-path case, SR-D15); publication only when documents flushed (T2a's delete-only and metadata-only cases, SR-D16) |
| **T3** per-acquire and publication cost | Acquire, release, and a hot-cache get on a view at 16 threads, one per physical core, within 1.1× of the same at 1 thread; acquire and release add at most 5% to that get; the cross-thread-release arm within 1.1× of same-thread release; publication time and the scan reported at 10, 100, 1,000 segments and H = 64, 256, 1,024 | `bench/shared_reader/acquire.cc`: one index, 1, 2, 4, 8, 16 threads; the same across 1,000 indexes (threads pick an index at random); an arm that releases each guard on another thread. **Aggregate row (SR-D8):** 10^4 open indexes publishing at 1 Hz, with one registry per pool partition (8 partitions, indexes spread evenly) and H sized as D9 states for Xapiand, against one node-wide registry; reports the lease holders' CPU per second spent scanning. The probe's cost under collisions (SR-D17): every thread starting at one hint, against spread hints. Comparison arm, reported, not gated: `std::atomic<std::shared_ptr>` on the same loop. Linux x86_64, Linux arm64, macOS arm64, FreeBSD, once each on the landing commit |
| **T4** memory | An index without a registry: the same allocation count as today and at most 8 bytes per segment plus one pointer over today; with a handle and an idle view: the bytes reported per segment count, target the D10 estimate; a registry's bytes per H; the clone bytes per publication under T5's overwrite load | A test with the counting allocator over 1, 3, 10, 100 segments, readerless and with a handle; `sizeof` checks; the clone bytes from a counter in the stress run |
| **T5** `readwhilewriting` | At N = 1, 4, 8, 16 readers: read throughput ≥ 1.0× RocksDB's and read p99 ≤ 1.0×, at fits and 2× data (the key-value ratchet's shape, PLAN "The `document` profile gate") | `bench/kv`: N reader threads share one strata index through one `basic_reader` lent by reference (RocksDB: N threads on one DB), one writer at 2 MB/s of logical bytes (db_bench's `MB_WRITE_PER_SEC`), `refresh()` every 1 s on the writer thread; and a refresh-on-demand variant where every read first waits for `publish_through` the writer's last position (D9). Reports reads/s, p50 and p99 per reader, the writer's achieved rate, `refresh()` p99, and refreshes and segments per second. States the asymmetry: RocksDB's readers see each write at once, strata's within one refresh interval in the cadence variant (D2, Q1) |
| **T6** `fwdrangewhilewriting` | Same targets, on seek plus 10 nexts | T5's run with `view.scan(from, 10, f)` against RocksDB's iterator `Seek` and 10 `Next` |
| **T7** NRT reopen | `refresh()` p99 (flush plus publication) ≤ 1.0× Lucene's reopen p99, and search QPS beside indexing ≥ 1.0× Lucene's | prospect's bench: luceneutil's nightly NRT shape (`nightlyBench.py:106-110`, as `benchmark-coverage.md:144` cites it): 1 indexing thread at 1,103 docs/s, one refresh per second, 4 search threads, scaled to a few minutes; Lucene with `SearcherManager.maybeRefresh` once per second (`nrtPerf.py`) |
| **T8** ordering litmus (SR-D4) | Zero free-while-read outcomes in at least 10^7 iterations natively on x86_64 in CI; the weakened-order mutant fails it | `hazard_litmus` (step 1), unsanitized: `hazard.hh`'s own claim, retry store, and validate against publish and scan, the store-load pair of the review's `sb_litmus.cc`; the mutant builds the record write as release and the validating load as acquire (the review saw about 1.5 × 10^-5 failures per iteration at that order). Also run on macOS arm64, where `-mcpu=apple-m1` compiles acquire to `ldapr`, which allows the reorder |
| **T9** stuck reader (SR-D7) | Retired views stay at or below H; no held file truncated; with a short storage budget, writes get `no_space` while the guard is held; space returns at the first commit after release; `views_held()` and the record's claim time name the guard | One guard held while the writer merges, commits, and reclaims for 100 rounds, then released |

T1 in strata and in prospect is the strata side of xapiand-on-strata's T10 and of phase-3's owed
"TSan over ... snapshot publication" (`phase-3.md:524`).

## Questions for the owner

**Q1 (answered: (a) for step 1; superseded by D12 when the write buffer lands). Do readers on
other threads need writes that are not yet refreshed?** The owner chose (a) for this design's
step 1 (recorded 2026-10-21). The owner's later direction the same day, one write buffer whose
readers see live writes (`write-buffer.md`), replaces D2 when that buffer lands (D12); step 1
does not wait for it. The question as revision 1 put it: RocksDB's readers see
every write as it lands; Lucene's see the last reopen; Xapian's see the last commit.
Options: (a) the last refresh only (D2), with D9's coalesced `publish_through` for a search that
must see its acknowledged writes; (b) a frozen, immutable copy of the buffer at publication, as
RocksDB's immutable memtables, with its own key index and live bits, and the writer's version map
extended to find documents in it; (c) at publication, encode the buffer into an in-memory segment
the view holds, written to its file later, so every reader path stays the segment reader's.
Recommendation: (a). It is what ARCHITECTURE item 9 already promises, the refresh interval and
`publish_through` are the knobs, and T5's two variants price it. If T5 or Xapiand needs more, (c)
is the follow-up, since it adds no second reader path; (b) adds a buffer search to prospect's
inverting layout.

**Q2 (answered: the non-blocking close). How does `close()` treat reads in flight?** Revision 0
recommended waiting; SR-D11 showed it blocks the owner and can deadlock. The owner chose the
architect's recommendation (recorded 2026-10-21): a non-blocking close that keeps held views, their
files, and the lock until the last release, then finishes in `try_finish_close()` (D11). D11's
destructor rule follows it.

## First buildable step

Step 1 is strata core only, on `phase4/shared-reader`. The re-check cleared items 1, 2, and 4 to
start now and items 3 and 5 once SR-D15 and SR-D16 are in the text (C6), which revision 2 does; the
code review confirms them at file:line. The write lease of D4 (with today's single writer) is new
in revision 2 and is part of the architect's review of the write buffer; items 3 and 5 build on
it. Q1's answer is today's behavior, so nothing here waits on the write buffer.

1. `include/strata/core/hazard.hh`: `reader_registry` (H records on their own lines, claim by
   compare-exchange from a thread-local hint, release by one store, the owner's scan with its
   claim-time table and high-water mark); tests of claim and release, the review's `parking` and
   `drift` cases (no parking, no mutex), T8's litmus with its mutant; TSan, ASan, UBSan.
2. D3's copy-on-write live bits in `open_segment`, every writer of `deleted` through one accessor,
   the write-lease debug assertion; merge jobs' sources and bit copies cleared by the owner at
   install, discard, and abandon, with the `open_segment` destruction assertion (SR-D5).
3. `basic_view<Layout>`, `basic_reader` (with `published_position()` and `generation()`),
   `view_guard`; `index_options::readers`; `basic_index::reader()`; the publishable predicate at
   every point D1's table names; retirement, the scan, and `views_held()` (D6).
4. D8's reads: the segment part of `get` and `scan_keys` factored so the index and the view call
   one implementation; the comment at `index.hh:1003-1006` corrected.
5. `retain()` referencing every unfreed view's segments (D7); commit's order and `reclaim()`'s
   data-file pass (D6); D11's non-blocking `close()`, `try_finish_close()`, the destructor, and
   move-assignment; `publish_through(p)` (D9).
6. Tests: T1, T2, and T9 on `document_layout` with every mutant T2 names; `install()`, `heal()`,
   `close()`, destruction, and move-assignment under live guards; retention timing with an idle
   handle equal to today's; T4's allocation test. CI green on every platform.

Then, each after the one before lands:

2. prospect: search over `view.segments()` and the view's statistics; T1 and T2 with search;
   any per-segment cache prospect adds (the filter bitsets, `ARCHITECTURE.md:125`) is per view or
   safe under concurrent views.
3. `bench/kv`: a threaded mode with a shared index, a rate-limited writer, and the
   refresh-on-demand variant (T5, T6), and `bench/shared_reader` with the aggregate row (T3);
   prospect's NRT driver (T7). The lead measures.
4. durability-modes step 5 may build its batch entry on a kept view (D4); that design decides.

## Risks

- **A long read holds disk.** A search or scan that runs for minutes keeps every segment its view
  holds, so merges free no space under it, and storage admission can refuse writes meanwhile
  (D6, D7, T9). RocksDB's iterators and Lucene's NRT readers do the same. The host bounds read
  durations and watches `views_held()` and the claim times.
- **H too small sheds reads.** `too_many_readers` under a burst is an overload error, not a wait.
  D9 sizes H from query concurrency times shard fan-out; the registry reports its high-water mark.
- **Publication on every flush.** An index flushing many times a second publishes as often, and
  each publication scans its owner's H records. T3's aggregate row states that cost; a point that
  changed nothing publishes nothing (D1).

## Changes in revision 1

| Finding | Change |
|---|---|
| SR-D1 | D1: one publishable predicate at every publication point, in a table; `reader()` refreshes first; `heal()` waits for a publishable point; T2(a) creates the first handle and heals with a pending replace; its mutant |
| SR-D2 | D11: the destructor and move-assignment follow `close()`; a protected view at destruction aborts in debug and leaks (with its files and lock) in release; T1 destroys and move-assigns with guards out |
| SR-D3 | D5: records claimed by a seq_cst compare-exchange from null to the view at a thread-local hint; release is one store of null from any thread; caches and pool removed (rejected alternative 3) |
| SR-D4 | T8, a native litmus with a weakened-order mutant; T2(a)'s schedule split into the protocol's atomic steps; T2's mutant list, each with its killer |
| SR-D5 | D4: the owner clears a merge job's sources and bit copies; a debug assertion that `open_segment` dies on the owner thread |
| SR-D6 | D6: commit's order (flush, publish, scan and free, `retain()`, `reclaim()`); `reclaim()` runs `retain()`'s data-file pass after freeing a view; D7 and T1: faultfs with mapping on, plus the POSIX IO |
| SR-D7 | D6: `views_held()`, the registry's high-water mark and claim times stamped by the owner's scan; T9 |
| SR-D8 | D4: one registry per owner thread; D1: no publication when nothing changed; D9: H sized by shard fan-out; T3's aggregate row |
| SR-D9 | D9: `publish_through(p)`, coalesced by construction; the handle's `published_position()` and `generation()`; T5's refresh-on-demand variant; C2 for the lead |
| SR-D10 | D1: a view reads only `seg` and its mapping and copies id and dead count; D11: `heal()`'s older views meet `corrupt_segment` until they release |
| SR-D11 | D11 and Q2: the non-blocking close |
| SR-D12 | D3: the count in the bitmap's allocation, 8 bytes per segment; T4 restated; D4: one handle per shard lent by reference; T3: one thread per physical core and a cross-thread-release arm |
| SR-D13 | Recorded in the table above; the lead's edits on landing (C5) |
| SR-D14 | Done criteria K1 to K8 in `docs/status/shared-reader.md`, from the review's C1 |

## Changes in revision 2

| Finding or input | Change |
|---|---|
| SR-D15 | D4: the view pointer is stored first, then the position and generation with release; `published_position()` and `generation()` load with acquire; D9's fast path states why it is safe; T2(a)'s fast-path case and the swapped-order mutant |
| SR-D16 | D1: publish when anything changed since the last view (the applied position moved, segments replaced by a merge or `heal()`, `install()`), never keyed on documents flushed; "publishable" tested after the flush call; T2(a)'s delete-only and metadata-only cases and their mutant |
| SR-D17 | D5: the probe reads a record relaxed and compare-exchanges only a null one; T3's aggregate row adds colliding hints |
| SR-D11 (re-check note) | D11: a closing index accepts only `try_finish_close()`, `closing()`, and `views_held()` |
| The checkout model (owner, 2026-10-21) | D4: "the owner" is the write-lease holder on any thread; the lease's handoff orders holders; every owner duty and debug assertion moves under the lease; one registry per pool partition instead of per owner thread (D4, D6, D9, D10, T3) |
| Q1, Q2 (owner's answers) | Recorded: (a) for step 1, superseded by D12 when the write buffer lands; the non-blocking close |
| The write buffer (owner, 2026-10-21) | D12: the table of what changes when it lands |
| Housekeeping | The ledger's State, Blocked on, and Done lines; the re-check committed as the architect wrote it (`f39dfc6`) |
