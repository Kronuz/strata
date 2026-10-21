# The shared reader: snapshots beside the writer

Revision 0, 2026-10-21. State: designing (ready for preliminary review). Concurrency (lifetimes
across threads), so the architect reviews this design before anything is built (PLAN's review
tiers, the "Durability, concurrency, security surface" row). Decisions D1 to D11, gate rows T1 to
T7, owner questions Q1 and Q2. Code is cited at strata `main` `75143f9`, prospect `main`
`59f36fe`, and `design/durability-modes` at `95357d9`. RocksDB 11.11 and Lucene at `9983b7c`
(both Apache 2.0) were read for their mechanisms; no code of theirs enters strata.

The owner pulled this forward from Phase 3R (2026-10-21, `docs/perf/benchmark-coverage.md:245`).
It unblocks the threaded key-value rows (RocksDB's `readwhilewriting` and `fwdrangewhilewriting`,
`benchmark-coverage.md:93-94`), the NRT reopen row (`:144`), and the 7a concurrency row
(`docs/design/xapiand-on-strata.md:290`, T10), because Xapiand searches a shard while it indexes.

## Summary

A reader on any thread acquires a **view**: an immutable record of the index as of its last
flush (segments, frozen live-docs bits, metadata, statistics, the applied position). The owner
thread builds a view at every flush point and publishes it through one atomic pointer. A reader
protects the view with a **hazard record** (one slot from a node-wide registry), so an acquire
writes only memory its own thread owns: no shared refcount, no lock, no per-(thread, index) slot.
The owner frees a retired view, and with it any segment mapping only that view held, after it
scans the registry and finds no record naming the view. That scan runs on the owner thread at its
own publish, commit, reclaim, and close, so no thread collects. Retention treats a segment a live
view holds like a pending merge's source: never queued, so never truncated under a mapping.

Readers see refreshed state only; the unflushed buffer stays the owner's (Lucene's NRT model,
and what ARCHITECTURE item 9 already says refresh does). Owner-thread `get` and `scan` keep
reading the buffer, so the writer still reads its own writes.

## What this builds on, and what it changes

| Document | What this design uses | What it changes |
|---|---|---|
| `docs/ARCHITECTURE.md` | Refresh publishes a snapshot, near real time, 1 s default (`:108-109`); memory ownership: a concurrent structure that defers a free bounds the deferred memory and the work, and does it on its owner's thread (`:188-192`); overload shed at a cap (`:180`) | "Read path" (`:156`): "the per-core handle" becomes "a view" (D5) |
| `docs/design/phase-3.md` | Retention at default 0 predecessors (`:157-171`, D4/I14); a running merge's sources stay referenced (`:172-173`); truncate only after the last mapping is released (`:197-198`); `visible_position()` (`:436`); T6's "TSan over ... snapshot publication" (`:524`) | Replaces the per-core snapshot handles published by message and the owner's per-segment core count with release messages (`:178-185`, R7's example mechanism) by D5 and D6. R7's requirements hold: one owner unmaps, it learns by its own scan, no refcount is shared on any read path, no collector. The architect rules whether that satisfies R7 |
| `docs/reviews/phase-3-design.md` R7 (`:707-717`) | The requirement itself | See above |
| `docs/design/core-split.md` | The core's `get` and `scan` (`:80-85`) | "The pulled cursor waits for snapshot handles" (`:84-85`): a view's scan is that cursor's home |
| `include/strata/core/index.hh` | `get` (`:954-981`), `scan`/`scan_keys` (`:990-1236`), `segments()` (`:755-767`), `retain()` (`:2592-2655`), `reclaim()` (`:636-682`), `publish_merges` and `install_merge` (`:1699-1819`) | D3 (copy-on-write live bits), D4 (publication), D7 (`retain()`), D8 (one read implementation for the index and its views), and the comment at `:1003-1006` (below) |
| prospect `docs/ARCHITECTURE.md:110-111`; `include/prospect/searcher.hh:26-30` | A search reads a span of `segment_snapshot` (segment, deleted bits, dead count) | Its snapshot comes from a view (step 2); "published to each core by message" becomes D5 |
| prospect `docs/gates/phase-3-ratchets.md:361`, `docs/testing/parity.md:58` | "reads while a writer commits need a shared-ownership reader" | Closed by T1 and T2 once prospect's step 2 lands |
| `design/durability-modes` D1 (DM-R3, DM-R23, DM-R43) | A checkpoint records a past refresh generation, "live-docs files encoded from that refresh's deleted bitmaps (which its reader snapshot holds)" (`durability-modes.md:834-843`); `retain()` keeps the durable head, the recorded generation, and the current state (`:844-875`) | Nothing in its rulings. A view is that reader snapshot; `retain()` gains a fourth set, the segments live views hold (D7). Whether its batch entry holds a view is that design's call (D4) |
| `docs/design/xapiand-on-strata.md` D13, T10 | Seam operations return `task::task<T>` (`:224-236`); T10's TSan row (`:290`; its `index.hh:685` is a stale pin) | Nothing; D9 says how a view composes with the seam |
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
| The buffer (`buffer_`, `active_`, `pending_docs_`), its key map `key_slots_`, and the scan's sorted view `view_` | every apply; flush clears them; a scan rebuilds `view_` on demand | `:2971-2978`, `:1096-1131` |
| Metadata `meta_` and the manifest `m_` | apply, commit | `:2979`, `:2966` |

