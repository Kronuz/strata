# The cold tier: millions of indexes per host

> Status: DRAFT (2026-11-02) revision 4, designing (ready for preliminary review): step 2, the
> catalog. D4 to D6 decide every step-2 finding (CT-R4, R6, R8, R10 to R13, R15, R23, R24; I5-N2,
> I5-N3; X-POOL); step 2's items and criteria S1 to S9 are in "Steps". Step 1 is on `main`
> (strata:081b7e7) but for K4 and item 6.

Cited at strata `main` `081b7e7` for D3 to D7, D10 and the steps; D9 and D11 keep step 1's
citations at `c82ac9f` (`index.hh` by symbol where step 1 or `defer_io` rewrote it). Xapiand's
`docs/design/phase7c.md` D1, D4 to D6, `reader-views.md` and `index-registry-and-lease.md` at
Xapiand:d5a2e746c; measurements
from `docs/perf/idle-index.md` on `main` and the architect's probes
(`strata-wt/review-cold-tier/.scratch/ct-review/probes/`, rerun 2026-10-27 with identical output);
shared-reader at `design/shared-reader` `15b801a`; write buffer at `design/write-modes` `ae2cf70`.
RocksDB 11.11 was read for its table cache; none of its code enters strata.

## Summary

An index lives in one of four tiers. **Hot** is today's open index. **Warm** keeps the index
object and its directory lock but no mapping and no view. **Cold** is closed: nothing in memory.
**Remote** is cold with its files on object storage (reserved). A catalog per pool partition pins
indexes for requests, opens them on demand, and demotes them by a sweep over counters of the OS
resources each tier holds, so mappings, descriptors and vnodes are bounded by the hot and warm set,
never by the number of indexes on disk.

The concurrency rests on a rule the shared reader already keeps (I1): **a mapping dies only with
its segment record, under the write lease, after no protected view holds it.** Demotion swaps every
record for an unmapped twin, as `heal()` swaps damaged records, and never strips a live one.

## Why

An idle 1-document index costs 5.9 KB dirty on macOS, 8.1 KB on Linux, 1 descriptor, 1 mapping,
and 2 vnodes on macOS. The walls come first: macOS fails with `ENFILE` at 126,507 open indexes
(`kern.maxvnodes` 263,168, system-wide, root-only), Linux at 65,491 under the stock
`vm.max_map_count` of 65,530, and strata has no handle cache (`idle-index.md`, "Limits"). Raising
the sysctls is the host's call and leaves memory linear in the total count, 8.1 GB per million.
The field: 1,000 shards per Elasticsearch node, 18 to 19 thousand Weaviate tenants per node, and
Turbopuffer's 250M+ namespaces as a hosted fleet with ~400 ms+ cold queries
(`product-landscape.md:93,122`, `design/landscape`).

## What this builds on, and what it changes

| Document | Used | Changed |
|---|---|---|
| shared-reader D1, D4 to D7, D9, D11 | views, hazard records, frees under the lease, held files never queued, `publish_through`, non-blocking close | demotion is a publication point; an "unpublished" sentinel beside null-means-closed (D3); `segment_view::seg` becomes an accessor (D2) |
| write-buffer D2 | checkout; chores by `chore_scope` or owed to the holder | demotion, the wake and the close are chores; a catalog pin is a checkout |
| write-buffer D5 | `apply()` is all or nothing | CT-R5's rollback is on `main` (`eb87572`, with `d3a59ee` for CT-R5a; `apply()` `:697`) |
| `defer-io.md`, `warm-indexes.md`; item 5's code review | `defer_io`; `demote()`, `promote()`, `map_owed`, the owed bit (`index.hh:1063-1103`) | the wake makes `map_owed` a backstop (D5); `unleased_maps()` counts per index (I5-N3); wake retries bounded (I5-N2) |
| Xapiand `phase7c.md` D1, D4 to D6; `reader-views.md`, `index-registry-and-lease.md` | no file syscall on a reactor; chores by class; the registry's turn and `refs`; reads take no turn | the registry becomes the catalog's payload, `refs` become pins, reads pin (D4, X-POOL); its D6.2 loops on `map_owed` (C8) |
| `owner.hh` | `lease_scope` waits on a chore's bit (`:120-122`); `chore_scope` never waits (`:151-163`) | the pin keeps the sweep's chores off a turn (D5); `write_lease::held_here()` (D3) |
| small-indexes, the lead's 2026-10-18 ruling | checks at first use, on the condition that a segment damaged at its size fails every read path clean and `heal()` reports it | Q3 carries the condition and its twelve cases (D11) |
| `ARCHITECTURE.md` | residency, suspension at query start (`:173-174`), overload shed (`:179-180`) | "footers and small sections at open" (`:149`) becomes "at first map" (Q3; the lead edits it on landing) |
| `DISTRIBUTION.md` | per-node rows (`:28-30`) | its "Cold" (`:94-98`, object storage) is this design's remote |
| `small-indexes.md`, `limits.md` | direction 2, "open lazily, close idle"; caps name their mechanism | this design is direction 2; `limits.md` gains D6's caps and mechanisms |
| `idle-index.md` | the per-index costs and cuts | three heap rows are mislabelled (N1); `main`'s note is the lead's to correct |

## Decisions

### D1. Four tiers, each defined by what it holds

| Tier | Holds | Dirty per index | fds | vnodes (macOS) | maps |
|---|---|---|---|---|---|
| Hot | open `basic_index`, the segments touched since promotion mapped, a view | today's measured 5.9 KB macOS, 8.1 KB Linux; target: no regression | 1 | 1 + mapped segments | mapped segments |
| Warm | open `basic_index` and directory lock, manifest, one unmapped record per segment, no view, idle core, a catalog slot | restated from measured pieces per segment count before it gates (N1); estimate about 1.7 KB of heap per one-segment index before the slot, `PosixIO` and slack | 1 | 1 | 0 |
| Cold | nothing in the process | 0 | 0 | 0 pinned | 0 |
| Remote | a cold index whose data files are on object storage (D8) | 0 | 0 | 0 | 0 |

On disk a 1-document index is 738 B in 3 files, 12 to 16 KB with blocks.

### D2. A segment is a record plus a body mapped at most once (cut 1)

Built in step 1 (`d0ab622`): `segment_body` and `open_segment` (`index.hh:2274-2387`).

- **The record** keeps the owner-written state: the entry (`ref`, which carries `doc_count`,
  `files.hh:80-94`), copy-on-write deleted bits sized from `ref.doc_count` (the constructor,
  `index.hh:2303-2304`), dead count, `damaged`, `unsynced`, and three **facts** the manifest needs
  and the entry lacks: `structures`, `version`, `total_length` (16 B, a known flag). **Only the
  owner writes them**, under the lease or before the record is published: where it makes a body
  (flush, merge output, install; `adopt()`, `:2314`), in commit's prepare (`learn_facts()`), at
  scheduling, and into twins. A reader's first map leaves them in the body;
  the owner reads the record's facts when known, else the body's after an acquire load of it.
