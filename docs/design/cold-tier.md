# The cold tier: millions of indexes per host

> Status: DRAFT (2026-10-27) revision 0, designing (ready for preliminary review). Concurrency and
> lifetimes across threads: the architect reviews it before step 1 is built.

Cited at strata `main` `98ce743`; measurements from `docs/perf/idle-index.md` on `perf/idle-index`
`2562e8f` (harness `bench/idle_index`, not yet on `main`); shared-reader at `design/shared-reader`
`15b801a`; write buffer at `design/write-modes` `ae2cf70`. RocksDB 11.11 was read for its table
cache; none of its code enters strata.

## Summary

An index lives in one of four tiers. **Hot** is today's open index. **Warm** keeps the index
object and its directory lock but no mapping and no view, about 1.5 KB. **Cold** is closed:
nothing in memory. **Remote** is cold with its files on object storage (reserved). A catalog per
pool partition pins indexes for requests, opens them on demand, and demotes them by a CLOCK sweep
over counters of the OS resources each tier holds, so mappings, descriptors and vnodes are bounded
by the hot and warm set, never by the number of indexes on disk.

The concurrency rests on a rule the shared reader already keeps (I1): **a mapping dies only with
its segment object, under the write lease, after no protected view holds it.** Demotion swaps in
unmapped twins, as `heal()` swaps damaged records, and never strips a live one.

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
| write-buffer D2 | checkout; chores by `chore_scope` or owed to the holder (`owner.hh:148-171`, `index.hh:725-739`) | demotion is a chore; a catalog pin is a checkout |
| `ARCHITECTURE.md` | residency, suspension at query start (`:173-174`), overload shed (`:179-180`) | "footers and small sections at open" (`:149`) becomes "at first map" (Q3) |
| `DISTRIBUTION.md` | per-node rows (`:28-30`) | its "Cold" (`:94-98`, object storage) is this design's remote |
| `small-indexes.md`, `limits.md` | direction 2, "open lazily, close idle"; caps name their mechanism | this design is direction 2; `limits.md` gains D6's caps |

## Decisions

### D1. Four tiers, each defined by what it holds