What is already safe for concurrent readers: a segment's bytes and its decoded structures are
immutable after open (`open_segment::seg`, `index.hh:1262`); the verified-region state is one
atomic per region, and two threads verifying one region at once is harmless
(`residency/residency.hh:155-157`); a get's decode buffer is per thread (`index.hh:279-312`).

## Decisions

### D1. A view is an immutable published generation, taken only at a flush point

A `basic_view<Layout>` holds, for one generation:

- per segment: a `std::shared_ptr<const open_segment>` (the reader reads only `seg` and the
  mapping, which never change after construction), its frozen live-docs bits
  (`std::shared_ptr<const bits>`, null when none is deleted, D3), and its dead count;
- the metadata, as a `std::shared_ptr<const std::map<...>>` shared until the owner changes it;
- the fields readers need from the manifest: `last_docid`, schema version, key prefix,
  configured structures, and the scoring statistics prospect computes from the segments
  (`searcher.hh:94-101`);
- its generation (a counter that increases at every publication) and its position, the applied
  position the view reflects, which is phase-3's `visible_position()` (`phase-3.md:436`).

A view is built whole by the owner, then published, and never written again. A reader touches no
member of `basic_index`.

Views are taken only where the buffer was just flushed and every applied transaction is complete:
the end of `refresh()`, of `commit()`, and of `install()`. Every flush runs at a transaction
boundary (apply flushes before stage 1, `index.hh:527`; the other flushes are `refresh()` and
`commit()`, `:567-571`, `:624-632`), so every view is a state some prefix of the transactions
produced, and a transaction is visible whole or not at all. A merge installed between refreshes
(`wait_merges()`, `merge()`, `force_merge()`) publishes a view only when nothing was applied since
the last view, since the live bits then would hold deletes whose replacements sit in the buffer,
and a replace would look like a delete. `heal()` publishes when it replaced a segment object.

### D2. Readers see refreshed state; the buffer stays the owner's

A view holds no buffered document. A write becomes visible to other threads at the next flush
point, which the host paces with `refresh()` (ARCHITECTURE item 9, 1 s by default). This is
Lucene's NRT model: `IndexWriter.getReader` flushes the in-memory buffer before it opens the
reader (`IndexWriter.java:508-581`, "flush at getReader", `flushAllThreads`). RocksDB differs: its
readers search the mutable memtable through a concurrent skip list
(`memtable/inlineskiplist.h:10-36`). Q1 asks whether shared readers need that.