- **The body** (mapping or owned bytes, and the decoded `segment`) is immutable and reached through
  an `std::atomic<const body*>` in the record that holds null, a body, or a static **failed**
  marker (D11). A thread that loads null maps: it opens the file through the index's IO, runs the
  first-map checks (D11), and installs the body, or the marker when the checks fail, by
  compare-exchange from null (acq_rel). The loser of either race (body against body, body against
  marker) destroys only its own body, at once on its thread, and then uses what won: a good body
  that loses to the marker throws `corrupt_segment`. No other thread saw the loser's body, and the
  body type carries no owner check (the record keeps `~open_segment`'s, `:2308`). A body or marker
  is never replaced or cleared before its record dies; the record's destructor frees a body, never
  the marker, which is static.
- **Open stops mapping.** Recovery builds records from the manifest, reads live-docs files, and
  checks each named file against the directory scan (D11). `options::eager_segments` maps all at
  open through the same code; it is the default until item 6 (K4, K7, item 5's CT-R21), and
  `defer_io` forces it on whatever the default (`:592`, `:4681`).
- **One accessor.** `segment_view::seg()` (`:1050-1057`) holds the record and does an acquire
  load that maps on null (`open_segment::seg()`, `map_body()`). Retries count on an atomic,
  `load_context::retries` (`:2262`, read by `media_retries()`, `:982`), written from reader
  threads once they map (`read_checked`, `:4859`).
- **prospect** fills its own `segment_snapshot::seg` raw pointer (prospect `searcher.hh:26-30`)
  where snapshots are built, in prospect's tests and Xapiand's seam; those sites call `seg()`,
  which a search does anyway (its statistics read every segment's count and length,
  `searcher.hh:95-101`). prospect pins strata by commit (`CMakeLists.txt:9`); the pin bump adopts
  `seg()`.

### D3. Demotion to warm: twins, then the view retires (cut 2)

Demotion to warm is a chore under the lease, only on an index warm's predicate admits (D10). It
swaps **every** record for a twin, bodied or not, as `heal()` swaps records (`:993`). A twin copies
`ref`, the facts, the bits (shared), `dead`, `deletes_dirty`, `damaged`, the load context, the
manifest's vouch, and the failed marker (static, so sharing it keeps I1); it has no body. `heal()`
copies the bits, `dead`, `deletes_dirty` and `ref` the same way (`:1022-1025`). Warm needs no
commit (D10), so a twin may stand for uncommitted deletes, and `commit_files()` writes a live-docs
file only where `deletes_dirty` is set (`:4427-4439`, else the entry keeps its generation and the
manifest takes `live_length` from the facts, `:4500-4501`). Demotion forces a publication, since
`publish_point()` skips an unchanged state (`:2664`), and calls `free_views()` (`:2769-2783`). A
record left in place could be mapped afterwards by a reader holding an older view (`build_view`
keeps `segs_`'s pointers, `:2674-2675`), and its body would live in the warm index until `close()`;
with every record twinned, a late map lands in the retired view's record (`twin_race.out`: 6 of 10
interleavings leave a body in `segs_` when only bodied records are twinned, 0 of 10 when all are).

Demotion is complete when its retired views are freed. Shared-reader D6 scans at publish, commit,
`reclaim()` and close, none of which a warm index runs, so the sweep runs `reclaim(0)` under
`chore_scope` on warm slots with retired views (D5), and those views' bodies count in the slot's
maps until then. `views_held()` matches records by segment id, not by pointer
(`:1371-1377`), so a twin and its retired original count once.

With the sentinel (step 3), demotion publishes the **unpublished** sentinel, a static view never
freed, instead of a view over the twins, and drops the empty active buffer the last view made
(`:2693-2698`; the next write makes one). Its acquire contract is open (CT-R7). That is cut 2: a
warm index holds the publication object and reader state, not a view or a buffer.

**Warm under `defer_io`: owed until promoted (CT-R21).** `defer_io` forces eager segments so that
`apply` makes no device call (`:592`, `:4681`; `defer-io.md`, L5-B1), and demotion is the only way
an unmapped record reaches such an index: flush and merge outputs adopt their bodies (`:2569-2574`,
`:3090`), eager `install()` and `heal()` map, and a reader only installs bodies. So the index keeps
one flag, set by `demote()` when it twins a record that can still be mapped and cleared by
`promote()`. It is a `std::atomic<bool>` (release store, acquire load) behind `promote_owed()`: the
owner writes it under the lease, and step 2's wake reads it without the lease to skip the chore hop
for an index with nothing owed; a stale read is harmless, since `map_owed` backstops it (CT-R26).

- **`promote()`** is a chore under the lease (Xapiand's `query` class): it maps every record
  without a body through D9's failure rule. A failed check marks the record; a host error rethrows
  with the bodies mapped so far kept, the flag still set and nothing fenced. Once every record holds
  a body or the marker it clears the flag. It writes and publishes nothing: readers reach the new
  bodies through the records their view already names.
- **`apply` under `defer_io` answers `verdict::map_owed`** while the flag is set, after the replay
  check and before `flush_owed` (`:706-715`), with nothing applied and no device call. The host runs
  `promote()` and applies the same transaction again before any later position, as for
  `flush_owed`. The answer ends (C15 of the L5 review), in strata and not by a host rule
  (CT-R22). A `promote()` that completes clears the flag, and one that throws gives the host its
  error, not another owed answer. `demote()` could still run between that `promote()` and the
  retry, since warm's predicate holds (`map_owed` applied nothing, so nothing is buffered), and the
  retry would answer `map_owed` again without bound. So `apply` also sets an owner bit when it
  answers `map_owed`, and `demote()` returns false and changes nothing while it is set. Like
  `drain_asked_`, the bit lives for the turn, the host's retries of one transaction: any `apply`
  that returns something other than `map_owed`, `flush_owed` or `drain_owed`, or throws, clears
  it, as does `close()`. `refresh()` and `commit()` do not: `refresh()` is `flush_owed`'s chore,
  and a clear there lets a `demote()` in before the retry, which answers a second `map_owed`.
  The turn's bound is **at most one of each owed answer, in any order, so at most three retries**;
  Xapiand's D6.2 ("retries the apply once more") becomes that loop when it adopts warm. This is
  not L5's heavy form, an owed map raised inside `evaluate` per record mid-transaction: the flag
  is index-wide and read before stage 1, because under `defer_io` unmapped records exist only
  between `demote()` and `promote()`. Debug builds hold `no_input_map_scope` (`:383-387`) across
  stage 1 under `defer_io`, so a map there asserts (`map_body()`, `:2372`), and assert at the
  check that with the flag clear every record is bodied or marked, which catches a future
  creation site that breaks I8's premise on any apply, not only one that reaches that record.
- **Reads pin, so they never meet an owed index** (CT-R23). Under the catalog every call, a
  view's acquire included, runs under a pin, and a pin is handed out only once the wake has run
  `promote()` (D5), so a read after demotion never maps on a reactor. A host that calls
  `demote()` itself, outside a catalog, and reads without promoting maps on the reading thread, as
  any lazy read does (D9), and the count below shows it.
- **The count is per index** (I5-N3). `unleased_maps()` counts input maps made on a thread that
  does not hold **this** index's lease: `load_context` keeps a pointer to the index's
  `write_lease`, and `map_body()` compares its holder word with the thread's token in every build
  (a new `write_lease::held_here()`; `held()` is always true without lease checks,
  `owner.hh:95-99`). Today's test, the thread's lease count (`index.hh:2457`), misses a map of
  index B made inside index A's chore. For a `defer_io` index in a catalog it is zero: the catalog
  asserts it at unpin in debug builds, and Xapiand's gate asserts it (C8). Step 3's sentinel makes
  a read that slips past the pin throw `index_unpublished` instead of mapping (CT-R7).
- **Why pins and not the other two.** Shipping the sentinel with step 2 still needs the pin: the
  sweep's `chore_scope` against a reactor's `apply`, which would wait on the chore bit
  (`owner.hh:120-122`), I7's host half and the policy's frequency all rest on it, and a pinned
  read never pays the sentinel's throw, promote and retry. Refusing `demote()` under `defer_io`
  leaves Xapiand hot and cold only, rejected below.
- **Why not the alternatives.** Excluding `defer_io` from warm (the review's (a)) leaves Xapiand,
  the many-indexes consumer this tier exists for, with hot and cold only. Its idle indexes are then
  hot, one map and one descriptor per segment and 5.9 KB macOS / 8.1 KB Linux for one document
  (measured, `idle-index.md`), or cold; on Linux the map soft cap (32,765, D6) bounds the hot set,
  and every wake past it is an open plus an eager map of every segment: 53 to 85 µs on Linux
  with the cache warm, about 0.5 ms with it dropped, and 0.4 to 1.1 ms p50 on macOS (measured,
  `idle-index.md:92`, `:95`, `:211`). A warm wake is a chore hop (unmeasured) plus one map per
  segment, with no recovery, lock or manifest read; 33 to 50 µs is the Linux gate VM's map plus
  first query on the old base (`:164`), and macOS's map is unmeasured (CT-R24). T2d decides the
  latency; the choice rests on memory and maps: warm idles at 0 maps, 1 descriptor and an
  estimated 2.6 to 3.7 KB (N1). A host chore alone (the review's (b)) leaves a forgotten wake as a
  silent file open on a reactor in release builds; the verdict costs one branch per apply and makes
  step 1 safe without the catalog. T1, T2 and T2d carry the `defer_io` rows.

### D4. The catalog, one per pool partition

`strata::catalog<H>` sits beside the partition's registry (shared-reader D4) and owns the
partition's non-cold indexes. The host supplies (Q2) an executor for every transition, a timer for
the sweep, a key-to-directory resolver, the caps' reserve and the `index_options`. `H` is the
host's per-index payload, made at open and destroyed at close: Xapiand keeps its turn and its
`WorkGroup` there (X-POOL), so residency has one owner.

- **Slots** live in a chunked array never freed while the catalog lives, reused through a free
  list. A slot (about 192 B plus `H`, an estimate) holds the key, the index, its reader handle,
  `H`, the last-use time, a waiter list (under the shard mutex), the per-slot resource counts
  (D6), the wake's failure count and retry time (D5), the policy's queue link (D6), and a 64-bit
  **word** `pins:24 | state:4 | freq:2 | gen:34`; state is one of `free`, `opening`, `open`,
  `waking`, `demoting`, `closing`.
- **The table** maps keys to slots in 64 shards, each a `hash_slots` (`index.hh:254-330`, an
  open-addressed table of 32-bit slot numbers with a full-key test) under a mutex held for one
  probe, insert or erase, never across I/O (CT-R15). Only non-cold indexes have a slot. RocksDB's
  table cache is this shape (`options.h:792,1114`, `db/table_cache.h:64`).
- **A pin is a checkout, and every call on a catalog index runs under one**: reads (a view's
  `acquire` through its release), writes (the host's turn), and the host's chores. By key: under
  the shard mutex, probe, then compare-exchange `{p, open, f, g}` to `{p+1, open, min(f+1, 3), g}`
  (acquire). By handle: a host that keeps `{slot, gen}` from an earlier pin re-pins with the same
  compare-exchange and no mutex while `gen` matches, else falls back to the key. Unpin stores the
  time (relaxed), then decrements (release), so the time never lands in a reused slot (CT-R13). A
  hit costs two read-modify-writes by handle, four by key; T4 reports both on the hottest key.
- **A pin never blocks a thread.** A pin that meets `opening`, `waking`, `demoting` or `closing`
  registers under the shard mutex and suspends; the transition stores its next state and resumes
  the waiters on their executors under the same mutex (CT-R13). A synchronous `try_pin` answers
  `would_wait` instead (CT-R6). Only a hard cap answers `too_many_open` (D6).
- **A miss** inserts an `opening` slot holding the pin (`gen` + 1) and posts the open to the
  executor (about 54 syscalls); the opener fills the slot's fields, then stores `open` (release)
  under the mutex. A failed open erases the slot and fails its waiters with the error.
- **Ownership checks** (CT-R15). Resolution must be injective: a second key on a held directory
  meets the directory lock's `EWOULDBLOCK` (completion `posix.h:435-452`), which fails the open as
  a host error, never retried. Debug builds assert at `live()` that the slot shows a pin or the
  transition the calling chore runs. A catalog destroyed with `closing` slots leaks their IOs
  with their locks, as `~basic_index` leaks the lock.

### D5. Transitions

```
 cold ─miss─▶ opening ─▶ open/hot ─sweep─▶ demoting ─▶ open/warm ─sweep─▶ closing ─▶ cold
                            ▲                             │ pin, flag set
                            └────────── waking ◀──────────┘
```

The sweep picks on the host's timer and touches no file there. Each transition is a chore on the
host's executor, because `demote()` unmaps released views and `close()` closes descriptors: file
syscalls, which no reactor or control loop may make (Xapiand `phase7c.md` D1).

- **Cold to open.** D4's miss. Under `defer_io` the open maps every segment (`index.hh:594`), so
  it opens hot.
- **Hot to warm.** The sweep compare-exchanges `{0, open}` to `{0, demoting}` (acquire; only then
  does it read the slot's fields, CT-R13) and posts a chore: `chore_scope`, then `demote()`. A
  held lease or a refusal restores `open`. Because every caller pins, the chore never meets a
  host's turn, and a reactor's `apply` never waits on the chore bit (`lease_scope` waits on it,
  `owner.hh:120-122`). Readers hold views only under pins, so `demote()`'s publication frees
  every retired view and the slot's maps reach zero; a view kept past its pin (a host bug) stays counted
  and `reclaim(0)` (`index.hh:948`) on a later pass frees it.
- **Warm to hot: the wake** (CT-R23). A pinner that, after its compare-exchange, loads
  `promote_owed()` true (acquire, CT-R26) moves the slot to `waking` by compare-exchange from
  `open`; the winner posts `promote()` (Xapiand's `query` class), and every such pinner waits. The
  pin is handed out only on `open` with the flag clear. The flag is set only by `demote()` under
  `{0, demoting}`, so no pinned caller ever reaches a record without a body under `defer_io`:
  `map_owed` and the owed bit become backstops, and a read after demotion never maps on a reactor.
  Without `defer_io` the flag is never set, and warm to hot is the first reader's lazy map (D2).
- **The wake's bound** (I5-N2). A wake that throws a host error fails its pinners with it and
  counts on the slot. Until the slot's retry time a pin fails at once with `wake_failed`, carrying
  the last error, and posts nothing; the retry time doubles from 10 ms to 10 s (estimates). After
  8 consecutive failures the sweep demotes the slot, dropping the bodies the partial wakes mapped,
  and closes it when cold's predicate holds, so the next pin's reopen reports the lasting error
  and erases the slot; otherwise the slot stays failing at one probe per 10 s and counts in
  `failing()`. `too_many_open` fails the pinners without counting: it
  is overload, not a fault. A wake that completes resets the count.
- **Warm to cold, only from warm** (CT-R6). The sweep compare-exchanges `{0, open}` to
  `{0, closing}` on a slot whose maps count is zero (warm, no retired view) and posts the close:
  `chore_scope`, then `close_ready()` (D10). A non-empty reclaim queue gets `reclaim()`, which
  posts its deletes to the executor and returns, and the slot goes back to `open` to close on a
  later pass (CT-R4). Then `close()`. Under the shard mutex the chore erases the slot (`gen` + 1,
  to the free list), or sets `opening` and reopens if pinners came. A `close()` that leaves
  `closing()` true (a view kept past its pin) keeps the slot `closing`, reports it in debug
  builds and counts it, and the sweep retries `try_finish_close()` each pass; its pinners stay
  suspended, which only a host bug reaches.

### D6. Counters, caps and policy

**Counters come from the IO** (CT-R8). The resolver's IO is wrapped as faultfs wraps one: every
file open and map of a slot's index reserves on the slot's and the catalog's counts by `fetch_add`
before the system call, returns the reservation when the call fails, and releases at `close` and
`munmap`. A reservation past a hard cap is returned and throws `too_many_open`, a host error under
D9 (never marks, never fences). A body that loses the install race (D2) is destroyed and releases
its reservation. The counts therefore include retired views' bodies, equal the kernel's at
quiescent points (I6, T7), and a slot's count names what evicting it frees. The thundering herd
is bounded: under `defer_io` only the wake maps; without it, transient reservations are at most
the concurrent readers of one record.

| Platform | Counts (stock value) | Soft / hard |
|---|---|---|
| Linux | maps; `vm.max_map_count` (65,530) less the process's maps at construction | 50% / 90%: 32,765 / 58,977 |
| macOS | vnodes, one per open file or live mapping of a distinct file, the directory's included; `kern.maxvnodes` (263,168 on the 32 GiB Mac), system-wide | 40% / 60%: 105,267 / 157,900 |
| Both | descriptors; the soft `RLIMIT_NOFILE` after the catalog raises it to the hard limit (bounded by `kern.maxfilesperproc` on macOS), less the host's reserve (default the smaller of 1,024 and a quarter of the limit) | 75% / 95% |
| Both | bytes on the host's `byte_budget` | the host's |

The raise needs no privilege (CT-R12); systemd's default soft 1,024 and macOS's 256 no longer give
a cap of zero. A catalog whose descriptor cap leaves fewer than 64 indexes fails at construction
with `EMFILE`. `limits.md` gains these rows with their mechanisms.

**Demotion.** The sweep demotes an unpinned hot slot idle 30 s, and over the maps soft cap (Linux)
or the vnodes soft cap (macOS) the hot slots with the oldest last-use among those a pass visits.

**Closing: S3-FIFO** (CT-R10). Open slots sit in a small FIFO (10% of the descriptor soft cap) or
a main FIFO, with a ghost of 64-bit key hashes the size of the main FIFO. The frequency is the
word's 2 bits, which the pin's compare-exchange sets, so a hit costs no extra store. A new slot
enters the small FIFO, or the main one when its hash is in the ghost. Over the descriptor soft
cap the sweep takes the small FIFO's head: frequency set, it moves to the main FIFO; clear, it
closes and its hash enters the ghost. Once the small FIFO is within its share, the main FIFO's
head is decremented and reinserted while its frequency is set, else closed. A hot candidate is
demoted first and closes on a later pass. Apart from the caps, a warm slot idle 10 min closes. The architect's model gives 0.817 at θ = 0.99 and 0.961 at θ = 1.2
where LRU gives 0.775 and 0.952 (`zipf_policy.out`), and a one-time scan enters only the small
FIFO.

**The sweep** runs per partition on the host's timer at 1 Hz, and when a reservation passes a soft
cap, which posts a pass to the executor. A pass visits at most 256 slots and posts at most 16
transitions (estimates, T4 tunes them). Reservations never evict inline: past a hard cap they fail,
and the evictor takes only an unpinned slot whose count of the resource over its cap is nonzero.

### D7. The idle core (cut 3)

Of the `index` object's 1,320 B, about 650 B is writer and maintenance state an idle index never
uses, plus 200 B of merge machinery (`idle-index.md`, cut 3; to recount on main's write-buffer
fields). It moves behind one pointer made at the first writer call, freed at demotion to warm
when the reclaim queue is empty. The queue and its debt stay outside it (CT-R4), since cold
requires them empty (D10).

### D8. Remote is reserved

A remote index is cold with no data files local; a pin miss fetches them on the executor under
`opening` before the cold path runs, so the catalog needs only that hook. Its design is
DISTRIBUTION's storage tiers (PLAN D6).

### D9. Every owner read of a segment is entry, map, output, or body

Each read or map of a segment in `index.hh` is one of four kinds. **Entry** reads the record
(entry, bits, facts) and never maps. **Map** may map a committed file and follows the failure rule
below. **Output** maps a file this index just wrote. **Body** reads a body its own path created or
mapped earlier, asserts it, and never maps (`open_segment::mapped()`). Step 1 built the table
(`d0ab622`); the rows cite `main`.

| Read | Where at `c82ac9f` | Kind | On failure |
|---|---|---|---|
| merge selection | `:1706-1711` | entry: `ref.doc_count`, bits | none |
| commit allowance (via `settle_storage`, also from `~hold`) | `:3545-3550`, `:3510-3512` | entry: `ref.doc_count` | none |
| commit: all-dead erase, live-docs files, `live_count` | `commit_files()`: `:4407`, `:4427-4439`, `:4500` | entry | none |
| commit: manifest's mask, versions, `total_length`; `live_length` without deletes | `:4501-4508` | entry: facts (`commit_files()` fences on unknown facts, `:4408-4410`) | none |
| commit: `live_length()` of a segment with new deletes | `:4436`, `live_length()` `:4549-4556` (body: `mapped()`) | map, in commit's prepare (`prepare_commit()`, `:4393-4402`) | the host's; a marked record throws `corrupt_segment` |
| `segments()`, `owned_segments`, `build_view` | `:1078`, `:2389-2396`, `:2669-2680` | entry: they take addresses | none |
| get, superseded, scan, digest through `seg()` | `get_segments` `:2047-2060`, `superseded` `:1954`, `scan_segments` `:2076-2123` and `:2170-2175`, `digest_of` `:1756-1759` | map, on the calling thread | to the caller |
| `apply()` stage 1: `locate_docid`, `lookup_key` | `:3684`, `:3703` | map, under the lease; under `defer_io` never: `map_owed` until `promote()` (D3) | rolled back (`apply()`, `:697`), to the host |
| `promote()` | new (D3) | map, under the lease, every record without a body | marker on a failed check; host errors to the host, the flag kept, unfenced |
| merge scheduling | `schedule()` `:2811-2821` | map, under the lease, before the job leaves the owner | source marked, or round skipped |
| merge job on the pool | `:3025-3059` | body: scheduling mapped its sources | none new |
| merge output | `map_output` `:3069-3072`, `adopt` `:3090`, published `:3131-3132` | output, on the pool | bytes kept |
| flush output (`streamed_segment`) | `:2564-2578`, from `flush()` in `guarded()` (`:841`, `:918`) | output, under the lease | bytes kept |
| `mark_damaged` | `:3159-3171` | body: the job's sources | none new |
| flush's install (`install_flushed`) | `:4204-4216` | body: the flush's own segments (`mapped()`) | none new |
| `install()`'s incoming segments | `load_files(..., true, ...)` `:1508`, copies `:1522-1540` | body: loaded whole | none new |
| `install()`'s own copies | `:1543-1560` | lazy: entry, an unmapped record with facts from the incoming body; eager: map, today's rule | none; eager: to the host, unfenced |
| `check()` | `:1454-1456` | map, all segments | to the caller |
| `heal()` | `:993-1035` | map: bodied and failed records only | host errors to the host |
| check-in chores, `run_chores` | `:2626-2638` | none: publish, build, scan, allowance | none |

**The failure rule.** An error at a map point splits as `read_checked` splits it (`:4871-4875`):
a check failing twice or `EIO` twice is the medium's and reports `corrupt_segment`; anything else
(`EMFILE`, `ENFILE`, `ENOMEM`, `EACCES`, `too_many_open`) is the host's, rethrown unchanged, never
counted as damage, never marking a record, never a fence. Per path:

- **Output maps** never fail on a host error. `map()` throws on an `mmap` failure (completion
  `posix.h:342-354`); a host error mapping a file this index just wrote keeps the bytes in memory,
  as a backend that cannot map does: `streamed_segment` and the merge read back through the file
  they hold (`map_output`, `:2580-2586`; `:2571-2575`, `:3069-3072`), with no new descriptor.
  Before step 1 such an error inside `guarded()` or from a merge job fenced a healthy index
  (`output_map_probe.out` at `98ce743`: `refresh()` with `map()` failing `ENOMEM` threw
  "durability failure", and the next apply and commit "the writer is fenced").
- **Commit** maps in `prepare_commit()` (`:921`) after the flush, `publish_merges()` and
  `maybe_merge()` (`:918-920`), since a flush can set deletes on records with no body, and before
  `guarded(commit_files)` (`:922`). It maps every record whose facts are unknown, or that has new
  deletes and no body, skipping records whose `dead` equals `ref.doc_count` (the erase at `:4407`
  drops them); a record with no live-docs generation takes `live_length` from its facts'
  `total_length` (`:4500-4501`). A host error throws and leaves the index as
  `refresh()` leaves it: flushed segments and installed merges in `segs_`, unpublished until the
  next `publish_point()`, nothing durable, `applied_since_commit_` unchanged, nothing fenced; facts
  and bodies the prepare installed are true and stay. Inside `guarded()` (`:2548-2557`) no input map
  remains. Recovery's fallback commit (`:4821`) maps the same way inside open, whose failure fails
  the open as before.
- **Commit with a marked record** (CT-R19). A record whose first map fails its checks carries the
  marker (D11), and every later commit's prepare throws `corrupt_segment`, unfenced, until `heal()`
  replaces it. `needs_heal()` (`:904-906`) reports marked records beside `damaged` ones; the host
  heals and commits again. Under eager segments the same damage fails the open.
- **Scheduling** maps each source before reserving storage. A source failing its checks is marked
  damaged and left out, as `mark_damaged` (`:3159-3171`) does for a finished merge, and is never
  swallowed as a host error. A host error drops the round with nothing reserved. Only `maybe_merge()`
  (`:879-890`)
  swallows it, beside `ENOSPC`, so `refresh()` and `commit()` complete and the next round retries;
  `merge()`, `force_merge()` and `force_merge_deletes()` rethrow, as the host asked for them. Each
  swallowed round counts on `load_context::skipped` (`:2263`), read by `merges_skipped()`
  (`:985`): skipped merges pile up
  segments, each a map per read, which feeds the shortage, and step 2's catalog reads the count.
- **`apply()`** relies on CT-R5's rollback (on `main`): it restores the buffer and positions, then
  rethrows. Under `defer_io` it never maps: a warm index answers `map_owed` (D3).
- **Readers** (`get`, `scan`, digest, search) get the error; the index changes only by D11's
  marker and the retry count.
- **No input map** runs in a destructor, a `noexcept` chore, inside `guarded()`, or in `apply`'s
  stage 1 under `defer_io`; output maps there never throw a host error. Debug builds assert them
  through `no_input_map_scope` (`:383-387`, `:2372`).

A get maps segments newest first down to its hit, all of them on a miss; a scan maps every segment
whose key range it reads (`scan_segments`, `:2082`).

### D10. Quiescence for warm and for cold

- **Warm** (`demote()`, under the lease): no buffered record (`buffered_records()`, `:4081`), no
  pending merge (`pending_`, `:5027`), no unsynced record. Posted deletes do not
  block it: a delete touches only the IO and its channel (`:3329-3331`), which demotion keeps.
  Nothing need be committed: demotion writes nothing (I2). `defer_io` does not exclude warm: a
  warm `defer_io` index answers `map_owed` until `promote()` (D3). `demote()` on an index that
  fails the predicate, or while a `map_owed` turn is outstanding (D3), returns false and changes
  nothing, the owed flag included. "Not closing" needs no clause: `close()` fences the index and
  `live()` throws `index_fenced` before the predicate.
- **Cold** (`close_ready()`, new, under the lease): warm's predicate, nothing uncommitted
  (`applied_since_commit_`), and, after a non-blocking `collect_deletes()`, no posted delete, an
  empty reclaim queue (`reclaim_`, `index.hh:5147` at `081b7e7`) and no debt (`debt_`, `:5159`),
  so `close()` releases a storage permit only for bytes no queue still owes (CT-R4).
  Otherwise the sweep retries on a later pass. `close()` waits for posted deletes without a timeout
  (`end_merges()` → `end_deletes()`, `:3278-3281`, `:3404-3411`). `work_pool_executor` allows a
  chore to wait on its index's own merge and delete groups (depth 2, `defer-io.md`), but the sweep
  is bounded work per call over a whole partition (D6), so it closes only what will not wait;
  with `posted_` empty it waits only for a reported task's tail, and debug builds assert no task
  is unrun.
- So an idle index reaches warm whatever its last call posted, and reaches cold once the
  executor has run its deletes. Today `posted_` shrinks only when an owner call collects
  (`quiescence_probe.out`: 5 pending after a commit, 5 after the executor ran them and after a
  check-in, 0 after `reclaim(0)`); the sweep's collect is that call, and its `reclaim()` empties
  the queue (D5).

### D11. Open checks names and sizes; a failed first map is marked

- **At open**, recovery records every scanned file's size (`track`, `recover()` `:4768`). Open
  checks each manifest-named segment's existence and size against those records, with no system
  call (`load_record()`, `:4949-4958`): a missing file fails the open with
  `corrupt_index("missing file")` and a size mismatch with `corrupt_index("file size differs from
  the manifest")`. Live-docs files are still read and checked at open (`read_live()`,
  `:4999-5012`), and metadata files as before.
- **At first map**, `open_body_of()`'s checks run (`:4969-4997`: size, version, footer CRC against
  the entry, identity, document count, structures and versions against the manifest, whole when
  eager) through `read_checked`'s retry, and report as `corrupt_segment`.
- **A failed map is marked.** A record whose checks fail in both attempts gets the failed marker by
  compare-exchange from null; later reads throw `corrupt_segment` at once, `needs_heal()` reports
  it, twins carry it (D3), and `heal()` reports it and replaces the record. Host errors never mark.
- **Q3's condition** (the lead's 2026-10-18 ruling; its test is strata `8b834ee` on
  `perf/small-indexes`): one byte damaged at the segment's size in the term index, the footer,
  postings, or stored records, then a search, a get by key with its stored record, or a merge.
  In all twelve cases every path that reads the damage throws `corrupt_segment`, any other answers
  exactly, and `heal()` reports the segment. Step 1 ports them to main's sections with lazy
  segments (K4, not yet built).

## Correctness under concurrency

**I1. A mapping dies only with its record**, under the lease, after the registry scan found no
protected view holding the record, and with no merge job holding it (shared-reader D4, D6). A body
or failed marker is installed at most once and never cleared, so a guard's segments stay mapped
through any demotion.

**I2. Demotion changes no file set.** Twins name the same files; `retain()` references files by
name (`:4593-4602`) and "a held file is never truncated" (shared-reader D7) is untouched.

**I3. One instance per directory**: one slot per key by the shard mutex, one owner across
processes by the directory lock, one performer per transition by the slot word.

**I4. Cold only when unpinned, warm, quiescent and committed** (D5, D10). The slot
compare-exchange excludes a pin; a maps count of zero excludes a retired view; `close_ready()`
excludes merges, posted deletes, buffered records, queued reclaims and debt; "nothing uncommitted"
excludes lost work, since `close()` drops it.

**I5. Demotion is invisible to reads**: the next view has the same position and digest, and so
does `promote()`'s.

**I6. Counters are exact at quiescent points**: each is reserved before the system call it counts
and returned when that call fails or its object goes (D6).

**I7. A slot outlives what reaches it.** A lazy map uses the slot's IO; a slot is freed only after
`close()` completes, so after every view is gone; a stale handle fails its `gen` check.

**I8. Under `defer_io`, `apply` makes no device call, warm or hot**: unmapped records exist only
between `demote()` and `promote()`, and the flag that `apply` reads before stage 1 covers exactly
that interval (D3). The answer repeats at most once per turn (the owed bit).

**I9. Under `defer_io`, a pinned caller never reaches a record without a body**: the flag is set
only under `{0, demoting}`, and a pin is handed out only on `open` with the flag clear (D5). So
`unleased_maps()` of a catalog's `defer_io` index stays zero.

**I10. A pin never blocks a thread**: it suspends, or a synchronous pin answers `would_wait` (D4).

## Gate rows

Lead-run on `main` under `quiet exclusive` on Linux and macOS, extending `bench/idle_index` with
`catalog` and `tier` modes. Every row keeps its raw files and states the kernel's limits
(`vm.max_map_count`, `kern.maxvnodes`, `kern.maxfilesperproc`, the soft and hard
`RLIMIT_NOFILE`). The idle-index baselines are re-measured first (C2). Branches make functional
claims only. "Cache dropped" is `echo 3 > /proc/sys/vm/drop_caches` on Linux and `purge` on macOS
before the pass.

| Row | Target | Procedure |
|---|---|---|
| **T1** memory per tier | Hot within 5% of the re-measured baseline. Warm at most the sum of its measured pieces plus 7% slack, and 0 maps: per segment the record's `sizeof`, its bits' allocation and control block; once the publication, the reader state, `basic_index`, the slot's share of its chunk, and until step 3 the view and the empty buffer (N1). Cold: no slot, ≤ 16 B per index (noise), 0 fds, 0 maps | `idle_index open ROOT 100000 shared tier {hot,warm,cold}` on 1-, 100- and 1,000-document fixtures with 1 and 10 segments, idle 5 s, metered as `idle-index.md`; the counting build for the heap. Again with `defer_io`: warm by the sweep, 0 maps; hot and a cold reopen map every segment |
| **T2** first query per tier | Per platform, fixture (1 and 1,000 documents), segment count (1 and 10), cache warm and dropped: hot within 5% of the re-measured baseline at p50 and p99; cold within 10% of the re-measured open plus `get`; warm reported (T2d gates it) | 100,000 indexes forced to one tier through the catalog, one `get` each in random order, one thread |
| **T2d** wake cost (CT-R24) | Warm below cold at p50 and p99, at every segment count, cache warm and dropped, on each platform. Where it is not, D3's latency sentence goes and the memory argument stands | Linux and macOS; 1 and 10 segments; cache warm and dropped; 100,000 `defer_io` indexes, one wake each in random order: (i) warm, `promote()` posted to `work_pool_executor` from a thread off the pool and the resume, then one `get`; (ii) cold, the open on the same executor, then one `get`; (iii) hot, the `get`. p50 and p99 per case, and `promote()`'s time over its segment count against the 33 to 50 µs estimate |
| **T2w** write after open | Reported, not gated: decides Q5 | The 10-segment fixture opened cold, one `apply` and a `commit`; the maps and the time of that first commit, against `eager_segments` |
| **T3** count far above the caps | Zero `ENFILE`, `EMFILE`, `ENOMEM` and `too_many_open`; at every 1 Hz sample the kernel's maps, descriptors and (macOS) vnodes below the hard caps, and within one sweep pass of the soft caps after the touch ends; Linux dentry and inode slab reported | 1M one-document indexes per platform (12 to 16 GB, D1); Linux on the gate VM with stock sysctls, macOS with its values stated (the development Mac's are raised, CT-R11). Caps: Linux 10,000 hot and 50,000 open; macOS 5,000 hot and 50,000 open (55,000 vnodes for one-segment indexes, under 105,267). 8 threads `get` every index once, then 1M Zipf requests at θ = 0.99. Samples: `/proc/self/maps` lines and `/proc/self/fd` entries; `mach_vm_region` regions, `proc_pidinfo` descriptors and `kern.num_vnodes` (system-wide, alone under the hold). 10M informational, not gated |
| **T4** Zipf over 1M | Hit ratio (hot or warm) at θ = 0.99 and 1.2 at least the S3-FIFO reference model's at the same capacity less 1 point; zero kernel errors. Reported: p50 and p99 per request at 1 and 8 threads, opens per second, the hit ratio over the 1M requests after a one-time scan of every key against before it, and a pin's cost by key and by handle on the hottest key at 8 threads | 1M one-document indexes. Caps: Linux 10,000 hot and 110,000 open (soft `RLIMIT_NOFILE` ≥ 150,000 after the raise, stated); macOS 5,000 hot and 50,000 open. A closed loop of 20M requests after 5M of warm-up per θ; the model is `zipf_policy` rerun at the row's open count |
| **T5** sanitizers | Zero TSan, ASan, UBSan reports; no truncation under a mapping; under `defer_io`, `faultfs::calls()` unchanged across every `apply` | Step 1 (on `main`): one index; readers get and scan, some holding guards across `demote()` and `promote()`; writers apply, refresh, commit, merge on `work_pool_executor`, `heal()`; faultfs and POSIX IO; `map()` failures on outputs and in `promote()`; with and without `defer_io`; interleaving counters each at least 1 (CT-R25). Step 2: 64 indexes under a catalog capped at 8 open, 12 descriptors and 16 maps, the sweep at 1 kHz; readers and writers pin by key and by handle; each cap driven to its hard value with every slot pinned (`too_many_open`, no kernel error); faultfs `EMFILE` in wakes, transient and persistent; indexes with queued reclaims; the storage budget against the bytes on disk after every close; with and without `defer_io`, `unleased_maps()` zero for every `defer_io` index. Counters each at least 1: pins meeting `opening`, `waking`, `demoting` and `closing`; busy restores; stale handles falling back to the key; wake failures in backoff |
| **T6** schedules | Every read equals the model; generations never decrease; mutants die | Step 1's schedules and mutants (on `main`, K1 and K5). Step 2: 1,000 seeded schedules on `inline_executor` over pin, unpin, re-pin by handle, the sweep's compare-exchanges, slot reuse, the wake and waiter resumption; the mutants are S2 to S7's |
| **T7** counter fidelity | The catalog's counts equal the kernel's at quiescent points | T5 paused every 1,000 steps: maps against `/proc/self/maps` or `mach_vm_region`, descriptors against `/proc/self/fd` or `proc_pidinfo` |

## Steps

**Step 1** is on `main`: items 1 to 3 (strata:66ddb16), CT-R5's rollback (strata:d3a59ee), and
item 5, `demote()`, `promote()` and `map_owed` (strata:081b7e7; `warm-indexes.md`; code review
`cold-tier-item5-code.md`, approve to land). K4 and item 6 (lazy segments as the default) remain,
with criteria K1 to K8 in the ledger.

**Step 2: the catalog** (D3's read side, D4 to D6, D10's cold predicate), in landable items. Each
item's criteria have a test and a mutant the test kills, listed in the ledger with the options
each runs with (C7's shape).

1. **Index prerequisites.** `close_ready()` (D10); the per-index unleased count (D3).
   - **S1.** Index B's record mapped inside index A's chore counts on B; a reader's map counts; a
     map in `promote()` does not. Mutant: test the thread's lease count (today's code).
   - **S2.** With 5 files queued for reclaim, `close_ready()` is false; after `reclaim()` and its
     deletes, true; after `close()` the storage budget equals the bytes the open indexes hold on
     disk (`quiescence_probe` part 2 as the oracle). Mutant: drop the queue clause.
2. **Counted IO and caps** (D6, CT-R8, CT-R12), usable by bare indexes.
   - **S3.** Each cap driven to its hard value: the reservation that passes it throws
     `too_many_open`, no kernel error, nothing marked or fenced; a failed system call and a lost
     install race return their reservations; counts equal the kernel's at quiescent points (T7);
     with the soft limit set to 256 and a higher hard one, construction raises it and caps from the
     raised value. Mutants: reserve after the system call (a barrier between the two maps at the cap
     less one lets both pass); keep a lost race's reservation; cap from `kern.maxfilesperproc`
     without the raise.
3. **Slots, table and pin** (D4, CT-R13, CT-R15), with opens single-flighted; no sweep yet.
   - **S4.** T6's pin schedules with slot reuse; a pin on a transition suspends and resumes, and
     `try_pin` answers `would_wait`; a second key on a held directory fails its open once. Mutants:
     time stored after the decrement (TSan and the schedule); `demoting` stored, or waiters
     resumed, outside the shard mutex (a parked waiter never resumed); the sweep reading fields
     before its compare-exchange; a re-pin by handle that skips the `gen` check; a lock conflict
     retried.
4. **The wake and its bound** (D5, CT-R23, I5-N2).
   - **S5.** Under `defer_io`, indexes demoted between pins: no pin is handed out before
     `promote()` completes, `unleased_maps()` stays zero, no `apply` answers `map_owed`. Mutants:
     hand out the pin before the wake ends; read the flag before the pin's compare-exchange.
   - **S6.** Persistent faultfs `EMFILE` in `promote()`: pins in backoff fail with `wake_failed`
     and post no chore; after 8 failures a committed slot closes and the next pin reports the
     reopen's error; an uncommitted one probes every 10 s; `too_many_open` does not count. Mutants:
     no backoff (a chore per pin); no close after the bound.
5. **The sweep** (D5, D6, CT-R4, CT-R6).
   - **S7.** Demotion only from `{0, open}`; cold only from warm with a maps count of zero and
     `close_ready()`; a queued reclaim posts and closes on a later pass; a view kept past its pin
     keeps the slot `closing` without blocking a thread. Mutants: close from hot; close with a
     retired view; demote a pinned slot; close with a queued reclaim.
6. **S3-FIFO** (D6, CT-R10).
   - **S8.** The catalog's policy code over a recorded Zipf trace with stub indexes is within
     0.5 point of `zipf_policy`'s S3-FIFO at θ = 0.99 and 1.2, and a one-time scan leaves the main
     FIFO's keys open. Mutants: the pin does not set the frequency; the ghost ignored.
7. **S9.** T5's step-2 run under TSan, ASan and UBSan with its counters, and CI green on every
   platform. Then the lead's T1 to T4 and T2d.

**Xapiand adopts it** in its own change (X-POOL, C8): the registry becomes the catalog's `H`, its
`refs` (`src/database/pool.h:191`) become pins held by reads and writes alike, its idle close goes, and its turn handles
`map_owed` with CT-R22's bounded loop beside `flush_owed`; its gate asserts every `defer_io`
index's `unleased_maps()` is zero.

Then step 3, the sentinel (CT-R7); step 4, the idle core (D7).

## Limits

- **Warm holds one descriptor and one vnode** for the lock, so macOS warm caps near 100,000, not
  at memory (Q1).
- **Misses cost an open.** At θ = 0.99 over 1M with 110,000 open, S3-FIFO misses 18% of
  requests (the model; LRU 22%). A macOS open is 0.4 to 1.1 ms p50, half its 54 syscalls repeat
  `fstat`/`geteuid`, so a miss-heavy load spends cores on opens; T4's opens per second show it.
- **A pin by key takes its shard's mutex**, so the hottest key's pins by key serialize there
  (6.5% of requests at θ = 0.99); a host re-pins by handle (D4).
- **A wake costs a chore hop** before the first pinned call on a demoted `defer_io` index (T2d).
- **A synchronous `get` on a warm index without `defer_io` maps** each segment it reads (D9);
  tasks map at their residency point.
- **The storage budget counts open indexes.** Cold indexes' files are on disk outside it; a host
  bounds the disk by its free space (`no_space`).
- **The first commit after an open maps** each segment whose facts are unknown (D9), unless a
  manifest field carries them (Q5), and a segment whose first map failed blocks commits until
  `heal()` (D9).
- **Corruption surfaces at first use**, as `corrupt_segment` to the reader (D11).
- **Out of scope**: cold under `strata::group` (needs the journal checkpoint; Track C).
- **FreeBSD is unmeasured**; it has `kern.maxvnodes`, so the macOS counters apply until measured.

## Notes

**N1. Warm's budget, corrected (CT-R9).** `warm_bytes_probe.out` (one-document indexes on
`PosixIO`, requested heap): hot is 5,394 B in 34 allocations; retiring and freeing the view
returns 304 B; the empty active buffer returns 920 B (`sizeof(buffer)` 896); the segment objects
1,866 B in 14; 2,304 B stay (`basic_index` 1,320 B, the publication 136 B, the reader state 96 B,
the manifest and file maps). So `idle-index.md`'s "the `view` 920" is the empty active buffer, its
"publication 208" is the view (`sizeof(view)` 208), and its "reader state 136" is the publication;
cut 2's 1,360 B saving rests on those labels. The review's arithmetic, not a measurement: warm as
D3 states it, with a record of about 200 B per segment and cut 3's 850 B, keeps about 2.6 KB of
heap per one-segment index, about 1.7 KB with the empty buffer dropped (step 3), both before the
slot (about 192 B plus `H`, an estimate, D4), `PosixIO` and allocator slack; step 1's demoted
index, which keeps a view and the buffer, about 3.7 KB. T1 restates warm from the measured pieces per segment count before it gates.

## Where each step-2 finding closes

| Finding | Decision | Where |
|---|---|---|
| CT-R4 | Cold requires an empty reclaim queue and no debt; the sweep posts `reclaim()` and closes on a later pass; the idle core leaves the queue outside | D5, D7, D10, S2 |
| CT-R6 | Cold only from warm with a maps count of zero; pins suspend, `try_pin` answers `would_wait`; a stall past a pin is counted, never waited on by a thread | D4, D5, S7 |
| CT-R8 | Counts from the wrapped IO, reserved by `fetch_add` before the system call, returned on failure and on a lost race; the evictor takes a slot holding the resource over its cap | D6, S3 |
| CT-R10 | S3-FIFO, its frequency set by the pin's compare-exchange | D6, S8, T4 |
| CT-R11 | T1 to T4 restated per platform and fixture, T2d added | Gate rows |
| CT-R12 | The soft `RLIMIT_NOFILE` raised to the hard limit at construction; caps from the raised value less a reserve | D6, S3 |
| CT-R13 | The three orders stated; re-pin by handle, two read-modify-writes | D4, S4 |
| CT-R15 | `hash_slots`, injective resolution, closing slots' IOs leaked, a debug pin check at `live()` | D4, S4 |
| CT-R23 | Reads pin; the wake runs `promote()` before a pin is handed out; the count is per index | D3, D5, I9, S5 |
| CT-R24 | The wake's cost per platform with the hop unmeasured; T2d | D3, T2d |
| I5-N2 | Wake failures back off from 10 ms to 10 s; after 8 a committed slot closes | D5, S6 |
| I5-N3 | `unleased_maps()` counts against this index's own lease | D3, S1 |
| X-POOL | The registry becomes the catalog's payload; reads and writes pin | D4, Steps |

I5-N1 (a `skipped` apply ends the turn) changes nothing: under the catalog a turn holds its pin,
so the sweep cannot demote inside it, and the owed bit is a backstop.

## Open for step 3 and later

- **CT-R7 (step 3).** The sentinel reads as an empty index: `acquire()` guards any non-null
  pointer (`hazard.hh:119-130`); it must throw `index_unpublished` or publish, never be in
  `pub_->current`, never be retired; the warm-to-hot build skips refresh's flush and merges; under
  `defer_io` that build is the wake's (D5).
- **C4.** When durability-modes' checkpoint batch lands, cold's predicate gains "no view the owner
  keeps".

## Questions for the owner

Q1 to Q5 are ruled (the lead's answers, upheld by the review).

**Q1. Should warm drop its descriptor?** Ruled (a): keep the per-index lock for steps 1 to 4; one
lock per partition is step 5 with its own review.

**Q2. Does the catalog belong in strata core?** Ruled (a): core, the host supplying caps, timer,
executor and key-to-directory resolver.

**Q3. Segment checks at first map?** Ruled (a), with `eager_segments` for tools, `check` and
installs, on D9's failure rules, D11's open check and twelve cases, and CT-R5 on `main` first.

**Q4. A summary per cold index?** Ruled (a): none.

**Q5. Should the manifest carry each segment's facts?** Ruled (a) for step 1, decided on numbers
before step 2. (a) No: the first commit after an open maps each segment whose facts are unknown
(D9), once per open. (b) Yes: `segment_ref` gains `structures`, `version` and `total_length` (16 B
per segment; a format change, open until release). Commits then map only records with new deletes
and no body (`live_length()`, `:4436`, `:4549-4556`), and a damaged segment no longer blocks
commits (D9, commit with a marked record). T2w decides it.

**Q6. May step 2 build before T2w decides Q5?** (a) Yes: no catalog item reads or writes the
facts, so Q5's answer changes no step-2 code, and T2w runs on the catalog's `tier` mode, which
step 2 builds. (b) No: hold step 2 until T2w runs on today's `main`. Recommendation: (a).

## What the review should look at

Revision 4 (step 2): D3's read side (reads pin, the per-index count, the wake's cost per platform),
D4 to D6, D7's queue, D10's `close_ready()`, I4, I6, I7, I9, I10, the gate rows (T1 to T4
restated, T2d), the step-2 items and S1 to S9, and the table above against each finding. Citations
for step 2 are at `main` `081b7e7`; D9 and D11 keep step 1's at `c82ac9f`.