| Tier | Holds | Dirty per index | fds | vnodes (macOS) | maps |
|---|---|---|---|---|---|
| Hot | open `basic_index`, the segments touched since promotion mapped, a view | today's measured 5.9 KB macOS, 8.1 KB Linux; target: no regression | 1 | 1 + mapped segments | mapped segments |
| Warm | open `basic_index` and directory lock, manifest, unmapped segment records, no view, idle core, a catalog slot | target ≤ 1.5 KB macOS, ≤ 2.2 KB Linux (estimate: the measurer's 1.4 and 2.0 KB after the three cuts, plus a ~130 B slot) | 1 | 1 | 0 |
| Cold | nothing in the process | 0 | 0 | 0 pinned | 0 |
| Remote | a cold index whose data files are on object storage (D8) | 0 | 0 | 0 | 0 |

On disk a 1-document index is 738 B in 3 files, 12 to 16 KB with blocks.

### D2. A segment is a record plus a body mapped at most once (cut 1)

`open_segment` (`index.hh:2093-2111`) splits. The **record** keeps the owner-written state (entry,
copy-on-write deleted bits, dead count, `damaged`, `unsynced`). The **body** (mapping or owned
bytes, and the decoded `segment`) is immutable and reached through an `std::atomic<const body*>`
in the record, null at open.

- **First use maps.** A thread that loads null opens and maps the file through the index's IO,
  runs the checks `load_segment_of` runs at open today (`:4458-4500`: size, footer CRC, identity,
  document count, structures, versions), and installs the body by compare-exchange from null
  (acq_rel). The loser destroys its unseen body. A body is never replaced or cleared before its
  record dies.
- **Open stops mapping.** Recovery builds records from the manifest and still reads live-docs
  files, and it already opens every file for its size (`:4319`), so a missing file still fails
  the open. The other checks move to first use (Q3); `options::eager_segments` maps all at open
  through the same code.
- **One accessor.** `segment_view::seg` (`:926-931`) becomes `seg()`, an acquire load that maps on
  null; prospect's call sites (`searcher.hh:98-99,223`) migrate. A task maps at its residency
  point, offloaded; a synchronous `get` maps inline. A merge maps its sources under the lease
  before the job leaves the owner.

### D3. Demotion to warm: twins, then the view retires (cut 2)

Demotion to warm is a chore under the lease, only on a **quiescent** index: no buffered record, no
pending merge (`pending_`, `:4512`), no unsynced segment, no posted delete (`deletes_pending()`,
`:851`), not closing. It swaps every record whose body is set for a twin (same entry, bits shared,
no body), as `heal()` swaps records (`:879`); retires the current view; and publishes the
**unpublished** sentinel, a static view never freed. The old records stay in the retired view
until a registry scan finds no record naming it (shared-reader D6); their mappings go then.

An acquire that meets the sentinel releases its record and awaits `publish_through(applied)`
(shared-reader D9, `:1227-1233`), which treats the sentinel as no view and builds one over the
twins, without I/O. `reader()` (`:1210-1222`) publishes the sentinel instead of building a view. That is cut 2: a warm index
holds the 136 B reader state, not the 1,496 B publication.

### D4. The catalog, one per pool partition

Beside the partition's registry (shared-reader D4), it owns the partition's open indexes, a
key-to-directory IO factory, the counters, and the CLOCK hand.

- **Slots** live in a chunked array never freed while the catalog lives. A slot holds the key,
  the index, its reader handle, a CLOCK bit, a last-use time, waiters, and a 64-bit word
  `pins:32 | state:8`, state one of `opening`, `open`, `demoting`, `closing`.
- **The table** maps keys to slots in 64 shards, each an open-addressed map under a mutex held
  for one probe, insert or erase, never across I/O. Only non-cold indexes have a slot. RocksDB's
  table cache is this shape: sharded handles bounded by `max_open_files` (`options.h:792,1114`,
  `db/table_cache.h:64`). `lru-cache` is not reused: it relinks a list per hit and is not
  thread-safe.
- **A pin is a checkout**: under the shard mutex, a compare-exchange from `{p, open}` to
  `{p+1, open}`, setting the CLOCK bit if clear. Unpin is a lock-free decrement plus a time store.
  One read-modify-write per request; a catalog guard holds its pin.

### D5. Transitions

```
  cold ──pin miss──▶ opening ──▶ open (hot ⇄ warm) ──sweep: {0,open}→{0,closing}──▶ closing ──▶ cold
                        ▲ waiters resumed                                              │ waiters
                        └──────────────────────────── reopen ◀─────────────────────────┘
```

- **Cold to open.** The first pinner inserts an `opening` slot holding its pin and opens on the
  host's executor (about 54 syscalls), never a reactor thread; later pinners wait on the slot and
  are posted to their executors. A failed open erases the slot and fails its waiters.
- **Warm to hot** is the first reader's view build (D3) and maps (D2).
- **Hot to warm.** The sweep compare-exchanges `{0, open}` to `{0, demoting}`, runs D3 under
  `chore_scope`, which never waits, and stores `open`; pinners meeting `demoting` wait. A held lease means busy: it
  restores `open` and moves on. The exchange keeps the host's lease-less reads (`get`, `scan`,
  which a pinned client may call, `index.hh:718-724`) from overlapping the chore.
- **Open to cold.** The sweep compare-exchanges `{0, open}` to `{0, closing}` (a racing pin fails
  its compare-exchange and waits), takes `chore_scope`, rechecks quiescence and that nothing is
  uncommitted (`applied_since_commit_`, `:4547`; demotion never commits), and calls `close()`.
  Under the shard mutex it then erases the slot, or sets `opening` and reopens if waiters came.
  If `close()` leaves the index `closing()` (a view outlived its pin), the slot stays `closing`,
  counted against every cap, and the sweep retries `try_finish_close()` (`:610-633`).

### D6. Policy: CLOCK over resource counters

The catalog counts what the OS limits: **maps** (a body installed or destroyed), **fds** (an
index opened or closed; an open uses one more briefly), **vnodes** on macOS (one per open index and
per mapped segment), and **bytes** (per-tier constants plus body sizes) on a `byte_budget`. Past a
soft cap the sweep demotes; at a hard cap a promotion (an open or a first map) that cannot first evict an
unpinned slot fails with `too_many_open`, ARCHITECTURE's overload error, never a wait. Caps come from the OS at
construction (defaults are presets, measured as `limits.md` requires):