Owner-thread reads are unchanged: `basic_index::get` and `scan` still read the buffer first
(`index.hh:965-981`, `1134-1236`). A host that needs a read of its own unrefreshed write (Xapiand's
real-time get) runs it on the owner, under D13's seam a task posted to the owner's executor.

### D3. Live-docs bits are copied on write

`open_segment::deleted` becomes a shared, copy-on-write bitmap. Publication marks each segment's
current bits shared; the owner's next delete in that segment (`index.hh:2028`) clones them first
and writes the clone, and the view keeps the old ones. Rollback (`:1997`) then edits the clone,
never a view's bits. This is Lucene's `PendingDeletes.getMutableBits` (`PendingDeletes.java:63-80`:
"Copy on write ... must now make a private clone").

The cost is one copy of `doc_count / 8` bytes per segment that takes a delete after a publication,
once per publication interval, and the old copy lives as long as a view holds it. The merge's own
copy of its sources' bits at scheduling (`index.hh:1420`) can share the frozen bits instead; step 1
leaves that copy as it is.

### D4. Publication is opt-in, owner-only, one pointer

- The node's hazard registry (D5) comes in through `index_options::readers`, beside `budget`,
  `storage`, and `background` (`index.hh:370-387`). An index without it cannot make readers and
  pays nothing beyond a null pointer.