| Platform | Mechanism (stock value) | Soft / hard |
|---|---|---|
| Linux | mapped segments, `vm.max_map_count` (65,530) less the process's maps at start | 50% / 90%: 32,765 / 58,977 |
| Linux | open indexes, `RLIMIT_NOFILE` soft limit less 1,024 for the host | 75% / 95% |
| macOS | vnodes, `kern.maxvnodes` (263,168 on the 32 GiB Mac), system-wide | 40% / 60%: 105,267 / 157,900 |
| macOS | open indexes, `kern.maxfilesperproc` (1,048,576) | 75% / 95% |
| Both | serving bytes | the host's budget |

The sweep runs on one host timer per partition at 1 Hz or faster, with bounded work per call, and
inline when a promotion crosses a soft cap. It skips pinned slots, clears set CLOCK bits, demotes
an unmarked hot slot over a hot cap or idle 30 s, and an unmarked warm slot over a warm cap or idle
10 min. A pin pays one conditional bit store where LRU relinks a list, RocksDB's HyperClockCache trade
(`cache.h:371`). Stock Linux then holds at most 32,765 mapped segments and macOS 105,267 vnodes
(say 80,000 warm and 12,000 one-segment hot indexes), whatever the index count.

### D7. The idle core (cut 3)

Of the `index` object's 1,320 B, about 650 B is writer and maintenance state an idle index never
uses, plus 200 B of merge machinery (`idle-index.md`, cut 3; to recount on main's write-buffer
fields). It moves behind one pointer made at the first writer call, freed at demotion to warm.

### D8. Remote is reserved

A remote index is cold with no data files local; a pin miss fetches them before the cold path
runs, so the catalog needs only that hook. Its design is DISTRIBUTION's storage tiers (PLAN D6).

## Correctness under concurrency

**I1. A mapping dies only with its record**, under the lease, after the registry scan found no
protected view holding the record, and with no merge job holding it (shared-reader D4, D6). A body
is installed at most once and never cleared, so a guard's segments stay mapped through any demotion.

**I2. Demotion changes no file set.** Twins name the same files; `retain()` and "a held file is
never truncated" (shared-reader D7) are untouched.

**I3. One instance per directory**: one slot per key by the shard mutex, one owner across
processes by the directory lock, one performer per transition by the slot word.

**I4. Cold only when unpinned, quiescent and committed.** The slot compare-exchange excludes a
pin; quiescence excludes merges, posted deletes and buffered records; "nothing uncommitted"
excludes lost work, since `close()` drops it (`:577`).

**I5. Demotion is invisible to reads**: the next view has the same position and digest.

**I6. Counters are exact at quiescent points**: each changes at the event it counts.

**I7. A slot outlives what reaches it.** A lazy map uses the slot's IO; a slot is freed only after
`close()` completes, so after every view is gone.

## Gate rows

Lead-run on `main` under `quiet exclusive`, extending `bench/idle_index` with `catalog` and `tier`
modes; branches make functional claims only.

| Row | Target | Procedure |
|---|---|---|
| **T1** memory per tier | Hot within 5% of 5.9 KB macOS, 8.1 KB Linux; warm ≤ 1.5 KB macOS, ≤ 2.2 KB Linux, 0 maps; cold ≤ 16 B (noise), 0 fds, 0 maps | `idle_index open ROOT 100000 shared tier {hot,warm,cold}` on 1-, 100- and 1,000-document fixtures, idle 5 s, metered as `idle-index.md`; the counting build for the heap |
| **T2** first query per tier | p99: hot ≤ 10 µs; warm ≤ 100 µs Linux, ≤ 1 ms macOS; cold ≤ 400 µs Linux cached, ≤ 5 ms Linux dropped, ≤ 2.5 ms macOS. Estimates from measured open p99s: Linux 230 to 330 µs cached, 1 to 4 ms dropped; macOS 0.8 to 1.9 ms | 100,000 indexes forced to one tier, one `get` each in random order; Linux also after `drop_caches` in the guest |
| **T3** 1M and 10M per host | No `ENFILE`, `EMFILE`, `ENOMEM`; counters within hard caps at every 100 ms sample; RSS within the bytes cap plus baseline; stock sysctls | `idle_index build` 1M and 10M (10M is 120 to 160 GB and 40M inodes: ext4 `-i 4096` or XFS; APFS clones ran 198 s per 1.1M), touch each index once, then 10 min of T4's load; Linux and macOS |
| **T4** Zipf over 1M | Hot-plus-warm hit ratio ≥ 0.80 at θ = 0.99 and ≥ 0.95 at θ = 1.2 with 110,000 open (a perfect frequency cache: 0.837, 0.967, from the Zipf CDF); p50 ≤ 10 µs; p99 ≤ T2's cold target; no cap exceeded | 8 threads, one `get` per request, Zipf keys, caps 10,000 hot and 110,000 open; reports p50/p99/p999, hits per tier, opens, closes, demotions per second |
| **T5** sanitizers | Zero TSan, ASan, UBSan reports; no truncation under a mapping | 64 indexes, catalog capped at 8 open and 16 maps so every operation crosses tiers; readers get and scan, some holding guards across pauses and releasing on other threads; writers apply, refresh, commit, merge on `pool_executor`, `heal()`; sweep every 1 ms; faultfs mapping on, 4 KiB `reclaim_step` (truncate abort as oracle), and POSIX IO |
| **T6** schedules | Every read equals the model; generations never decrease; mutants die | 1,000 seeded schedules on `inline_executor` (shared-reader T2a's shape) over the reader's load, claim, validate, body install, read, release; the sweep's slot exchange, twin swap, publish, scan, free, close; pinners on `opening` and `closing`. Mutants: body cleared in place; no pin check before warm or cold; body stored without compare-exchange; demotion with a merge, posted delete or uncommitted work; slot erased before `close()` returns |
| **T7** counter fidelity | Counters equal the kernel's at quiescent points | T5 paused every 1,000 steps: maps against `/proc/self/maps` or `mach_vm_region`, fds against `/proc/self/fd` or `proc_pidinfo` |

## First buildable step

**Step 1: cut 1 and `demote()`, without the catalog** (one developer, about a week):

1. Record and body (D2), the compare-exchange install, the checks moved from `load_segment_of`,
   `options::eager_segments`.
2. `segment_view::seg()`, strata's reads migrated; prospect's three call sites in a companion change.
3. `basic_index::demote()`: D3 without the sentinel (twins, then a new view over them).
4. T6 for these steps, T5 on one index, T1's warm row without a slot.

Then step 2, the catalog (D4 to D6, T3 to T5); step 3, the sentinel; step 4, the idle core. Only
Q3 touches step 1, and only `eager_segments`' default.

## Limits

- **Warm holds one descriptor and one vnode** for the lock, so macOS warm caps near 100,000, not
  at memory (Q1).
- **Misses cost an open.** At θ = 0.99 over 1M, at least 16% of requests miss 110,000 open indexes. A macOS
  open is 0.4 to 1.1 ms p50, half its 54 syscalls repeat `fstat`/`geteuid`, so a miss-heavy load
  silently spends cores on opens; T4's opens per second show it.
- **A synchronous `get` on a warm index blocks for one map**, estimated 33 to 50 µs from the old
  base's lazy segments; tasks map at their residency point.
- **Corruption surfaces at first use**, as `corrupt_segment` to the reader (Q3).
- **Out of scope**: cold under `strata::group` (needs the journal checkpoint; Track C).
- **A guard kept past its pin** holds a `closing` slot and its caps; `views_held()` names it.
- **FreeBSD is unmeasured**; it has `kern.maxvnodes`, so the macOS counters apply until measured.

## Questions for the owner

**Q1. Should warm drop its descriptor?** (a) Keep the per-index lock. (b) One lock per partition,
held by the catalog, with a marker in the partition root that a standalone `open()` beneath it
must lock first: warm holds 0 fds and 0 vnodes and is bounded by memory alone. Recommendation:
(a) for steps 1 to 4, (b) as step 5 with its own review, since it changes who may open an index.

**Q2. Does the catalog belong in strata core?** Small-indexes F3 parked the handle cache as host
policy. (a) Core, the host supplying caps, times and the key-to-directory resolver. (b) Xapiand.
Recommendation: (a): demotion is correct only with strata's lease, views and records.

**Q3. Segment checks at first map?** (a) Yes, with `eager_segments` for tools, `check` and
installs. (b) At open, mapping and unmapping at once, keeping the syscalls cut 1 removes.
Recommendation: (a); a damaged segment still fails closed, one query later.

**Q4. A summary per cold index?** (a) None; existence is a directory lookup. (b) Document count
and applied position in the catalog, 16 B × 10M = 160 MB. Recommendation: (a) until a host needs
routing without opening.

## What review should look at

1. I1 and D3: the twin swap against lazy installs racing it.
2. D5's slot word and shard mutex against pins, waiters and the `closing` fallback.
3. Owner-held references quiescence may miss (a checkpoint batch, `heal()`'s retry).