- `basic_index::reader()`, called on the owner thread, returns a `basic_reader` handle, copyable
  and usable from any thread. The handle and the index share a heap-allocated state (the current
  view's atomic pointer and the registry), so moving the index (`index.hh:497`) moves nothing a
  reader points at. The first `reader()` publishes the current flushed state; from then on the
  owner publishes at every point D1 names. Handle copies touch the state's refcount; acquires do
  not.
- The current view is a `std::atomic<const view*>`; the owner keeps the owning pointer and a
  short list of retired views. The owner publishes with a sequentially consistent store, retires
  the old view, then frees what D6 allows.
- Every `shared_ptr` a view holds is copied when the owner builds it and dropped when the owner
  frees it, so those refcounts are touched by the owner thread alone. No reader ever changes a
  count.

A view is also what `durability-modes` D1 calls the refresh generation's reader snapshot: its
segments and frozen bits are what a checkpoint of that generation encodes
(`durability-modes.md:838-843`). An owner may keep a past view out of retirement while a
checkpoint batch reads it; that design decides whether its batch entry does.

### D5. Acquire and release: a hazard record, never a shared write

A node-wide `reader_registry` holds H hazard records, each an `std::atomic<const void*>` on its
own 128-byte line (Apple's M-series cache line; 64 bytes elsewhere). H is the host's, set once,
like the budgets.

```cpp
// Any thread. Returns a guard holding the view, or throws too_many_readers.
rec = registry.claim();                       // from this thread's cache of free records
for (;;) {
	v = state->current.load(seq_cst);         // null after close: throws index_closed
	rec->ptr.store(v, seq_cst);
	if (state->current.load(seq_cst) == v) { return view_guard{rec, v}; }
}
// Release, any thread: rec->ptr.store(nullptr, release), the record back to this thread's cache.
```

The owner's side, after it publishes a new view and retires the old: load every record (seq_cst)
and free each retired view no record names. In the single total order of sequentially consistent
operations, a reader whose validation read `v` stored `v` into its record before the owner stored
the newer view, so before the owner's scan loaded that record: the scan sees `v`, or sees the
reader's later release, whose release store the scan's load synchronizes with before the free. A
reader whose store came after the scan's load validates against the newer pointer and retries.
This is Michael's hazard pointers (IEEE TPDS 15(6), 2004), the mechanism C++26 standardizes as
`std::hazard_pointer` (P2530). Address reuse is harmless here: a reader that validates a recycled
address holds the current view, which is protected.

The protocol uses sequentially consistent operations and no standalone fence, because
ThreadSanitizer does not model `std::atomic_thread_fence`, and T1 must run clean under it. The
fast path is a thread-local record cache, one store, two loads, and a compare; release is one
store. Nothing writes a line another thread writes, except a record released on a thread other
than the one that claimed it (a coroutine resumed elsewhere, D9), which returns the record to the
releasing thread's cache. Caches above a bound return records to the registry's pool under a
mutex, which the fast path never takes.

The registry is fixed at H records, so at most H views are protected node-wide. When none is free,
`acquire()` throws `too_many_readers`, the overload error ARCHITECTURE sheds at a cap (`:180`),
never a wait. The registry lives in strata core (`strata/core/hazard.hh`): no library in the
Kronuz catalog provides hazard pointers or epochs (`atomic-shared-ptr` wraps
`std::atomic<std::shared_ptr>`). It moves to its own repository when a second consumer needs it.

Rejected, with the reason each loses:

1. `std::atomic<std::shared_ptr<view>>` (Kronuz/atomic-shared-ptr on libc++). Every acquire and
   release is a read-modify-write on the one control block every reader thread shares, plus the
   library's lock, which libc++ takes from a global pool. That is the shared refcount R7 rules
   out, contended on the hottest path. Lucene's `ReferenceManager.acquire` is this design
   (`ReferenceManager.java:91-110`, `tryIncRef`), and T3 measures it as a comparison arm.
2. RocksDB's thread-local `SuperVersion` cache: a `Swap` to `kSVInUse` on acquire and a
   `CompareAndSwap` on return, with the installer scraping every thread's slot to `kSVObsolete`
   (`db/column_family.cc:1388-1434`, `1491`). The acquire is as cheap as D5's, but each slot is per
   (thread, column family): for millions of indexes per host, T threads times the open indexes
   readers ever touched, each slot holding a refcount on a possibly stale version. D5's state is
   per thread and per node.
3. Epoch-based reclamation (one epoch word per thread). Cheaper per acquire, but one stalled
   reader stops every index on the node from freeing any view, which is deferred reclamation of
   unbounded work (PLAN Goal 5, `docs/PLAN.md:112-119`). A hazard record holds exactly one view.
4. phase-3's per-core handles published by message (`phase-3.md:178-185`). Readers would have to
   be executors with a message loop, which Xapiand's offload pool under D13's `sync_wait` bridge
   and the benchmarks' plain threads are not; each publication would cost a message per subscribed
   core and each (core, index) pair a slot. A reactor host can still keep a view per core on top
   of D5 by holding a guard per core and index.

### D6. The owner frees views; the deferred memory is bounded

Retired views are freed only on the owner thread, after a scan of the registry: at every
publication, at `commit()` before `retain()`, at `reclaim()`, and at `close()`. Freeing a view
drops its `shared_ptr`s, and a segment the index dropped and no other view or merge holds is
destroyed then: its mapping is unmapped there, on the owner thread.

Bounds: after a scan, the retired views an index keeps are those some record names, at most H.
A view a reader releases after the scan waits for the owner's next call; an idle index frees it at
its next owner call or at `close()`, which small-indexes' idle closing reaches. Nothing is freed
by a reader, a timer, or a collector thread. The scan reads H records, a cost per publication
(T3), never per acquire.

### D7. What keeps files and mappings alive

A view holds its segments' `open_segment`s and so their mappings. `retain()` adds the segment
names of every view not yet freed (current and retired) to its referenced set, beside the pending
merges' sources (`index.hh:2621-2624`). A file a view maps is therefore never queued for
`reclaim()`, which keeps phase-3's rule that a file is truncated only after its last mapping is
released (`phase-3.md:197-198`) at `retained_predecessors` 0. Unlinking a mapped file would be
safe; truncating it would fault the reader with SIGBUS, so the rule is load-bearing, and T1 runs
with a 4 KiB `reclaim_step` so that every queued file is truncated.

The files a view holds stay counted by storage admission until the view is freed and a later
`retain()` queues them, as a merge's sources do. A long scan or search therefore delays the return
of the space of the segments merged away under it; at most H views can, node-wide. RocksDB pins
files the same way through `Version` refs (`db/version_set.h:1078-1081`, `db/version_edit.h:274`),
and Lucene through the deleter's refs on an NRT reader's segments
(`IndexWriter.java:6012`, `incRefDeleter`; `StandardDirectoryReader.java:149`).

With durability-modes, `retain()` keeps four sets: the durable head (DM-R23), the recorded
generation (DM-R43), the current state, and the live views. They compose by union; none of
their rules changes.

### D8. Reads on a view, and one read implementation

`view_guard` (and the view it points at) offers the const reads:

- `get(key, f)` and `get(key)`: the segments newest first, each through the key range, the
  filter, and the dictionary with the view's frozen bits, the loop `index.hh:972-980` runs today;
- `scan(from[, to][, n], f)`: the k-way merge of `scan_keys` (`index.hh:1134-1236`) over the
  view's segments, with no buffer runs and no `scanning_` count, so any number of threads scan one
  view at once and `f` may call the writer, which cannot change the view;
- `segments()`: the `segment_view`s (`index.hh:755-761`), whose `deleted` points at frozen bits;
  prospect builds its `segment_snapshot` span from them (`searcher.hh:26-30`) and its statistics
  from the view (`searcher.hh:94-101`);
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
resume and release it on another thread; destroying a cancelled task's frame releases it.
Acquire never suspends and never blocks: it loops only when the owner published between its load
and its validation, at most once per publication.

Under xapiand-on-strata D13, the seam's `search` and `get` take a guard from the shard's
`basic_reader` on whichever thread runs them, and a query keeps one view for its whole run, so its
pages are consistent. Real-time get (D2) and every write go to the owner. The guards one host
thread holds at once are bounded by the query concurrency ARCHITECTURE already caps per core,
which is how H is sized (T4).

### D10. Costs, stated as estimates until T3 and T4 measure them

| What | Cost (estimate) | Measured by |
|---|---|---|
| Acquire and release | a thread-local cache pop, one seq_cst store, two seq_cst loads, a compare, and one release store; no read-modify-write on a line another thread writes | T3 |
| A read on a view | the owner-thread read's cost; the view adds one indirection per segment | T3 |
| Publication (owner) | one view allocation, about 150 bytes plus 48 per segment; one bitmap clone per segment that took a delete since the last publication; a scan of H records | T3 |
| An index without readers | one null pointer in `basic_index`; no allocation | T4 |
| An index with a reader handle | the shared state (about 64 bytes) and its current view; one retired view per reader op in flight on it, at most H | T4 |
| The registry, per node | H records times 128 bytes; H = 256 is 32 KiB | T4 |

For the many-indexes workload (millions of indexes per host, tens of thousands open), the cost is
per index only where a reader exists, and per node, not per (thread, index), everywhere else.

### D11. Close, move, install, heal

- `close()` stores null into the current pointer, so new acquires throw `index_closed`, then
  frees the views no record names. Views still protected hold mappings of files a reopen could
  truncate as orphans, so `close()` must not leave them behind; Q2 asks how it waits.
- A moved index keeps its shared state (D4); handles follow it.
- `install()` publishes the installed content; earlier views keep the old segments until freed,
  and `retain()` skips their files meanwhile (D7).
- `heal()` replaces segment objects with re-read ones (`index.hh:740`); views keep the old
  objects, whose bytes passed the same checks, and the next publication carries the new ones.

## Gate rows

Performance rows are measured by the lead on `main`, under a strict hold, in minutes-long runs
(BENCHMARKS tiers); the branch makes functional claims only.

| Row | Target | Procedure |
|---|---|---|
| **T1** TSan stress | Zero reports under TSan, and zero under ASan and UBSan, on Linux x86_64, macOS arm64, and FreeBSD; the retired views of an index never above H (an assertion); no SIGBUS | `shared_reader_stress` (step 1): 1 writer and N = 1, 4, 16 reader threads on one index of each layout (`document_layout` in strata, prospect's search layout in prospect, step 2). The writer applies puts, replaces, deletes by key and by docid, rejected transactions, and metadata, calls `refresh()` every k transactions (k random in 1 to 64), commits every 10 refreshes, runs background merges on `pool_executor` with `auto_merge`, `force_merge`, `heal()`, a checkpoint and `install()`, and closes at the end; `reclaim_step` 4 KiB. Readers loop acquire, then a get, a 10-key scan, or (prospect) a top-10 search, holding some guards across a random pause. At least 10^7 reader operations, 10^4 publications, 10^2 merges installed, per platform; the seeds and counts are printed |
| **T2** differential | Every read equals the model at the view's generation; no transaction seen in part; generations never decrease per reader | The model is a `std::map` of key to (value, version), snapshotted by the writer at each publication under the view's generation. (a) Deterministic: on `inline_executor`, one thread interleaves reader steps between owner steps by a seeded schedule, 1,000 seeds, replayable. (b) Threaded: T1's readers log (generation, operation, result), checked against the model after the run. Transactions move value between key pairs whose sum is invariant, so a torn view shows as a wrong sum. `view.digest()` equals `index.digest()` taken at that publication. Mutants that must fail: validation skipped, the bitmap clone skipped, views left out of `retain()`, a retired view freed without the scan |
| **T3** per-acquire and publication cost | Acquire, release, and a hot-cache get on a view at 16 threads within 1.1× of the same at 1 thread (flat: no shared write); acquire and release add at most 5% to that get; publication time and the scan reported at 10, 100, 1,000 segments and H = 64, 256, 1,024 | `bench/shared_reader/acquire.cc`: one index, 1, 2, 4, 8, 16 threads; and the same across 1,000 indexes (threads pick an index at random). Comparison arm, reported, not gated: `std::atomic<std::shared_ptr>` on the same loop. Linux x86_64, Linux arm64, macOS arm64, FreeBSD, once each on the landing commit |
| **T4** memory | An index without readers: no allocation and at most 8 bytes over today (`sizeof` and an allocation counter); with a handle and an idle view: the bytes reported per segment count, target the D10 estimate; the registry per H; the clone bytes per publication under T5's overwrite load | A test with the counting allocator over 1, 3, 10, 100 segments; the clone bytes from a counter in the stress run |
| **T5** `readwhilewriting` | At N = 1, 4, 8, 16 readers: read throughput ≥ 1.0× RocksDB's and read p99 ≤ 1.0×, at fits and 2× data (the key-value ratchet's shape, PLAN "The `document` profile gate") | `bench/kv`: N reader threads share one strata index through a `basic_reader` (RocksDB: N threads on one DB), one writer at 2 MB/s of logical bytes (db_bench's `MB_WRITE_PER_SEC`), `refresh()` every 1 s on the writer thread. Reports reads/s, p50 and p99 per reader, the writer's achieved rate, and `refresh()` p99. States the asymmetry: RocksDB's readers see each write at once, strata's within one refresh interval (D2, Q1) |
| **T6** `fwdrangewhilewriting` | Same targets, on seek plus 10 nexts | T5's run with `view.scan(from, 10, f)` against RocksDB's iterator `Seek` and 10 `Next` |
| **T7** NRT reopen | `refresh()` p99 (flush plus publication) ≤ 1.0× Lucene's reopen p99, and search QPS beside indexing ≥ 1.0× Lucene's | prospect's bench: luceneutil's nightly NRT shape (`nightlyBench.py:106-110`, as `benchmark-coverage.md:144` cites it): 1 indexing thread at 1,103 docs/s, one refresh per second, 4 search threads, scaled to a few minutes; Lucene with `SearcherManager.maybeRefresh` once per second (`nrtPerf.py`) |

T1 in strata and in prospect is the strata side of xapiand-on-strata's T10 and of phase-3's owed
"TSan over ... snapshot publication" (`phase-3.md:524`).

## Questions for the owner

**Q1. Do readers on other threads need writes that are not yet refreshed?** RocksDB's readers see
every write as it lands; Lucene's see the last reopen; Xapian's see the last commit.
Options: (a) the last refresh only (D2); (b) a frozen, immutable copy of the buffer at
publication, as RocksDB's immutable memtables, with its own key index and live bits, and the
writer's version map extended to find documents in it; (c) at publication, encode the buffer into
an in-memory segment the view holds, written to its file later, so every reader path stays the
segment reader's. Recommendation: (a). It is what ARCHITECTURE item 9 already promises, it costs
nothing new, and the refresh interval is the knob. If T5 or Xapiand needs sub-interval
visibility, (c) is the follow-up, since it adds no second reader path; (b) adds a buffer search to
prospect's inverting layout.

**Q2. How does `close()` treat reads in flight?** Options: (a) `close()` stops new acquires,
then waits until no record names its views, then frees them; it deadlocks if the closing thread
itself holds a guard of that index, which a debug build detects by the guard's claiming thread;
(b) `close()` throws `readers_active` and changes nothing, and the host retries after draining its
readers; (c) `close()` detaches, and the last guard
to release a retired view frees it, which needs a refcount on the release path and a free off the
owner thread. Recommendation: (a), with an optional deadline after which it behaves as (b).

## First buildable step

Step 1 is strata core only, on `phase4/shared-reader` once the architect's preliminary review
clears it (concurrency). Neither question blocks it if the owner takes the recommendations: Q1's
(a) is today's behavior, and Q2 changes only `close()`.

1. `include/strata/core/hazard.hh`: `reader_registry` (H records on their own lines, per-thread
   record caches, the pool under a mutex), claim, protect and validate, release, and the owner's
   scan; unit tests of the protocol under TSan.
2. Copy-on-write live bits in `open_segment` (D3), every writer of `deleted` through one mutable
   accessor (`index.hh:2028`, `1997`, `1802`, `2953`, `heal()`'s move at `736`).
3. `basic_view<Layout>`, `basic_reader`, `view_guard`; `index_options::readers`;
   `basic_index::reader()`; publication at the points D1 names; retirement and the scan (D6).
4. D8's reads: the segment part of `get` and `scan_keys` factored so the index and the view call
   one implementation; the comment at `index.hh:1003-1006` corrected.
5. `retain()` referencing every unfreed view's segments (D7); views freed before `retain()` in
   `commit()`, in `reclaim()`, and in `close()` (Q2's (a)).
6. Tests: T1 and T2 on `document_layout` with their mutants; a test that `install()` and `heal()`
   under live views keep every read valid; T4's allocation test. TSan, ASan, and UBSan in CI.

Then, each after the one before lands:

2. prospect: search over `view.segments()` and the view's statistics; T1 and T2 with search;
   any per-segment cache prospect adds (the filter bitsets, `ARCHITECTURE.md:125`) is per view or
   safe under concurrent views.
3. `bench/kv`: a threaded mode with a shared index and a rate-limited writer (T5, T6), and
   `bench/shared_reader` (T3); prospect's NRT driver (T7). The lead measures.
4. durability-modes step 5 may build its batch entry on a kept view (D4); that design decides.

## Risks

- **A long read holds disk.** A search or scan that runs for minutes keeps every segment its view
  holds, so merges free no space under it, and storage admission can refuse writes meanwhile
  (D7). RocksDB's iterators and Lucene's NRT readers do the same. The host bounds read durations;
  the index reports how many views it holds and their bytes, so the host can see it.
- **H too small sheds reads.** `too_many_readers` under a burst is an overload error, not a wait.
  T4 sizes H from the per-core query cap; the registry reports its high-water mark.
- **Publication on every flush.** An index flushing many times a second publishes as often, and
  each publication scans H records. T3 states that cost; publication can coalesce to the host's
  refresh cadence if it shows.
